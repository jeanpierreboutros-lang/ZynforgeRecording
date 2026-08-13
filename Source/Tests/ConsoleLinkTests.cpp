// Headless tests for the X32/M32 console link (Source/Network/ConsoleLink.h).
// No console needed: setSendHook() intercepts every outgoing OSC message
// (exact addresses + typed args asserted against the public X32 protocol)
// and injectReply() plays the console's side of each exchange.

#include <juce_osc/juce_osc.h>

#include "../Network/ConsoleLink.h"

namespace zynforge
{
    class ConsoleLinkTests final : public juce::UnitTest
    {
    public:
        ConsoleLinkTests() : juce::UnitTest ("Console link (X32 OSC)", "zynforge") {}

        struct Sent
        {
            juce::String addr;
            std::vector<juce::OSCArgument> args;
        };

        // A size assertion that FAILS the test instead of letting the next
        // line index an empty vector. A regression that changed the message
        // count used to segfault the whole binary here, which aborted the
        // suite and hid every test after it -- a failing assertion has to stay
        // a failing assertion.
        template <typename T>
        bool haveAtLeast (const std::vector<T>& v, int n, const juce::String& what)
        {
            if ((int) v.size() >= n) return true;
            expect (false, what + ": expected >= " + juce::String (n)
                           + " messages, got " + juce::String ((int) v.size()));
            return false;
        }

        // Same idea for arg indexing: check before you dereference.
        template <typename Msg>
        bool expectAtLeastOneFloatArg (const Msg& m)
        {
            const bool ok = m.args.size() >= 1 && m.args[0].isFloat32();
            expect (ok, "expected one float arg");
            return ok;
        }
        template <typename Msg>
        bool expectAtLeastOneIntArg (const Msg& m)
        {
            const bool ok = m.args.size() >= 1 && m.args[0].isInt32();
            expect (ok, "expected one int arg");
            return ok;
        }

