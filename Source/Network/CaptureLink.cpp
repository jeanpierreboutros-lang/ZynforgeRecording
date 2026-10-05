#include "CaptureLink.h"

#include <chrono>
#include <cerrno>
#if JUCE_MAC || JUCE_LINUX
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/socket.h>
#endif

namespace zynforge::capture
{
    namespace
    {
        // Guard against an unbounded / malformed peer: a single line may not
        // exceed this many raw bytes before we tear the socket down.
        constexpr size_t kMaxLineBytes = 1u << 20;   // 1 MiB
        using Clock = std::chrono::steady_clock;
        constexpr auto kWriteLockWait = std::chrono::milliseconds (250);
        thread_local const CaptureServer* currentCaptureReader = nullptr;

        struct FrameSerializationAborted {};

        // JUCE's JSON formatter ignores false OutputStream::write results, so
        // a private exception is needed to stop its per-character formatting
        // loop. It never escapes makeBoundedFrame; all temporary state is owned.
        class BoundedFrameStream final : public juce::OutputStream
        {
        public:
            explicit BoundedFrameStream (Clock::time_point limit) : deadline (limit) {}

            bool write (const void* bytes, size_t size) override
            {
                checkDeadline();
                if (size > kMaxLineBytes - data.getDataSize() || ! data.write (bytes, size))
                    throw FrameSerializationAborted {};
                return true;
            }
            void flush() override {}
            bool setPosition (juce::int64) override { return false; }
            juce::int64 getPosition() override { return data.getPosition(); }

            void checkDeadline() const
            {
                if (Clock::now() >= deadline) throw FrameSerializationAborted {};
            }

            juce::String finish()
            {
                checkDeadline();
                auto line = data.toUTF8() + "\n";
                checkDeadline();
                return line;
            }

        private:
            Clock::time_point deadline;
            juce::MemoryOutputStream data { 1024 };
        };

        bool makeBoundedFrame (const juce::var& value, Clock::time_point deadline, juce::String& result)
        {
            try
            {
                BoundedFrameStream stream (deadline);
                stream.checkDeadline();
                juce::JSON::writeToStream (stream, value, true);
                result = stream.finish();
                return true;
            }
            catch (const FrameSerializationAborted&)
            {
                return false;
            }
        }

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
                // Validate complete lines before decoding or dispatching too:
                // the final read may contain both the excess byte and newline.
                if (i - start > kMaxLineBytes) return false;
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

