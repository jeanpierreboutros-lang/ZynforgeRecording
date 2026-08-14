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

#include "ConsoleTransport.h"

#include <atomic>
#include <memory>
#include <thread>

namespace zynforge
{
    // ── OSC over UDP ────────────────────────────────────────────────────────
    // One socket shared by sender + receiver: these desks reply to the
    // request's SOURCE port, so we must listen where we send from.
    class OscUdpTransport final : public ConsoleTransport,
                                  private juce::OSCReceiver::Listener<
                                      juce::OSCReceiver::MessageLoopCallback>
    {
    public:
        OscUdpTransport()           { receiver.addListener (this); }
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
            if (! receiver.connectToSocket (*socket))  { socket.reset(); return false; }
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
            if (onMessage) onMessage (cm);   // OSCReceiver already hops to the message thread
        }

        std::unique_ptr<juce::DatagramSocket> socket;
        juce::OSCSender   sender;
        juce::OSCReceiver receiver;
        std::atomic<bool> connected { false };
    };

    // ── ASCII line protocol over TCP (Yamaha SCP) ───────────────────────────
    // SCP is newline-terminated text, e.g.
    //     get MIXER:Current/InCh/Label/Name 0 0
    //     OK get MIXER:Current/InCh/Label/Name 0 0 "KICK"
    // We split the wire into address = first token after the verb, args = the
    // remainder; the PROFILE decides what any of it means.
    class TextLineTransport final : public ConsoleTransport
    {
    public:
        ~TextLineTransport() override { disconnect(); }

        juce::String getName() const override { return "SCP/TCP"; }
        bool isConnected() const override     { return running.load(); }

        bool connect (const juce::String& host, int port) override
        {
            disconnect();
            socket = std::make_unique<juce::StreamingSocket>();
            if (! socket->connect (host, port, 2000)) { socket.reset(); return false; }
            running.store (true);
            reader = std::thread ([this] { readLoop(); });
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
            // reach here.
            running.store (false);
            if (socket != nullptr) socket->close();   // unblocks the reader
            if (reader.joinable()) reader.join();
            socket.reset();
        }

        bool send (const ConsoleMessage& m) override
        {
            if (! running.load() || socket == nullptr || m.isEmpty()) return false;
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
            const auto utf8 = line.toRawUTF8();
            const int total = (int) std::strlen (utf8);
            int sent = 0;
            while (sent < total)
            {
                const int n = socket->write (utf8 + sent, total - sent);
                if (n <= 0) return false;
                sent += n;
            }
            return true;
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
            if (lead == "OK" || lead == "NOTIFY" || lead == "ERROR" || lead == "OKM")
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

    private:
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
                if (scratch.getSize() > (1u << 20)) break;   // runaway line: tear down
            }
            running.store (false);
        }

        std::unique_ptr<juce::StreamingSocket> socket;
        std::thread       reader;
        std::atomic<bool> running { false };
    };

    // ── MIDI over TCP (Allen & Heath) ───────────────────────────────────────
    // A&H desks speak ordinary MIDI on a TCP stream. We frame on status bytes
    // and hand the PROFILE a synthetic address plus the raw bytes as args, so
    // the dialect (which NRPN/SysEx means what) stays out of the transport.
    class MidiTcpTransport final : public ConsoleTransport
    {
    public:
        ~MidiTcpTransport() override { disconnect(); }

        juce::String getName() const override { return "MIDI/TCP"; }
        bool isConnected() const override     { return running.load(); }

        bool connect (const juce::String& host, int port) override
        {
            disconnect();
            socket = std::make_unique<juce::StreamingSocket>();
            if (! socket->connect (host, port, 2000)) { socket.reset(); return false; }
            running.store (true);
            reader = std::thread ([this] { readLoop(); });
            return true;
        }

        void disconnect() override
        {
            // See TextLineTransport::disconnect -- an unconditional join. A
            // reader that exited on its own (desk dropped the link) must still
            // be joined or destroying it calls std::terminate().
            running.store (false);
            if (socket != nullptr) socket->close();
            if (reader.joinable()) reader.join();
            socket.reset();
        }

        // Outbound: args are raw MIDI byte values (0..255) built by the profile.
        bool send (const ConsoleMessage& m) override
        {
            if (! running.load() || socket == nullptr || m.args.empty()) return false;
            std::vector<juce::uint8> bytes;
            bytes.reserve (m.args.size());
            for (const auto& a : m.args)
                bytes.push_back ((juce::uint8) juce::jlimit (0, 255, (int) a));
            int sent = 0;
            const int total = (int) bytes.size();
            while (sent < total)
            {
                const int n = socket->write (bytes.data() + sent, total - sent);
                if (n <= 0) return false;
                sent += n;
            }
            return true;
        }

        // Frame a byte stream into MIDI messages. Static + public so the suite
        // can prove the framing (running-status, SysEx spanning reads) without
        // a socket. Appends complete messages to `out`, keeps the remainder.
        static void frame (juce::MemoryBlock& buffer, std::vector<ConsoleMessage>& out)
        {
            const auto* d = static_cast<const juce::uint8*> (buffer.getData());
            const int   n = (int) buffer.getSize();
            int consumed = 0, i = 0;

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
            juce::uint8 runningStatus = 0;

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

            while (i < n)
            {
                const juce::uint8 s = d[i];

                if (s < 0x80)                                  // data byte
                {
                    if (runningStatus == 0) { ++i; continue; }  // genuinely stray -- no status yet
                    const int need = dataBytesFor (runningStatus);
                    if (need == 0) { ++i; continue; }
                    if (i + need > n) break;                    // incomplete -- wait for more
                    // Re-materialise the implied status byte so the dialect
                    // always sees a complete message.
                    juce::uint8 msg[3] = { runningStatus, 0, 0 };
                    for (int k = 0; k < need; ++k) msg[k + 1] = d[i + k];
                    emit (msg, need + 1);
                    i += need; consumed = i; continue;
                }

                if (s == 0xF0)                                // SysEx: run to 0xF7
                {
                    int e = i + 1;
                    while (e < n && d[e] != 0xF7) ++e;
                    if (e >= n) break;                        // incomplete -- wait for more
                    emit (d + i, e - i + 1);
                    i = e + 1; consumed = i;
                    runningStatus = 0;                        // SysEx cancels running status
                    continue;
                }

                const int need = dataBytesFor (s) + 1;
                if (i + need > n) break;                      // incomplete
                emit (d + i, need);
                // Channel messages arm running status; system messages clear it
                // (realtime bytes, which are 1 byte, deliberately do NOT --
                // but they also never reach here as a multi-byte message).
                runningStatus = (s < 0xF0) ? s : 0;
                i += need; consumed = i;
            }

            if (consumed > 0)
            {
                juce::MemoryBlock rest;
                if (n > consumed) rest.append (d + consumed, (size_t) (n - consumed));
                buffer = std::move (rest);
            }
        }

    private:
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

                std::vector<ConsoleMessage> msgs;
                frame (scratch, msgs);
                for (auto& m : msgs) deliverOnMessageThread (std::move (m));
                if (scratch.getSize() > (1u << 20)) break;
            }
            running.store (false);
        }

        std::unique_ptr<juce::StreamingSocket> socket;
        std::thread       reader;
        std::atomic<bool> running { false };
    };
}
