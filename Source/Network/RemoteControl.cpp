// See RemoteControl.h. These bodies moved here from AudioEngine.cpp; only
// their calls back into the engine now go through `engine`.

#include "RemoteControl.h"

#include "CompanionServer.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/OscRemote.h"

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

    bool RemoteControl::requestConsoleChannelNames (const juce::String& host, int port, int dialect)
    {
        if (host.trim().isEmpty() || port <= 0 || port > 65535) return false;
        juce::OSCSender sender;
        if (! sender.connect (host.trim(), port)) return false;

        // Best-effort "report your channels" nudge per console dialect -- their
        // OSC implementations differ, so we fire a couple of plausible refresh /
        // subscribe forms. The channel-name replies come back on the normal
        // receive path (each dialect parser already handles channel/N/name).
        switch (dialect)
        {
            case 2:   // Allen & Heath
                sender.send (juce::OSCMessage ("/sq/channels/?"));
                sender.send (juce::OSCMessage ("/sq/refresh"));
                break;
            case 3:   // SSL Live
                sender.send (juce::OSCMessage ("/sslnet/channel/?"));
                sender.send (juce::OSCMessage ("/sslnet/refresh"));
                break;
            case 4:   // Yamaha / RIVAGE
                sender.send (juce::OSCMessage ("/Yamaha/Channels/?"));
                sender.send (juce::OSCMessage ("/RIVAGE/Channels/?"));
                break;
            default:  // DiGiCo (1) / generic
                sender.send (juce::OSCMessage ("/Console/Channels/?"));
                sender.send (juce::OSCMessage ("/Console/Channels", (juce::int32) 1));
                sender.send (juce::OSCMessage ("/info"));
                break;
        }
        sender.disconnect();
        if (isOscDebug())
            logOsc ("-> " + host.trim() + ":" + juce::String (port)
                    + "  request channel names (dialect " + juce::String (dialect) + ")");
        return true;
    }

    void RemoteControl::logOsc (const juce::String& line)
    {
        const juce::ScopedLock sl (oscLogLock);
        oscLog.add (juce::Time::getCurrentTime().formatted ("%H:%M:%S  ") + line);
        while (oscLog.size() > 200) oscLog.remove (0);
    }
    juce::StringArray RemoteControl::getOscLog() const
    {
        const juce::ScopedLock sl (oscLogLock);
        return oscLog;
    }
    void RemoteControl::clearOscLog()
    {
        const juce::ScopedLock sl (oscLogLock);
        oscLog.clear();
    }

    void RemoteControl::setConsoleNameCapture (bool on)
    {
        if (on) capturedConsoleNames.clear();
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
