#include "CaptureSupervisor.h"
#include "HelperExit.h"

#include <juce_events/juce_events.h>

#include <array>
#include <fcntl.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace zynforge::capture
{
    bool HelperProcess::start (const juce::StringArray& arguments)
    {
        if (arguments.isEmpty()) return false;
        std::vector<std::string> storage;
        for (const auto& argument : arguments)
            if (argument.isNotEmpty()) storage.push_back (argument.toStdString());
        std::vector<char*> argv;
        for (auto& argument : storage) argv.push_back (argument.data());
        argv.push_back (nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        if (posix_spawn_file_actions_init (&actions) != 0) return false;
        if (posix_spawnattr_init (&attributes) != 0)
        {
            posix_spawn_file_actions_destroy (&actions);
            return false;
        }
        // Every descriptor not named here is closed in the helper.
        bool ready = posix_spawnattr_setflags (&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT) == 0;
        ready = ready && posix_spawn_file_actions_addopen (&actions, STDIN_FILENO,  "/dev/null", O_RDONLY, 0) == 0;
        ready = ready && posix_spawn_file_actions_addopen (&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0;
        ready = ready && posix_spawn_file_actions_addopen (&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
        pid_t child = 0;
        const bool spawned = ready && posix_spawn (&child, argv[0], &actions, &attributes, argv.data(), environ) == 0;
        posix_spawnattr_destroy (&attributes);
        posix_spawn_file_actions_destroy (&actions);
        if (! spawned) return false;
        pid = child;
        lastExitCode = -1;
        return true;
    }

    bool HelperProcess::isRunning()
    {
        if (pid <= 0) return false;
        int state = 0;
        const auto reaped = waitpid (pid, &state, WNOHANG);
        if (reaped == 0) return true;
        if (reaped == pid)
            lastExitCode = WIFEXITED (state) ? WEXITSTATUS (state)
                         : WIFSIGNALED (state) ? 128 + WTERMSIG (state) : -1;
        pid = 0;   // exited, or no longer this process's child to ask about
        return false;
    }

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

    // juce::ChildProcess::kill is SIGKILL, which can leave an audio driver wedged
    // with the device still open. The helper closes its device on SIGTERM.
    int CaptureSupervisor::askChildrenToExit (const juce::String& programName)
    {
        std::array<pid_t, 256> children {};
        const int count = proc_listchildpids (::getpid(), children.data(), (int) sizeof (children));
        int asked = 0;
        for (int i = 0; i < count && i < (int) children.size(); ++i)
        {
            char name[64] {};
            if (proc_name (children[(size_t) i], name, sizeof (name)) > 0 && programName == name
                && ::kill (children[(size_t) i], SIGTERM) == 0)
                ++asked;
        }
        return asked;
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
        bool answeredWrongly = false;
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
                answeredWrongly = true;
                break;
            }
            if (! process.isRunning()) break;   // crashed on startup
        }
        // Say what happened. Only the helper's own exit code blames the port:
        // one that quit for another reason, or never answered, says so.
        const bool exited = ! process.isRunning();
        const int exitCode = process.exitCode();
        askChildrenToExit (binaryPath.getFileName());
        launched = false;
        const auto portText = juce::String (port);
        lastConnectError = exited && exitCode == helperexit::kExitCannotListen
                             ? "The capture daemon could not listen on port " + portText
                               + "; another program may hold it."
                         : exited
                             ? "The capture daemon quit while starting (exit code " + juce::String (exitCode)
                               + "), before it answered."
                         : answeredWrongly
                             ? "Something answered on port " + portText
                               + ", but not as a capture daemon this build can talk to."
                             : "The capture daemon started but did not answer on port " + portText
                               + " within 2 seconds.";
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

            // Fires ONLY when we know the daemon is idle -- either a fresh status
            // said so, or the daemon acked the quit. It can never fire on a
            // default/unknown status.
            if (quitAccepted && process.isRunning())
                askChildrenToExit (binaryPath.getFileName());
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
                                            juce::String* refusal, bool* unanswered,
                                            const juce::String& expectedDevice)
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
        if (expectedDevice.isNotEmpty())
        {
            if (! rec.configuration.isObject()) rec.configuration = juce::var (new juce::DynamicObject());
            rec.configuration.getDynamicObject()->setProperty ("expectedDevice", expectedDevice);
        }
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

    // Only a fresh status can say so: a daemon busy elsewhere says nothing, and
    // asking it again would only hold the window.
    bool CaptureSupervisor::heldDeviceDiffers (const juce::String& wanted) const
    {
        const std::lock_guard<std::mutex> g (statusLock);
        return statusSeen.load() && juce::Time::currentTimeMillis() - statusAtMs <= 2000
            && status.captureDevice != wanted;
    }

    juce::String CaptureSupervisor::deviceMismatch (const juce::String& ownDevice) const
    {
        const auto held = lastStatus().captureDevice;
        if (! hasStatus() || juce::Time::currentTimeMillis() - lastStatusAtMs() > 2000)
            return "The capture daemon has not said which audio device it holds. Press RECORD again, "
                   "or turn the capture daemon off to record in-process.";
        if (held == ownDevice && held.isNotEmpty()) return {};
        if (held.isEmpty())
            return "The capture daemon holds no audio device, and has been sent " + ownDevice
                 + ". Press RECORD again, or turn the capture daemon off to record in-process.";
        return "The capture daemon holds " + held + ", not " + ownDevice
             + ". It has been told to change. Press RECORD again.";
    }

    bool CaptureSupervisor::configureCapture (const juce::var& config)
    {
        const auto text = juce::JSON::toString (config);
        const auto wanted = config.getProperty ("captureDevice", {});
        // Two processes each hold a device, and the daemon's can fall back or die
        // by itself, leaving this text exactly what it last accepted.
        const bool moved = text == configuredCapture && ! wanted.isVoid()
                        && heldDeviceDiffers (wanted.toString());
        if (text == configuredCapture && ! moved) return true;
        const auto now = juce::Time::currentTimeMillis();
        if ((text == failedCapture || moved) && now < retryCaptureAtMs) return false;
        if (! isAttached() || isDaemonRecording()) return false;
        Command c; c.action = Action::ConfigureCapture; c.configuration = config;
        if (! client.request (c, 5000).ok)
        {
            // The idle UI polls configuration for pre-roll. A missing device
            // must not cause a new blocking request on every UI timer tick.
            failedCapture = text; retryCaptureAtMs = juce::Time::currentTimeMillis() + 5000;
            // A device the daemon refused can leave it holding none, so what it
            // accepted before says nothing about it now.
            configuredCapture.clear();
            return false;
        }
        failedCapture.clear();
        configuredCapture = text;
        // Its next status still shows the old device.
        retryCaptureAtMs = juce::Time::currentTimeMillis() + 5000;
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
