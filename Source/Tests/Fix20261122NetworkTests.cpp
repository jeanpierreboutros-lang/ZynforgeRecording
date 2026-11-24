// Desk macros, the capture helper and the console link: the fixes from the
// 2026-11-22 audit. Each test names what it guards and fails on the code from
// before the fix (the transport ones by hanging or by an unbounded count).
//
//   1. A desk macro STOP needs a value; a key that sends the address alone on
//      press and on release acts once.
//   2. A capture helper of another build that is busy at attach is kept, named
//      and refused for a take, and replaced once it is idle.
//   3. The Yamaha SCP and A&H MIDI/TCP transports queue inbound messages in a
//      bounded backlog, and their writes can't hold the message thread.
//   4. A helper START that can't hold the take's claim refuses.
//   5. A console patch save during a take says pending until it has happened,
//      and the soundcheck list follows what was written.

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include "../Audio/AudioEngine.h"
#include "../Capture/CaptureDaemon.h"
#include "../Capture/CaptureSupervisor.h"
#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureLink.h"
#include "../Network/ConsoleLink.h"
#include "../Network/ConsoleTransports.h"
#include "../Network/DeskMacro.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <vector>

// ThreadSanitizer slows every send several times over; the hold limit below is
// for the real app, so under TSan it gets three times the room.
#if defined (__has_feature)
 #if __has_feature (thread_sanitizer)
  #define ZF_NETFIX22_UNDER_TSAN 1
 #endif
#endif
#ifndef ZF_NETFIX22_UNDER_TSAN
 #define ZF_NETFIX22_UNDER_TSAN 0
#endif

namespace zynforge
{
    namespace
    {
        constexpr double kRate  = 48000.0;
        constexpr int    kBlock = 256;

        // The message thread runs what it was posted only while this pumps.
        bool pumpUntil (const std::function<bool()>& done, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return done();
        }

        // Waits WITHOUT running the message thread: what is posted stays posted.
        bool waitWithoutPumping (const std::function<bool()>& done, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done())
            {
                if (juce::Time::getMillisecondCounterHiRes() > end) return false;
                juce::Thread::sleep (2);
            }
            return true;
        }

        juce::File scratchDir (const juce::String& name)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-fix1122-" + name + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool startOnSomePort (capture::CaptureDaemon& daemon, int inputs = 1)
        {
            juce::Random random;
            for (int attempt = 0; attempt < 20; ++attempt)
                if (daemon.start (50000 + random.nextInt (9000), inputs)) return true;
            return false;
        }

        bool writeAllBytes (juce::StreamingSocket& socket, const void* data, size_t size)
        {
            const auto* bytes = static_cast<const char*> (data);
            size_t sent = 0;
            while (sent < size)
            {
                const int n = socket.write (bytes + sent, (int) (size - sent));
                if (n <= 0) return false;
                sent += (size_t) n;
            }
            return true;
        }

        // Every remote transport a macro asks the host for, counted.
        struct TransportSpy
        {
            std::vector<AudioEngine::RemoteTransportAction> actions;
            void attach (AudioEngine& engine)
            {
                engine.setRemoteTransportHandler ([this] (auto action, juce::String&) -> std::optional<bool>
                {
                    actions.push_back (action);
                    return true;
                });
            }
            int count (AudioEngine::RemoteTransportAction which) const
            {
                int n = 0;
                for (auto a : actions) n += a == which ? 1 : 0;
                return n;
            }
        };

        const char* const kInBlocks[] { "/config/routing/IN/1-8",  "/config/routing/IN/9-16",
                                        "/config/routing/IN/17-24", "/config/routing/IN/25-32" };

        // An X32 as the link hears one: answers routing queries, takes writes.
        struct FakeDesk
        {
            std::vector<int> routing { 4, 5, 6, 7 };
            ConsoleLink* link { nullptr };
            std::vector<ConsoleMessage> replies;

            void attach (ConsoleLink& l)
            {
                link = &l;
                l.setMessageHook ([this] (const ConsoleMessage& m) { heard (m); });
            }

            void heard (const ConsoleMessage& m)
            {
                for (int b = 0; b < 4; ++b)
                {
                    if (m.address != kInBlocks[b]) continue;
                    if (m.hasArgs()) routing[(size_t) b] = m.intArg();
                    else replies.push_back (ConsoleMessage (m.address, { routing[(size_t) b] }));
                }
            }

            void answer()
            {
                auto pending = std::move (replies);
                replies.clear();
                for (const auto& r : pending) link->injectReply (r);
            }
        };

