#include "CaptureSupervisor.h"

#include <juce_events/juce_events.h>

namespace zynforge::capture
{
    CaptureSupervisor::~CaptureSupervisor()
    {
        alive->store (false);
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
        configuredCapture.clear();
        failedCapture.clear(); retryCaptureAtMs = 0;
        lastConnectError.clear();
        port = wantPort;

        client.onStatus = [this] (const EngineStatus& s)
        {
            { const std::lock_guard<std::mutex> g (statusLock);
              status = s; statusAtMs = juce::Time::currentTimeMillis(); }
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
            lastConnectError = "Something else answers on port " + juce::String (port)
                             + " and it is not a capture daemon this build can talk to. "
                               "If a daemon take is running there, it is not being watched.";
            return false;                // fail LOUD rather than spawn a twin
        }

        // Nothing there -- launch the binary.
        if (! binaryPath.existsAsFile()) binaryPath = discoverBinary();
        if (! binaryPath.existsAsFile())
        {
            lastConnectError = "The capture daemon program was not found.";
            return false;
        }

        juce::StringArray cmd { binaryPath.getFullPathName(),
                                "--port", juce::String (port) };
        cmd.addArray (extraArgs);
        if (! process.start (cmd))
        {
            lastConnectError = "The capture daemon program could not be started.";
            return false;
        }
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
        lastConnectError = "The capture daemon started but never answered; port "
                         + juce::String (port) + " may be busy.";
        return false;
    }

    CaptureSupervisor::Reattach CaptureSupervisor::reattach()
    {
        if (isAttached()) return Reattach::attached;
        if (port <= 0) return Reattach::noAnswer;   // no daemon was ever here to look for
        { const std::lock_guard<std::mutex> g (statusLock); statusAtMs = 0; }
        statusSeen.store (false);
        if (! client.connect ("127.0.0.1", port)) return Reattach::nothingThere;
        if (! client.hello (1500).ok)
        {
            client.disconnect();
            return Reattach::noAnswer;
        }
        // A restarted daemon knows none of this GUI's configuration.
        configuredCapture.clear();
        failedCapture.clear(); retryCaptureAtMs = 0;
        everAttached  = true;
        deathReported = false;
        return Reattach::attached;
    }

    void CaptureSupervisor::forgetRollingTake()
    {
        const std::lock_guard<std::mutex> g (statusLock);
        status.recording = false;
    }

    void CaptureSupervisor::disconnect()
    {
        client.disconnect();
        statusSeen.store (false);
        { const std::lock_guard<std::mutex> g (statusLock); statusAtMs = 0; }
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
        // The link going down is what wakes a STOP that is waiting for its reply.
        if (stopInFlight.load()) client.disconnect();
        joinStop();

        // A ROLLING take must survive the GUI: NEVER kill a daemon unless we
        // POSITIVELY know it is idle. Before the first status push arrives we
        // do NOT know its state (lastStatus().recording defaults to false), so
        // hasStatus() must gate any assumption of idleness -- otherwise a fast
        // Record-then-quit force-kills a take mid-roll.
        if (launched)
        {

            // Ask the daemon to quit. It accepts only when idle (ack ok) and
            // refuses mid-take (ok == false), so a rolling take survives even
            // if our local status was stale.
            const bool quitAccepted = requestQuit();

            if (quitAccepted)
                for (int i = 0; i < 20 && process.isRunning(); ++i)
                    juce::Thread::sleep (50);

            client.disconnect();

            // Kill fallback fires ONLY when we know the daemon is idle -- either
            // a fresh status said so, or the daemon acked the quit. It can never
            // fire on a default/unknown status.
            if (quitAccepted && process.isRunning())
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

    juce::int64 CaptureSupervisor::lastStatusAtMs() const
    {
        const std::lock_guard<std::mutex> g (statusLock);
        return statusAtMs;
    }

    bool CaptureSupervisor::startRecording (const juce::File& sessionDir, int trackCount,
                                            int captureFormat, const juce::BigInteger& armedTracks,
                                            const juce::var& timeHint,
                                            juce::String* refusal, bool* unanswered)
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
        rec.configuration = timeHint;
        // Await the daemon's reply (bounded): true ONLY when the daemon
        // confirms the recorder actually started. A socket-write success is
        // NOT proof the take is rolling, so we no longer light Record on it.
        const auto reply = client.request (rec, startReplyTimeoutMs);
        if (! reply.ok)
        {
            if (refusal != nullptr) *refusal = reply.error;
            if (unanswered != nullptr) *unanswered = reply.error.isEmpty();
        }
        return reply.ok;
    }

    bool CaptureSupervisor::configureCapture (const juce::var& config)
    {
        const auto text = juce::JSON::toString (config);
        if (text == configuredCapture) return true;
        if (text == failedCapture && juce::Time::currentTimeMillis() < retryCaptureAtMs) return false;
        if (! isAttached() || isDaemonRecording()) return false;
        Command c; c.action = Action::ConfigureCapture; c.configuration = config;
        if (! client.request (c, 5000).ok)
        {
            // The idle UI polls configuration for pre-roll. A missing device
            // must not cause a new blocking request on every UI timer tick.
            failedCapture = text; retryCaptureAtMs = juce::Time::currentTimeMillis() + 5000;
            return false;
        }
        failedCapture.clear();
        configuredCapture = text;
        return true;
    }

    bool CaptureSupervisor::stopRecording (bool* didStop, juce::String* error)
    {
        if (didStop != nullptr) *didStop = false;
        if (error != nullptr) error->clear();
        if (! isAttached())
        {
            if (error != nullptr) *error = "capture daemon is not attached";
            return false;
        }
        Command stop; stop.action = Action::StopRecording;
        // Await the daemon's ack. It closes every file on every copy first, and
        // on a slow drive that is not a matter of seconds. A dropped link wakes
        // this at once; the bound is only for a daemon that went silent.
        const auto reply = client.request (stop, 10 * 60 * 1000);
        if (didStop != nullptr) *didStop = reply.completed;
        if (error != nullptr) *error = reply.error;
        return reply.ok;
    }

    void CaptureSupervisor::joinStop()
    {
        if (stopThread.joinable()) stopThread.join();
    }

    void CaptureSupervisor::stopRecordingAsync (std::function<void (StopResult)> done)
    {
        joinStop();
        stopInFlight.store (true);
        stopThread = std::thread ([this, done = std::move (done), token = alive]
        {
            StopResult result;
            result.clean = stopRecording (&result.stopped, &result.error);
            juce::MessageManager::callAsync ([this, done, token, result]
            {
                if (! token->load()) return;
                joinStop();
                stopInFlight.store (false);
                done (result);
            });
        });
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
