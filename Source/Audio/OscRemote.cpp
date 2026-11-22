#include "OscRemote.h"
#include "AudioEngine.h"
#include "../Network/ConsoleLog.h"
#include "../Network/SecureToken.h"

namespace zynforge
{
    OscRemote::OscRemote (AudioEngine& e) : engine (e)
    {
        // Match anything -- we route per-dialect inside oscMessageReceived.
        addListener (this);
        onRefusedPacket = [this] (const Source& from, const juce::String& what)
        { engine.getRemotes().logOsc ("osc <- " + from.toString() + "  " + what); };
    }

    OscRemote::~OscRemote()
    {
        stop();
        removeListener (this);
    }

    bool OscRemote::start (int udpPort)
    {
        stop();
        accessToken = securetoken::generate (entropyProviderForTests);
        if (accessToken.isEmpty()) return false;
        port      = udpPort;
        listening = connect (udpPort);
        if (! listening) accessToken.clear();
        return listening;
    }

    void OscRemote::stop()
    {
        if (listening) disconnect();
        listening = false;
        accessToken.clear();
    }

    void OscRemote::setDialect (Dialect d) { dialect = d; }

    void OscRemote::oscMessageReceived (const juce::OSCMessage& m)
    {
        // Logged before anything can refuse it: a desk test needs to see what
        // arrived, and a log can't change the session.
        if (engine.getRemotes().isOscDebug())
            engine.getRemotes().logOsc ("osc <- " + getCurrentSource().toString() + "  " + consolelog::describe (m));

        if (engine.areControlsLocked()) return;

        // Always try the matching dialect first, fall back to Generic.
        switch (dialect)
        {
            case Dialect::DiGiCo:      if (handleDiGiCo     (m)) return; break;
            case Dialect::AllenHeath:  if (handleAllenHeath (m)) return; break;
            case Dialect::SSL:         if (handleSSL        (m)) return; break;
            case Dialect::Yamaha:      if (handleYamaha     (m)) return; break;
            case Dialect::Generic:     break;
        }
        handleGeneric (m);
    }

