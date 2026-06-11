#include "CaptureLink.h"

namespace zynforge::capture
{
    namespace
    {
        // Read available bytes, append to `buffer`, and dispatch every
        // COMPLETE newline-terminated line through `onLine`. Returns false
        // when the socket should be torn down (peer closed / error). Blocks
        // up to ~200 ms per call so the caller can re-check its run flag.
        bool pumpLines (juce::StreamingSocket& s, juce::MemoryBlock& /*unused*/,
                        juce::String& buffer,
                        const std::function<void (const juce::String&)>& onLine)
        {
            const int ready = s.waitUntilReady (true, 200);
            if (ready < 0) return false;       // error
            if (ready == 0) return true;       // timeout -- caller re-checks run flag

            char buf[4096];
            const int got = s.read (buf, sizeof (buf), false);
            if (got <= 0) return false;        // 0 = peer closed cleanly; <0 = error
            buffer += juce::String::fromUTF8 (buf, got);

            for (int nl; (nl = buffer.indexOfChar ('\n')) >= 0; )
            {
                const auto line = buffer.substring (0, nl).trim();
                buffer = buffer.substring (nl + 1);
                if (line.isNotEmpty()) onLine (line);
            }
            return true;
        }

        bool writeAll (juce::StreamingSocket& s, const juce::String& line)
        {
            const auto utf8 = line.toRawUTF8();
            const int total = (int) std::strlen (utf8);
            int sent = 0;
            while (sent < total)
            {
                const int n = s.write (utf8 + sent, total - sent);
                if (n <= 0) return false;      // SIGPIPE is ignored -> -1, not a crash
                sent += n;
            }
            return true;
        }
    }

    // ── CaptureServer ───────────────────────────────────────────────────────

    bool CaptureServer::listen (int port, const juce::String& bindAddr)
    {
        stop();
        listener = std::make_unique<juce::StreamingSocket>();
        if (! listener->createListener (port, bindAddr))
        {
            listener.reset();
            return false;
        }
        listenPort.store (listener->getBoundPort() > 0 ? listener->getBoundPort() : port);
        running.store (true);
        acceptThread = std::thread ([this] { acceptLoop(); });
        return true;
    }

    void CaptureServer::stop()
    {
        // Never call from onCommand (it fires on the reader thread): the
        // join below would deadlock on itself. Assert in debug.
        jassert (std::this_thread::get_id() != readThread.get_id());
        if (! running.exchange (false)) return;
        if (listener != nullptr) listener->close();
        {
            const std::lock_guard<std::mutex> g (writeLock);
            if (client != nullptr) client->close();   // unblock the reader's wait
        }
        if (acceptThread.joinable()) acceptThread.join();
        if (readThread.joinable())   readThread.join();
        listener.reset();
        listenPort.store (-1);
        clientConnected.store (false);
    }

    void CaptureServer::acceptLoop()
    {
        while (running.load() && listener != nullptr)
        {
            auto sock = std::unique_ptr<juce::StreamingSocket> (listener->waitForNextConnection());
            if (sock == nullptr) break;
            // One GUI at a time: a NEW connection supersedes the old one.
            // Close the old client's socket first so its readLoop exits --
            // otherwise the join below would block until the old GUI
            // disconnected on its own, wedging the accept loop (and the new
            // connection) indefinitely.
            {
                const std::lock_guard<std::mutex> g (writeLock);
                if (client != nullptr) client->close();
            }
            if (readThread.joinable()) readThread.join();
            readThread = std::thread ([this, s = std::move (sock)] () mutable
            {
                readLoop (std::move (s));
            });
        }
    }

