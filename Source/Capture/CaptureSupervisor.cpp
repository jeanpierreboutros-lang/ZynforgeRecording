#include "CaptureSupervisor.h"

namespace zynforge::capture
{
    CaptureSupervisor::~CaptureSupervisor()
    {
        shutdown();
    }

    juce::File CaptureSupervisor::discoverBinary()
    {
        const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        // App bundle layout: <build>/ZynforgeRecording_artefacts/<cfg>/App.app/
        // Contents/MacOS/exe -> the daemon sits in the sibling artefacts dir.
        for (auto dir = exe.getParentDirectory(); dir != juce::File();
             dir = dir.getParentDirectory())
        {
            const auto cfg = dir.getFileName();   // e.g. "Release" when inside artefacts
            const auto candidate = dir.getParentDirectory().getParentDirectory()
                                       .getChildFile ("ZynforgeCapture_artefacts")
                                       .getChildFile (cfg)
                                       .getChildFile ("ZynforgeCapture");
            if (candidate.existsAsFile()) return candidate;
            // Installed layout: daemon shipped inside the app bundle.
            const auto bundled = dir.getChildFile ("ZynforgeCapture");
            if (bundled.existsAsFile()) return bundled;
            if (dir.getParentDirectory() == dir) break;
        }
        return {};
    }

    bool CaptureSupervisor::connectOrLaunch (int wantPort, const juce::StringArray& extraArgs)
    {
        disconnect();
        port = wantPort;

        client.onStatus = [this] (const EngineStatus& s)
        {
            { const std::lock_guard<std::mutex> g (statusLock); status = s; }
            statusSeen.store (true);
        };

        // Phase 2: ATTACH first -- a daemon left rolling by a dead/quit GUI
        // answers here and the take is re-adopted, not duplicated.
        if (client.connect ("127.0.0.1", port))
        {
            if (client.hello (1500).ok)
            {
                launched      = false;   // pre-existing daemon, not ours to kill
                deathReported = false;
                return true;
            }
            client.disconnect();         // wrong version / not our protocol
            return false;                // fail LOUD rather than spawn a twin
        }

        // Nothing there -- launch the binary.
        if (! binaryPath.existsAsFile()) binaryPath = discoverBinary();
        if (! binaryPath.existsAsFile()) return false;

        juce::StringArray cmd { binaryPath.getFullPathName(),
                                "--port", juce::String (port) };
        cmd.addArray (extraArgs);
        if (! process.start (cmd)) return false;
        launched = true;

        // The daemon needs a beat to bind; retry the connect briefly.
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            juce::Thread::sleep (100);
            if (client.connect ("127.0.0.1", port))
            {
                if (client.hello (1500).ok) { deathReported = false; return true; }
                client.disconnect();
                break;
            }
            if (! process.isRunning()) break;   // crashed on startup
        }
        process.kill();
        launched = false;
        return false;
    }

    void CaptureSupervisor::disconnect()
    {
        client.disconnect();
        statusSeen.store (false);
    }

    bool CaptureSupervisor::requestQuit()
    {
        if (! isAttached()) return false;
        Command q; q.action = Action::Quit;
        return client.send (q);
    }

    void CaptureSupervisor::shutdown()
    {
        // A ROLLING take must survive the GUI: never stop a recording
        // daemon. An idle daemon we spawned is asked to exit cleanly over
        // the wire (Quit is refused mid-take by the daemon itself, so this
        // is double-safe); kill() is only the unresponsive-process fallback.
        const bool recording = isDaemonRecording();
        if (launched && ! recording)
        {
            requestQuit();
            for (int i = 0; i < 20 && process.isRunning(); ++i)
                juce::Thread::sleep (50);
        }
        client.disconnect();
        if (launched && ! recording && process.isRunning())
            process.kill();
        launched = false;
    }

    EngineStatus CaptureSupervisor::lastStatus() const
    {
        const std::lock_guard<std::mutex> g (statusLock);
        return status;
    }

    bool CaptureSupervisor::startRecording (const juce::File& sessionDir, int trackCount,
                                            int captureFormat, const juce::BigInteger& armedTracks)
    {
        if (! isAttached()) return false;

        Command tc;  tc.action  = Action::SetTrackCount;    tc.intValue  = trackCount;
        Command fmt; fmt.action = Action::SetCaptureFormat; fmt.intValue = captureFormat;
        if (! client.send (tc) || ! client.send (fmt)) return false;
        for (int i = 0; i < trackCount; ++i)
        {
            Command arm; arm.action = Action::ArmTrack;
            arm.trackIndex = i; arm.boolValue = armedTracks[i];
            if (! client.send (arm)) return false;
        }
        Command rec; rec.action = Action::StartRecording;
        rec.sessionDir = sessionDir.getFullPathName();
        return client.send (rec);
    }

    bool CaptureSupervisor::stopRecording()
    {
        if (! isAttached()) return false;
        Command stop; stop.action = Action::StopRecording;
        return client.send (stop);
    }

    void CaptureSupervisor::tick()
    {
        if (deathReported) return;

        // Dead = we launched it and the process is gone, OR the link we had
        // dropped (daemon-side close / crash of an attached daemon).
        const bool processDead = launched && ! process.isRunning();
        const bool linkDropped = ! client.isConnected() && statusSeen.load();
        if (! processDead && ! linkDropped) return;

        deathReported = true;
        const bool wasRecording = lastStatus().recording;
        if (onDaemonDied) onDaemonDied (wasRecording);
    }
}