    // Never acted on, so the log is all a bundle gets.
    void OscRemote::oscBundleReceived (const juce::OSCBundle& bundle)
    {
        if (! engine.getRemotes().isOscDebug()) return;
        juce::StringArray lines;
        consolelog::describeBundle (bundle, lines);
        for (const auto& line : lines)
            engine.getRemotes().logOsc ("osc <- " + getCurrentSource().toString() + "  " + line);
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    void OscRemote::recordStart()
    {
        juce::String error;
        engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StartRecord, error);
    }
    void OscRemote::recordStop()
    {
        juce::String error;
        engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StopRecord, error);
    }
    void OscRemote::playStart()
    {
        juce::String error;
        engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StartPlay, error);
    }
    void OscRemote::playStop()
    {
        juce::String error;
        engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StopPlay, error);
    }

    // Shown where the tablet is being set up: the status line and the OSC log.
    // Once a second at most, as a held or repeating button would flood it.
    void OscRemote::refuseValuelessStop (double nowMs)
    {
        if (nowMs - lastValuelessStopNoticeMs < 1000.0) return;
        lastValuelessStopNoticeMs = nowMs;
        const juce::String notice ("OSC STOP refused -- /zynforge/stop needs a value: "
                                   "1 on press, 0 on release. Set the tablet button to send one.");
        engine.getRemotes().logOsc ("osc: " + notice);
        engine.getRemotes().showHostStatus (notice);
    }

    // Never the token itself, which a log line or a screen photo would carry
    // off. Every few seconds at most: a fader on the tablet sends dozens.
    void OscRemote::refuseWrongToken (double nowMs)
    {
        if (nowMs - lastWrongTokenNoticeMs < kWrongTokenNoticeMs) return;
        lastWrongTokenNoticeMs = nowMs;
        const juce::String notice ("OSC command ignored -- its access token is missing or out of date. Copy the "
                                   "token again from Control Surfaces > Open Sound Control.");
        engine.getRemotes().logOsc ("osc: " + notice);
        engine.getRemotes().showHostStatus (notice);
    }

    void OscRemote::dropMarker (const juce::String& name)
    {
        // Pass the name INTO the drop so it names the just-dropped marker
        // atomically. The old drop-then-rename(count-1) hit the timeline-LAST
        // marker instead, because MarkersManager::drop re-sorts by position.
        engine.getRemotes().dropRemoteMarker (name);
    }

    void OscRemote::sceneRecalled (int scene, const juce::String& sceneName)
    {
        engine.getRemotes().onConsoleScene (scene, sceneName, scenefollow::Path::OscListener);
    }

    void OscRemote::setChannelName (int idx1, const juce::String& s)
    {
        // Route through the engine's capture-aware path so "Create session
        // from console" can collect names for channels that don't exist yet.
        engine.getRemotes().onConsoleChannelName (idx1, s);
    }
    // Channel N is desk channel N, as for names and gain: the strips that
    // listen to it, wherever they sit (they went to strip position N, so a
    // colour or a mute landed on another strip than its name after a drag or
    // a PATCH change). Pair-aware + lock-held: the old check-then-index was a
    // TOCTOU against the vector shrinking, and it half-updated stereo pairs.
    void OscRemote::setChannelMute (int idx1, bool m) { engine.getRemotes().onRemoteChannelMute (idx1, m); }
    void OscRemote::setChannelArm  (int idx1, bool a) { engine.getRemotes().onRemoteChannelArm (idx1, a); }
    void OscRemote::setChannelColour (int idx1, juce::uint32 rgb)
    {
        engine.getRemotes().onRemoteChannelColour (idx1, (juce::uint32) (rgb | 0xff000000));
    }
    void OscRemote::setChannelGain (int idx1, float dB)
    {
        engine.setDeskChannelGain (idx1 - 1, dB);
    }

    // ── Argument-fetch helpers ──────────────────────────────────────────────

    namespace
    {
        bool toBool (const juce::OSCArgument& a)
        {
            if (a.isInt32())   return a.getInt32() != 0;
            if (a.isFloat32()) return a.getFloat32() > 0.5f;
            if (a.isString())  return a.getString().getIntValue() != 0;
            return false;
        }
        int toInt (const juce::OSCArgument& a, int dflt = 0)
        {
            if (a.isInt32())   return a.getInt32();
            if (a.isFloat32()) return (int) a.getFloat32();
            if (a.isString())  return a.getString().getIntValue();
            return dflt;
        }
        juce::String toString (const juce::OSCArgument& a)
        {
            if (a.isString()) return a.getString();
            if (a.isInt32())  return juce::String (a.getInt32());
            return {};
        }
        // A recall's optional name, after its number (as ConsoleDialect reads
        // the DiGiCo one). Without it the recall goes by "Scene N".
        juce::String sceneNameArg (const juce::OSCMessage& m)
        {
            return m.size() >= 2 && m[1].isString() ? m[1].getString() : juce::String();
        }
        float toFloat (const juce::OSCArgument& a)
        {
            if (a.isFloat32()) return a.getFloat32();
            if (a.isInt32())   return (float) a.getInt32();
            if (a.isString())  return a.getString().getFloatValue();
            return 0.0f;
        }
    }

    // ── Dialect handlers ────────────────────────────────────────────────────

    bool OscRemote::handleGeneric (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();
        const double nowMs = testNowMs >= 0.0 ? testNowMs : juce::Time::getMillisecondCounterHiRes();

        // Generic OSC is reachable over UDP and has no connection identity.
        // Require an unguessable token as the final argument for every command
        // that can change the session. Console dialects below intentionally do
        // not expose transport/arm/mute at all because those protocols cannot
        // carry this application token.
        if (! testDispatch
            && (accessToken.isEmpty() || m.size() == 0 || ! m[m.size() - 1].isString()
                || m[m.size() - 1].getString() != accessToken))
        {
            // A tablet left with an old token (the listener restarted) was
            // ignored in silence, STOP included. A desk's own addresses fall
            // through here too and are none of this.
            if (path.startsWith ("/zynforge/")) refuseWrongToken (nowMs);
            return false;
        }

        // Authentication is an envelope, never a command argument. The test
        // dispatch seam omits that envelope deliberately.
        const int payloadCount = m.size() - (testDispatch ? 0 : 1);

        // A tablet's push button sends on press AND on release, so a 0 here is
        // a release and means nothing, as on the desk macro port: a press and
        // its release must never be two taps of the two-tap STOP, and letting
        // go of RECORD must never be one. STOP is /zynforge/stop. A held or
        // bouncing STOP repeats; repeats are counted from the last one seen.
        const bool releaseValue = payloadCount >= 1 && (m[0].isInt32() || m[0].isFloat32())
                               && toFloat (m[0]) == 0.0f;
        if (path == "/zynforge/record")
        {
            if (payloadCount < 1) return false;
            if (toBool (m[0])) recordStart();
            return true;
        }
        if (path == "/zynforge/play")
        {
            if (payloadCount < 1) return false;
            if (! toBool (m[0])) playStop(); else playStart();
            return true;
        }
        if (path == "/zynforge/stop")
        {
            // STOP must say press (1) or release (0), as RECORD and PLAY do. A
            // button set to send the address alone sends the same packet on
            // press and on release, and a press held past the repeat filter
            // ended a take by itself: its release was the second STOP tap.
            if (payloadCount < 1)
            {
                refuseValuelessStop (nowMs);
                return true;
            }
            if (! toBool (m[0])) return true;   // the release
            if (! stopDebouncer.admit ({ deskmacro::Command::Stop, 1 }, nowMs)) return true;
            playStop(); recordStop();
            return true;
        }

        // A marker or scene button sends its value on press and 0 on release.
        // The release is nothing; a repeat inside the debounce window is the
        // same press. The marker's name is a string argument, never the value.
        if (path == "/zynforge/marker")
        {
            if (releaseValue) return true;
            const bool valued = payloadCount >= 1 && ! m[0].isString();
            const int nameAt = valued ? 1 : 0;
            const auto name = payloadCount > nameAt && m[nameAt].isString() ? m[nameAt].getString() : juce::String();
            // A button that sends the address alone sends it on press and on
            // release: held past the repeat filter it dropped two markers. The
            // same message again within 2 s is that key's release, as on the
            // desk macro port (deskmacro::ValuelessPairs, audit 2026-11-22).
            if (valued) markerPairs.forget (deskmacro::Command::Marker);
            else if (markerPairs.isRelease (deskmacro::Command::Marker, nowMs)) return true;
            else markerPairs.pressed (deskmacro::Command::Marker, nowMs);
            if (! markerDebouncer.admit ({ deskmacro::Command::Marker, name.hashCode() }, nowMs)) return true;
            dropMarker (name);
            return true;
        }
        if (path == "/zynforge/scene")
        {
            if (payloadCount < 1) return false;
            if (releaseValue) return true;
            // A string first is the scene's name (README); a number may carry a
            // name after it. A string of digits stays a number, as it was.
            const bool named = m[0].isString() && ! m[0].getString().trim().containsOnly ("0123456789");
            const int scene = named ? 0 : toInt (m[0], 0);
            const auto name = named ? m[0].getString() : payloadCount > 1 ? toString (m[1]) : juce::String();
            // A repeat is the same scene by what it is called, not by number:
            // every named scene used to be "scene 0" and share one slot.
            const auto label = scenefollow::sceneLabel (scene, name).toLowerCase();
            if (! sceneDebouncer.admit ({ deskmacro::Command::MarkerGoto, label.hashCode() }, nowMs)) return true;
            sceneRecalled (scene, name);
            return true;
        }

        // /zynforge/channel/N/name|arm|mute|colour
        if (path.startsWith ("/zynforge/channel/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (18), "/", "");
            if (parts.size() >= 2 && payloadCount >= 1)
            {
                const int ch1 = parts[0].getIntValue();
                const auto& key = parts[1];
                if (key == "name")   { setChannelName   (ch1, toString (m[0])); return true; }
                if (key == "mute")   { setChannelMute   (ch1, toBool   (m[0])); return true; }
                if (key == "arm")    { setChannelArm    (ch1, toBool   (m[0])); return true; }
                if (key == "gain" || key == "trim" || key == "preamp")
                                     { setChannelGain   (ch1, toFloat  (m[0])); return true; }
                if (key == "colour" || key == "color")
                {
                    setChannelColour (ch1, (juce::uint32) toInt (m[0]));
                    return true;
                }
            }
        }
        return false;
    }

    // Shared channel-op dispatch -- every dialect supports the same
    // set of per-channel actions, so once the dialect's path prefix
    // has been stripped down to {<channel-number>, <key>}, the actual
    // setter call is identical.
    bool OscRemote::dispatchChannelOp (int ch1, const juce::String& key,
                                       const juce::OSCMessage& m, bool allowControl)
    {
        if (m.size() < 1) return false;
        const auto k = key.toLowerCase();
        if (k == "name")   { setChannelName   (ch1, toString (m[0])); return true; }
        if (allowControl && k == "mute") { setChannelMute (ch1, toBool (m[0])); return true; }
        if (allowControl && k == "arm")  { setChannelArm  (ch1, toBool (m[0])); return true; }
        // Console preamp / input trim (dB) -> trim-follow. "fader" is the mix
        // fader, a different thing, so it's deliberately NOT mapped here.
        if (k == "gain" || k == "trim" || k == "preamp")
        {
            setChannelGain (ch1, toFloat (m[0])); return true;
        }
        if (k == "colour" || k == "color")
        {
            setChannelColour (ch1, (juce::uint32) toInt (m[0]));
            return true;
        }
        return false;
    }

    bool OscRemote::handleDiGiCo (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        if (path == "/Console/Snapshots/recall" && m.size() >= 1)
        { sceneRecalled (toInt (m[0]), sceneNameArg (m)); return true; }
        if (path == "/Console/Marker")
        { dropMarker (m.size() > 0 ? toString (m[0]) : juce::String()); return true; }

        if (path.startsWith ("/Console/Channels/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (18), "/", "");
            if (parts.size() >= 2)
                return dispatchChannelOp (parts[0].getIntValue(), parts[1], m);
        }
        return false;
    }

    bool OscRemote::handleAllenHeath (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        if (path == "/sq/scene/recall" && m.size() >= 1)
        { sceneRecalled (toInt (m[0]), sceneNameArg (m)); return true; }
        if (path == "/sq/marker")
        { dropMarker (m.size() > 0 ? toString (m[0]) : juce::String()); return true; }

        if (path.startsWith ("/sq/ch"))
        {
            // /sq/chN/key -- N is the digit run immediately after "ch".
            const auto rest = path.substring (6);
            int idx = 0;
            while (idx < rest.length() && juce::CharacterFunctions::isDigit (rest[idx])) ++idx;
            const int ch1 = rest.substring (0, idx).getIntValue();
            const auto key = rest.substring (idx + 1);
            return dispatchChannelOp (ch1, key, m);
        }
        return false;
    }

    bool OscRemote::handleSSL (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        if (path == "/sslnet/snapshot/recall" && m.size() >= 1)
        { sceneRecalled (toInt (m[0]), sceneNameArg (m)); return true; }
        if (path == "/sslnet/marker")
        { dropMarker (m.size() > 0 ? toString (m[0]) : juce::String()); return true; }

        if (path.startsWith ("/sslnet/channel/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (16), "/", "");
            if (parts.size() >= 2)
                return dispatchChannelOp (parts[0].getIntValue(), parts[1], m);
        }
        return false;
    }

    bool OscRemote::handleYamaha (const juce::OSCMessage& m)
    {
        // DM7 / RIVAGE PM expose both /Yamaha/* and /RIVAGE/* prefixes.
        const auto path = m.getAddressPattern().toString();

        auto eitherIs = [&] (const juce::String& y, const juce::String& r) -> bool
        { return path == y || path == r; };

        if (eitherIs ("/Yamaha/Scene/recall",  "/RIVAGE/Scene/recall")  && m.size() >= 1)
        { sceneRecalled (toInt (m[0]), sceneNameArg (m)); return true; }
        if (eitherIs ("/Yamaha/Marker",           "/RIVAGE/Marker"))
        { dropMarker (m.size() > 0 ? toString (m[0]) : juce::String()); return true; }

        auto handleChannel = [&] (const juce::String& prefix) -> bool
        {
            if (! path.startsWith (prefix)) return false;
            const auto parts = juce::StringArray::fromTokens (path.substring (prefix.length()), "/", "");
            if (parts.size() < 2) return false;
            return dispatchChannelOp (parts[0].getIntValue(), parts[1], m);
        };
        if (handleChannel ("/Yamaha/CH/")) return true;
        if (handleChannel ("/RIVAGE/CH/")) return true;
        return false;
    }
}
