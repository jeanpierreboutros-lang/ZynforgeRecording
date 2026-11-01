// See RemoteControl.h. These bodies moved here from AudioEngine.cpp; only
// their calls back into the engine now go through `engine`.

#include "RemoteControl.h"

#include "CompanionServer.h"
#include "ConsoleLog.h"
#include "DeskMacro.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/OscRemote.h"

#include <juce_events/juce_events.h>
#include <juce_osc/juce_osc.h>

namespace zynforge
{
    // Publish the companion's stable owner before any device callback can
    // start: the audio thread feeds it from the first block.
    RemoteControl::RemoteControl (AudioEngine& e)
        : engine (e), companion (std::make_unique<CompanionServer> (e))
    {
    }

    RemoteControl::~RemoteControl() = default;

    bool RemoteControl::startCompanionServer (int port)
    {
        // Default to loopback -- the engineer opts into LAN exposure
        // via startCompanionServerOnLan when they actually need it.
        return companion->start (port, "127.0.0.1");
    }

    bool RemoteControl::startCompanionServerOnLan (int port)
    {
        return companion->start (port, "0.0.0.0");
    }

    juce::String RemoteControl::getCompanionAccessUrl() const
    {
        return companion != nullptr ? companion->getAccessUrl() : juce::String();
    }

    bool RemoteControl::isCompanionExposedOnLan() const
    {
        return companion != nullptr && companion->isExposedOnLan();
    }
    void RemoteControl::stopCompanionServer()
    {
        if (companion != nullptr) companion->stop();
    }
    bool RemoteControl::isCompanionServerRunning() const noexcept
    {
        return companion != nullptr && companion->isRunning();
    }
    int RemoteControl::getCompanionServerPort() const noexcept
    {
        return companion != nullptr ? companion->getPort() : -1;
    }

    bool RemoteControl::startOsc (int udpPort, int dialectIndex)
    {
        if (osc == nullptr) osc = std::make_unique<OscRemote> (engine);
        osc->setDialect ((OscRemote::Dialect) juce::jlimit (0, 4, dialectIndex));
        return osc->start (udpPort);
    }

    void RemoteControl::stopOsc()
    {
        if (osc != nullptr) osc->stop();
    }
    bool RemoteControl::isOscListening() const
    {
        return osc != nullptr && osc->isListening();
    }
    int RemoteControl::getOscPort() const
    {
        return osc != nullptr ? osc->getPort() : 0;
    }
    juce::String RemoteControl::getOscAccessToken() const
    {
        return osc != nullptr ? osc->getAccessToken() : juce::String();
    }
    int RemoteControl::getOscDialect() const
    {
        return osc != nullptr ? (int) osc->getDialect() : 0;
    }

    bool RemoteControl::requestConsoleChannelNames (const juce::String& consoleHost, int port, int dialect)
    {
        if (consoleHost.trim().isEmpty() || port <= 0 || port > 65535) return false;
        juce::OSCSender sender;
        if (! sender.connect (consoleHost.trim(), port)) return false;
        const auto target = consoleHost.trim() + ":" + juce::String (port);
        const auto send = [&] (const juce::OSCMessage& m)
        {
            sender.send (m);
            if (isOscDebug()) logOsc ("osc -> " + target + "  " + consolelog::describe (m));
        };

        // Best-effort "report your channels" nudge per console dialect -- their
        // OSC implementations differ, so we fire a couple of plausible refresh /
        // subscribe forms. The channel-name replies come back on the normal
        // receive path (each dialect parser already handles channel/N/name).
        switch (dialect)
        {
            case 2:   // Allen & Heath
                send (juce::OSCMessage ("/sq/channels/?"));
                send (juce::OSCMessage ("/sq/refresh"));
                break;
            case 3:   // SSL Live
                send (juce::OSCMessage ("/sslnet/channel/?"));
                send (juce::OSCMessage ("/sslnet/refresh"));
                break;
            case 4:   // Yamaha / RIVAGE
                send (juce::OSCMessage ("/Yamaha/Channels/?"));
                send (juce::OSCMessage ("/RIVAGE/Channels/?"));
                break;
            default:  // DiGiCo (1) / generic
                send (juce::OSCMessage ("/Console/Channels/?"));
                send (juce::OSCMessage ("/Console/Channels", (juce::int32) 1));
                send (juce::OSCMessage ("/info"));
                break;
        }
        sender.disconnect();
        return true;
    }