        void runTest() override
        {
            beginTest ("enterSoundcheck queries all four IN blocks, then flips to CARD");
            {
                ConsoleLink link;
                std::vector<Sent> sent;
                link.setSendHook ([&sent] (const juce::OSCMessage& m)
                {
                    Sent s; s.addr = m.getAddressPattern().toString();
                    for (const auto& a : m) s.args.push_back (a);
                    sent.push_back (std::move (s));
                });

                link.enterSoundcheck();
                // Phase 1: four argument-less queries.
                expectEquals ((int) sent.size(), 4);
                if (haveAtLeast (sent, 4, "enterSoundcheck queries"))
                {
                expectEquals (sent[0].addr, juce::String ("/config/routing/IN/1-8"));
                expectEquals (sent[1].addr, juce::String ("/config/routing/IN/9-16"));
                expectEquals (sent[2].addr, juce::String ("/config/routing/IN/17-24"));
                expectEquals (sent[3].addr, juce::String ("/config/routing/IN/25-32"));
                }
                for (const auto& s : sent)
                    expect (s.args.empty(), "query must carry no arguments");

                // Console replies: an AES50-A show patch (blocks 4..7),
                // NOT analog -- the stash must keep it verbatim.
                for (int b = 0; b < 4; ++b)
                {
                    juce::OSCMessage reply (juce::OSCAddressPattern (sent[(size_t) b].addr),
                                            (juce::int32) (4 + b));
                    link.injectReply (reply);
                }

                // Phase 2: four CARD sets (enum 16..19), typed int32.
                expectEquals ((int) sent.size(), 8);
                for (int b = 0; b < 4; ++b)
                {
                    const auto& s = sent[(size_t) (4 + b)];
                    expectEquals (s.addr, sent[(size_t) b].addr);
                    expect (s.args.size() == 1 && s.args[0].isInt32());
                    expectEquals ((int) s.args[0].getInt32(), 16 + b);
                }
                expect (link.getPatch() == ConsoleLink::Patch::Soundcheck);

                // Back to stage: restores the AES50 patch, not analog.
                link.exitSoundcheck();
                expectEquals ((int) sent.size(), 12);
                for (int b = 0; b < 4; ++b)
                {
                    const auto& s = sent[(size_t) (8 + b)];
                    expect (s.args.size() == 1 && s.args[0].isInt32());
                    expectEquals ((int) s.args[0].getInt32(), 4 + b);
                }
                expect (link.getPatch() == ConsoleLink::Patch::Stage);
            }

            beginTest ("exitSoundcheck without a stash refuses (never guesses a patch)");
            {
                ConsoleLink link;
                int sentCount = 0;
                link.setSendHook ([&sentCount] (const juce::OSCMessage&) { ++sentCount; });
                link.exitSoundcheck();
                expectEquals (sentCount, 0);
            }

            beginTest ("captureGains polls /headamp/NNN/gain and collects float replies");
            {
                ConsoleLink link;
                std::vector<Sent> sent;
                link.setSendHook ([&sent] (const juce::OSCMessage& m)
                {
                    Sent s; s.addr = m.getAddressPattern().toString();
                    for (const auto& a : m) s.args.push_back (a);
                    sent.push_back (std::move (s));
                });

                link.captureGains (3);
                expectEquals ((int) sent.size(), 3);
                if (haveAtLeast (sent, 3, "captureGains polls"))
                {
                    expectEquals (sent[0].addr, juce::String ("/headamp/000/gain"));
                    expectEquals (sent[1].addr, juce::String ("/headamp/001/gain"));
                    expectEquals (sent[2].addr, juce::String ("/headamp/002/gain"));
                }

                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/000/gain"), 0.5f));
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/001/gain"), 0.0f));
                link.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/002/gain"), 1.0f));

                const auto& g = link.getCapturedGains();
                expectEquals ((int) g.size(), 3);
                expectWithinAbsoluteError (g.at (0), 0.5f, 1.0e-6f);
                // Raw 0..1 maps -12..+60 dB (via the active X32 profile).
                expectWithinAbsoluteError (link.gainToDb (g.at (0)), 24.0f, 1.0e-4f);
                expectWithinAbsoluteError (link.gainToDb (g.at (1)), -12.0f, 1.0e-4f);
                expectWithinAbsoluteError (link.gainToDb (g.at (2)), 60.0f, 1.0e-4f);

                // Restore writes the same floats back to the same heads.
                sent.clear();
                link.restoreGains();
                expectEquals ((int) sent.size(), 3);
                if (haveAtLeast (sent, 1, "restoreGains writes")
                    && expectAtLeastOneFloatArg (sent[0]))
                    expectWithinAbsoluteError (sent[0].args[0].getFloat32(), 0.5f, 1.0e-6f);
            }

            beginTest ("connect -> disconnect -> reconnect rebinds a fresh socket");
            {
                // Regression: juce::DatagramSocket::shutdown() invalidates the
                // handle permanently, so a reused socket can't re-bind. A
                // fresh socket per connect() must let reconnect succeed.
                ConsoleLink link;          // no send hook -> real socket path
                expect (link.connect ("127.0.0.1", 10023), "first connect failed");
                expect (link.isConnected());
                link.disconnect();
                expect (! link.isConnected());
                expect (link.connect ("127.0.0.1", 10023), "RECONNECT failed -- socket not rebound");
                expect (link.isConnected());
                link.disconnect();
            }

            beginTest ("stashed patch + gains round-trip through the session JSON");
            {
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-console-" + juce::Uuid().toString());
                dir.createDirectory();

                ConsoleLink a;
                a.setSendHook ([] (const juce::OSCMessage&) {});
                a.enterSoundcheck();
                for (int b = 0; b < 4; ++b)
                    a.injectReply (juce::OSCMessage (
                        juce::OSCAddressPattern (juce::String (b == 0 ? "/config/routing/IN/1-8"
                                                : b == 1 ? "/config/routing/IN/9-16"
                                                : b == 2 ? "/config/routing/IN/17-24"
                                                         : "/config/routing/IN/25-32")),
                        (juce::int32) (10 + b)));                      // AES50-B patch
                a.injectReply (juce::OSCMessage (juce::OSCAddressPattern ("/headamp/004/gain"), 0.25f));
                expect (a.saveTo (dir));
                expect (dir.getChildFile (ConsoleLink::kStateFileName).existsAsFile());

                ConsoleLink b;
                b.setSendHook ([] (const juce::OSCMessage&) {});
                expect (b.loadFrom (dir));
                expectEquals ((int) b.getCapturedGains().size(), 1);
                expectWithinAbsoluteError (b.getCapturedGains().at (4), 0.25f, 1.0e-6f);

                // The reloaded link can restore the stage patch it never
                // queried itself -- show-night state survives to VSC day.
                std::vector<Sent> sent;
                b.setSendHook ([&sent] (const juce::OSCMessage& m)
                {
                    Sent s; s.addr = m.getAddressPattern().toString();
                    for (const auto& arg : m) s.args.push_back (arg);
                    sent.push_back (std::move (s));
                });
                b.exitSoundcheck();
                expectEquals ((int) sent.size(), 4);
                if (haveAtLeast (sent, 1, "exitSoundcheck restores")
                    && expectAtLeastOneIntArg (sent[0]))
                    expectEquals ((int) sent[0].args[0].getInt32(), 10);

                dir.deleteRecursively();
            }

            beginTest ("profile selection drives capabilities + a native-VSC desk refuses OSC repatch");
            {
                ConsoleLink link;
                // Default is the X32 reference profile: full OSC control.
                expect (link.getProfile().kind == ConsoleProfile::Kind::BehringerX32);
                expect (link.getProfile().canRepatch);
                expect (link.getProfile().canCaptureGains);

                // A large-format desk: native VSC, no OSC repatch/gain.
                link.setProfile (ConsoleProfile::Kind::DiGiCo);
                expect (link.getProfile().kind == ConsoleProfile::Kind::DiGiCo);
                expect (! link.getProfile().canRepatch);
                expect (link.getProfile().hasNativeVsc);

                int sent = 0;
                link.setSendHook ([&sent] (const juce::OSCMessage&) { ++sent; });
                link.enterSoundcheck();      // must NOT touch the wire
                link.captureGains (8);       // must NOT touch the wire
                expectEquals (sent, 0, "native-VSC profile sent OSC it shouldn't");

                // The X32 profile uses port 10023; the catalogue has 6 entries.
                expectEquals (consoleProfileFor (ConsoleProfile::Kind::BehringerX32).defaultPort, 10023);
                expect ((int) consoleProfiles().size() >= 5);
            }
        }
    };

    static ConsoleLinkTests consoleLinkTests;
}
