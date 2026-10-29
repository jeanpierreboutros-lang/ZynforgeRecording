// Regressions from the Network-folder audit (2026-08-14).
//
// The headline one is a process abort: a console dropping its TCP link left a
// finished-but-unjoined reader thread, and destroying a joinable std::thread
// calls std::terminate(). The rest are protocol/state-machine defects that all
// end the same way -- a desk or a peer left in a state the engineer has to
// discover for themselves.

#include <juce_core/juce_core.h>

#include "../Network/ConsoleLink.h"
#include "../Network/ConsoleTransports.h"

#include <cmath>
#include <limits>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    class NetworkAuditTests final : public juce::UnitTest
    {
    public:
        NetworkAuditTests() : juce::UnitTest ("Network audit fixes", "zynforge") {}

        void runTest() override
        {
            beginTest ("A transport whose peer vanished still joins its reader (no std::terminate)");
            {
                // THE BUG: readLoop clears `running` itself when the peer closes,
                // and disconnect() early-returned on `! running.exchange(false)`
                // -- skipping the join. The destructor then destroyed a joinable
                // std::thread, which aborts the process. Reproduced here by
                // connecting to a listener and killing it from under the client.
                juce::StreamingSocket server;
                const int port = 47921;
                expect (server.createListener (port, "127.0.0.1"), "test listener failed");

                {
                    TextLineTransport t;
                    expect (t.connect ("127.0.0.1", port), "connect failed");
                    std::unique_ptr<juce::StreamingSocket> accepted (server.waitForNextConnection());
                    expect (accepted != nullptr);

                    accepted->close();          // peer vanishes -> readLoop exits, running=false
                    server.close();
                    juce::Thread::sleep (400);  // let the reader notice and return
                    expect (! t.isConnected(), "test premise: the reader has already exited");

                    // Pre-fix this destructor called std::terminate(). Reaching
                    // the next line at all IS the assertion.
                    t.disconnect();
                }
                expect (true, "survived transport teardown after a peer drop");
            }

            beginTest ("A duplicate routing reply cannot complete the patch stash early");
            {
                // THE BUG: completion counted DECREMENTS, not distinct blocks. We
                // subscribe to pushed updates on connect, so one unsolicited
                // routing message during the read window finished the count with
                // an incomplete stash: the desk flipped to card anyway and
                // exitSoundcheck then refused, stranding it on card returns.
                ConsoleLink link;
                std::vector<juce::String> sent;
                link.setSendHook ([&sent] (const juce::OSCMessage& m)
                { sent.push_back (m.getAddressPattern().toString()); });

                link.enterSoundcheck();
                expectEquals ((int) sent.size(), 4, "four block queries");

                // Three real replies + the FIRST block echoed a second time.
                const char* addr[] = { "/config/routing/IN/1-8",   "/config/routing/IN/9-16",
                                       "/config/routing/IN/17-24", "/config/routing/IN/25-32" };
                for (int b = 0; b < 3; ++b)
                    link.injectReply (juce::OSCMessage (juce::OSCAddressPattern (addr[b]),
                                                        (juce::int32) (4 + b)));
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern (addr[0]),
                                                    (juce::int32) 4));   // duplicate

                expect (link.getPatch() != ConsoleLink::Patch::Soundcheck,
                        "a duplicate must NOT complete the stash -- block 4 is still missing");
                expectEquals ((int) sent.size(), 4, "and must not trigger the flip-to-card writes");

                // The genuinely missing block completes it.
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern (addr[3]),
                                                    (juce::int32) 7));
                expect (link.getPatch() == ConsoleLink::Patch::Soundcheck);
                expectEquals ((int) sent.size(), 8, "now the four card writes go out");

                // And the stash is complete, so the show patch can be restored.
                link.exitSoundcheck();
                expectEquals ((int) sent.size(), 12, "restore wrote all four blocks");
                expect (link.getPatch() == ConsoleLink::Patch::Stage);
            }

            beginTest ("MIDI framing handles running status and system-common messages");
            {
                juce::MemoryBlock buf;
                std::vector<ConsoleMessage> out;

                // CC on channel 1, then TWO running-status continuations. These
                // were being discarded as stray data bytes -- and NRPN sweeps,
                // which is most of what A&H sends, are exactly this shape.
                const juce::uint8 rs[] = { 0xB0, 0x63, 0x01, 0x62, 0x02, 0x06, 0x7F };
                buf.append (rs, sizeof (rs));
                MidiTcpTransport::frame (buf, out);
                expectEquals ((int) out.size(), 3, "running status must yield three CCs");
                for (const auto& m : out)
                    expectEquals (m.intArg (0), 0xB0, "the implied status byte is re-materialised");
                expectEquals (out[1].intArg (1), 0x62);
                expectEquals (out[2].intArg (2), 0x7F);

                // Song position (0xF2) carries two data bytes; sizing it at zero
                // desynced everything after it.
                out.clear(); buf.reset();
                const juce::uint8 sc[] = { 0xF2, 0x10, 0x20, 0xC0, 0x05 };
                buf.append (sc, sizeof (sc));
                MidiTcpTransport::frame (buf, out);
                expectEquals ((int) out.size(), 2, "F2 must consume its two data bytes");
                expectEquals (out[1].intArg (0), 0xC0, "the program change survives intact");
                expectEquals (out[1].intArg (1), 0x05);

                // Running status persists across TCP reads, and an interleaved
                // realtime clock byte does not cancel it.
                out.clear(); buf.reset();
                juce::uint8 status = 0;
                const juce::uint8 first[] = { 0xB0, 0x10, 0x20 };
                buf.append (first, sizeof (first));
                MidiTcpTransport::frame (buf, out, status);
                const juce::uint8 second[] = { 0xF8, 0x11, 0x21 };
                buf.append (second, sizeof (second));
                MidiTcpTransport::frame (buf, out, status);
                expectEquals ((int) out.size(), 3);
                expectEquals (out[1].intArg (0), 0xF8);
                expectEquals (out[2].intArg (0), 0xB0);
                expectEquals (out[2].intArg (1), 0x11);
            }

            beginTest ("An SCP string argument with a quote in it can't corrupt the line");
            {
                // Round-trip through the real tokeniser: an unescaped quote used
                // to close the token early and shift every later argument.
                auto m = TextLineTransport::parseLine (
                    "OK get MIXER:Current/InCh/Label/Name 3 0 \"KICK \\\"IN\\\"\"");
                expectEquals (m.intArg (0), 3, "the numeric args must still line up");
                expectEquals ((int) m.args.size(), 3);
            }

            beginTest ("A console TCP link that drops is noticed, not reported as up");
            {
                // The SCP / MIDI-over-TCP reader exits when the desk goes away,
                // but ConsoleLink never asked the transport: the menu kept
                // offering "disconnect <host>" while names and scene markers
                // had silently stopped for the rest of the show.
                int port = 47940;
                for (const auto kind : { ConsoleProfile::Kind::Yamaha, ConsoleProfile::Kind::AllenHeath })
                {
                    juce::StreamingSocket server;
                    while (port < 47950 && ! server.createListener (port, "127.0.0.1")) ++port;
                    expect (port < 47950, "test listener failed");

                    ConsoleLink link;
                    link.setProfile (kind);
                    juce::StringArray statuses;
                    link.onStatus = [&] (const juce::String& s) { statuses.add (s); };
                    expect (link.connect ("127.0.0.1", port), "connect failed");
                    std::unique_ptr<juce::StreamingSocket> accepted (server.waitForNextConnection());
                    expect (accepted != nullptr);
                    if (accepted != nullptr) accepted->close();   // the desk goes away
                    server.close();
                    ++port;

                    pumpUntil ([&] { return ! link.isConnected(); }, 3000);
                    expect (! link.isConnected(), link.getProfile().displayName
                                                  + ": a dropped link still reports connected");
                    expect (statuses.joinIntoString ("\n").containsIgnoreCase ("lost"),
                            "the engineer was not told the console link dropped");
                }
            }

            beginTest ("Head-amp gains from the wire are finite and inside the 0..1 wire range");
            {
                // The comment in loadJson said "the wire path also clamps"; it
                // did not. A NaN or out-of-range reply was stored and later sent
                // back to the desk verbatim by restoreGains.
                ConsoleLink link;   // X32: the only profile with head-amp capture
                std::vector<ConsoleMessage> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.captureGains (2);
                link.injectReply (ConsoleMessage ("/headamp/000/gain",
                                                  { std::numeric_limits<double>::quiet_NaN() }));
                expectEquals ((int) link.getCapturedGains().count (0), 0, "a NaN head-amp reply was stored");
                link.injectReply (ConsoleMessage ("/headamp/000/gain", { 7.5 }));
                link.injectReply (ConsoleMessage ("/headamp/001/gain", { -3.0 }));
                expect (link.isGainCaptureComplete());
                const auto& gains = link.getCapturedGains();
                if (gains.count (0) == 1) expectEquals (gains.at (0), 1.0f);
                if (gains.count (1) == 1) expectEquals (gains.at (1), 0.0f);

                sent.clear();
                link.restoreGains();
                expectEquals ((int) sent.size(), 2);
                for (const auto& m : sent)
                {
                    const float v = m.floatArg();
                    expect (std::isfinite (v) && v >= 0.0f && v <= 1.0f,
                            m.address + " restored an out-of-range gain to the desk");
                }
            }
        }

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
               #else
                juce::Thread::sleep (5);
               #endif
            }
            return predicate();
        }
    };

    static NetworkAuditTests networkAuditTests;
}