        // Native poll/nonblocking send avoids JUCE's shared read mutex. The
        // same absolute deadline covers lock acquisition, partial sends and
        // the reply; a dozing peer cannot pin the caller indefinitely.
        // All callers serialize socket close with this socket's write lock.
        bool writeAll (juce::StreamingSocket& s, const juce::String& line,
                       Clock::time_point deadline = Clock::now() + kWriteLockWait)
        {
           #if JUCE_MAC || JUCE_LINUX
            // MSG_DONTWAIT alone did not bound send() on a fresh macOS socket:
            // a non-reading peer held it beyond the request deadline. Keep the
            // descriptor itself nonblocking, preserving all other status flags.
            // JUCE read(false) uses the same mode; no reader switches it back.
            // Socket close is excluded by the caller's write lock or exclusive
            // ownership of a not-yet-published handshake socket.
            const int handle = s.getRawSocketHandle();
            const int statusFlags = ::fcntl (handle, F_GETFL, 0);
            if (statusFlags < 0
                || ((statusFlags & O_NONBLOCK) == 0
                    && ::fcntl (handle, F_SETFL, statusFlags | O_NONBLOCK) < 0))
                return false;
           #endif
            const auto utf8 = line.toRawUTF8();
            const int total = (int) std::strlen (utf8);
            int sent = 0;
            while (sent < total)
            {
                const auto remaining = deadline - Clock::now();
                if (remaining <= Clock::duration::zero()) return false;
               #if JUCE_MAC || JUCE_LINUX
                pollfd descriptor { s.getRawSocketHandle(), POLLOUT, 0 };
                const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds> (remaining).count();
                const int ready = ::poll (&descriptor, 1, (int) juce::jmax<int64_t> (1, milliseconds));
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
                    return false;
                int flags = MSG_DONTWAIT;
               #ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
               #endif
                const int n = (int) ::send (descriptor.fd, utf8 + sent, (size_t) (total - sent), flags);
               #else
                const int n = s.write (utf8 + sent, total - sent);
               #endif
                if (n <= 0)
                {
                   #if JUCE_MAC || JUCE_LINUX
                    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                        continue;
                   #endif
                    return false;
                }
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
        if (! endpointIdentity.create (listenPort.load()))
        {
            listener.reset();
            listenPort.store (-1);
            return false;
        }
        running.store (true);
        acceptThread = std::thread ([this] { acceptLoop(); });
        return true;
    }

    void CaptureServer::stop()
    {
        // Never call from onCommand (it fires on the reader thread): the
        // join below would deadlock on itself. Assert in debug.
        jassert (currentCaptureReader != this);
        // The flag decides whether to do WORK; it must never decide whether to
        // JOIN. Only stop() clears `running` today, so the early return was safe
        // -- but it is the exact shape that aborted the process in the console
        // TCP transports (a reader that clears its own flag, then a destructor
        // that destroys a joinable thread -> std::terminate). Unconditional.
        running.store (false);
        if (listener != nullptr) listener->close();
        {
            // shared_ptr's atomic free functions let shutdown obtain a safe
            // owning reference even when a writer currently holds writeLock.
            // Reading the shared_ptr after a timed lock failure was a data race
            // with readLoop resetting it.
            auto live = std::atomic_load_explicit (&client, std::memory_order_acquire);
            const std::lock_guard<std::timed_mutex> guard (writeLock);
            if (live != nullptr) live->close();
        }
        if (acceptThread.joinable()) acceptThread.join();
        if (readThread.joinable())   readThread.join();
        listener.reset();
        endpointIdentity.reset();
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
            // Authenticate the candidate before touching the established
            // controller. Each connection has a fresh challenge.
            const auto serverNonce = securetoken::generate();
            if (serverNonce.isEmpty()) continue;
            auto* challenge = new juce::DynamicObject();
            challenge->setProperty ("type", "challenge");
            challenge->setProperty ("version", kProtocolVersion);
            challenge->setProperty ("nonce", serverNonce);
            if (! writeAll (*sock, frame (juce::var (challenge)))) continue;
            Command hello;
            bool accepted = false, rejected = false;
            juce::String serverProof;
            juce::MemoryBlock pendingBytes;
            const auto handshakeDeadline = Clock::now() + std::chrono::milliseconds (1500);
            while (running.load() && ! accepted && ! rejected && Clock::now() < handshakeDeadline)
            {
                if (! pumpLines (*sock, pendingBytes, [&] (const juce::String& line)
                {
                    if (accepted || rejected) return;
                    bool parsed = false;
                    const auto candidate = Command::fromJson (juce::JSON::parse (line), parsed);
                    if (! parsed) { rejected = true; return; }
                    Reply refusal; refusal.id = candidate.id;
                    if (candidate.action != Action::Hello)
                        refusal.error = "authenticated hello handshake required before commands";
                    else if (candidate.version != kProtocolVersion)
                    {
                        refusal.error = "capture protocol version mismatch";
                        rejected = true;
                    }
                    else if (candidate.authProof.isEmpty() && identity::validNonce (candidate.authNonce))
                        return; // initial Hello; the next must prove the challenge
                    else
                    {
                        const auto transcript = serverNonce + ":" + candidate.authNonce + ":"
                                              + juce::String (kProtocolVersion);
                        if (identity::validNonce (candidate.authNonce)
                            && identity::equalProof (candidate.authProof,
                                identity::authenticate (endpointIdentity.secret(), "client:" + transcript)))
                        {
                            hello = candidate;
                            serverProof = identity::authenticate (endpointIdentity.secret(), "server:" + transcript);
                            accepted = true;
                            return;
                        }
                        refusal.error = "capture endpoint authentication failed";
                        rejected = true;
                    }
                    if (! writeAll (*sock, frame (refusal.toJson()), handshakeDeadline)) rejected = true;
                })) break;
            }
            if (! accepted || ! running.load()) continue;
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
                auto old = std::atomic_load_explicit (&client, std::memory_order_acquire);
                {
                    std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
                    if (g.owns_lock() && old != nullptr)
                        writeAll (*old, frame (encodeBye ("superseded")));
                }
                // Bounded writes allow close to share the lock, preventing a
                // native descriptor from being recycled during a send.
                const std::lock_guard<std::timed_mutex> guard (writeLock);
                if (old != nullptr) old->close();
            }
            if (readThread.joinable()) readThread.join();
            readThread = std::thread ([this, s = std::move (sock), hello, serverProof] () mutable
            {
                readLoop (std::move (s), hello, serverProof);
            });
        }
    }

