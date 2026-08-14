#include "CaptureLink.h"

#include <chrono>

namespace zynforge::capture
{
    namespace
    {
        // Guard against an unbounded / malformed peer: a single line may not
        // exceed this many raw bytes before we tear the socket down.
        constexpr size_t kMaxLineBytes = 1u << 20;   // 1 MiB

        // Read available bytes, accumulate them RAW in `scratch`, and dispatch
        // every COMPLETE newline-terminated line through `onLine`. Buffering at
        // the BYTE level (not the decoded-String level) means a multibyte UTF-8
        // sequence that straddles a 4096-byte read boundary stays intact until
        // its whole line arrives, then decodes correctly -- ASCII is unaffected.
        // Returns false when the socket should be torn down (peer closed /
        // error / oversized line). Blocks up to ~200 ms per call so the caller
        // can re-check its run flag.
        bool pumpLines (juce::StreamingSocket& s, juce::MemoryBlock& scratch,
                        const std::function<void (const juce::String&)>& onLine)
        {
            const int ready = s.waitUntilReady (true, 200);
            if (ready < 0) return false;       // error
            if (ready == 0) return true;       // timeout -- caller re-checks run flag

            char buf[4096];
            const int got = s.read (buf, sizeof (buf), false);
            if (got <= 0) return false;        // 0 = peer closed cleanly; <0 = error

            scratch.append (buf, (size_t) got);

            // Split on '\n' at the byte level; decode only complete lines.
            const auto* base = static_cast<const char*> (scratch.getData());
            const size_t size = scratch.getSize();
            size_t start = 0;
            for (size_t i = 0; i < size; ++i)
            {
                if (base[i] != '\n') continue;
                const auto line = juce::String::fromUTF8 (base + start, (int) (i - start)).trim();
                start = i + 1;
                if (line.isNotEmpty()) onLine (line);
            }

            // Keep the unterminated remainder (partial line / partial multibyte
            // sequence) for the next read.
            if (start > 0)
            {
                const size_t remaining = size - start;
                juce::MemoryBlock rest;
                if (remaining > 0) rest.append (base + start, remaining);
                scratch = std::move (rest);
            }

            // A line that never terminates must not grow the buffer forever.
            if (scratch.getSize() > kMaxLineBytes) return false;
            return true;
        }

        // NOTE: do NOT poll writability with StreamingSocket::waitUntilReady
        // here. It takes the socket's readLock even for a WRITE check (see
        // juce_Socket.cpp), and this socket always has a reader thread parked in
        // waitUntilReady(true, 200) -- so the writer starves on the lock and
        // every send fails. Tried it; it broke the whole capture-daemon suite.
        //
        // A peer that is alive but not reading still wedges THIS write, which is
        // unavoidable with a blocking socket API. What must not happen is that
        // wedge spreading: writeLock is a timed_mutex so other writers fail fast
        // instead of queueing behind it, and the stuck write is interrupted the
        // way this codebase already does it everywhere else -- by closing the
        // socket from another thread.
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

