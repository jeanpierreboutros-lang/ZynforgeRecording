#pragma once

#include <juce_core/juce_core.h>

#include "../Network/CaptureLink.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <sys/types.h>

namespace zynforge { class Fix20261124HelperTests; }

namespace zynforge::capture
{
    // GUI-side supervisor for the zynforge-capture daemon (Phases 1d + 2 of
    // the capture-process split).
    //
    // Lifecycle (Phase 2 semantics baked in):
    //  * connectOrLaunch() tries to ATTACH to an already-running daemon
    //    first -- a restarted GUI re-adopts a rolling take instead of
    //    spawning a second daemon. Only when nothing answers does it launch
    //    the binary (HelperProcess, below) and connect.
    //  * tick() is the GUI-side watchdog (call from the UI timer): it
    //    detects a dead daemon / dropped link and reports via onDaemonDied
    //    -- LOUDLY if it died mid-take. It never auto-relaunches mid-take
    //    (the take is already lost; the engineer must see that).
    //  * shutdown() leaves a RECORDING daemon rolling (the whole point: the
    //    GUI quitting must not stop the take); an idle daemon we launched
    //    is asked to exit, by Quit and then SIGTERM. It is never killed. An
    //    idle one we attached to is asked by Quit alone (it is not our child).
    //  * An idle daemon of another build (HelperBuild.h) found at attach is
    //    asked to quit and this build's helper launched in its place; one with
    //    a take rolling, or one that can't be shown to be idle (closing a take
    //    it sends no status; or it did not answer Quit), is kept and named,
    //    no take starts on it, and tick() replaces it as soon as it is idle.
    //  * A launch first waits (bounded) for a helper that has closed the port
    //    to let go of the port's key, launches again when the one it started
    //    could not listen, and keeps as its own only the helper that holds the
    //    port: one that lost the port to another is reaped, never reported dead.
    //
    // The daemon side needs no watchdog: a vanished GUI just drops the
    // socket (SIGPIPE ignored) and recording continues.
    //
    // Message-thread only, like the rest of the engine surface; status
    // pushes arrive on the client reader thread and are double-buffered
    // under statusLock.

    // The helper process as launched, with nothing of the window's but what it
    // is given. juce::ChildProcess forks and execs with every descriptor the
    // window holds still open, so a helper it launched kept the window's twin
    // and OSC listeners (and open audio files) for as long as it lived: the
    // window could not listen on those ports again. This spawns with
    // POSIX_SPAWN_CLOEXEC_DEFAULT, so the helper gets /dev/null for stdin,
    // stdout and stderr and nothing else. Destroying it never stops the helper.
    class HelperProcess
    {
    public:
        bool start (const juce::StringArray& arguments);
        bool isRunning();
        // The exit status once it has exited (a signal reads as 128 + signal), else -1.
        int  exitCode() const noexcept { return lastExitCode; }
        // 0 once it has exited and been reaped (isRunning), or before start.
        pid_t processId() const noexcept { return pid; }
        // SIGTERM: the helper closes its device and exits.
        void askToExit();

    private:
        pid_t pid { 0 };
        int   lastExitCode { -1 };
    };

    class CaptureSupervisor
    {
    public:
        CaptureSupervisor() = default;
        ~CaptureSupervisor();

        // Where the daemon binary lives. Tests + the app set this
        // explicitly (the app resolves it next to its own bundle).
        void setBinaryPath (const juce::File& f) { binaryPath = f; }
        // Best-effort discovery relative to the running executable
        // (build tree: ../../../../ZynforgeCapture_artefacts/<config>/).
        static juce::File discoverBinary();
        // SIGTERM to this process's children of that name. Returns how many.
        static int askChildrenToExit (const juce::String& programName);

        // Attach to a live daemon on `port`, else launch + connect.
        // extraArgs are appended to the launch command (tests pass
        // "--no-device"). False if neither attach nor launch worked.
        bool connectOrLaunch (int port, const juce::StringArray& extraArgs = {});
        // A lost link, tried again. Attach only: this never launches, so it
        // cannot start a second daemon beside one that stopped answering.
        enum class Reattach
        {
            attached,       // the daemon answered and proved itself
            nothingThere,   // nothing is listening: no daemon is running
            noAnswer        // something holds the port but did not prove itself
        };
        Reattach reattach();
        // The last status called a take rolling and that is no longer known
        // to be so. For a caller that has proved it, or is walking away from it.
        void forgetRollingTake();
        void disconnect();           // drop the link, leave the daemon running
        void shutdown();             // drop link; stop the daemon ONLY if idle + we launched it

        bool isAttached()   const noexcept { return client.isConnected(); }
        bool didLaunch()    const noexcept { return launched; }
        int  getPort()      const noexcept { return port; }

        // Latest status push (thread-safe copy) + whether one ever arrived.
        EngineStatus lastStatus() const;
        juce::int64  lastStatusAtMs() const;
        bool         hasStatus()  const noexcept { return statusSeen.load(); }
        bool         isDaemonRecording() const   { return lastStatus().recording; }

