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
                everAttached  = true;
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
                if (client.hello (1500).ok)
                {
                    // Twin-launch race: if our spawned process already exited it
                    // lost the port bind and we actually attached to a
                    // pre-existing daemon -- don't claim ownership (we must not
                    // later kill a daemon we didn't spawn).
                    if (! process.isRunning()) launched = false;
                    everAttached  = true;
                    deathReported = false;
                    return true;
                }
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
        everAttached = false;   // an intentional detach must not read as a death
    }

    bool CaptureSupervisor::requestQuit()
    {
        if (! isAttached()) return false;
        Command q; q.action = Action::Quit;
        // Await the daemon's ack: ok == true means it accepted (was idle) and
        // is exiting; ok == false means it REFUSED (mid-take) -- so the caller
        // must never force-kill on a false.
        return client.request (q, 1500).ok;
    }

    void CaptureSupervisor::shutdown()
    {
        // A ROLLING take must survive the GUI: NEVER kill a daemon unless we
        // POSITIVELY know it is idle. Before the first status push arrives we
        // do NOT know its state (lastStatus().recording defaults to false), so
        // hasStatus() must gate any assumption of idleness -- otherwise a fast
        // Record-then-quit force-kills a take mid-roll.
        if (launched)
        {
            const bool positivelyIdle = hasStatus() && ! isDaemonRecording();

            // Ask the daemon to quit. It accepts only when idle (ack ok) and
            // refuses mid-take (ok == false), so a rolling take survives even
            // if our local status was stale.
            const bool quitAccepted = requestQuit();

            if (positivelyIdle || quitAccepted)
                for (int i = 0; i < 20 && process.isRunning(); ++i)
                    juce::Thread::sleep (50);

            client.disconnect();

            // Kill fallback fires ONLY when we know the daemon is idle -- either
            // a fresh status said so, or the daemon acked the quit. It can never
            // fire on a default/unknown status.
            if ((positivelyIdle || quitAccepted) && process.isRunning())
                process.kill();
        }
        else
        {
            client.disconnect();
        }
        launched     = false;
        everAttached = false;
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
        // Await the daemon's reply (bounded): true ONLY when the daemon
        // confirms the recorder actually started. A socket-write success is
        // NOT proof the take is rolling, so we no longer light Record on it.
        const auto reply = client.request (rec, 5000);
        return reply.ok;
    }

    bool CaptureSupervisor::stopRecording()
    {
        if (! isAttached()) return false;
        Command stop; stop.action = Action::StopRecording;
        // Await the daemon's ack (bounded). Closing files + manifest can take a
        // moment, so allow a generous timeout.
        const auto reply = client.request (stop, 10000);
        return reply.ok;
    }

    void CaptureSupervisor::tick()
    {
        if (deathReported) return;

        // Dead = we launched it and the process is gone, OR the link we had
        // dropped for a reason OTHER than an intentional supersede. Gating on
        // everAttached (not statusSeen) means the death of an ATTACHED daemon
        // that dropped BEFORE its first status push is still detected; gating
        // out wasSuperseded() means a newer connection kicking our socket does
        // NOT read as a daemon death.
        const bool processDead = launched && ! process.isRunning();
        const bool linkDropped = everAttached && ! client.isConnected()
                                    && ! client.wasSuperseded();
        if (! processDead && ! linkDropped) return;

        deathReported = true;
        const bool wasRecording = lastStatus().recording;
        if (onDaemonDied) onDaemonDied (wasRecording);
    }
}
