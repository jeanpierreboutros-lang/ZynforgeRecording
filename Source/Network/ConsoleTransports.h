#pragma once

// The three concrete console transports. See ConsoleTransport.h for why the
// seam exists.
//
//   OscUdpTransport   -- Behringer X32 / M32, Behringer WING, DiGiCo, generic
//   TextLineTransport -- Yamaha SCP (ASCII commands, one per line, over TCP)
//   MidiTcpTransport  -- Allen & Heath (raw MIDI bytes over TCP)
//
// Only the OSC one has ever been exercised against real hardware. The other
// two are wire-format correct by construction (framing is simple and fully
// determined) but their DIALECTS are unverified -- which is precisely why
// ConsoleLink gates every WRITE behind a successful handshake.

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include "ConsoleLog.h"
#include "ConsoleTransport.h"
#include "OscPacketReceiver.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if JUCE_MAC || JUCE_LINUX
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/socket.h>
#endif

namespace zynforge
{
    // ── OSC over UDP ────────────────────────────────────────────────────────
    // One socket shared by sender + receiver: these desks reply to the
    // request's SOURCE port, so we must listen where we send from. That
    // socket takes datagrams from ANY host, so it is parsed by the
    // depth-capped OscPacketReceiver, never juce::OSCReceiver.
    class OscUdpTransport final : public ConsoleTransport,
                                  private juce::OSCReceiver::Listener<
                                      juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        OscUdpTransport()
        {
            receiver.addListener (this);
            receiver.onRefusedPacket = [this] (const OscPacketReceiver::Source& from, const juce::String& what)
            { if (onUnhandledTraffic) onUnhandledTraffic (from.toString(), what); };
        }
        ~OscUdpTransport() override { receiver.removeListener (this); disconnect(); }

        juce::String getName() const override { return "OSC/UDP"; }
        // atomic: isConnected() is read from other threads, and the other two
        // transports already use one. A torn plain bool is UB, not a style nit.
        bool isConnected() const override     { return connected.load (std::memory_order_acquire); }

        bool connect (const juce::String& host, int port) override
        {
            disconnect();
            // A FRESH socket every time: DatagramSocket::shutdown() permanently
            // invalidates the handle, so a reused one can never re-bind.
            socket = std::make_unique<juce::DatagramSocket> (false);
            if (! socket->bindToPort (0))              { socket.reset(); return false; }
            // A desk answers from the address we talk to. Anything else landing
            // on this port is not a reply, and a forged patch or gain value
            // would later be written back to the desk.
            if (! receiver.connectToSocket (*socket, oscwire::normaliseSourceIp (host)))
            { socket.reset(); return false; }
            if (! sender.connectToSocket (*socket, host, port))
            {
                // The receiver's reader thread references `socket` -- detach it
                // BEFORE freeing, or we free a socket it's still using.
                receiver.disconnect();
                socket.reset();
                return false;
            }
            connected.store (true, std::memory_order_release);
            return true;
        }

        void disconnect() override
        {
            if (! connected.exchange (false, std::memory_order_acq_rel)) return;
            sender.disconnect();
            receiver.disconnect();
            socket.reset();
        }

        bool send (const ConsoleMessage& m) override
        {
            if (! isConnected() || m.isEmpty()) return false;
            juce::OSCMessage osc { juce::OSCAddressPattern (m.address) };
            for (const auto& a : m.args)
            {
                if      (a.isInt() || a.isInt64()) osc.addInt32 ((juce::int32) (int) a);
                else if (a.isDouble())             osc.addFloat32 ((float) (double) a);
                else                               osc.addString (a.toString());
            }
            return sender.send (osc);
        }

        // Exposed so ConsoleLink's test seam can play the console's side.
        void injectForTests (const juce::OSCMessage& m) { oscMessageReceived (m); }
        int  localPortForTests() const { return socket != nullptr ? socket->getBoundPort() : -1; }