        // Recording control over the wire. startRecording syncs the config
        // first (track count, format, per-track arms), then starts. timeHint:
        // a live timecode for the BWF stamp (bwf::TimeAnchor::toJson), or void.
        // False with `refusal` set: the daemon said no. False with `unanswered`
        // set: no reply came in time, so the take may be rolling all the same.
        // expectedDevice: the device the daemon must hold, checked again at its end.
        bool startRecording (const juce::File& sessionDir, int trackCount,
                             int captureFormat, const juce::BigInteger& armedTracks,
                             const juce::var& timeHint = {},
                             juce::String* refusal = nullptr, bool* unanswered = nullptr,
                             const juce::String& expectedDevice = {});
        bool stopRecording (bool* didStop = nullptr, juce::String* error = nullptr);
        // STOP without holding the caller: closing every file on every copy can
        // take far longer than a window can wait. `done` runs on the message
        // thread. clean: finished with no failure. stopped: the take ended,
        // clean or not. Neither: no word from the daemon.
        struct StopResult { bool clean { false }, stopped { false }; juce::String error; };
        void stopRecordingAsync (std::function<void (StopResult)> done);
        bool isStopInFlight() const noexcept { return stopInFlight.load(); }
        int  startReplyTimeoutMs { 5000 };
        juce::String lastConnectError;   // why connectOrLaunch last failed
        // False with `refusal` set: the daemon refused this configuration (now,
        // or the last time it was sent, while the retry waits). False with it
        // empty: nothing was asked, or no answer came.
        bool configureCapture (const juce::var&, juce::String* refusal = nullptr);
        // The build the attached daemon named in its hello; empty for one built
        // before builds were named. And the one this window ships.
        juce::String helperBuild() const { return attachedBuild; }
        juce::String expectedHelperBuild();
        bool helperIsOtherBuild() { return isAttached() && attachedBuild != expectedHelperBuild(); }
        // The last connectOrLaunch asked an idle daemon of another build to
        // quit and launched this build's in its place (or tick() did since).
        bool didReplaceOtherBuild() const noexcept { return replacedOtherBuild; }
        // Attached to a daemon of another build that was kept: it was rolling a
        // take, or it could not be shown to be idle at attach (one closing a
        // take sends no status until it is done; one may not answer Quit). It
        // used to be kept with nothing said, and driven. Now no take starts on
        // it: configureCapture sends it nothing, and deviceMismatch and
        // startRecording refuse with otherBuildNotice(). tick() replaces it as
        // soon as a fresh status says it is idle, its take's claim is free and
        // mayReplaceOtherBuild agrees.
        bool keepsOtherBuild() const noexcept { return otherBuildKept && client.isConnected(); }
        juce::String otherBuildNotice() const;
        // The window's say before tick() swaps helpers under it: false while it
        // has a take in hand (rolling, closing, its metadata still saving). Unset
        // means yes. Message thread.
        std::function<bool()> mayReplaceOtherBuild;
        // Message thread, from tick(): the kept daemon was replaced by this
        // build's (true), or it quit and this build's could not be started
        // (false; lastConnectError says why, and none is attached).
        std::function<void (bool launched)> onOtherBuildReplaced;
        // Why this daemon must not take a RECORD for a window whose input device
        // is ownDevice, from its latest status: a kept daemon of another build
        // first (keepsOtherBuild), then the device. Empty when it may.
        juce::String deviceMismatch (const juce::String& ownDevice) const;
        juce::String configuredCapture;
        juce::String failedCapture;
        juce::int64 retryCaptureAtMs { 0 };
        // Ask an IDLE daemon to exit (refused mid-take by the daemon).
        bool requestQuit();

        // GUI-side watchdog -- call at timer rate. Fires onDaemonDied(wasRecording)
        // once per death; safe to keep calling afterwards.
        void tick();
        std::function<void (bool wasRecording)> onDaemonDied;

    private:
        friend class zynforge::Fix20261124HelperTests;
        CaptureClient      client;
        HelperProcess      process;
        juce::File         binaryPath;
        int                port      { 0 };
        bool               launched  { false };
        bool               everAttached  { false };   // a link was established at least once
        bool               deathReported { false };

        mutable std::mutex statusLock;
        EngineStatus       status;
        juce::int64        statusAtMs { 0 };
        std::atomic<bool>  statusSeen { false };

        bool heldDeviceDiffers (const juce::String& wanted) const;
        bool idleByFreshStatus() const;
        bool retireOtherBuild();
        bool launchAndAttach();
        bool attachToLaunched (bool& answeredWrongly);
        bool launchedHelperAnswers();
        // A helper that has closed its port can still hold the port's key a
        // while (CaptureServer::stop), and one launched meanwhile can't listen.
        // A launch waits this long for the key, and starts the helper again,
        // after a pause, when the one it started could not listen.
        static constexpr int kEndpointReleaseWaitMs = 4000;
        static constexpr int kLaunchAttempts        = 3;
        static constexpr int kRelaunchPauseMs       = 250;    // doubled each time
        static constexpr int kRelaunchReleaseWaitMs = 1000;
        bool isOtherBuild (const juce::String& build);
        void replaceOtherBuildWhenIdle();
        bool               otherBuildKept { false };
        juce::int64        otherBuildIdleSinceMs { 0 };   // 0 = not seen idle since the last check
        juce::int64        nextReplaceAttemptMs { 0 };
        int                failedReplaceAttempts { 0 };
        juce::StringArray  launchArgs;                    // connectOrLaunch's extraArgs, for a later launch
        // Idle this long before tick() swaps helpers: the window has seen the
        // take end by then.
        static constexpr juce::int64 kOtherBuildIdleGraceMs = 1000;
        juce::String       attachedBuild;
        juce::int64        attachedAtMs { 0 };   // when the last hello was answered
        juce::String       failedCaptureReason;
        bool               replacedOtherBuild { false };
        void joinStop();
        std::thread        stopThread;
        std::atomic<bool>  stopInFlight { false };
        // A queued completion may outlive this object.
        std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureSupervisor)
    };
}