    void CaptureServer::readLoop (std::unique_ptr<juce::StreamingSocket> sock,
                                 Command hello, juce::String serverProof)
    {
        currentCaptureReader = this;
        const juce::ScopeGuard clearReader { [] { currentCaptureReader = nullptr; } };
        std::shared_ptr<juce::StreamingSocket> shared (std::move (sock));
        {
            const std::lock_guard<std::timed_mutex> g (writeLock);
            std::atomic_store_explicit (&client, shared, std::memory_order_release);
        }
        clientConnected.store (true);

        Reply greeting;
        greeting.id = hello.id;
        greeting.ok = greeting.completed = true;
        greeting.authProof = std::move (serverProof);
        if (sendReply (greeting) && onCommand) onCommand (hello);

        juce::MemoryBlock scratch;
        while (running.load())
        {
            const bool keep = pumpLines (*shared, scratch,
                [this] (const juce::String& line)
            {
                const auto v = juce::JSON::parse (line);
                if (messageType (v) != "cmd") return;
                bool ok = false;
                const auto cmd = Command::fromJson (v, ok);
                if (! ok) return;

                if (cmd.action == Action::Hello)
                {
                    Reply r;
                    r.id = cmd.id;
                    r.error = "hello handshake already completed; reconnect to authenticate again";
                    sendReply (r);
                    return;
                }
                if (onCommand) onCommand (cmd);
            });
            if (! keep) break;
        }

        {
            const std::lock_guard<std::timed_mutex> g (writeLock);
            std::atomic_store_explicit (&client,
                                        std::shared_ptr<juce::StreamingSocket>(),
                                        std::memory_order_release);
        }
        clientConnected.store (false);
    }

    bool CaptureServer::writeLine (const juce::var& v)
    {
        // Fail fast instead of queueing behind a write that's stuck on a dead
        // peer -- that queueing is what let one dozing GUI stall the daemon's
        // whole status pump.
        std::unique_lock<std::timed_mutex> g (writeLock, kWriteLockWait);
        if (! g.owns_lock()) return false;
        auto live = std::atomic_load_explicit (&client, std::memory_order_acquire);
        if (live == nullptr) return false;
        if (writeAll (*live, frame (v))) return true;
        live->close(); // discard an incomplete frame before any later status
        return false;
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
        endpointPort = port;
        authenticated.store (false);
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
        authenticated.store (false);
        {
            const std::lock_guard<std::timed_mutex> guard (writeLock);
            if (socket != nullptr) socket->close();
        }
        if (readThread.joinable()) readThread.join();
        socket.reset();
        {
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = 0;
            replyGot  = false;
            challengeNonce.clear();
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
                    if (authenticated.load() && onStatus) onStatus (decodeStatus (v));
                }
                else if (type == "challenge" && ! authenticated.load())
                {
                    const auto nonce = v.getProperty ("nonce", "").toString();
                    if ((int) v.getProperty ("version", 0) == kProtocolVersion && identity::validNonce (nonce))
                    {
                        const std::lock_guard<std::mutex> guard (replyLock);
                        if (challengeNonce.isEmpty()) challengeNonce = nonce;
                        replyCv.notify_all();
                    }
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
                    if (onReply && (authenticated.load() || ! r.ok)) onReply (r);
                }
                else if (type == "bye" && authenticated.load())
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
        return writeLine (v, Clock::now() + kWriteLockWait);
    }

