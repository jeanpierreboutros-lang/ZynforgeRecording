#pragma once

#include <juce_core/juce_core.h>

#include "CaptureProtocol.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
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
        juce::StreamingSocket* client { nullptr };   // valid while a reader thread runs
        std::mutex             writeLock;            // serialises writes to `client`
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
        // Send Hello and block for the daemon's reply (version negotiation).
        // Returns the reply; reply.ok == false on timeout or version mismatch.
        Reply hello (int timeoutMs = 1000);

        std::function<void (const EngineStatus&)> onStatus;
        std::function<void (const Reply&)>        onReply;

    private:
        void readLoop();
        bool writeLine (const juce::var&);

        std::unique_ptr<juce::StreamingSocket> socket;
        std::mutex        writeLock;
        std::thread       readThread;
        std::atomic<bool> connected { false };

        // Hello round-trip sync.
        std::mutex              helloLock;
        std::condition_variable helloCv;
        bool                    helloPending { false };
        bool                    helloGot     { false };
        Reply                   helloReply;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CaptureClient)
    };
}