    void CaptureServer::readLoop (std::unique_ptr<juce::StreamingSocket> sock)
    {
        {
            const std::lock_guard<std::mutex> g (writeLock);
            client = sock.get();
        }
        clientConnected.store (true);

        juce::String buffer;
        juce::MemoryBlock scratch;
        while (running.load())
        {
            const bool keep = pumpLines (*sock, scratch, buffer, [this] (const juce::String& line)
            {
                const auto v = juce::JSON::parse (line);
                if (messageType (v) != "cmd") return;
                bool ok = false;
                const auto cmd = Command::fromJson (v, ok);
                if (! ok) return;

                // Auto-handle Hello: reply with version compatibility BEFORE
                // delivering the command upstream.
                if (cmd.action == Action::Hello)
                {
                    Reply r;
                    r.version = kProtocolVersion;
                    r.ok      = versionsCompatible (cmd.version, kProtocolVersion);
                    if (! r.ok) r.error = "version mismatch (gui " + juce::String (cmd.version)
                                            + " vs daemon " + juce::String (kProtocolVersion) + ")";
                    sendReply (r);
                }
                if (onCommand) onCommand (cmd);
            });
            if (! keep) break;
        }

        {
            const std::lock_guard<std::mutex> g (writeLock);
            client = nullptr;
        }
        clientConnected.store (false);
    }

    bool CaptureServer::writeLine (const juce::var& v)
    {
        const std::lock_guard<std::mutex> g (writeLock);
        if (client == nullptr) return false;
        return writeAll (*client, frame (v));
    }

    bool CaptureServer::sendStatus (const EngineStatus& s) { return writeLine (encodeStatus (s)); }
    bool CaptureServer::sendReply  (const Reply& r)        { return writeLine (r.toJson()); }

    // ── CaptureClient ───────────────────────────────────────────────────────

    bool CaptureClient::connect (const juce::String& host, int port)
    {
        disconnect();
        socket = std::make_unique<juce::StreamingSocket>();
        if (! socket->connect (host, port, 2000))
        {
            socket.reset();
            return false;
        }
        connected.store (true);
        readThread = std::thread ([this] { readLoop(); });
        return true;
    }

    void CaptureClient::disconnect()
    {
        // Never call from onStatus / onReply (they fire on the reader
        // thread): the join below would deadlock on itself.
        jassert (std::this_thread::get_id() != readThread.get_id());
        // ALWAYS join a joinable reader -- even when `connected` is already
        // false. The readLoop clears `connected` itself when the DAEMON drops
        // the connection; gating the join on that flag left the finished
        // thread unjoined, and destroying a joinable std::thread calls
        // std::terminate (crashed the suite). It also reset the socket while
        // the reader could still be touching it.
        connected.store (false);
        if (socket != nullptr) socket->close();
        if (readThread.joinable()) readThread.join();
        socket.reset();
        {
            const std::lock_guard<std::mutex> g (helloLock);
            helloPending = false;
            helloGot = false;
        }
        helloCv.notify_all();
    }

    void CaptureClient::readLoop()
    {
        juce::String buffer;
        juce::MemoryBlock scratch;
        while (connected.load() && socket != nullptr)
        {
            const bool keep = pumpLines (*socket, scratch, buffer, [this] (const juce::String& line)
            {
                const auto v = juce::JSON::parse (line);
                const auto type = messageType (v);
                if (type == "status")
                {
                    if (onStatus) onStatus (decodeStatus (v));
                }
                else if (type == "reply")
                {
                    const auto r = Reply::fromJson (v);
                    {
                        const std::lock_guard<std::mutex> g (helloLock);
                        if (helloPending) { helloReply = r; helloGot = true; helloPending = false; }
                    }
                    helloCv.notify_all();
                    if (onReply) onReply (r);
                }
            });
            if (! keep) break;
        }
        connected.store (false);
    }

    bool CaptureClient::writeLine (const juce::var& v)
    {
        const std::lock_guard<std::mutex> g (writeLock);
        if (! connected.load() || socket == nullptr) return false;
        return writeAll (*socket, frame (v));
    }

    bool CaptureClient::send (const Command& c) { return writeLine (c.toJson()); }

    Reply CaptureClient::hello (int timeoutMs)
    {
        {
            const std::lock_guard<std::mutex> g (helloLock);
            helloPending = true;
            helloGot = false;
        }
        Command h; h.action = Action::Hello; h.version = kProtocolVersion;
        if (! send (h))
        {
            const std::lock_guard<std::mutex> g (helloLock);
            helloPending = false;
            return {};                          // ok == false
        }
        std::unique_lock<std::mutex> lk (helloLock);
        helloCv.wait_for (lk, std::chrono::milliseconds (timeoutMs), [this] { return helloGot; });
        return helloGot ? helloReply : Reply{};
    }
}