        // How long any writer will wait for the write lock before giving up.
        constexpr auto kWriteLockWait = std::chrono::milliseconds (250);
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
        // The flag decides whether to do WORK; it must never decide whether to
        // JOIN. Only stop() clears `running` today, so the early return was safe
        // -- but it is the exact shape that aborted the process in the console
        // TCP transports (a reader that clears its own flag, then a destructor
        // that destroys a joinable thread -> std::terminate). Unconditional.
        running.store (false);
        if (listener != nullptr) listener->close();
        {
            std::shared_ptr<juce::StreamingSocket> live;
            {
                std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
                live = client;
            }
            if (live != nullptr) live->close();       // unblock the reader's wait
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
            if (sock == nullptr)
            {
                // nullptr means "listener closed" OR a transient accept error
                // (EMFILE and friends). Treating both as shutdown left the
                // server not accepting while isRunning() still said it was up.
                // Only a cleared `running` is a real shutdown.
                if (! running.load()) break;
                juce::Thread::sleep (200);
                continue;
            }
            // One GUI at a time: a NEW connection supersedes the old one.
            // Tell the old client it's being superseded ON PURPOSE (a "bye")
            // BEFORE closing its socket, so it distinguishes an intentional
            // kick from a real daemon death and doesn't raise a false mid-take
            // "daemon died" alarm. Then close the socket so its readLoop exits
            // -- otherwise the join below would block until the old GUI
            // disconnected on its own, wedging the accept loop (and the new
            // connection) indefinitely.
            {
                // Grab a reference under the lock if we can get it quickly; a
                // wedged writer must not stop us superseding the connection.
                std::shared_ptr<juce::StreamingSocket> old;
                {
                    std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
                    old = client;
                    if (g.owns_lock() && old != nullptr)
                        writeAll (*old, frame (encodeBye ("superseded")));
                }
                // Close OUTSIDE the lock: this is what interrupts a write that's
                // stuck on a dead peer, so it must not queue behind that write.
                // The shared_ptr keeps the socket alive while we do it.
                if (old != nullptr) old->close();
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
        std::shared_ptr<juce::StreamingSocket> shared (std::move (sock));
        {
            const std::lock_guard<std::timed_mutex> g (writeLock);
            client = shared;
        }
        clientConnected.store (true);

        juce::MemoryBlock scratch;
        while (running.load())
        {
            const bool keep = pumpLines (*shared, scratch, [this] (const juce::String& line)
            {
                const auto v = juce::JSON::parse (line);
                if (messageType (v) != "cmd") return;
                bool ok = false;
                const auto cmd = Command::fromJson (v, ok);
                if (! ok) return;

                // Auto-handle Hello: reply with version compatibility BEFORE
                // delivering the command upstream. Echo the request id so the
                // client can correlate this reply to its Hello.
                if (cmd.action == Action::Hello)
                {
                    Reply r;
                    r.id      = cmd.id;
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
            const std::lock_guard<std::timed_mutex> g (writeLock);
            client = nullptr;
        }
        clientConnected.store (false);
    }

    bool CaptureServer::writeLine (const juce::var& v)
    {
        // Fail fast instead of queueing behind a write that's stuck on a dead
        // peer -- that queueing is what let one dozing GUI stall the daemon's
        // whole status pump.
        std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
        if (! g.owns_lock() || client == nullptr) return false;
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
        superseded.store (false);
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
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = 0;
            replyGot  = false;
        }
        replyCv.notify_all();      // wake any in-flight request() -- link is gone
    }

    void CaptureClient::readLoop()
    {
        juce::MemoryBlock scratch;
        while (connected.load() && socket != nullptr)
        {
            const bool keep = pumpLines (*socket, scratch, [this] (const juce::String& line)
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
                        // Correlate by id: only satisfy the pending request when
                        // the reply's id matches. A stray / late reply for a
                        // different (already-completed) request can't latch here.
                        const std::lock_guard<std::mutex> g (replyLock);
                        if (pendingId != 0 && r.id == pendingId)
                        {
                            pendingReply = r;
                            replyGot     = true;
                            pendingId    = 0;
                        }
                    }
                    replyCv.notify_all();
                    if (onReply) onReply (r);
                }
                else if (type == "bye")
                {
                    // The daemon intentionally superseded/closed us -- not a
                    // death. tick() reads this to suppress a false alarm.
                    superseded.store (true);
                }
            });
            if (! keep) break;
        }
        connected.store (false);
        replyCv.notify_all();      // unblock any request() waiting on a dead link
    }

    bool CaptureClient::writeLine (const juce::var& v)
    {
        std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
        if (! g.owns_lock() || ! connected.load() || socket == nullptr) return false;
        return writeAll (*socket, frame (v));
    }

    bool CaptureClient::send (const Command& c) { return writeLine (c.toJson()); }

    Reply CaptureClient::request (const Command& c, int timeoutMs)
    {
        Command cc = c;
        cc.id = nextId.fetch_add (1);   // fresh id -> only THIS reply satisfies us
        {
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = cc.id;
            replyGot  = false;
        }
        if (! send (cc))
        {
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = 0;
            return {};                  // ok == false (write failed / not connected)
        }
        std::unique_lock<std::mutex> lk (replyLock);
        replyCv.wait_for (lk, std::chrono::milliseconds (timeoutMs),
                          [this] { return replyGot || ! connected.load(); });
        pendingId = 0;
        return replyGot ? pendingReply : Reply{};   // timeout / dropped link -> ok == false
    }

    Reply CaptureClient::hello (int timeoutMs)
    {
        Command h; h.action = Action::Hello; h.version = kProtocolVersion;
        return request (h, timeoutMs);
    }
}