        juce::String fileDeskPatch (const juce::File& dir)
        {
            return juce::JSON::parse (dir.getChildFile (ConsoleLink::kStateFileName))
                       .getProperty ("deskPatch", {}).toString();
        }
    }

    class Fix20261122NetworkTests final : public juce::UnitTest
    {
    public:
        Fix20261122NetworkTests() : juce::UnitTest ("Fixes 2026-11-22 network", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            aDeskMacroStopNeedsAValue();                        // 1
            aValuelessKeyActsOncePerPress();                    // 1
            aBusyHelperOfAnotherBuildIsNamedThenReplaced();     // 2
            aConsoleFloodIsCappedAndCounted();                  // 3
            aDeskThatStopsReadingCantHoldTheMessageThread();    // 3
            aTakeThatCantBeClaimedDoesNotRoll();                // 4
            aPatchSaveDuringATakeIsPendingUntilWritten();       // 5
        }

        // ── 1. Desk macro STOP ─────────────────────────────────────────────

        void aDeskMacroStopNeedsAValue()
        {
            using A = AudioEngine::RemoteTransportAction;

            beginTest ("Desk macro: a STOP with no value is refused, and says to send 1 on press");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                juce::StringArray shown;
                engine.getRemotes().setHost ({ [&shown] (const juce::String& m) { shown.add (m); }, {}, {} });
                DeskMacroListener macro (engine);
                // A key that sends the address alone, pressed, held, released, pressed again.
                for (double t : { 0.0, 600.0, 1200.0, 1900.0, 4000.0 })
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), t);
                expect (spy.actions.empty(), "a valueless STOP reached the host");
                const auto text = shown.joinIntoString (" | ");
                expect (text.contains ("needs a value") && text.contains ("1 on press"), text);
                expectEquals (shown.size(), 3, "the refusal is said at most once a second: " + text);
                engine.getRemotes().setHost ({});
                engine.setRemoteTransportHandler ({});
            }

            beginTest ("Desk macro: a STOP press and its release are one tap, however long the key is held");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                // The two-tap guard ends a take on a second StopAll inside 2 s:
                // one press and its release must never be two.
                double t = 10000.0;
                for (double held : { 100.0, 300.0, 600.0, 1500.0, 1999.0 })
                {
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", (juce::int32) 1), t);
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", (juce::int32) 0), t + held);
                    t += 5000.0;
                }
                expectEquals (spy.count (A::StopAll), 5, "a press and its release were not one tap each");
                expectEquals ((int) spy.actions.size(), 5);

                // The address alone on press and release, held 0.25-2 s: the
                // old path's take-ending case. Nothing reaches the host at all.
                spy.actions.clear();
                for (double held : { 300.0, 1200.0, 1900.0 })
                {
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), t);
                    macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop"), t + held);
                    t += 5000.0;
                }
                expect (spy.actions.empty(), "a valueless press and release still reached the host");

                // Two deliberate presses still make the two taps.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", (juce::int32) 1), t);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", (juce::int32) 1), t + 600.0);
                expectEquals (spy.count (A::StopAll), 2, "two presses were not two taps");
                engine.setRemoteTransportHandler ({});
            }
        }

        void aValuelessKeyActsOncePerPress()
        {
            using A = AudioEngine::RemoteTransportAction;

            beginTest ("Desk macro: a valueless MARKER press and its release drop one marker");
            {
                const auto dir = scratchDir ("macro-marker");
                AudioEngine engine;
                engine.getMarkers().setContext (dir, kRate);
                engine.setEditCursorSample (48000);
                DeskMacroListener macro (engine);
                const juce::OSCMessage marker ("/zynforge/macro/marker");

                macro.dispatchForTest (marker, 10000.0);    // press
                macro.dispatchForTest (marker, 10600.0);    // its release, held 0.6 s
                expectEquals (engine.getMarkers().getCount(), 1, "one press dropped two markers");
                macro.dispatchForTest (marker, 11000.0);    // the next press
                macro.dispatchForTest (marker, 11120.0);    // a quick release (inside the debounce)
                expectEquals (engine.getMarkers().getCount(), 2, "the next press was lost, or doubled");
                macro.dispatchForTest (marker, 11500.0);    // and another
                expectEquals (engine.getMarkers().getCount(), 3, "a press after a quick release was lost");

                // A key that sends values: 1 on press, 0 on release.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker", (juce::int32) 1), 20000.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker", (juce::int32) 0), 20700.0);
                expectEquals (engine.getMarkers().getCount(), 4);
                engine.getMarkers().clearContext();
                dir.deleteRecursively();
            }

            beginTest ("Desk macro: a valueless RECORD, PLAY or PAUSE press and its release act once");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record"), 0.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record"), 800.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 5000.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 5400.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/pause"), 9000.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/pause"), 10500.0);
                expectEquals (spy.count (A::StartRecord), 1);
                expectEquals (spy.count (A::StartPlay), 1);
                expectEquals (spy.count (A::StopPlay), 1);
                // Past the window the same address is a press again.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 20000.0);
                expectEquals (spy.count (A::StartPlay), 2, "a press long after the last one was taken as a release");
                engine.setRemoteTransportHandler ({});
            }
        }

        // ── 2. A busy helper of another build ──────────────────────────────

        void aBusyHelperOfAnotherBuildIsNamedThenReplaced()
        {
            using namespace capture;

            beginTest ("Helper: one of another build that is busy at attach is named, refused for a take, then replaced");
            {
                const auto binary = CaptureSupervisor::discoverBinary();
                expect (binary.existsAsFile(), "no helper binary next to the tests");
                if (! binary.existsAsFile()) return;

                CaptureDaemon older;
                older.setTestModeNoDevice (true);
                older.setBuildId ("an-older-build");
                // Quit stops this helper as the real one exits: from another
                // thread, never the reader the Quit arrived on.
                juce::WaitableEvent quitEvent, stopHeld, releaseStop;
                std::atomic<bool> quitAsked { false };
                std::thread stopper ([&] { quitEvent.wait (60000); older.stop(); });
                const juce::ScopeGuard joinStopper { [&] { releaseStop.signal(); quitEvent.signal(); stopper.join(); } };
                older.onQuitRequest = [&] { quitAsked = true; quitEvent.signal(); };
                expect (startOnSomePort (older));
                const int port = older.getPort();
                older.prepareForTests (kRate, kBlock, 1);
                older.getRecorder().getTrack (0).armed.store (true);
                const auto session = scratchDir ("older-busy");
                const juce::ScopeGuard removeSession { [&] { session.deleteRecursively(); } };

                // A take, and its STOP held inside the helper as a slow drive
                // holds it; the window that sent it is gone.
                CaptureClient first;
                expect (first.connect ("127.0.0.1", port) && first.hello (2000).ok);
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                expect (first.request (start, 3000).ok, "fixture: the older helper did not roll");
                older.beforeStopReplyForTests = [&] { stopHeld.signal(); releaseStop.wait (30000); };
                Command stop; stop.action = Action::StopRecording;
                expect (first.send (stop) && stopHeld.wait (3000), "fixture: the STOP was not held");
                first.disconnect();

                // A window of this build attaches meanwhile.
                CaptureSupervisor window;
                window.setBinaryPath (binary);
                expect (window.connectOrLaunch (port, { "--no-device", "--inputs", "1" }), window.lastConnectError);
                expect (! window.didLaunch(), "fixture: the busy helper was not kept");
                expect (window.keepsOtherBuild(), "a busy helper of another build was kept with nothing to say so");
                expect (window.otherBuildNotice().containsIgnoreCase ("another build"), window.otherBuildNotice());
                expect (window.deviceMismatch ("Any Interface").containsIgnoreCase ("another build"),
                        "RECORD's check did not name it: " + window.deviceMismatch ("Any Interface"));
                juce::String refusal;
                expect (! window.configureCapture (juce::var (new juce::DynamicObject()), &refusal),
                        "a helper about to be replaced was configured");
                juce::BigInteger arms; arms.setBit (0);
                bool unanswered = true;
                expect (! window.startRecording (session, 1, 0, arms, {}, &refusal, &unanswered),
                        "a take started on a busy helper of another build");
                expect (refusal.containsIgnoreCase ("another build"), refusal);
                expect (! unanswered, "a refusal read as an unanswered START");
                expect (! older.isRecording());

                // It finishes the STOP; the window's tick replaces it once idle.
                std::atomic<int> replacedCalls { 0 };
                std::atomic<bool> replacedStarted { false };
                window.onOtherBuildReplaced = [&] (bool started) { ++replacedCalls; replacedStarted = started; };
                releaseStop.signal();
                const auto end = juce::Time::getMillisecondCounterHiRes() + 20000.0;
                while (replacedCalls.load() == 0 && juce::Time::getMillisecondCounterHiRes() < end)
                {
                    window.tick();
                    juce::Thread::sleep (50);
                }
                expectEquals (replacedCalls.load(), 1, "the older helper was never replaced once idle");
                expect (replacedStarted.load(), "this build's helper did not start: " + window.lastConnectError);
                expect (quitAsked.load(), "the older helper was never asked to quit");
                expect (window.didLaunch(), "the window still drives the older helper");
                expect (window.didReplaceOtherBuild());
                expect (! window.keepsOtherBuild());
                expectEquals (window.helperBuild(), window.expectedHelperBuild(),
                              "the helper now attached is not this build's");
                window.shutdown();
                expect (pumpUntil ([&]
                {
                    CaptureClient probe;
                    const bool up = probe.connect ("127.0.0.1", port) && probe.hello (300).ok;
                    probe.disconnect();
                    return ! up;
                }, 5000), "the launched helper did not quit");
                older.beforeStopReplyForTests = nullptr;
            }
        }

        // ── 3. Console TCP transports ──────────────────────────────────────

        struct Ticker final : juce::Timer
        {
            std::atomic<int> ticks { 0 };
            void timerCallback() override { ++ticks; }
        };

        template <typename Transport>
        void floodOnce (const juce::String& label, const juce::MemoryBlock& burst, int total)
        {
            beginTest (label + ": a flood waits in a bounded backlog, the rest is dropped and counted");
            {
                juce::StreamingSocket desk;
                expect (desk.createListener (0, "127.0.0.1"));
                Transport link;
                std::atomic<int> heard { 0 };
                juce::StringArray reports;
                link.onMessage = [&] (const ConsoleMessage&) { ++heard; juce::Thread::sleep (1); };
                link.onUnhandledTraffic = [&] (const juce::String&, const juce::String& what) { reports.add (what); };
                expect (link.connect ("127.0.0.1", desk.getBoundPort()), "connect failed");
                std::unique_ptr<juce::StreamingSocket> wire (desk.waitForNextConnection());
                expect (wire != nullptr);
                if (wire == nullptr) return;

                // The message thread is busy (this test runs on it) while the
                // desk sends everything.
                expect (writeAllBytes (*wire, burst.getData(), burst.getSize()));
                const bool settled = waitWithoutPumping ([&]
                    { return link.getInboundOverflowCount() >= total - 2048; }, 10000);
                expect (settled, "the backlog never filled: " + juce::String (link.getInboundOverflowCount())
                                     + " dropped of " + juce::String (total));
                expectEquals (heard.load(), 0, "a message reached the handler while the message thread was busy");

                // Drained one turn at a time: timers keep running under it.
                Ticker ticker;
                ticker.startTimer (10);
                int ticksAtHalf = -1;
                pumpUntil ([&]
                {
                    if (ticksAtHalf < 0 && heard.load() >= 1000) ticksAtHalf = ticker.ticks.load();
                    return heard.load() + link.getInboundOverflowCount() >= total;
                }, 30000);
                pumpUntil ([] { return false; }, 100);   // nothing more is coming
                ticker.stopTimer();
                expectLessOrEqual (heard.load(), 2048, "more was queued than the cap allows");
                expectGreaterThan (link.getInboundOverflowCount(), (juce::int64) 0);
                expectEquals ((juce::int64) heard.load() + link.getInboundOverflowCount(), (juce::int64) total,
                              "a message was neither delivered nor counted");
                expectGreaterThan (ticksAtHalf, 10, "timers stood behind the whole backlog");
                expect (reports.joinIntoString (" | ").contains ("dropped"),
                        "the drops were never reported: " + reports.joinIntoString (" | "));
                link.onMessage = nullptr;
                link.disconnect();
            }
        }

        void aConsoleFloodIsCappedAndCounted()
        {
            constexpr int total = 6000;
            juce::MemoryOutputStream lines;
            for (int i = 0; i < total; ++i)
                lines << "NOTIFY set MIXER:Current/InCh/Label/Name " << (i % 64) << " 0 \"CH\"\n";
            floodOnce<TextLineTransport> ("SCP/TCP", lines.getMemoryBlock(), total);

            juce::MemoryBlock midi;
            for (int i = 0; i < total; ++i)
            {
                const juce::uint8 cc[] { 0xB0, 0x07, (juce::uint8) (i % 128) };
                midi.append (cc, sizeof (cc));
            }
            floodOnce<MidiTcpTransport> ("MIDI/TCP", midi, total);
        }

        void aDeskThatStopsReadingCantHoldTheMessageThread()
        {
            beginTest ("SCP/TCP: a desk that stops reading can't hold the message thread; what can't go is dropped and counted");
            {
                juce::StreamingSocket desk;
                expect (desk.createListener (0, "127.0.0.1"));
                TextLineTransport link;
                juce::StringArray reports;
                link.onUnhandledTraffic = [&] (const juce::String&, const juce::String& what) { reports.add (what); };
                expect (link.connect ("127.0.0.1", desk.getBoundPort()), "connect failed");
                std::unique_ptr<juce::StreamingSocket> wire (desk.waitForNextConnection());   // never read
                expect (wire != nullptr);

                const auto name = juce::String::repeatedString ("N", 2000);
                int refused = 0;
                const auto before = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < 4000; ++i)
                    if (! link.send (ConsoleMessage ("set MIXER:Current/InCh/Label/Name", { i % 64, 0, name })))
                        ++refused;
                const auto took = juce::Time::getMillisecondCounterHiRes() - before;
                const double holdLimitMs = ZF_NETFIX22_UNDER_TSAN ? 6000.0 : 2000.0;
                expectLessThan (took, holdLimitMs, "sending held the message thread for " + juce::String (took, 0) + " ms");
                expectGreaterThan (refused, 0, "8 MB went into a queue capped at 1 MB");
                expectGreaterThan (link.getSendDropCount(), (juce::int64) 0, "the drops were not counted");
                pumpUntil ([&] { return reports.size() > 0; }, 3000);
                expect (reports.joinIntoString (" | ").containsIgnoreCase ("not sent"),
                        "the drops were not reported: " + reports.joinIntoString (" | "));

                const auto closing = juce::Time::getMillisecondCounterHiRes();
                link.disconnect();
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - closing, 1500.0,
                                "disconnect waited on a write the desk would never take");
            }

            beginTest ("MIDI/TCP: bytes go out whole and in order through the writer, a zero byte too");
            {
                juce::StreamingSocket desk;
                expect (desk.createListener (0, "127.0.0.1"));
                MidiTcpTransport link;
                expect (link.connect ("127.0.0.1", desk.getBoundPort()), "connect failed");
                std::unique_ptr<juce::StreamingSocket> wire (desk.waitForNextConnection());
                expect (wire != nullptr);
                if (wire == nullptr) return;
                expect (link.send (ConsoleMessage ("midi", { 0xB0, 0x00, 0x01 })));
                expect (link.send (ConsoleMessage ("midi", { 0xF0, 0x00, 0x00, 0x1A, 0xF7 })));
                std::vector<juce::uint8> got;
                const auto end = juce::Time::getMillisecondCounterHiRes() + 3000;
                while (got.size() < 8 && juce::Time::getMillisecondCounterHiRes() < end)
                {
                    if (wire->waitUntilReady (true, 50) <= 0) continue;
                    juce::uint8 bytes[64];
                    const int n = wire->read (bytes, sizeof (bytes), false);
                    if (n <= 0) break;
                    got.insert (got.end(), bytes, bytes + n);
                }
                expect (got == std::vector<juce::uint8> ({ 0xB0, 0x00, 0x01, 0xF0, 0x00, 0x00, 0x1A, 0xF7 }),
                        "the desk read something else");
                link.disconnect();
            }
        }

        // ── 4. The take's claim ────────────────────────────────────────────

        void aTakeThatCantBeClaimedDoesNotRoll()
        {
            using namespace capture;

            beginTest ("Helper: START refuses when the take's claim can't be held, and nothing rolls unclaimed");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                expect (startOnSomePort (daemon));
                daemon.prepareForTests (kRate, kBlock, 1);
                daemon.getRecorder().getTrack (0).armed.store (true);
                const auto session = scratchDir ("claim");

                // Another helper writing into this folder. flock refuses a
                // second open in the same process too.
                identity::TakeClaim other;
                expect (other.hold (session), "fixture: the claim could not be taken");
                CaptureClient client;
                expect (client.connect ("127.0.0.1", daemon.getPort()) && client.hello (2000).ok);
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                const auto refused = client.request (start, 5000);
                expect (! refused.ok, "a take rolled without its claim");
                expect (refused.error.containsIgnoreCase ("claim"), "the refusal did not say why: " + refused.error);
                expect (! daemon.isRecording());
                expect (! session.getChildFile ("recording.session").exists(), "a refused take left a session marker");

                other.release();
                const auto started = client.request (start, 5000);
                expect (started.ok, started.error);
                expect (daemon.isRecording());
                expect (identity::TakeClaim::isHeld (session), "the take rolls unclaimed");
                Command stop; stop.action = Action::StopRecording;
                expect (client.request (stop, 10000).completed);
                client.disconnect();
                daemon.stop();
                session.deleteRecursively();
            }
        }

        // ── 5. Console patch saves during a take ───────────────────────────

        void aPatchSaveDuringATakeIsPendingUntilWritten()
        {
            beginTest ("Console: a STAGE confirmed during a take says the save is pending, and the list follows the write");
            {
                const auto dir = scratchDir ("patch-pending");
                ConsoleLink link;
                FakeDesk desk;
                juce::StringArray said;
                bool rolling = false;
                link.onStatus = [&] (const juce::String& s) { said.add (s); };
                link.takeHoldsDesk = [&] { return rolling; };
                desk.attach (link);
                link.setSessionDir (dir);
                link.enterSoundcheck();
                desk.answer();
                expect (link.isOnSoundcheckPatch(), "fixture: no SOUNDCHECK");
                expectEquals (fileDeskPatch (dir), juce::String ("soundcheck"));
                expect (link.sessionsOnSoundcheck.contains (dir));

                link.exitSoundcheck();     // sent before RECORD
                rolling = true;            // the desk's answer lands after it
                said.clear();
                desk.answer();
                expect (link.getPatch() == ConsoleLink::Patch::Stage);
                const auto text = said.joinIntoString (" | ");
                expect (text.contains ("being updated"), "the status did not say the save is pending: " + text);
                expect (link.sessionsOnSoundcheck.contains (dir), "the list was cleared before anything was written");
                expect (pumpUntil ([&] { return link.patchSavesInFlight == 0; }, 5000), "the save never answered");
                expectEquals (fileDeskPatch (dir), juce::String ("stage"));
                expect (! link.sessionsOnSoundcheck.contains (dir), "a written STAGE left the session listed");
                dir.deleteRecursively();
            }

            beginTest ("Console: a background patch save that fails is said, and the session stays listed");
            {
                const auto dir = scratchDir ("patch-failed");
                ConsoleLink link;
                FakeDesk desk;
                juce::StringArray said;
                bool rolling = false;
                link.onStatus = [&] (const juce::String& s) { said.add (s); };
                link.takeHoldsDesk = [&] { return rolling; };
                desk.attach (link);
                link.setSessionDir (dir);
                link.enterSoundcheck();
                desk.answer();
                expect (link.isOnSoundcheckPatch(), "fixture: no SOUNDCHECK");

                link.exitSoundcheck();
                rolling = true;
                dir.deleteRecursively();   // the drive went away under the take
                said.clear();
                desk.answer();
                expect (pumpUntil ([&] { return link.patchSavesInFlight == 0; }, 5000), "the save never answered");
                const auto text = said.joinIntoString (" | ");
                expect (text.contains ("could not be saved"), "a failed save was not said: " + text);
                expect (link.sessionsOnSoundcheck.contains (dir), "a session whose save failed left the list");
            }

            beginTest ("Console: another session's file that can't be written stays on the soundcheck list");
            {
                const auto dir = scratchDir ("patch-current");
                const auto other = scratchDir ("patch-other");
                expect (other.getChildFile (ConsoleLink::kStateFileName)
                             .replaceWithText ("{\"deskPatch\": \"soundcheck\"}"));
                ConsoleLink link;
                link.setSessionDir (dir);
                link.sessionsOnSoundcheck.add (other);
                link.patch = ConsoleLink::Patch::Stage;

                ::chmod (other.getFullPathName().toRawUTF8(), 0555);
                expect (link.persistPatch() == ConsoleLink::PatchSave::saved, "the current session was not saved");
                expect (link.sessionsOnSoundcheck.contains (other), "a file that still says SOUNDCHECK left the list");
                expectEquals (fileDeskPatch (other), juce::String ("soundcheck"));

                ::chmod (other.getFullPathName().toRawUTF8(), 0755);
                expect (link.persistPatch() == ConsoleLink::PatchSave::saved);
                expectEquals (fileDeskPatch (other), juce::String ("stage"));
                expect (! link.sessionsOnSoundcheck.contains (other), "a written file stayed on the list");
                dir.deleteRecursively();
                other.deleteRecursively();
            }
        }
    };

    static Fix20261122NetworkTests fix20261122NetworkTests;
}
