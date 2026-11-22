#include "DeskMacro.h"

#include "ConsoleLog.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/SceneFollow.h"

namespace zynforge
{
    namespace
    {
        constexpr double kPrevGraceSeconds = 1.0;
    }

    DeskMacroListener::DeskMacroListener (AudioEngine& e) : engine (e)
    {
        addListener (this);
        onRefusedPacket = [this] (const Source& from, const juce::String& what)
        { engine.getRemotes().logOsc ("macro <- " + from.toString() + "  " + what); };
    }

    DeskMacroListener::~DeskMacroListener()
    {
        stop();
        removeListener (this);
    }

    bool DeskMacroListener::start (int udpPort, const juce::String& sourceIp)
    {
        stop();
        const auto pinned = deskmacro::normaliseSourceIp (sourceIp);
        if (pinned.isEmpty()) return false;
        if (! connect (udpPort, pinned)) return false;
        port      = udpPort;
        source    = pinned;
        listening = true;
        return true;
    }

    void DeskMacroListener::stop()
    {
        if (listening) disconnect();
        listening = false;
    }

    void DeskMacroListener::dispatchForTest (const juce::OSCMessage& m, double nowMs)
    {
        handle (m, nowMs);
    }

    void DeskMacroListener::oscMessageReceived (const juce::OSCMessage& m)
    {
        handle (m, juce::Time::getMillisecondCounterHiRes());
    }

    void DeskMacroListener::oscBundleReceived (const juce::OSCBundle& bundle)
    {
        juce::StringArray lines;
        consolelog::describeBundle (bundle, lines);
        for (const auto& line : lines)
            engine.getRemotes().logOsc ("macro <- " + getCurrentSource().toString() + "  " + line);
    }

    void DeskMacroListener::handle (const juce::OSCMessage& m, double nowMs)
    {
        engine.getRemotes().logOsc ("macro <- " + getCurrentSource().toString()
                                    + "  " + consolelog::describe (m));
        const auto parsed = deskmacro::parse (m);
        if (parsed.command == deskmacro::Command::None) return;

        // STOP must say press (1) or release (0). A key set to send the
        // address alone sends the same packet on press and on release, and a
        // press held past the debounce window ended a take by itself: its
        // release was the second STOP tap.
        const bool addressAlone = deskmacro::isValueless (parsed);
        if (addressAlone && parsed.command == deskmacro::Command::Stop)
        {
            refuseValuelessStop (nowMs);
            return;
        }

        // Every sighting counts for the debouncer, a release's too, so a key
        // that repeats while held stays one press.
        const bool fresh = debouncer.admit (parsed, nowMs);
        if (! addressAlone)
            valueless.forget (parsed.command);
        else if (valueless.isRelease (parsed.command, nowMs))
        {
            engine.getRemotes().logOsc ("macro: the same address again inside 2 s, taken as the key's release "
                                        "and ignored -- send 1 on press for every press to count");
            return;
        }
        if (! fresh)
        {
            engine.getRemotes().logOsc ("macro: repeat inside the debounce window, ignored");
            return;
        }
        if (addressAlone) valueless.pressed (parsed.command, nowMs);
        perform (parsed);
    }

    void DeskMacroListener::report (const juce::String& message)
    {
        engine.getRemotes().logOsc ("macro: " + message);
        engine.getRemotes().showHostStatus ("Desk macro: " + message);
    }

    // Said where the desk is being set up: the status line and the OSC log.
    // Once a second at most, as a held or repeating key would flood both.
    void DeskMacroListener::refuseValuelessStop (double nowMs)
    {
        if (nowMs - lastValuelessStopNoticeMs < 1000.0) return;
        lastValuelessStopNoticeMs = nowMs;
        report ("STOP refused -- /zynforge/macro/stop needs a value: 1 on press, 0 on release. "
                "Set the desk macro to send one.");
    }

    void DeskMacroListener::locateToIndex (int index, const juce::String& refusalIfMissing)
    {
        auto& remotes = engine.getRemotes();
        if (const auto why = remotes.deskLocateRefusal(); why.isNotEmpty())
        {
            report ("marker jump refused -- " + why);
            return;
        }
        const auto& markers = engine.getMarkers().getAll();
        if (index < 0 || index >= (int) markers.size())
        {
            report (refusalIfMissing);
            return;
        }
        const auto marker = markers[(size_t) index];
        if (const auto why = remotes.deskLocateRefusal (marker.sampleOffset); why.isNotEmpty())
        {
            report ("marker jump refused -- " + why);
            return;
        }
        remotes.locate (marker.sampleOffset);
        report ("jumped to marker " + juce::String (index + 1) + ": " + marker.name);
    }

    void DeskMacroListener::perform (const deskmacro::Parsed& p)
    {
        using Command = deskmacro::Command;
        using Action  = AudioEngine::RemoteTransportAction;
        auto& remotes = engine.getRemotes();
        auto& player  = engine.getPlayer();

        if (engine.areControlsLocked())
        {
            report ("refused -- session is locked; unlock on the host");
            return;
        }

        // The host's own boundary: RECORD pre-flight and the two-tap STOP live
        // behind it, and it answers with the refusal the engineer should see.
        const auto transport = [&] (Action action, const juce::String& label)
        {
            juce::String error;
            const bool ok = engine.performRemoteTransport (action, error);
            const auto outcome = ok ? label : label + " -- " + error;
            // Pre-flight has just put its exact reason on the status line, and
            // the error only says to go and read it.
            if (! ok && action == Action::StartRecord) remotes.logOsc ("macro: " + outcome);
            else                                       report (outcome);
        };

        const auto refuseWhileBusy = [&] (const juce::String& label)
        {
            if (! remotes.isSessionBusy()) return false;
            report (label + " refused -- a session operation is running");
            return true;
        };

        switch (p.command)
        {
            case Command::Record:
                transport (Action::StartRecord, "RECORD");
                break;

            // StopAll is the host's STOP button: playback when idle, and the
            // two-tap guard when a take is rolling. Never busy-gated, so a
            // metadata save can't make STOP refuse during a take.
            case Command::Stop:
                transport (Action::StopAll, "STOP");
                break;

            case Command::Play:
                if (! refuseWhileBusy ("PLAY")) transport (Action::StartPlay, "PLAY");
                break;

            // Stopping the player under a punch would move the point its audio
            // is spliced at; a take ends with STOP.
            case Command::Pause:
                if (engine.isRecording()) report ("PAUSE refused -- capture is running; use STOP");
                else                      transport (Action::StopPlay, "PAUSE");
                break;

            case Command::Marker:
                if (refuseWhileBusy ("marker")) break;
                if (engine.getRemotes().dropRemoteMarker() >= 0)
                    report ("marker dropped");
                else
                    report ("marker refused -- " + engine.getRemotes().getMarkerRefusal());
                break;

            case Command::MarkerNext:
                locateToIndex (scenefollow::nextMarker (engine.getMarkers().getAll(), player.getPositionSamples()),
                               "no marker ahead of the playhead");
                break;

            case Command::MarkerPrev:
            {
                const auto grace = player.isPlaying()
                    ? (juce::int64) (juce::jmax (1.0, player.getSampleRate()) * kPrevGraceSeconds)
                    : (juce::int64) 0;
                locateToIndex (scenefollow::previousMarker (engine.getMarkers().getAll(),
                                                            player.getPositionSamples(), grace),
                               "no marker behind the playhead");
                break;
            }

            case Command::MarkerGoto:
                locateToIndex (p.value - 1, "there is no marker " + juce::String (p.value));
                break;

            case Command::None:
            case Command::Count:
                break;
        }
    }
}