    private:
        void oscMessageReceived (const juce::OSCMessage& m) override
        {
            ConsoleMessage cm { m.getAddressPattern().toString() };
            for (const auto& a : m)
            {
                if      (a.isInt32())   cm.args.emplace_back ((int) a.getInt32());
                else if (a.isFloat32()) cm.args.emplace_back ((double) a.getFloat32());
                else if (a.isString())  cm.args.emplace_back (a.getString());
            }
            cm.source = receiver.getCurrentSource().toString();
            cm.wire   = consolelog::describe (m);   // keeps the blobs `args` drops
            if (onMessage) onMessage (cm);   // the receiver already hops to the message thread
        }

        void oscBundleReceived (const juce::OSCBundle& bundle) override
        {
            if (! onUnhandledTraffic) return;
            juce::StringArray lines;
            consolelog::describeBundle (bundle, lines);
            for (const auto& line : lines)
                onUnhandledTraffic (receiver.getCurrentSource().toString(), line);
        }

        std::unique_ptr<juce::DatagramSocket> socket;
        juce::OSCSender   sender;
        OscPacketReceiver receiver;
        std::atomic<bool> connected { false };
    };

    // ── What the two TCP transports share ───────────────────────────────────
    // A reader thread frames the stream (the subclass's consume) and hands each
    // message to the bounded inbound backlog (deliverOnMessageThread). A writer
    // thread owns every write: send() only queues the bytes, so a desk that
    // stops reading can never hold the message thread. send() used to write on
    // it, blocking, with no deadline. The queue is capped; a message that does
    // not fit, or that the desk takes nothing of within kWriteBudgetMs, is
    // dropped, counted (getSendDropCount) and reported through
    // onUnhandledTraffic. A write cut off part-way would glue the rest of the
    // next message to what went out, so that closes the link instead, and
    // ConsoleLink's watchdog reports it lost.
    class TcpStreamTransport : public ConsoleTransport
    {
    public:
        // A subclass's destructor calls disconnect() first: its reader may be
        // inside consume(), which is the subclass's.
        ~TcpStreamTransport() override { disconnect(); }

        bool isConnected() const override { return running.load(); }

        bool connect (const juce::String& host, int port) override
        {
            disconnect();
            resetFraming();
            auto s = std::make_unique<juce::StreamingSocket>();
            if (! s->connect (host, port, 2000)) return false;
           #if JUCE_MAC || JUCE_LINUX
            // The writer polls with a deadline and never waits in the kernel.
            // JUCE's read (false) puts the descriptor in the same mode.
            const int handle = s->getRawSocketHandle();
            const int statusFlags = ::fcntl (handle, F_GETFL, 0);
            if (statusFlags < 0 || ::fcntl (handle, F_SETFL, statusFlags | O_NONBLOCK) < 0) return false;
            #ifdef SO_NOSIGPIPE
            int on = 1;
            ::setsockopt (handle, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof (on));
            #endif
           #endif
            socket = std::move (s);
            overflowSource = host + ":" + juce::String (port);
            clearInbound (true);
            {
                const std::lock_guard<std::mutex> g (outLock);
                outQueue.clear();
                outBytes = 0;
            }
            sendDropped.store (0, std::memory_order_relaxed);
            running.store (true);
            reader = std::thread ([this] { readLoop(); });
            writer = std::thread ([this] { writeLoop(); });
            return true;
        }

        void disconnect() override
        {
            // ALWAYS join a joinable reader, even when `running` is ALREADY
            // false. readLoop clears the flag itself when the DESK drops the
            // connection (power cycle, switch reboot, idle TCP timeout), so
            // gating the join on it left a finished-but-unjoined thread --
            // and destroying a joinable std::thread calls std::terminate().
            // The whole process aborted on the next Connect / profile change /
            // quit after a console dropped its link. CaptureLink::disconnect
            // documents having been bitten by exactly this; the lesson didn't
            // reach here. The writer the same; and the writer goes first,
            // before the socket closes: it writes to the raw descriptor, which
            // the system could hand to another file once closed.
            running.store (false);
            { const std::lock_guard<std::mutex> g (outLock); }   // a writer about to wait sees the flag
            outReady.notify_all();
            if (writer.joinable()) writer.join();
            if (socket != nullptr) socket->close();   // unblocks the reader
            if (reader.joinable()) reader.join();
            socket.reset();
            {
                const std::lock_guard<std::mutex> g (outLock);
                outQueue.clear();
                outBytes = 0;
            }
            clearInbound (false);
        }

        // Outgoing messages dropped since connect(): the queue was full, or the
        // desk took nothing of one within kWriteBudgetMs.
        juce::int64 getSendDropCount() const noexcept { return sendDropped.load (std::memory_order_relaxed); }

        static constexpr size_t kMaxQueuedSends = 1024;
        static constexpr size_t kMaxQueuedBytes = 1u << 20;
        static constexpr double kWriteBudgetMs  = 1000.0;

    protected:
        // Reader thread: frame what `scratch` holds, deliver every whole
        // message through deliverOnMessageThread and keep the rest. False
        // tears the link down (a runaway line).
        virtual bool consume (juce::MemoryBlock& scratch) = 0;
        virtual void resetFraming() {}

        // Message thread: hands one message's bytes to the writer. False when
        // there is no link, or when it was dropped because the queue is full.
        bool queueBytes (std::vector<juce::uint8> bytes)
        {
            if (! running.load() || bytes.empty()) return false;
            {
                const std::lock_guard<std::mutex> g (outLock);
                if (outQueue.size() < kMaxQueuedSends && outBytes + bytes.size() <= kMaxQueuedBytes)
                {
                    outBytes += bytes.size();
                    outQueue.push_back (std::move (bytes));
                    outReady.notify_one();
                    return true;
                }
            }
            noteSendDropped ("not sent: the desk is not taking what was already sent");
            return false;
        }

    private:
        static constexpr double kSendReportMs = 1000.0;

        void readLoop()
        {
            juce::MemoryBlock scratch;
            char buf[2048];
            while (running.load() && socket != nullptr)
            {
                const int ready = socket->waitUntilReady (true, 200);
                if (ready < 0) break;
                if (ready == 0) continue;
                const int got = socket->read (buf, sizeof (buf), false);
                if (got <= 0) break;
                scratch.append (buf, (size_t) got);
                if (! consume (scratch)) break;
            }
            running.store (false);
        }

        void writeLoop()
        {
            for (;;)
            {
                std::vector<juce::uint8> next;
                {
                    std::unique_lock<std::mutex> g (outLock);
                    while (running.load() && outQueue.empty())
                        outReady.wait_for (g, std::chrono::milliseconds (200));
                    if (! running.load()) return;
                    next = std::move (outQueue.front());
                    outQueue.pop_front();
                    outBytes -= next.size();
                }
                const auto wrote = writeWithinBudget (next);
                if (wrote == Wrote::all) continue;
                if (! running.load()) return;   // closing: nothing to say
                if (wrote == Wrote::nothing)
                {
                    noteSendDropped ("not sent: the desk took none of it within "
                                     + juce::String ((int) kWriteBudgetMs) + " ms");
                    continue;
                }
                sendDropped.fetch_add (1, std::memory_order_relaxed);
                reportOnMessageThread (overflowSource, "link closed: a message to the desk was cut off part-way, "
                                                       "and the rest of the stream would not have read as sent");
                running.store (false);   // the reader leaves within its wait; the watchdog says the link is lost
                return;
            }
        }

        // all: every byte went out. nothing: the deadline passed with none
        // written, so the stream is still whole. broken: part of it went, or
        // the socket failed.
        enum class Wrote { all, nothing, broken };

        Wrote writeWithinBudget (const std::vector<juce::uint8>& bytes)
        {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + kWriteBudgetMs;
            size_t sent = 0;
           #if JUCE_MAC || JUCE_LINUX
            const int handle = socket->getRawSocketHandle();
            while (sent < bytes.size() && running.load())
            {
                const auto remaining = deadline - juce::Time::getMillisecondCounterHiRes();
                if (remaining <= 0.0) break;
                // Short slices, so disconnect() never waits on a full budget.
                pollfd descriptor { handle, POLLOUT, 0 };
                const int ready = ::poll (&descriptor, 1, (int) juce::jlimit (1.0, 50.0, remaining));
                if (ready < 0 && errno == EINTR) continue;
                if (ready < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return Wrote::broken;
                if (ready == 0) continue;
                int flags = MSG_DONTWAIT;
               #ifdef MSG_NOSIGNAL
                flags |= MSG_NOSIGNAL;
               #endif
                const auto n = ::send (handle, bytes.data() + sent, bytes.size() - sent, flags);
                if (n < 0)
                {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    return Wrote::broken;
                }
                if (n == 0) return Wrote::broken;
                sent += (size_t) n;
            }
           #else
            while (sent < bytes.size() && running.load()
                   && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                if (socket->waitUntilReady (false, 50) <= 0) continue;
                const int n = socket->write (bytes.data() + sent, (int) (bytes.size() - sent));
                if (n <= 0) return Wrote::broken;
                sent += (size_t) n;
            }
           #endif
            if (sent == bytes.size()) return Wrote::all;
            return sent == 0 ? Wrote::nothing : Wrote::broken;
        }

        // Message thread (a full queue) or the writer (a write that timed
        // out): counted always, said at most once a second.
        void noteSendDropped (const juce::String& why)
        {
            const auto count = sendDropped.fetch_add (1, std::memory_order_relaxed) + 1;
            const auto now = juce::Time::getMillisecondCounterHiRes();
            auto next = nextSendReportMs.load (std::memory_order_relaxed);
            if (now < next) return;
            if (! nextSendReportMs.compare_exchange_strong (next, now + kSendReportMs)) return;
            reportOnMessageThread (overflowSource, "-> " + why + " (" + juce::String (count) + " dropped so far)");
        }

        std::unique_ptr<juce::StreamingSocket> socket;
        std::thread       reader;
        std::thread       writer;
        std::atomic<bool> running { false };
        std::mutex        outLock;
        std::condition_variable outReady;
        std::deque<std::vector<juce::uint8>> outQueue;   // guarded by outLock
        size_t            outBytes { 0 };                // guarded by outLock
        std::atomic<juce::int64>  sendDropped { 0 };
        std::atomic<double>       nextSendReportMs { 0.0 };
    };

    // ── ASCII line protocol over TCP (Yamaha SCP) ───────────────────────────
    // SCP is newline-terminated text, e.g.
    //     get MIXER:Current/InCh/Label/Name 0 0
    //     OK get MIXER:Current/InCh/Label/Name 0 0 "KICK"
    // We split the wire into address = first token after the verb, args = the
    // remainder; the PROFILE decides what any of it means.
    class TextLineTransport final : public TcpStreamTransport
    {
    public:
        ~TextLineTransport() override { disconnect(); }

        juce::String getName() const override { return "SCP/TCP"; }

        // Queued for the writer thread (TcpStreamTransport): true once queued.
        bool send (const ConsoleMessage& m) override
        {
            if (! isConnected() || m.isEmpty()) return false;
            juce::String line = m.address;
            for (const auto& a : m.args)
            {
                line << " ";
                // Escape embedded quotes -- an unescaped one closes the token
                // early and corrupts the rest of the line.
                if (a.isString()) line << "\"" << a.toString().replace ("\"", "\\\"") << "\"";
                else              line << a.toString();
            }
            line << "\n";
            const auto* utf8 = line.toRawUTF8();
            return queueBytes (std::vector<juce::uint8> (utf8, utf8 + std::strlen (utf8)));
        }

        // Split one SCP line into a ConsoleMessage. Static + public so the
        // suite can prove the parse without a socket.
        static ConsoleMessage parseLine (const juce::String& raw)
        {
            const auto line = raw.trim();
            if (line.isEmpty()) return {};
            // Tokenise on spaces, honouring "quoted strings" (names contain spaces).
            juce::StringArray tok;
            juce::String cur;
            bool inQuotes = false;
            for (int i = 0; i < line.length(); ++i)
            {
                const auto c = line[i];
                if (c == '"')                 { inQuotes = ! inQuotes; continue; }
                if (c == ' ' && ! inQuotes)   { if (cur.isNotEmpty()) { tok.add (cur); cur.clear(); } continue; }
                cur << c;
            }
            if (cur.isNotEmpty()) tok.add (cur);
            if (tok.isEmpty()) return {};

            // A reply leads with a status verb (OK / NOTIFY / ERROR); a bare
            // push may not. Drop a leading verb so `address` is the parameter.
            int first = 0;
            const auto lead = tok[0].toUpperCase();
            // An ERROR repeats the request the desk refused. It answers
            // nothing, so it must never read as the parameter it names: as a
            // handshake, or as a channel name. It keeps ERROR as its address
            // and the rest as one text argument, for the log.
            if (lead == "ERROR")
            {
                tok.remove (0);
                return ConsoleMessage ("ERROR", { juce::var (tok.joinIntoString (" ")) });
            }
            if (lead == "OK" || lead == "NOTIFY" || lead == "OKM")
                first = 1;
            if (first >= tok.size()) return {};
            // "get"/"set" may follow the status verb; skip that too.
            const auto verb = tok[first].toLowerCase();
            if (verb == "get" || verb == "set" || verb == "ssrecall_ex" || verb == "sscurrent_ex")
                ++first;
            if (first >= tok.size()) return ConsoleMessage (tok[juce::jmax (0, first - 1)]);

            ConsoleMessage m { tok[first] };
            for (int i = first + 1; i < tok.size(); ++i)
            {
                const auto& t = tok[i];
                if (t.containsOnly ("-0123456789") && t.isNotEmpty())      m.args.emplace_back (t.getIntValue());
                else if (t.containsOnly ("-.0123456789") && t.isNotEmpty()) m.args.emplace_back ((double) t.getDoubleValue());
                else                                                        m.args.emplace_back (t);
            }
            return m;
        }

    protected:
        bool consume (juce::MemoryBlock& scratch) override
        {
            // Split on '\n' at the BYTE level so a multi-byte character
            // straddling a read boundary survives (same approach as
            // CaptureLink::pumpLines).
            const auto* base = static_cast<const char*> (scratch.getData());
            const size_t size = scratch.getSize();
            size_t start = 0;
            for (size_t i = 0; i < size; ++i)
            {
                if (base[i] != '\n') continue;
                const auto line = juce::String::fromUTF8 (base + start, (int) (i - start));
                start = i + 1;
                auto m = parseLine (line);
                if (! m.isEmpty()) deliverOnMessageThread (std::move (m));
            }
            if (start > 0)
            {
                juce::MemoryBlock rest;
                if (size > start) rest.append (base + start, size - start);
                scratch = std::move (rest);
            }
            return scratch.getSize() <= (1u << 20);   // runaway line: tear down
        }
    };

    // ── MIDI over TCP (Allen & Heath) ───────────────────────────────────────
    // A&H desks speak ordinary MIDI on a TCP stream. We frame on status bytes
    // and hand the PROFILE a synthetic address plus the raw bytes as args, so
    // the dialect (which NRPN/SysEx means what) stays out of the transport.
    class MidiTcpTransport final : public TcpStreamTransport
    {
    public:
        ~MidiTcpTransport() override { disconnect(); }

        juce::String getName() const override { return "MIDI/TCP"; }

        // Outbound: args are raw MIDI byte values (0..255) built by the
        // profile. Queued for the writer thread: true once queued.
        bool send (const ConsoleMessage& m) override
        {
            if (! isConnected() || m.args.empty()) return false;
            std::vector<juce::uint8> bytes;
            bytes.reserve (m.args.size());
            for (const auto& a : m.args)
                bytes.push_back ((juce::uint8) juce::jlimit (0, 255, (int) a));
            return queueBytes (std::move (bytes));
        }

        // Frame a byte stream into MIDI messages. Static + public so the suite
        // can prove the framing (running-status, SysEx spanning reads) without
        // a socket. Appends complete messages to `out`, keeps the remainder.
        static void frame (juce::MemoryBlock& buffer, std::vector<ConsoleMessage>& out,
                           juce::uint8& runningStatus)
        {
            const auto* d = static_cast<const juce::uint8*> (buffer.getData());
            const int   n = (int) buffer.getSize();

            auto emit = [&out] (const juce::uint8* p, int len)
            {
                ConsoleMessage m { "midi" };
                m.args.reserve ((size_t) len);
                for (int k = 0; k < len; ++k) m.args.emplace_back ((int) p[k]);
                out.push_back (std::move (m));
            };

            // RUNNING STATUS: after "B0 00 01" a desk may send "00 02" meaning
            // another CC on the same channel. Those continuations were being
            // discarded as stray data bytes -- and NRPN parameter sweeps, which
            // is most of what A&H sends, are exactly that. The comment at the
            // top of this class and the framing test both claimed running
            // status was handled; neither was true.
            // DATA bytes that follow a status byte. System-common messages were
            // all sized 0, so 0xF1 / 0xF2 / 0xF3 desynced the stream by their
            // own data bytes.
            auto dataBytesFor = [] (juce::uint8 st) -> int
            {
                const juce::uint8 hi = (juce::uint8) (st & 0xF0);
                if (hi == 0xC0 || hi == 0xD0) return 1;        // prog change / chan pressure
                if (hi >= 0x80 && hi <= 0xE0) return 2;        // note / CC / bend
                switch (st)                                     // system common
                {
                    case 0xF1: case 0xF3: return 1;             // quarter-frame / song select
                    case 0xF2:            return 2;             // song position
                    default:              return 0;             // realtime (F8..FF), F6, F7
                }
            };

            // Consume one byte at a time: realtime can interrupt a channel
            // message OR SysEx, including across TCP reads. Retain only the
            // unfinished message, with already-delivered realtime removed.
            std::vector<juce::uint8> pending;
            int required = 0;
            bool sysex = false;
            for (int i = 0; i < n; ++i)
            {
                const auto byte = d[i];
                if (byte >= 0xF8)
                {
                    emit (d + i, 1);
                    continue;
                }
                if (byte >= 0x80)
                {
                    if (byte == 0xF7 && sysex)
                    {
                        pending.push_back (byte);
                        emit (pending.data(), (int) pending.size());
                        pending.clear();
                        sysex = false;
                        continue;
                    }
                    // A new non-realtime status interrupts an incomplete
                    // message. Never interpret the status byte as message data.
                    pending.assign (1, byte);
                    sysex = byte == 0xF0;
                    runningStatus = byte < 0xF0 ? byte : 0;
                    required = dataBytesFor (byte) + 1;
                    if (! sysex && required == 1)
                    {
                        emit (pending.data(), 1);
                        pending.clear();
                    }
                    continue;
                }
                if (pending.empty())
                {
                    if (runningStatus == 0) continue;
                    pending.push_back (runningStatus);
                    required = dataBytesFor (runningStatus) + 1;
                }
                pending.push_back (byte);
                if (! sysex && (int) pending.size() == required)
                {
                    emit (pending.data(), required);
                    pending.clear();
                }
            }
            juce::MemoryBlock rest;
            if (! pending.empty()) rest.append (pending.data(), pending.size());
            buffer = std::move (rest);
        }

        static void frame (juce::MemoryBlock& buffer, std::vector<ConsoleMessage>& out)
        {
            juce::uint8 status = 0;
            frame (buffer, out, status);
        }

    protected:
        bool consume (juce::MemoryBlock& scratch) override
        {
            std::vector<ConsoleMessage> msgs;
            frame (scratch, msgs, runningStatus);
            for (auto& m : msgs) deliverOnMessageThread (std::move (m));
            return scratch.getSize() <= (1u << 20);
        }

        void resetFraming() override { runningStatus = 0; }

    private:
        juce::uint8 runningStatus { 0 };   // the reader's, between connects
    };
}