    void RemoteControl::logOsc (const juce::String& line)
    {
        if (! isOscDebug()) return;
        const auto now = juce::Time::getCurrentTime();
        // The one place every line passes: whatever a formatter or a desk's
        // string carried, the live token never reaches a copyable log. Scrub
        // before cutting to length, or a cut could keep part of it.
        const auto token = getOscAccessToken();
        const auto clean = token.isNotEmpty() ? line.replace (token, "[redacted]") : line;
        auto entry = now.formatted ("%Y-%m-%d %H:%M:%S.")
                   + juce::String (now.getMilliseconds()).paddedLeft ('0', 3) + "  "
                   + clean.substring (0, kOscLogMaxChars);

        const juce::ScopedLock sl (oscLogLock);
        oscLog.push_back (std::move (entry));
        ++oscLogRevision;
        while ((int) oscLog.size() > kOscLogMaxLines)
        {
            oscLog.pop_front();
            ++oscLogEvicted;
        }
    }
    juce::StringArray RemoteControl::getOscLog() const
    {
        const juce::ScopedLock sl (oscLogLock);
        juce::StringArray out;
        out.ensureStorageAllocated ((int) oscLog.size());
        for (const auto& line : oscLog) out.add (line);
        return out;
    }
    juce::StringArray RemoteControl::getOscLogTail (int maxLines) const
    {
        const juce::ScopedLock sl (oscLogLock);
        juce::StringArray out;
        const auto skip = oscLog.size() > (size_t) maxLines ? oscLog.size() - (size_t) maxLines : (size_t) 0;
        for (auto it = oscLog.begin() + (std::ptrdiff_t) skip; it != oscLog.end(); ++it) out.add (*it);
        return out;
    }
    juce::int64 RemoteControl::getOscLogRevision() const
    {
        const juce::ScopedLock sl (oscLogLock);
        return oscLogRevision;
    }
    juce::int64 RemoteControl::getOscLogEvicted() const
    {
        const juce::ScopedLock sl (oscLogLock);
        return oscLogEvicted;
    }
    void RemoteControl::clearOscLog()
    {
        const juce::ScopedLock sl (oscLogLock);
        oscLog.clear();
        oscLogEvicted = 0;
        ++oscLogRevision;
    }

    juce::String RemoteControl::formatConsoleLog (const juce::String& consoleLinkSummary) const
    {
        static const juce::StringArray dialects { "Generic", "DiGiCo", "Allen & Heath", "SSL", "Yamaha" };
        juce::String out;
        out << "ZynForge Recording console log\n";
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            out << "Version: " << app->getApplicationVersion() << "\n";
        out << "Saved: " << juce::Time::getCurrentTime().toString (true, true, true, true) << "\n"
            << "Computer: " << juce::SystemStats::getComputerName() << "\n"
            << consoleLinkSummary << "\n"
            << "OSC listener: "
            << (isOscListening() ? "port " + juce::String (getOscPort()) + ", dialect " + dialects[getOscDialect()]
                                 : juce::String ("off")) << "\n"
            << "Desk macro listener: "
            << (isDeskMacroListening() ? "port " + juce::String (getDeskMacroPort())
                                         + ", desk " + getDeskMacroSource()
                                         + ", " + juce::String (getDeskMacroDropped())
                                         + " dropped from other sources"
                                       : juce::String ("off")) << "\n";
        const auto evicted = getOscLogEvicted();
        if (evicted > 0)
            out << "Oldest " << evicted << " lines were discarded by the "
                << kOscLogMaxLines << "-line cap\n";
        out << "\n" << getOscLog().joinIntoString ("\n") << "\n";
        return out;
    }

    void RemoteControl::setConsoleNameCapture (bool on)
    {
        if (on)
        {
            capturedConsoleNames.clear();
            capturedStereoLinks.clear();
        }
        consoleCapture.store (on, std::memory_order_relaxed);
    }

    void RemoteControl::onConsoleChannelName (int oneBasedIndex, const juce::String& name)
    {
        if (oneBasedIndex >= 1 && consoleCapture.load (std::memory_order_relaxed))
            capturedConsoleNames[oneBasedIndex] = name;
        const int i = oneBasedIndex - 1;
        if (i >= 0 && i < engine.getRecorder().getNumTracks())
            engine.setTrackName (i, name);
    }

    std::map<int, juce::String> RemoteControl::takeCapturedConsoleNames()
    {
        auto out = capturedConsoleNames;
        capturedConsoleNames.clear();
        return out;
    }

    void RemoteControl::onConsoleStereoLink (int leftCh1, bool linked)
    {
        if (leftCh1 >= 1 && consoleCapture.load (std::memory_order_relaxed))
            capturedStereoLinks[leftCh1] = linked;
    }

    std::map<int, bool> RemoteControl::takeCapturedStereoLinks()
    {
        auto out = capturedStereoLinks;
        capturedStereoLinks.clear();
        return out;
    }

    bool RemoteControl::isSessionBusy() const
    {
        return engine.isSessionTransitionActive() || (host.isSessionBusy && host.isSessionBusy());
    }

