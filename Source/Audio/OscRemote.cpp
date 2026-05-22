#include "OscRemote.h"
#include "AudioEngine.h"

namespace zynforge
{
    OscRemote::OscRemote (AudioEngine& e) : engine (e)
    {
        // Match anything — we route per-dialect inside oscMessageReceived.
        addListener (this);
    }

    OscRemote::~OscRemote()
    {
        stop();
        removeListener (this);
    }

    bool OscRemote::start (int udpPort)
    {
        stop();
        port      = udpPort;
        listening = connect (udpPort);
        return listening;
    }

    void OscRemote::stop()
    {
        if (! listening) return;
        disconnect();
        listening = false;
    }

    void OscRemote::setDialect (Dialect d) { dialect = d; }

    void OscRemote::oscMessageReceived (const juce::OSCMessage& m)
    {
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

    // ── Helpers ─────────────────────────────────────────────────────────────

    void OscRemote::recordStart()
    {
        const auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                              .getChildFile ("Zynforge Sessions");
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
        engine.startRecording (root.getChildFile ("Session_" + stamp));
    }
    void OscRemote::recordStop() { engine.stopRecording(); }
    void OscRemote::playStart()  { engine.startPlayback(); }
    void OscRemote::playStop()   { engine.stopPlayback(); }

    void OscRemote::dropMarker (const juce::String& name)
    {
        const int n = engine.dropMarkerAtCurrentPosition();
        if (n > 0 && name.isNotEmpty())
            engine.getMarkers().renameMarker (n - 1, name);
    }

    void OscRemote::dropSceneMarker (int scene)
    {
        dropMarker ("Scene " + juce::String (scene));
    }

    void OscRemote::setChannelName (int idx1, const juce::String& s)
    {
        engine.setTrackName (idx1 - 1, s);
    }
    void OscRemote::setChannelMute (int idx1, bool m)
    {
        const int i = idx1 - 1;
        if (i < 0 || i >= engine.getRecorder().getNumTracks()) return;
        engine.getRecorder().getTrack (i).muted.store (m, std::memory_order_relaxed);
    }
    void OscRemote::setChannelArm (int idx1, bool a)
    {
        const int i = idx1 - 1;
        if (i < 0 || i >= engine.getRecorder().getNumTracks()) return;
        engine.getRecorder().getTrack (i).armed.store (a, std::memory_order_relaxed);
    }
    void OscRemote::setChannelColour (int idx1, juce::uint32 rgb)
    {
        engine.setTrackColour (idx1 - 1, juce::Colour ((juce::uint32) (rgb | 0xff000000)));
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
    }

    // ── Dialect handlers ────────────────────────────────────────────────────

    bool OscRemote::handleGeneric (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        if (path == "/zynforge/record")
        {
            if (m.size() == 0 || ! toBool (m[0])) recordStop(); else recordStart();
            return true;
        }
        if (path == "/zynforge/play")
        {
            if (m.size() == 0 || ! toBool (m[0])) playStop(); else playStart();
            return true;
        }
        if (path == "/zynforge/stop") { playStop(); recordStop(); return true; }

        if (path == "/zynforge/marker")
        {
            dropMarker (m.size() > 0 ? toString (m[0]) : juce::String());
            return true;
        }
        if (path == "/zynforge/scene")
        {
            dropSceneMarker (m.size() > 0 ? toInt (m[0], 0) : 0);
            return true;
        }

        // /zynforge/channel/N/name|arm|mute|colour
        if (path.startsWith ("/zynforge/channel/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (18), "/", "");
            if (parts.size() >= 2 && m.size() >= 1)
            {
                const int ch1 = parts[0].getIntValue();
                const auto& key = parts[1];
                if (key == "name")   { setChannelName   (ch1, toString (m[0])); return true; }
                if (key == "mute")   { setChannelMute   (ch1, toBool   (m[0])); return true; }
                if (key == "arm")    { setChannelArm    (ch1, toBool   (m[0])); return true; }
                if (key == "colour" || key == "color")
                {
                    setChannelColour (ch1, (juce::uint32) toInt (m[0]));
                    return true;
                }
            }
        }
        return false;
    }

    bool OscRemote::handleDiGiCo (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        // Snapshots fired on the console → mark in our session.
        if (path == "/Console/Snapshots/recall" && m.size() >= 1)
        {
            dropSceneMarker (toInt (m[0]));
            return true;
        }
        // Transport sync
        if (path == "/Console/Transport/record" && m.size() >= 1)
        { if (toBool (m[0])) recordStart(); else recordStop(); return true; }
        if (path == "/Console/Transport/play"   && m.size() >= 1)
        { if (toBool (m[0])) playStart();   else playStop();   return true; }

        // Channel name / mute sync. /Console/Channels/N/name
        if (path.startsWith ("/Console/Channels/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (18), "/", "");
            if (parts.size() >= 2 && m.size() >= 1)
            {
                const int ch1 = parts[0].getIntValue();
                if (parts[1] == "name") { setChannelName (ch1, toString (m[0])); return true; }
                if (parts[1] == "mute") { setChannelMute (ch1, toBool   (m[0])); return true; }
            }
        }
        return false;
    }

    bool OscRemote::handleAllenHeath (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        // SQ / Avantis OSC: /sq/scene/recall, /sq/ch{N}/name, /sq/ch{N}/mute
        if (path == "/sq/scene/recall" && m.size() >= 1)
        { dropSceneMarker (toInt (m[0])); return true; }
        if (path == "/sq/transport/record" && m.size() >= 1)
        { if (toBool (m[0])) recordStart(); else recordStop(); return true; }
        if (path == "/sq/transport/play" && m.size() >= 1)
        { if (toBool (m[0])) playStart();   else playStop();   return true; }

        if (path.startsWith ("/sq/ch"))
        {
            // /sq/chN/key  →  N is digits right after 'ch'
            const auto rest = path.substring (6);
            int idx = 0; while (idx < rest.length() && juce::CharacterFunctions::isDigit (rest[idx])) ++idx;
            const int ch1 = rest.substring (0, idx).getIntValue();
            const auto key = rest.substring (idx + 1);
            if (m.size() >= 1)
            {
                if (key == "name") { setChannelName (ch1, toString (m[0])); return true; }
                if (key == "mute") { setChannelMute (ch1, toBool   (m[0])); return true; }
            }
        }
        return false;
    }

    bool OscRemote::handleSSL (const juce::OSCMessage& m)
    {
        const auto path = m.getAddressPattern().toString();

        // SSL Live: /sslnet/snapshot/recall, /sslnet/channel/N/name, etc.
        if (path == "/sslnet/snapshot/recall" && m.size() >= 1)
        { dropSceneMarker (toInt (m[0])); return true; }
        if (path == "/sslnet/transport/record" && m.size() >= 1)
        { if (toBool (m[0])) recordStart(); else recordStop(); return true; }
        if (path == "/sslnet/transport/play" && m.size() >= 1)
        { if (toBool (m[0])) playStart();   else playStop();   return true; }

        if (path.startsWith ("/sslnet/channel/"))
        {
            const auto parts = juce::StringArray::fromTokens (path.substring (16), "/", "");
            if (parts.size() >= 2 && m.size() >= 1)
            {
                const int ch1 = parts[0].getIntValue();
                if (parts[1] == "name") { setChannelName (ch1, toString (m[0])); return true; }
                if (parts[1] == "mute") { setChannelMute (ch1, toBool   (m[0])); return true; }
            }
        }
        return false;
    }

    bool OscRemote::handleYamaha (const juce::OSCMessage& m)
    {
        // DM7 / RIVAGE PM OSC: vendor uses /Yamaha/* and sometimes /RIVAGE/*.
        const auto path = m.getAddressPattern().toString();

        if ((path == "/Yamaha/Scene/recall" || path == "/RIVAGE/Scene/recall") && m.size() >= 1)
        { dropSceneMarker (toInt (m[0])); return true; }
        if ((path == "/Yamaha/Transport/record" || path == "/RIVAGE/Transport/record") && m.size() >= 1)
        { if (toBool (m[0])) recordStart(); else recordStop(); return true; }
        if ((path == "/Yamaha/Transport/play"   || path == "/RIVAGE/Transport/play")   && m.size() >= 1)
        { if (toBool (m[0])) playStart();   else playStop();   return true; }

        // /Yamaha/CH/N/Name  /RIVAGE/CH/N/Name
        auto handleChannel = [&] (const juce::String& prefix) -> bool
        {
            if (! path.startsWith (prefix)) return false;
            const auto parts = juce::StringArray::fromTokens (path.substring (prefix.length()), "/", "");
            if (parts.size() < 2 || m.size() < 1) return false;
            const int ch1 = parts[0].getIntValue();
            const auto k  = parts[1].toLowerCase();
            if (k == "name") { setChannelName (ch1, toString (m[0])); return true; }
            if (k == "mute") { setChannelMute (ch1, toBool   (m[0])); return true; }
            return false;
        };
        if (handleChannel ("/Yamaha/CH/")) return true;
        if (handleChannel ("/RIVAGE/CH/")) return true;

        return false;
    }
}
