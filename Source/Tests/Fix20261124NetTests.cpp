// The twin link, desk macros and Generic OSC markers: the fixes from the
// 2026-11-24 audit. Each test names what it guards and fails on the code from
// before the fix.
//
//   1. A follower recording in-process whose own STOP is still closing its
//      files answers the leader's STOP with how that close ends, never with
//      "stopped" before it has: a failed save reaches the leader.
//   2. A desk macro key that sends a float or text value is still ignored
//      (integers only), and the status line says to send an integer.
//   3. Generic OSC named markers on different buttons each drop their marker;
//      a valueless press and its release still pair, by name.

#include <juce_core/juce_core.h>
#include <juce_osc/juce_osc.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/OscRemote.h"
#include "../Network/DeskMacro.h"
#include "../Network/TwinLink.h"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace zynforge
{
    namespace
    {
        const juce::String kTwinCode = "7K3M-Q2XD-9HVB-T4WN";

        twin::Config fastTwinConfig (twin::Role role, int port)
        {
            twin::Config c;
            c.role = role;
            c.port = port;
            c.pairingCode = kTwinCode;
            c.linkTimeoutMs = 1200;
            c.handshakeTimeoutMs = 600;
            c.reconnectMs = 50;
            c.statusPeriodMs = 40;
            if (role == twin::Role::Follower) { c.bindAddress = "127.0.0.1"; c.localName = "mini"; }
            else                              { c.peerAddress = "127.0.0.1"; c.localName = "macbook"; }
            return c;
        }

        bool settleLinks (const std::vector<twin::TwinLink*>& links, const std::function<bool()>& done,
                          int timeoutMs = 8000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            for (;;)
            {
                for (auto* link : links) link->tick();
                if (done()) return true;
                if (juce::Time::getMillisecondCounterHiRes() > end) return false;
                juce::Thread::sleep (4);
            }
        }

        void runLinks (const std::vector<twin::TwinLink*>& links, int ms)
        {
            settleLinks (links, [] { return false; }, ms);
        }

        struct NoticeList
        {
            juce::StringArray notices;
            bool noticed (const juce::String& text) const
            {
                for (const auto& notice : notices)
                    if (notice.containsIgnoreCase (text)) return true;
                return false;
            }
        };

        // The leader: its own take is whatever the test says.
        class LeaderHost final : public twin::TwinHost, public NoticeList
        {
        public:
            twin::MacStatus status;
            int stopCalls { 0 };

            twin::MacStatus twinStatus() override { return status; }
            bool twinStartRecord (juce::String&) override { status.recording = true; return true; }
            void twinStopRecord (std::function<void (bool, juce::String)> done) override
            {
                ++stopCalls;
                done (true, {});
            }
            void twinDropMarker (const juce::String&) override {}
            void twinNotify (const juce::String& text) override { notices.add (text); }
        };

        // A follower that records in-process, as MainComponent does: its own
        // STOP ends the take at once (not recording) and the files close
        // afterwards (finalizing). A link STOP that lands meanwhile is answered
        // with how that close ends (MainComponent::twinStopRecord waits for it).
        class InProcessFollower final : public twin::TwinHost, public NoticeList
        {
        public:
            twin::MacStatus status;
            int startCalls { 0 }, stopCalls { 0 };
            bool linkStoppedATake { false };
            std::function<void (bool, juce::String)> waitingAnswer;

            twin::MacStatus twinStatus() override { return status; }
            bool twinStartRecord (juce::String&) override
            {
                ++startCalls;
                status.recording = true;
                return true;
            }
            void twinStopRecord (std::function<void (bool, juce::String)> done) override
            {
                ++stopCalls;
                if (status.recording)
                {
                    linkStoppedATake = true;
                    status.recording = false;
                    status.finalizing = true;
                }
                waitingAnswer = std::move (done);
            }
            void twinDropMarker (const juce::String&) override {}
            void twinNotify (const juce::String& text) override { notices.add (text); }

            // STOP pressed on this Mac: the take ends now, its files close later.
            void stopHere() { status.recording = false; status.finalizing = true; }
            // The files have closed; whoever waits on it hears how.
            void closed (bool ok, const juce::String& why)
            {
                status.finalizing = false;
                if (auto answer = std::exchange (waitingAnswer, {})) answer (ok, why);
            }
        };

        struct TwinPair
        {
            LeaderHost        leaderHost;
            InProcessFollower followerHost;
            std::unique_ptr<twin::TwinLink> leader   { std::make_unique<twin::TwinLink> (leaderHost) };
            std::unique_ptr<twin::TwinLink> follower { std::make_unique<twin::TwinLink> (followerHost) };
            std::vector<twin::TwinLink*> both { leader.get(), follower.get() };

            TwinPair()
            {
                leaderHost.status.sessionName = followerHost.status.sessionName = "Show";
                leaderHost.status.sampleRate = followerHost.status.sampleRate = 48000.0;
                leaderHost.status.armedTracks = followerHost.status.armedTracks = 2;
            }

            ~TwinPair()
            {
                follower->stop();
                leader->stop();
            }

            bool linked() const
            {
                return leader->snapshot().state == twin::LinkState::Linked
                    && follower->snapshot().state == twin::LinkState::Linked
                    && leader->snapshot().peer.has_value()
                    && follower->snapshot().peer.has_value();
            }

            juce::String leaderEvent() const { return leader->snapshot().lastEvent; }
        };

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

        int countContaining (const juce::StringArray& lines, const juce::String& text)
        {
            int n = 0;
            for (const auto& line : lines) n += line.containsIgnoreCase (text) ? 1 : 0;
            return n;
        }

        juce::File scratchDir (const juce::String& name)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-fix1124-" + name + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }
    }

    class Fix20261124NetTests final : public juce::UnitTest
    {
    public:
        Fix20261124NetTests() : juce::UnitTest ("Fixes 2026-11-24 network", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };

            aFollowerStillSavingAnswersWithItsOutcome();   // 1
            aDeskMacroWithAFloatOrTextSaysSo();            // 2
            namedOscMarkersPairByName();                   // 3
        }

        // ── 1. Twin follower STOP while its own STOP is closing ────────────

        bool bring (TwinPair& pair)
        {
            juce::String error;
            expect (pair.follower->start (fastTwinConfig (twin::Role::Follower, 0), error), error);
            expect (settleLinks (pair.both, [&] { return pair.follower->getBoundPort() > 0; }), "follower never bound");
            expect (pair.leader->start (fastTwinConfig (twin::Role::Leader, pair.follower->getBoundPort()), error), error);
            const bool up = settleLinks (pair.both, [&] { return pair.linked(); });
            expect (up, "the two endpoints never linked");
            return up;
        }

        // A linked take, rolling on both Macs.
        bool rollLinkedTake (TwinPair& pair)
        {
            pair.leaderHost.status.recording = true;
            pair.leader->noteLocalTakeStarted();
            const bool rolling = settleLinks (pair.both, [&]
            {
                return pair.followerHost.status.recording && pair.leaderEvent() == "Follower is recording";
            });
            expect (rolling, "the follower never joined the take: " + pair.leaderEvent());
            return rolling;
        }

        void leaderStops (TwinPair& pair)
        {
            pair.leaderHost.status.recording = false;
            pair.leader->noteLocalStopAccepted();
        }

        void aFollowerStillSavingAnswersWithItsOutcome()
        {
            beginTest ("Twin: STOP on the follower, then the leader's STOP; a failed save reaches the leader");
            {
                TwinPair pair;
                if (! bring (pair) || ! rollLinkedTake (pair)) return;

                // The operator stops the follower at about the moment of the
                // leader's STOP; its own tick sees the take end first.
                pair.followerHost.stopHere();
                runLinks (pair.both, 150);
                leaderStops (pair);

                const bool heard = settleLinks (pair.both, [&]
                {
                    return pair.leaderEvent() == "Follower stopped" || pair.followerHost.waitingAnswer != nullptr;
                }, 4000);
                expect (heard, "the leader's STOP never reached the follower");
                expect (pair.leaderEvent() != "Follower stopped",
                        "the leader was told its follower stopped while the follower was still saving");
                runLinks (pair.both, 150);
                expect (pair.leaderEvent() != "Follower stopped", "the leader heard 'stopped' before the save ended");

                // The save fails.
                pair.followerHost.closed (false, "Recording stopped, but the integrity report could not be saved");
                expect (settleLinks (pair.both, [&] { return pair.leaderHost.noticed ("FOLLOWER STOP PROBLEM"); }, 4000),
                        "the follower's failed save never reached the leader: " + pair.leaderEvent());
                expect (pair.leaderHost.noticed ("integrity report"), pair.leaderEvent());
                expect (! pair.followerHost.linkStoppedATake, "the leader's STOP stopped something on the follower");
                expectEquals (pair.leaderHost.stopCalls, 0, "the link stopped the leader");
            }

            beginTest ("Twin: the follower's own STOP closing cleanly is answered 'stopped' only once it has closed");
            {
                TwinPair pair;
                if (! bring (pair) || ! rollLinkedTake (pair)) return;
                pair.followerHost.stopHere();
                runLinks (pair.both, 150);
                leaderStops (pair);
                const bool heard = settleLinks (pair.both, [&]
                {
                    return pair.leaderEvent() == "Follower stopped" || pair.followerHost.waitingAnswer != nullptr;
                }, 4000);
                expect (heard, "the leader's STOP never reached the follower");
                expect (pair.leaderEvent() != "Follower stopped",
                        "the leader was told its follower stopped while the follower was still saving");
                pair.followerHost.closed (true, {});
                expect (settleLinks (pair.both, [&] { return pair.leaderEvent() == "Follower stopped"; }, 4000),
                        "a clean close was never reported: " + pair.leaderEvent());
                expect (! pair.leaderHost.noticed ("FOLLOWER STOP PROBLEM"));
                expect (! pair.followerHost.linkStoppedATake);
            }

            beginTest ("Twin: a leader STOP handled before the follower's tick noticed its own STOP also waits");
            {
                TwinPair pair;
                if (! bring (pair) || ! rollLinkedTake (pair)) return;
                leaderStops (pair);
                // Let the frame reach the follower's inbox without the follower
                // acting on it, then stop the follower first: the leader's STOP
                // is handled before its tick has seen its own take end.
                runLinks ({ pair.leader.get() }, 250);
                pair.followerHost.stopHere();
                const bool heard = settleLinks (pair.both, [&]
                {
                    return pair.leaderEvent() == "Follower stopped" || pair.followerHost.waitingAnswer != nullptr;
                }, 4000);
                expect (heard, "the leader's STOP never reached the follower");
                expect (pair.leaderEvent() != "Follower stopped",
                        "the leader was told its follower stopped while the follower was still saving");
                pair.followerHost.closed (false, "backup audio write failed");
                expect (settleLinks (pair.both, [&] { return pair.leaderHost.noticed ("backup audio write failed"); }, 4000),
                        "the follower's failed save never reached the leader: " + pair.leaderEvent());
                expect (! pair.followerHost.linkStoppedATake, "the leader's STOP stopped something on the follower");
            }

            beginTest ("Twin: a follower take that has already closed answers the leader's STOP at once, touching nothing");
            {
                TwinPair pair;
                if (! bring (pair) || ! rollLinkedTake (pair)) return;
                pair.followerHost.stopHere();
                runLinks (pair.both, 100);
                pair.followerHost.closed (true, {});
                runLinks (pair.both, 100);
                leaderStops (pair);
                expect (settleLinks (pair.both, [&] { return pair.leaderEvent() == "Follower stopped"; }, 4000),
                        pair.leaderEvent());
                expectEquals (pair.followerHost.stopCalls, 0, "a STOP for a closed take asked the follower to stop");
            }
        }

        // ── 2. Desk macro values that are not integers ────────────────────

        void aDeskMacroWithAFloatOrTextSaysSo()
        {
            using A = AudioEngine::RemoteTransportAction;

            beginTest ("Desk macro: a float or text value is ignored, and the status line says to send an integer");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                juce::StringArray shown;
                engine.getRemotes().setHost ({ [&shown] (const juce::String& m) { shown.add (m); }, {}, {} });
                DeskMacroListener macro (engine);

                // TouchOSC's default: 1.0 on press, 0.0 on release.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record", 1.0f), 10000.0);
                expect (spy.actions.empty(), "a float RECORD was acted on; the desk macro port takes integers only");
                const auto first = shown.joinIntoString (" | ");
                expectEquals (countContaining (shown, "integer"), 1, "a float RECORD vanished with nothing said: " + first);
                expect (first.contains ("RECORD") && first.contains ("1 on press"), first);

                // Its release, inside the second: not said again.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/record", 0.0f), 10300.0);
                expectEquals (countContaining (shown, "integer"), 1, "the notice is not rate-limited to once a second");

                // Text, a second later: said, and names the key.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/stop", juce::String ("1")), 11500.0);
                expectEquals (countContaining (shown, "integer"), 2, shown.joinIntoString (" | "));
                expect (shown[shown.size() - 1].contains ("STOP"), shown[shown.size() - 1]);

                // Two arguments are no command either.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker", (juce::int32) 1, (juce::int32) 1), 13000.0);
                expectEquals (countContaining (shown, "integer"), 3, shown.joinIntoString (" | "));

                // The marker number, as a float.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/goto", 3.0f), 14500.0);
                expectEquals (countContaining (shown, "integer"), 4, shown.joinIntoString (" | "));
                expect (shown[shown.size() - 1].containsIgnoreCase ("marker"), shown[shown.size() - 1]);

                expect (spy.actions.empty(), "a value that is not an integer reached the host");

                // An address that is no macro stays as quiet as before.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/arm", 1.0f), 20000.0);
                expectEquals (countContaining (shown, "integer"), 4, "an unknown address was answered");

                // An integer still works, and says nothing about integers.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play", (juce::int32) 1), 22000.0);
                expectEquals (spy.count (A::StartPlay), 1);
                expectEquals (countContaining (shown, "integer"), 4, shown.joinIntoString (" | "));

                engine.getRemotes().setHost ({});
                engine.setRemoteTransportHandler ({});
            }
        }

        // ── 3. Generic OSC named markers ──────────────────────────────────

        void namedOscMarkersPairByName()
        {
            const auto names = [] (MarkersManager& markers)
            {
                juce::StringArray list;
                for (const auto& marker : markers.getAll()) list.add (marker.name);
                list.sort (true);
                return list.joinIntoString (",");
            };

            beginTest ("Generic OSC: named markers on different buttons each drop their marker");
            {
                const auto dir = scratchDir ("osc-named");
                const juce::ScopeGuard remove { [&] { dir.deleteRecursively(); } };
                AudioEngine engine;
                auto& markers = engine.getMarkers();
                markers.setContext (dir, 48000);
                engine.setEditCursorSample (0);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                // Two press-only buttons, a second apart.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Verse")), 10000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Chorus")), 11000.0);
                expectEquals (markers.getCount(), 2, "Chorus was taken as Verse's release and dropped");
                expectEquals (names (markers), juce::String ("Chorus,Verse"));

                // Two buttons that send on press and release, overlapping:
                // each release pairs with its own press.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Verse")), 20000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Chorus")), 20300.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Verse")), 20600.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Chorus")), 20900.0);
                expectEquals (markers.getCount(), 4, "overlapping presses and releases miscounted: " + names (markers));
                markers.clearContext();
            }

            beginTest ("Generic OSC: one named button's press and release still drop one marker");
            {
                const auto dir = scratchDir ("osc-same-name");
                const juce::ScopeGuard remove { [&] { dir.deleteRecursively(); } };
                AudioEngine engine;
                auto& markers = engine.getMarkers();
                markers.setContext (dir, 48000);
                engine.setEditCursorSample (0);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Bridge")), 30000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Bridge")), 30700.0);
                expectEquals (markers.getCount(), 1, "a press and its release dropped two markers");
                // Past the window the same button is a press again.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Bridge")), 33000.0);
                expectEquals (markers.getCount(), 2);

                // An unnamed button and a named one are different keys.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker"), 40000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Outro")), 40400.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker"), 40800.0);                          // release
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", juce::String ("Outro")), 41200.0); // release
                expectEquals (markers.getCount(), 4, names (markers));
                expect (names (markers).contains ("Outro"), "Outro was taken as the unnamed button's release: " + names (markers));

                // A button that sends values is unaffected: 1 on press, 0 on release.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 1, juce::String ("Solo")), 50000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/marker", (juce::int32) 0, juce::String ("Solo")), 50300.0);
                expectEquals (markers.getCount(), 5, names (markers));
                markers.clearContext();
            }

            beginTest ("Desk macro: a valueless key still pairs its press and release");
            {
                AudioEngine engine;
                TransportSpy spy;
                spy.attach (engine);
                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 0.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/pause"), 300.0);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/play"), 700.0);    // PLAY's release
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/pause"), 1000.0);  // PAUSE's release
                expectEquals (spy.count (AudioEngine::RemoteTransportAction::StartPlay), 1);
                expectEquals (spy.count (AudioEngine::RemoteTransportAction::StopPlay), 1);
                engine.setRemoteTransportHandler ({});
            }
        }
    };

    static Fix20261124NetTests fix20261124NetTests;
}