    scenefollow::Facts RemoteControl::sceneFacts() const
    {
        scenefollow::Facts f;
        f.capturing    = engine.isRecording();
        f.followOn     = engine.isSceneFollowEnabled();
        f.locked       = engine.areControlsLocked();
        f.seekLocked   = engine.getPlayer().isSeekLocked();
        f.sessionBusy  = isSessionBusy();
        f.playerLoaded = engine.getPlayer().isLoaded();
        return f;
    }

    juce::String RemoteControl::deskLocateRefusal() const
    {
        if (engine.isRecording()) return "capture is running";
        return scenefollow::locateRefusal (sceneFacts());
    }

    void RemoteControl::locate (juce::int64 samplePosition)
    {
        engine.getPlayer().setPositionSamples (samplePosition);
        // PLAY starts from a placed cursor, so one left behind would send the
        // next PLAY back to where the engineer last clicked, not to this song.
        engine.clearEditCursor();
        if (host.revealPosition) host.revealPosition (samplePosition);
    }

    scenefollow::Action RemoteControl::onConsoleScene (int sceneNumber, const juce::String& sceneName,
                                                       scenefollow::Path path)
    {
        using scenefollow::Action;
        const auto label = scenefollow::sceneLabel (sceneNumber, sceneName);
        if (recallEchoes.isEcho (path, label, juce::Time::getMillisecondCounter()))
            return Action::Nothing;
        const auto& markers = engine.getMarkers().getAll();
        const auto decision = scenefollow::decide (sceneFacts(), markers, label,
                                                   engine.getPlayer().getPositionSamples());
        const auto prefix = "Console scene \"" + label + "\" -- ";
        const auto tell = [&] (const juce::String& outcome)
        {
            logOsc ("scene: " + prefix + outcome);
            showHostStatus (prefix + outcome);
        };
        switch (decision.action)
        {
            case Action::DropMarker:
                if (engine.dropMarkerAtCurrentPosition (label, "scene") >= 0)
                    tell ("marker dropped");
                break;

            case Action::Locate:
            {
                const auto position = markers[(size_t) decision.markerIndex].sampleOffset;
                const auto rate = juce::jmax (1.0, engine.getPlayer().getSampleRate());
                const auto seconds = (juce::int64) ((double) position / rate);
                const auto markerNumber = decision.markerIndex + 1;
                locate (position);
                tell ("followed to marker " + juce::String (markerNumber)
                      + " at " + juce::String (seconds / 60).paddedLeft ('0', 2) + ":"
                      + juce::String (seconds % 60).paddedLeft ('0', 2));
                break;
            }

            case Action::NoMatch:
                tell ("Follow found no marker with that name");
                break;

            case Action::Refused:
                tell ("Follow refused: " + decision.refusal);
                break;

            // Fired constantly while rehearsing, and nothing happened, so the
            // status line and its toast stay quiet. The log still shows it.
            case Action::Nothing:
                logOsc ("scene: " + prefix + "no take rolling and Follow is off");
                break;
        }
        return decision.action;
    }

    bool RemoteControl::startDeskMacro (int udpPort, const juce::String& deskIp)
    {
        if (engine.areControlsLocked()) return false;
        if (deskMacro == nullptr) deskMacro = std::make_unique<DeskMacroListener> (engine);
        return deskMacro->start (udpPort, deskIp);
    }

    void RemoteControl::stopDeskMacro()
    {
        if (deskMacro != nullptr) deskMacro->stop();
    }
    bool RemoteControl::isDeskMacroListening() const
    {
        return deskMacro != nullptr && deskMacro->isListening();
    }
    int RemoteControl::getDeskMacroPort() const
    {
        return deskMacro != nullptr ? deskMacro->getPort() : 0;
    }
    juce::String RemoteControl::getDeskMacroSource() const
    {
        return deskMacro != nullptr ? deskMacro->getSourceIp() : juce::String();
    }
    juce::int64 RemoteControl::getDeskMacroDropped() const
    {
        return deskMacro != nullptr ? deskMacro->getDroppedSourceCount() : 0;
    }
    juce::String RemoteControl::getDeskMacroLastDropped() const
    {
        return deskMacro != nullptr ? deskMacro->getLastDroppedSource() : juce::String();
    }

    bool RemoteControl::enableControlSurface (const juce::String& key,
                                              const juce::String& inName, const juce::String& outName)
    {
        disableControlSurface();
        surface = std::make_unique<MidiControlSurface> (engine);
        const bool ok = surface->start (inName, outName);
        if (ok) activeSurfaceKey = key;
        else    { surface.reset(); activeSurfaceKey.clear(); }
        return ok;
    }

    void RemoteControl::disableControlSurface()
    {
        if (surface != nullptr) { surface->stop(); surface.reset(); }
        activeSurfaceKey.clear();
    }

    void RemoteControl::feedCompanionStream (const float* L, const float* R, int numSamples) noexcept
    {
        companion->feedStreamSamples (L, R, numSamples);
    }
}