    bool CaptureClient::writeLine (const juce::var& v, Clock::time_point deadline)
    {
        // Formatting is part of the same request deadline. An expired or
        // oversized frame has sent no bytes, so leave the connection reusable.
        juce::String line;
        if (! makeBoundedFrame (v, deadline, line)) return false;
        std::unique_lock<std::timed_mutex> g (writeLock, std::defer_lock);
        g.try_lock_until (deadline);
        if (! g.owns_lock() || ! connected.load() || socket == nullptr) return false;
        if (writeAll (*socket, line, deadline)) return true;
        // A partial JSON frame cannot be reused for the next request.
        connected.store (false);
        socket->close();
        replyCv.notify_all();
        return false;
    }

    bool CaptureClient::send (const Command& c)
    {
        return (c.action == Action::Hello || authenticated.load()) && writeLine (c.toJson());
    }

    Reply CaptureClient::request (const Command& c, int timeoutMs)
    {
        if (c.action != Action::Hello && ! authenticated.load())
        {
            Reply refusal; refusal.error = "authenticated hello handshake required before commands";
            return refusal;
        }
        const auto deadline = Clock::now() + std::chrono::milliseconds (juce::jmax (0, timeoutMs));
        Command cc = c;
        cc.id = nextId.fetch_add (1);   // fresh id -> only THIS reply satisfies us
        {
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = cc.id;
            replyGot  = false;
        }
        if (! writeLine (cc.toJson(), deadline))
        {
            const std::lock_guard<std::mutex> g (replyLock);
            pendingId = 0;
            return {};                  // ok == false (write failed / not connected)
        }
        std::unique_lock<std::mutex> lk (replyLock);
        replyCv.wait_until (lk, deadline, [this] { return replyGot || ! connected.load(); });
        pendingId = 0;
        return replyGot ? pendingReply : Reply{};   // timeout / dropped link -> ok == false
    }

    Reply CaptureClient::hello (int timeoutMs)
    {
        Command h; h.action = Action::Hello; h.version = kProtocolVersion;
        const auto deadline = Clock::now() + std::chrono::milliseconds (juce::jmax (0, timeoutMs));
        h.authNonce = securetoken::generate();
        if (h.authNonce.isEmpty() || ! writeLine (h.toJson(), deadline)) return {};
        juce::String nonce;
        {
            std::unique_lock<std::mutex> lock (replyLock);
            replyCv.wait_until (lock, deadline, [this] { return challengeNonce.isNotEmpty() || ! connected.load(); });
            nonce = challengeNonce;
        }
        const auto key = identity::Endpoint::load (endpointPort);
        if (! identity::validNonce (nonce) || key.isEmpty())
        {
            Reply failure; failure.error = "capture endpoint identity is unavailable";
            return failure;
        }
        const auto transcript = nonce + ":" + h.authNonce + ":" + juce::String (kProtocolVersion);
        h.authProof = identity::authenticate (key, "client:" + transcript);
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - Clock::now()).count();
        auto result = request (h, (int) juce::jmax<int64_t> (0, remaining));
        if (! result.ok || result.version != kProtocolVersion
            || ! identity::equalProof (result.authProof, identity::authenticate (key, "server:" + transcript)))
        {
            result.ok = result.completed = false;
            result.error = "capture endpoint authentication failed";
            return result;
        }
        authenticated.store (true);
        return result;
    }
}
