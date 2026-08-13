// Console transport + dialect tests.
//
// No console needed and none possible: these prove the PLUMBING (framing,
// tokenising, encode/parse round-trips, and the handshake gate) rather than
// the dialects themselves. Only the X32 address model is trusted; every other
// dialect here was written from published protocol conventions with no desk to
// try it on, which is exactly why ConsoleLink refuses to WRITE on an
// unconfirmed dialect. That refusal is the single most important thing in this
// file -- it is what makes shipping an unverified console family safe.

#include <juce_core/juce_core.h>

#include "../Network/ConsoleLink.h"
#include "../Network/ConsoleTransports.h"

namespace zynforge
{
    class ConsoleTransportTests final : public juce::UnitTest
    {
    public:
        ConsoleTransportTests() : juce::UnitTest ("Console transports + dialects", "zynforge") {}

        static ConsoleMessage msg (const juce::String& addr, std::vector<juce::var> a = {})
        { return ConsoleMessage (addr, std::move (a)); }

        void runTest() override
        {
            // ── SCP line tokenising (Yamaha) ────────────────────────────────
            beginTest ("SCP: a reply line tokenises into address + typed args");
            {
                auto m = TextLineTransport::parseLine (
                    "OK get MIXER:Current/InCh/Label/Name 3 0 \"KICK IN\"");
                expectEquals (m.address, juce::String ("MIXER:Current/InCh/Label/Name"),
                              "status verb + get must be stripped");
                expectEquals ((int) m.args.size(), 3);
                expectEquals (m.intArg (0), 3);
                expectEquals (m.stringArg (2), juce::String ("KICK IN"),
                              "a quoted name with a space must survive as ONE arg");

                // A pushed NOTIFY has the same shape.
                auto n = TextLineTransport::parseLine ("NOTIFY sscurrent_ex scene 1 12");
                expectEquals (n.address, juce::String ("scene"));
                expectEquals (n.intArg ((int) n.args.size() - 1), 12);

                expect (TextLineTransport::parseLine ("").isEmpty(), "blank line -> empty");
                expect (TextLineTransport::parseLine ("   ").isEmpty());
            }

            // ── MIDI framing (Allen & Heath) ────────────────────────────────
            beginTest ("MIDI/TCP: framing handles running bytes, SysEx and split reads");
            {
                juce::MemoryBlock buf;
                std::vector<ConsoleMessage> out;

                // Program change (2 bytes) + CC (3 bytes) in one read.
                const juce::uint8 a[] = { 0xC0, 0x07, 0xB0, 0x00, 0x01 };
                buf.append (a, sizeof (a));
                MidiTcpTransport::frame (buf, out);
                expectEquals ((int) out.size(), 2, "two complete messages");
                expectEquals (out[0].intArg (0), 0xC0);
                expectEquals (out[0].intArg (1), 0x07);
                expectEquals ((int) buf.getSize(), 0, "nothing left over");

                // A message split ACROSS reads must not be emitted early.
                out.clear();
                const juce::uint8 half[] = { 0x90, 0x40 };      // note-on missing velocity
                buf.append (half, sizeof (half));
                MidiTcpTransport::frame (buf, out);
                expect (out.empty(), "an incomplete message must be held back");
                expectEquals ((int) buf.getSize(), 2, "and kept in the buffer");
                const juce::uint8 rest[] = { 0x7F };
                buf.append (rest, sizeof (rest));
                MidiTcpTransport::frame (buf, out);
                expectEquals ((int) out.size(), 1, "completed on the next read");
                expectEquals (out[0].intArg (2), 0x7F);

                // SysEx spanning a read boundary.
                out.clear(); buf.reset();
                const juce::uint8 sx1[] = { 0xF0, 0x00, 0x00, 0x1A };
                buf.append (sx1, sizeof (sx1));
                MidiTcpTransport::frame (buf, out);
                expect (out.empty(), "unterminated SysEx must be held back");
                const juce::uint8 sx2[] = { 0x50, 0x10, 0xF7 };
                buf.append (sx2, sizeof (sx2));
                MidiTcpTransport::frame (buf, out);
                expectEquals ((int) out.size(), 1, "SysEx completes across reads");
                expectEquals (out[0].intArg (0), 0xF0);
                expectEquals (out[0].intArg (6), 0xF7);
            }

            // ── Dialect parse: the READ tier, per family ────────────────────
            beginTest ("Dialects parse channel names + scene recalls");
            {
                {   // X32
                    auto d = x32Dialect();
                    auto e = d.parse (msg ("/ch/04/config/name", { juce::String ("SNARE") }));
                    expect (e.type == ConsoleEvent::Type::ChannelName);
                    expectEquals (e.index, 4);
                    expectEquals (e.text, juce::String ("SNARE"));

                    auto s = d.parse (msg ("/-show/prepos/current", { 7 }));
                    expect (s.type == ConsoleEvent::Type::SceneRecalled);
                    expectEquals (s.index, 7);

                    auto g = d.parse (msg ("/headamp/005/gain", { 0.25 }));
                    expect (g.type == ConsoleEvent::Type::HeadAmpGain);
                    expectEquals (g.index, 5);
                    expectWithinAbsoluteError (g.value, 0.25f, 0.0001f);

                    auto h = d.parse (msg ("/info", { juce::String ("X32") }));
                    expect (h.type == ConsoleEvent::Type::HandshakeOk);

                    expect (d.parse (msg ("/nonsense")).type == ConsoleEvent::Type::None);
                }
                {   // WING
                    auto d = wingDialect();
                    auto e = d.parse (msg ("/ch/12/$name", { juce::String ("GTR L") }));
                    expect (e.type == ConsoleEvent::Type::ChannelName);
                    expectEquals (e.index, 12);
                    expectEquals (e.text, juce::String ("GTR L"));
                }
                {   // DiGiCo
                    auto d = digicoDialect();
                    auto e = d.parse (msg ("/Console/Channels/9/name", { juce::String ("VOX") }));
                    expect (e.type == ConsoleEvent::Type::ChannelName);
                    expectEquals (e.index, 9);
                    auto s = d.parse (msg ("/Console/Snapshots/recall", { 3, juce::String ("Song 3") }));
                    expect (s.type == ConsoleEvent::Type::SceneRecalled);
                    expectEquals (s.index, 3);
                    expectEquals (s.text, juce::String ("Song 3"));
                }
                {   // Yamaha, fed through the REAL line tokeniser
                    auto d = yamahaScpDialect();
                    auto e = d.parse (TextLineTransport::parseLine (
                        "OK get MIXER:Current/InCh/Label/Name 5 0 \"BASS DI\""));
                    expect (e.type == ConsoleEvent::Type::ChannelName);
                    expectEquals (e.index, 6, "SCP channels are 0-based on the wire, 1-based to us");
                    expectEquals (e.text, juce::String ("BASS DI"));
                }
                {   // Allen & Heath: scene recall as a MIDI program change
                    auto d = allenHeathMidiDialect();
                    auto e = d.parse (msg ("midi", { 0xC0, 0x05 }));
                    expect (e.type == ConsoleEvent::Type::SceneRecalled);
                    expectEquals (e.index, 6, "PC is 0-based; scenes read 1-based");

                    auto h = d.parse (msg ("midi", { 0xF0, 0x00, 0x00, 0x1A, 0x50, 0xF7 }));
                    expect (h.type == ConsoleEvent::Type::HandshakeOk,
                            "the A&H SysEx enquiry reply confirms the dialect");
                }
            }

            // ── THE SAFETY PROPERTY ────────────────────────────────────────
            // An untrusted dialect must not be able to write to a desk until
            // the console has identified itself. This is what lets a dialect
            // written from documentation ship at all.
            beginTest ("An unconfirmed dialect is READ-ONLY -- no writes reach the desk");
            {
                ConsoleLink link;
                link.setProfile (ConsoleProfile::Kind::DiGiCo);
                expect (! link.getProfile().dialectTrusted, "test premise: DiGiCo is untrusted");

                int sent = 0;
                juce::String lastStatus;
                link.onStatus = [&lastStatus] (const juce::String& s) { lastStatus = s; };
                link.setMessageHook ([&sent] (const ConsoleMessage&) { ++sent; });

                expect (! link.canWrite(), "must not be writable before the handshake");
                link.captureGains (8);
                link.enterSoundcheck();
                link.restoreGains();
                expectEquals (sent, 0, "NO message may reach an unconfirmed desk");
                expect (lastStatus.isNotEmpty(), "and the refusal must be explained, not silent");

                // The handshake flips it.
                link.confirmDialectForTests();
                expect (link.canWrite(), "a confirmed dialect unlocks the write tier");
            }

            beginTest ("A trusted dialect (X32) writes without waiting for a handshake");
            {
                ConsoleLink link;   // defaults to the X32 reference profile
                expect (link.getProfile().dialectTrusted);
                int sent = 0;
                link.setMessageHook ([&sent] (const ConsoleMessage&) { ++sent; });
                expect (link.canWrite(), "the reference dialect must not regress to gated");
                link.enterSoundcheck();
                expectEquals (sent, 4, "still queries all four IN blocks immediately");
            }

            // ── The read tier reaches the host ─────────────────────────────
            beginTest ("Channel names + scene recalls surface to the host");
            {
                ConsoleLink link;
                link.setMessageHook ([] (const ConsoleMessage&) {});

                int gotCh = -1, gotScene = -1;
                juce::String gotName;
                link.onChannelName   = [&] (int c, const juce::String& n) { gotCh = c; gotName = n; };
                link.onSceneRecalled = [&] (int s, const juce::String&)   { gotScene = s; };

                link.injectReply (msg ("/ch/07/config/name", { juce::String ("HAT") }));
                expectEquals (gotCh, 7);
                expectEquals (gotName, juce::String ("HAT"));

                link.injectReply (msg ("/-show/prepos/current", { 11 }));
                expectEquals (gotScene, 11);
            }

            beginTest ("Every profile carries a transport, a dialect and honest capabilities");
            {
                for (const auto& p : consoleProfiles())
                {
                    expect (p.displayName.isNotEmpty(), "profile with no name");
                    expect (p.defaultPort > 0, "profile with no port: " + p.displayName);
                    expect (p.dialect.parse != nullptr,
                            "every dialect must at least parse (even passively): " + p.displayName);
                    // The core promise: nothing may claim a WRITE capability
                    // unless its dialect can actually encode that write.
                    if (p.canRepatch)
                        expect (p.dialect.queryInBlock && p.dialect.setInBlock,
                                p.displayName + " claims repatch with no encoder");
                    if (p.canCaptureGains)
                        expect (p.dialect.queryHeadAmp && p.dialect.setHeadAmp,
                                p.displayName + " claims gain capture with no encoder");
                    if (p.canReadNames)
                        expect (p.dialect.queryChannelName != nullptr,
                                p.displayName + " claims name reads with no encoder");
                }
            }
        }
    };

    static ConsoleTransportTests consoleTransportTests;
}
