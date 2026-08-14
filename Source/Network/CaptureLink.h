#pragma once

#include <juce_core/juce_core.h>

#include "CaptureProtocol.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <chrono>
#include <thread>

namespace zynforge::capture
{
    // Phase 1b of the capture-process split: the local-socket TRANSPORT for
    // the daemon<->GUI CaptureProtocol. Persistent bidirectional connection
    // (commands GUI->daemon, status/replies daemon->GUI) over newline-JSON.
    // Transport only -- NOT wired to a recorder yet; the daemon binary
    // (Phase 1c) will own the recorder and drive a CaptureServer.
    //
    // Loopback by default. SIGPIPE is ignored process-wide (Main.cpp), so a
    // write to a dropped peer fails the call instead of killing the process.

    // ── Daemon side ────────────────────────────────────────────────────────
    // Listens on a port, holds ONE GUI connection at a time, reads Command
    // lines, pushes status/reply lines. Auto-handles the Hello handshake
    // (version check + reply) before delivering the Hello to onCommand.
    class CaptureServer
    {
    public:
        CaptureServer() = default;
        ~CaptureServer() { stop(); }

        bool listen (int port, const juce::String& bindAddr = "127.0.0.1");
        void stop();
        bool isListening() const noexcept { return running.load(); }
        bool hasClient()   const noexcept { return clientConnected.load(); }
        int  getPort()     const noexcept { return listenPort.load(); }

        // Fired on the reader thread for each parsed Command (a Hello is
        // delivered AFTER its auto-reply is sent).
        std::function<void (const Command&)> onCommand;

        // Push to the connected GUI. Thread-safe; no-op if no client. Returns
        // false if the write failed (peer gone).
        bool sendStatus (const EngineStatus&);
        bool sendReply  (const Reply&);

    private:
        void acceptLoop();
        void readLoop (std::unique_ptr<juce::StreamingSocket>);
        bool writeLine (const juce::var&);

        std::unique_ptr<juce::StreamingSocket> listener;
        // shared_ptr, not a raw pointer: the accept loop needs to CLOSE a stuck
        // client's socket without holding writeLock (that's what interrupts a
        // blocking write), and it must not race the reader thread destroying it.
        std::shared_ptr<juce::StreamingSocket> client;   // valid while a reader thread runs
        // TIMED: a peer that is alive but not reading blocks whoever is writing
        // to it. With a plain mutex every OTHER writer -- the status pump, the
        // accept loop's "bye" -- queued behind that stuck write, so one dozing
        // GUI wedged the daemon. A timed acquire fails fast instead.
        std::timed_mutex       writeLock;            // serialises writes to `client`
        std::thread            acceptThread, readThread;
        std::atomic<bool>      running { false };
        std::atomic<bool>      clientConnected { false };
        std::atomic<int>       listenPort { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureServer)
    };

    // ── GUI side ───────────────────────────────────────────────────────────
    // Connects to the daemon, sends Commands, receives status/reply lines.
    class CaptureClient
    {
    public:
        CaptureClient() = default;
        ~CaptureClient() { disconnect(); }

        bool connect (const juce::String& host, int port);
        void disconnect();
        bool isConnected() const noexcept { return connected.load(); }

        bool send (const Command&);              // false if not connected / write failed
        // Send a command with a fresh correlation id and BLOCK for the daemon's
        // matching reply. Returns the reply; reply.ok == false on timeout, write
        // failure, or a dropped link. A late/stray reply for a different request
        // can never satisfy this one (id-correlated).
        Reply request (const Command&, int timeoutMs = 1000);
        // Send Hello and block for the daemon's reply (version negotiation).
        // Returns the reply; reply.ok == false on timeout or version mismatch.
        Reply hello (int timeoutMs = 1000);

        // True once the daemon told us (via a "bye") that our connection was
        // intentionally superseded by a newer client -- NOT a daemon death.
        bool wasSuperseded() const noexcept { return superseded.load(); }

        std::function<void (const EngineStatus&)> onStatus;
        std::function<void (const Reply&)>        onReply;

    private:
        void readLoop();
        bool writeLine (const juce::var&);

        std::unique_ptr<juce::StreamingSocket> socket;
        std::timed_mutex  writeLock;
        std::thread       readThread;
        std::atomic<bool> connected  { false };
        std::atomic<bool> superseded { false };

        // Request/reply correlation: one outstanding request at a time (all
        // GUI calls are synchronous message-thread round-trips).
        std::mutex              replyLock;
        std::condition_variable replyCv;
        std::atomic<int>        nextId       { 1 };
        int                     pendingId    { 0 };     // 0 = nothing awaited
        bool                    replyGot     { false };
        Reply                   pendingReply;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureClient)
    };
}
