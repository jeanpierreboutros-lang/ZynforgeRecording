#pragma once

#include <juce_core/juce_core.h>

#include "../Network/CaptureLink.h"

#include <atomic>
#include <functional>
#include <mutex>

namespace zynforge::capture
{
    // GUI-side supervisor for the zynforge-capture daemon (Phases 1d + 2 of
    // the capture-process split).
    //
    // Lifecycle (Phase 2 semantics baked in):
    //  * connectOrLaunch() tries to ATTACH to an already-running daemon
    //    first -- a restarted GUI re-adopts a rolling take instead of
    //    spawning a second daemon. Only when nothing answers does it launch
    //    the binary (juce::ChildProcess) and connect.
    //  * tick() is the GUI-side watchdog (call from the UI timer): it
    //    detects a dead daemon / dropped link and reports via onDaemonDied
    //    -- LOUDLY if it died mid-take. It never auto-relaunches mid-take
    //    (the take is already lost; the engineer must see that).
    //  * shutdown() leaves a RECORDING daemon rolling (the whole point: the
    //    GUI quitting must not stop the take); an idle daemon we launched
    //    is asked to exit (SIGTERM via ChildProcess::kill).
    //
    // The daemon side needs no watchdog: a vanished GUI just drops the
    // socket (SIGPIPE ignored) and recording continues.
    //
    // Message-thread only, like the rest of the engine surface; status
    // pushes arrive on the client reader thread and are double-buffered
    // under statusLock.
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

        // Attach to a live daemon on `port`, else launch + connect.
        // extraArgs are appended to the launch command (tests pass
        // "--no-device"). False if neither attach nor launch worked.
        bool connectOrLaunch (int port, const juce::StringArray& extraArgs = {});
        void disconnect();           // drop the link, leave the daemon running
        void shutdown();             // drop link; stop the daemon ONLY if idle + we launched it

        bool isAttached()   const noexcept { return client.isConnected(); }
        bool didLaunch()    const noexcept { return launched; }
        int  getPort()      const noexcept { return port; }

        // Latest status push (thread-safe copy) + whether one ever arrived.
        EngineStatus lastStatus() const;
        bool         hasStatus()  const noexcept { return statusSeen.load(); }
        bool         isDaemonRecording() const   { return lastStatus().recording; }

        // Recording control over the wire. startRecording syncs the config
        // first (track count, format, per-track arms), then starts.
        bool startRecording (const juce::File& sessionDir, int trackCount,
                             int captureFormat, const juce::BigInteger& armedTracks);
        bool stopRecording();
        // Ask an IDLE daemon to exit (refused mid-take by the daemon).
        bool requestQuit();

        // GUI-side watchdog -- call at timer rate. Fires onDaemonDied(wasRecording)
        // once per death; safe to keep calling afterwards.
        void tick();
        std::function<void (bool wasRecording)> onDaemonDied;

    private:
        CaptureClient      client;
        juce::ChildProcess process;
        juce::File         binaryPath;
        int                port      { 0 };
        bool               launched  { false };
        bool               everAttached  { false };   // a link was established at least once
        bool               deathReported { false };

        mutable std::mutex statusLock;
        EngineStatus       status;
        std::atomic<bool>  statusSeen { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureSupervisor)
    };
}
