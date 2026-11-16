// Console link, OSC remote, desk macros, control surfaces and Scene Follow:
// the fixes from the 2026-11-16 audit (NET-01 .. NET-11). Each test names the
// finding it guards and fails on the code from before the fix.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/ControlSurfacesDialog.h"
#include "../UI/MainComponent.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/OscRemote.h"
#include "../Audio/SceneFollow.h"
#include "../Network/ConsoleLink.h"
#include "../Network/DeskMacro.h"

#include <CoreFoundation/CoreFoundation.h>
#include <cstring>
#include <functional>
#include <map>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        juce::File scratchDir()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-netfix-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        void recordBaseTake (const juce::File& dir, int tracks, int blocks)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, 0.1f);
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        const char* const kInBlocks[] { "/config/routing/IN/1-8",  "/config/routing/IN/9-16",
                                        "/config/routing/IN/17-24", "/config/routing/IN/25-32" };

        // An X32 as the link hears one (the same shape as NetworkStateTests').
        struct FakeDesk
        {
            std::vector<int> routing { 4, 5, 6, 7 };
            std::map<int, float> headamps;
            bool silent { false };
            ConsoleLink* link { nullptr };
            std::vector<ConsoleMessage> wire, replies;

            static int blockOf (const juce::String& address)
            {
                for (int b = 0; b < 4; ++b)
                    if (address == kInBlocks[b]) return b;
                return -1;
            }

            static int headampOf (const juce::String& address)
            {
                if (! address.startsWith ("/headamp/")) return -1;
                return address.fromFirstOccurrenceOf ("/headamp/", false, false).getIntValue();
            }

            void attach (ConsoleLink& l)
            {
                link = &l;
                l.setMessageHook ([this] (const ConsoleMessage& m) { heard (m); });
            }

            void heard (const ConsoleMessage& m)
            {
                wire.push_back (m);
                if (const int block = blockOf (m.address); block >= 0)
                {
                    if (m.hasArgs()) routing[(size_t) block] = m.intArg();
                    else if (! silent) replies.push_back (ConsoleMessage (m.address, { routing[(size_t) block] }));
                }
                else if (const int amp = headampOf (m.address); amp >= 0)
                {
                    if (m.hasArgs()) headamps[amp] = m.floatArg();
                    else if (! silent && headamps.count (amp) != 0)
                        replies.push_back (ConsoleMessage (m.address, { (double) headamps[amp] }));
                }
            }

            void answer()
            {
                auto pending = std::move (replies);
                replies.clear();
                for (const auto& r : pending) link->injectReply (r);
            }

            int routingQueries() const
            {
                int n = 0;
                for (const auto& m : wire)
                    if (blockOf (m.address) >= 0 && ! m.hasArgs()) ++n;
                return n;
            }
        };

        juce::Button* findButton (juce::Component& parent, const juce::String& text)
        {
            for (auto* child : parent.getChildren())
                if (auto* b = dynamic_cast<juce::Button*> (child); b != nullptr && b->getButtonText() == text)
                    return b;
            return nullptr;
        }

        template <typename T>
        T* findChild (juce::Component& parent)
        {
            for (auto* child : parent.getChildren())
                if (auto* c = dynamic_cast<T*> (child)) return c;
            return nullptr;
        }

        int freeUdpPort (int from)
        {
            for (int p = from; p < from + 40; ++p)
            {
                juce::DatagramSocket probe (false);
                if (probe.bindToPort (p, "127.0.0.1")) return p;
            }
            return 0;
        }
    }

    class NetFix20261116Tests final : public juce::UnitTest
    {
    public:
        NetFix20261116Tests() : juce::UnitTest ("Net fixes 2026-11-16", "zynforge") {}

        struct Bench
        {
            MainComponent& main;
            juce::File session { scratchDir() };

            explicit Bench (MainComponent& m, int tracks = 3, int blocks = 200) : main (m)
            {
                recordBaseTake (session, tracks, blocks);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Bench()
            {
                if (main.sessionLocked) main.onLockToggled();
                engine().setSceneFollowEnabled (false);
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                pumpUntil ([this] { return ! engine().isRecording() && ! main.captureMetadataPending
                                           && main.pendingMetadataSaves == 0; });
                main.consoleLink.setMessageHook (nullptr);
                main.consoleLink.disconnect();
                engine().stopPlayback();
                engine().setPunchModeOn (false);
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.lastTrackCount = -1;
                session.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            MainComponent main;
            main.setSize (1320, 820);

            manualPunchOutlivesARemoteStopOfPlayback (main);    // NET-01, NET-08
            remoteLocatesCantRollIntoAPunch (main);             // NET-02
            emptyDeskNamesNeverEraseAStrip();                   // NET-03
            aPushedPatchChangeIsReadAgain();                    // NET-04
            aTimedOutGainCaptureKeepsTheSavedGains();           // NET-05
            namedGenericScenes();                               // NET-06
            deskChannelsAddressTheListeningStrip();             // NET-07
            surfaceStopAndRecordLight (main);                   // NET-08
            oscPanelKeepsTheTabletsToken();                     // NET-09
            surfacePortsTakeEffectOnSave();                     // NET-10
            aLostLinkCanBeCleared (main);                       // NET-11
        }

        // ── NET-01 ─────────────────────────────────────────────────────────

        static void startManualPunchWhileRolling (MainComponent& main, Bench& b)
        {
            b.engine().setTrackArmed (0, true);
            b.engine().getPlayer().setPositionSamples (4096);
            b.engine().startPlayback();
            MainComponent::RecordStartPlan plan;
            plan.dir = b.session;
            plan.continueTake = true;
            plan.punchAt = b.engine().getPlayer().getPositionSamples();
            plan.numTracks = b.recorder().getNumTracks();
            plan.armed = 1;
            main.runRecordPlan (plan);
        }

        void manualPunchOutlivesARemoteStopOfPlayback (MainComponent& main)
        {
            using Action = AudioEngine::RemoteTransportAction;
            beginTest ("NET-01: a remote or surface stop of playback can't leave a manual punch recording");
            {
                Bench b (main);
                startManualPunchWhileRolling (main, b);
                expect (main.manualPunchActive && b.engine().isRecording() && b.engine().isPlaying(),
                        "fixture: no manual punch rolled -- " + main.statusLabel.getText());

                juce::String error;
                expect (! b.engine().performRemoteTransport (Action::StopPlay, error),
                        "a tablet's PLAY release stopped playback under a manual punch");
                expect (error.contains ("STOP on the Mac"), error);
                expect (! b.engine().performRemoteTransport (Action::TogglePlay, error),
                        "a remote PLAY toggle stopped playback under a manual punch");

                MidiControlSurface surface (b.engine());
                surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::noteOn (1, mcu::Stop, (juce::uint8) 127));
                surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::noteOn (1, mcu::Play, (juce::uint8) 127));
                OscRemote osc (b.engine());
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/play", (juce::int32) 0));

                expect (b.engine().isPlaying(), "playback stopped under the punch");
                expect (main.manualPunchActive && b.engine().isRecording(), "the punch ended by itself");
                expect (main.statusLabel.getText().containsIgnoreCase ("STOP on the Mac"), main.statusLabel.getText());

                // STOP on the Mac ends it, in one press.
                main.onStopClicked();
                pumpUntil ([&] { return ! b.engine().isRecording() && ! main.captureMetadataPending
                                        && main.pendingMetadataSaves == 0; });
                expect (! b.engine().isRecording(), "STOP on the Mac did not end the punch");
                expect (! main.manualPunchActive);
            }
        }

        // ── NET-02 ─────────────────────────────────────────────────────────

        static constexpr juce::int64 kIn = 96000, kOut = 144000, kPast = 200000;

        void remoteLocatesCantRollIntoAPunch (MainComponent& main)
        {
            beginTest ("NET-02: a remote locate can't put a rolling playhead before the PUNCH range");
            {
                Bench b (main, 3, 1300);   // ~6.9 s: the markers and the range sit inside the take
                auto& engine = b.engine();
                auto& player = engine.getPlayer();
                engine.setTrackArmed (0, true);
                engine.setPunchModeOn (true);
                player.selectRange (kIn, kOut);
                engine.getMarkers().drop (10000, "Verse");
                engine.getMarkers().drop (300000, "Outro");
                player.setPositionSamples (kPast);
                expect (engine.startPlayback(), "fixture: playback did not roll");

                engine.setSceneFollowEnabled (true);
                const auto action = engine.getRemotes().onConsoleScene (1, "Verse", scenefollow::Path::ConsoleLink);
                expect (action == scenefollow::Action::Refused, "Scene Follow sent the playhead back before the range");
                expectEquals (player.getPositionSamples(), kPast);
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());

                DeskMacroListener macro (engine);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/prev"), 10000.0);
                expectEquals (player.getPositionSamples(), kPast, "a macro PREV sent the playhead back before the range");
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/goto", (juce::int32) 1), 20000.0);
                expectEquals (player.getPositionSamples(), kPast, "a macro GOTO sent the playhead back before the range");
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());

                // Past the range is fine: nothing rolls into it from there.
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/goto", (juce::int32) 2), 30000.0);
                expectEquals (player.getPositionSamples(), (juce::int64) 300000, "a jump past the range was refused");

                // The jog wheel asks the same question.
                player.setPositionSamples (1000);
                MidiControlSurface surface (engine);
                surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::controllerEvent (1, mcu::kJogCc, 1));
                expectEquals (player.getPositionSamples(), (juce::int64) 1000, "the jog moved a rolling playhead before the range");

                // Stopped, a remote can't start the roll (remote PLAY is refused
                // in PUNCH mode), so a locate before the range is harmless.
                engine.stopPlayback();
                player.setPositionSamples (kPast);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/goto", (juce::int32) 1), 40000.0);
                expectEquals (player.getPositionSamples(), (juce::int64) 10000, "a stopped locate was refused");
            }

            beginTest ("NET-02: the rule itself refuses before the range only while rolling");
            {
                using namespace scenefollow;
                Marker verse;
                verse.sampleOffset = 500;
                verse.name = "Verse";
                std::vector<Marker> markers { verse };
                Facts f;
                f.followOn = true;
                f.playerLoaded = true;
                f.punchRange = juce::Range<juce::int64> (600, 800);
                expect (decide (f, markers, "Verse", 900).action == Action::Locate);
                f.rolling = true;
                expect (decide (f, markers, "Verse", 900).action == Action::Refused);
                f.punchRange = {};
                expect (decide (f, markers, "Verse", 900).action == Action::Locate, "no PUNCH range, no refusal");
            }
        }

        // ── NET-03 ─────────────────────────────────────────────────────────

        void emptyDeskNamesNeverEraseAStrip()
        {
            beginTest ("NET-03: an empty desk channel name on the OSC listener never erases a strip's name");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setStripCount (2);
                engine.setTrackName (0, "KiCK");
                OscRemote osc (engine);

                osc.setDialect (OscRemote::Dialect::DiGiCo);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/name", juce::String()));
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("KiCK"),
                              "an empty DiGiCo name erased the strip");
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/1/name", juce::String ("   ")));
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("KiCK"),
                              "a blank DiGiCo name erased the strip");

                osc.setDialect (OscRemote::Dialect::Generic);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/name", juce::String()));
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("KiCK"),
                              "an empty Generic name erased the strip");

                // A real name still lands, trimmed as the console link trims it.
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/name", juce::String ("  SNARE ")));
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("SNARE"));
                engine.clearAllStripOverrides();
            }
        }

        // ── NET-04 ─────────────────────────────────────────────────────────

        void aPushedPatchChangeIsReadAgain()
        {
            beginTest ("NET-04: a routing push that contradicts STAGE reads the desk again, never a stale green");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.checkDeskPatch();
                desk.answer();
                expect (link.getPatch() == ConsoleLink::Patch::Stage, "fixture: the desk was not read as on stage");

                // A scene recall on the desk moves block 1 to the card returns,
                // and the X32 pushes it under /xremote.
                desk.routing[0] = 16;
                const int queriesBefore = desk.routingQueries();
                link.injectReply (ConsoleMessage (kInBlocks[0], { 16 }));
                expect (link.getPatch() != ConsoleLink::Patch::Stage, "the pushed card return still showed STAGE");
                expect (desk.routingQueries() > queriesBefore, "the push did not start a read of the desk");
                desk.answer();
                expect (link.getPatch() != ConsoleLink::Patch::Stage, "the read-back of card returns showed STAGE");
            }

            beginTest ("NET-04: a push that agrees with what is known reads nothing");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.checkDeskPatch();
                desk.answer();
                const int queriesBefore = desk.routingQueries();
                link.injectReply (ConsoleMessage (kInBlocks[1], { 5 }));
                expectEquals (desk.routingQueries(), queriesBefore, "an agreeing push started a read");
                expect (link.getPatch() == ConsoleLink::Patch::Stage);
            }

            beginTest ("NET-04: on SOUNDCHECK, a push that moves a block off the card reads the desk");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.enterSoundcheck();
                desk.answer();
                expect (link.isOnSoundcheckPatch(), "fixture: not on SOUNDCHECK");
                for (auto& r : desk.routing) r -= 12;   // put back by hand: 4, 5, 6, 7
                const int queriesBefore = desk.routingQueries();
                link.injectReply (ConsoleMessage (kInBlocks[2], { 6 }));
                expect (desk.routingQueries() > queriesBefore, "the push did not start a read of the desk");
                desk.answer();
                expect (link.getPatch() == ConsoleLink::Patch::Stage, "the desk put back by hand still showed SOUNDCHECK");
            }
        }

        // ── NET-05 ─────────────────────────────────────────────────────────

        void aTimedOutGainCaptureKeepsTheSavedGains()
        {
            beginTest ("NET-05: a timed-out head-amp capture keeps the last complete set for Restore");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                desk.headamps = { { 0, 0.25f }, { 1, 0.5f } };
                link.captureGains (2);
                desk.answer();
                expect (link.isGainCaptureComplete(), "fixture: the first capture did not complete");

                // A second capture on a network that loses a reply.
                desk.headamps = { { 0, 0.75f } };
                link.captureGains (2);
                desk.answer();
                link.gainDeadlineMs = 1;
                link.timerCallback();
                expect (link.isGainCaptureComplete(), "the timed-out capture left a partial set behind");
                expectEquals ((int) link.getCapturedGains().size(), 2);
                expectWithinAbsoluteError (link.getCapturedGains().count (1) != 0 ? link.getCapturedGains().at (1) : -1.0f,
                                           0.5f, 1.0e-6f, "the saved gain was not the one kept");

                // Restore still works, with the set saved before.
                desk.wire.clear();
                link.restoreGains();
                int writes = 0;
                for (const auto& m : desk.wire) if (m.hasArgs()) ++writes;
                expectEquals (writes, 2, "Restore refused or wrote a partial set");
            }

            beginTest ("NET-05: a capture that never had a complete set leaves nothing to restore");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                desk.headamps = { { 0, 0.75f } };
                link.captureGains (2);
                desk.answer();
                link.gainDeadlineMs = 1;
                link.timerCallback();
                expect (! link.isGainCaptureComplete());
                desk.wire.clear();
                link.restoreGains();
                int writes = 0;
                for (const auto& m : desk.wire) if (m.hasArgs()) ++writes;
                expectEquals (writes, 0, "a partial capture reached the desk");
            }
        }

        // ── NET-06 ─────────────────────────────────────────────────────────

        void namedGenericScenes()
        {
            beginTest ("NET-06: Generic /zynforge/scene with a name recalls that name, each name its own press");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                auto& remotes = engine.getRemotes();
                remotes.clearOscLog();
                remotes.setOscDebug (true);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::Generic);

                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", juce::String ("Verse")), 10000.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", juce::String ("Chorus")), 10100.0);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/scene", juce::String ("Chorus")), 10150.0);
                // What the scene rule was asked, one line per recall it heard.
                juce::StringArray recalls;
                for (const auto& line : remotes.getOscLog())
                    if (line.contains ("scene: Console scene \""))
                        recalls.add (line.fromFirstOccurrenceOf ("Console scene ", false, false)
                                         .upToFirstOccurrenceOf (" -- ", false, false));
                remotes.setOscDebug (false);
                remotes.clearOscLog();
                const auto heard = recalls.joinIntoString (", ");
                expectEquals (recalls.size(), 2, "one recall per named press: " + heard);
                expect (recalls.contains ("\"Verse\""), "the scene's name was lost: " + heard);
                expect (recalls.contains ("\"Chorus\""), "a second named scene was debounced as the first: " + heard);
            }
        }

        // ── NET-07 ─────────────────────────────────────────────────────────

        void deskChannelsAddressTheListeningStrip()
        {
            beginTest ("NET-07: colour, mute and arm reach the strip that listens to the desk channel, as names do");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setStripCount (3);
                // Strip 1 hears desk channel 3, strip 3 hears desk channel 1.
                engine.setTrackInputRouting (0, 2);
                engine.setTrackInputRouting (2, 0);
                OscRemote osc (engine);
                auto& rec = engine.getRecorder();

                osc.setDialect (OscRemote::Dialect::Generic);
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/name", juce::String ("KiCK")));
                expectEquals (rec.getTrack (2).getNameThreadSafe(), juce::String ("KiCK"), "fixture: names moved");
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/colour", (juce::int32) 0x00ff00));
                expect (rec.getTrack (2).colourARGB.load() == 0xff00ff00u, "the colour missed the strip that hears channel 1");
                expect (rec.getTrack (0).colourARGB.load() != 0xff00ff00u, "the colour went to strip position 1");
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/mute", (juce::int32) 1));
                expect (rec.getTrack (2).muted.load(), "mute missed the strip that hears channel 1");
                expect (! rec.getTrack (0).muted.load(), "mute went to strip position 1");
                osc.dispatchForTest (juce::OSCMessage ("/zynforge/channel/1/arm", (juce::int32) 1));
                expect (rec.getTrack (2).armed.load(), "arm missed the strip that hears channel 1");
                expect (! rec.getTrack (0).armed.load(), "arm went to strip position 1");

                osc.setDialect (OscRemote::Dialect::DiGiCo);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/3/colour", (juce::int32) 0x0000ff));
                expect (rec.getTrack (0).colourARGB.load() == 0xff0000ffu, "a desk colour missed the strip that hears channel 3");
                expect (rec.getTrack (2).colourARGB.load() == 0xff00ff00u, "a desk colour went to strip position 3");
                engine.clearAllStripOverrides();
            }
        }

        // ── NET-08 ─────────────────────────────────────────────────────────

        void surfaceStopAndRecordLight (MainComponent& main)
        {
            beginTest ("NET-08: a surface STOP during a take says so, and the surface's REC light follows the take");
            {
                Bench b (main);
                MidiControlSurface surface (b.engine());
                std::vector<juce::MidiMessage> sent;
                surface.sendHookForTests = [&] (const juce::MidiMessage& m) { sent.push_back (m); };
                const auto wasSent = [&] (const juce::MidiMessage& wanted)
                {
                    for (const auto& m : sent)
                        if (m.getRawDataSize() == wanted.getRawDataSize()
                            && std::memcmp (m.getRawData(), wanted.getRawData(), (size_t) m.getRawDataSize()) == 0)
                            return true;
                    return false;
                };
                surface.timerCallback();

                b.engine().setTrackArmed (0, true);
                MainComponent::RecordStartPlan plan;
                plan.dir = b.session;
                plan.continueTake = true;
                plan.continueAppend = true;
                plan.numTracks = b.recorder().getNumTracks();
                plan.armed = 1;
                main.runRecordPlan (plan);
                expect (b.engine().isRecording() && ! main.manualPunchActive,
                        "fixture: no take rolled -- " + main.statusLabel.getText());

                sent.clear();
                surface.timerCallback();
                expect (wasSent (mcu::led (mcu::Action::Record, 0, true)), "the surface's REC light stayed dark during a take");

                main.showStatus ("something else");
                surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::noteOn (1, mcu::Stop, (juce::uint8) 127));
                expect (b.engine().isRecording(), "a surface STOP ended a take");
                expect (main.statusLabel.getText().contains ("Surface STOP doesn't end a take"), main.statusLabel.getText());

                main.onStopClicked();
                main.onStopClicked();
                pumpUntil ([&] { return ! b.engine().isRecording() && ! main.captureMetadataPending
                                        && main.pendingMetadataSaves == 0; });
                sent.clear();
                surface.timerCallback();
                expect (wasSent (mcu::led (mcu::Action::Record, 0, false)), "the surface's REC light stayed lit after STOP");
            }
        }

        // ── NET-09 ─────────────────────────────────────────────────────────

        void oscPanelKeepsTheTabletsToken()
        {
            beginTest ("NET-09: Apply with the same port, or ticking another OSC dialect, keeps the tablet's token");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                const int port = freeUdpPort (48310);
                expect (port != 0, "fixture: no free UDP port");
                updateSettings (engine.getAppProps(), [&] (juce::PropertiesFile& p) { p.setValue ("oscListenPort", port); });
                expect (remotes.startOsc (port, 0), "fixture: the listener did not start");
                const auto token = remotes.getOscAccessToken();

                {
                    cs_detail::OscSettings panel (engine, cs_detail::protocols()[0], {});
                    auto* apply = findButton (panel, "Apply");
                    expect (apply != nullptr, "fixture: no Apply button");
                    if (apply != nullptr) apply->onClick();
                }
                expect (remotes.isOscListening());
                expectEquals (remotes.getOscAccessToken(), token, "Apply with nothing changed made a new token");

                expectEquals (cs_detail::setProtoEnabled (engine, cs_detail::protocols()[1], true), juce::String());
                expectEquals (remotes.getOscDialect(), 1);
                expectEquals (remotes.getOscAccessToken(), token, "ticking DiGiCo made a new token");
                expectEquals (cs_detail::setProtoEnabled (engine, cs_detail::protocols()[0], true), juce::String());
                expectEquals (remotes.getOscAccessToken(), token, "ticking OSC back made a new token");

                // A new port is a new listener.
                const int other = freeUdpPort (port + 1);
                {
                    cs_detail::OscSettings panel (engine, cs_detail::protocols()[0], {});
                    if (auto* editor = findChild<juce::TextEditor> (panel)) editor->setText (juce::String (other));
                    if (auto* apply = findButton (panel, "Apply")) apply->onClick();
                }
                expectEquals (remotes.getOscPort(), other, "a new port was not taken");
                remotes.stopOsc();
            }

            beginTest ("NET-09: a packet with the wrong token says so, once in a while");
            {
                AudioEngine engine;
                auto& remotes = engine.getRemotes();
                juce::StringArray notices;
                RemoteControl::Host host;
                host.showStatus = [&] (const juce::String& s) { notices.add (s); };
                remotes.setHost (host);
                const int port = freeUdpPort (48360);
                expect (port != 0 && remotes.startOsc (port, 0), "fixture: the listener did not start");

                juce::OSCSender tablet;
                expect (tablet.connect ("127.0.0.1", port));
                expect (tablet.send (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1, juce::String ("stale-token"))));
                expect (tablet.send (juce::OSCMessage ("/zynforge/stop", (juce::int32) 1, juce::String ("stale-token"))));
                pumpUntil ([&] { return ! notices.isEmpty(); }, 2000);
                pumpUntil ([] { return false; }, 200);
                expectEquals (notices.size(), 1, "a wrong token was ignored silently, or flooded: "
                                                 + notices.joinIntoString (" | "));
                expect (notices.joinIntoString (" ").containsIgnoreCase ("token"), notices.joinIntoString (" | "));
                expect (! notices.joinIntoString (" ").contains (remotes.getOscAccessToken()), "the live token was shown");

                // A desk on a console dialect falling through says nothing.
                notices.clear();
                remotes.setOscDialect (1);
                expect (tablet.send (juce::OSCMessage ("/Console/Whatever", (juce::int32) 1)));
                pumpUntil ([] { return false; }, 300);
                expect (notices.isEmpty(), notices.joinIntoString (" | "));
                remotes.stopOsc();
                remotes.setHost ({});
            }
        }

        // ── NET-10 ─────────────────────────────────────────────────────────

        void surfacePortsTakeEffectOnSave()
        {
            beginTest ("NET-10: saving new MIDI ports for the surface that is on reopens it on them");
            {
                auto first  = juce::MidiOutput::createNewDevice ("ZF NetFix Surface A");
                auto second = juce::MidiOutput::createNewDevice ("ZF NetFix Surface B");
                expect (first != nullptr && second != nullptr, "fixture: no virtual MIDI sources");
                if (first == nullptr || second == nullptr) return;
                const auto listed = [] (const juce::String& name)
                {
                    for (const auto& d : juce::MidiInput::getAvailableDevices())
                        if (d.name == name) return true;
                    return false;
                };
                expect (pumpUntil ([&] { return listed ("ZF NetFix Surface A") && listed ("ZF NetFix Surface B"); }, 3000),
                        "fixture: the virtual sources were not listed");

                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setStripCount (2);
                const auto& mackie = cs_detail::protocols()[5];
                updateSettings (engine.getAppProps(), [] (juce::PropertiesFile& p)
                {
                    p.setValue ("cs_mackie_midiin", "ZF NetFix Surface A");
                    p.setValue ("cs_mackie_midiout", juce::String());
                });
                expectEquals (cs_detail::setProtoEnabled (engine, mackie, true), juce::String(), "fixture: no surface");

                {
                    cs_detail::MidiSurfaceSettings panel (engine, mackie);
                    auto* inBox = findChild<juce::ComboBox> (panel);
                    expect (inBox != nullptr, "fixture: no MIDI input picker");
                    if (inBox != nullptr)
                        for (int i = 0; i < inBox->getNumItems(); ++i)
                            if (inBox->getItemText (i) == "ZF NetFix Surface B")
                                inBox->setSelectedItemIndex (i, juce::dontSendNotification);
                    if (auto* save = findButton (panel, "Save")) save->onClick();
                }
                expectEquals (engine.getRemotes().getActiveSurfaceKey(), juce::String ("mackie"), "the surface went off");

                // A press on the new port reaches the session.
                second->sendMessageNow (juce::MidiMessage::noteOn (1, mcu::buttonNote (mcu::Action::Mute, 0), (juce::uint8) 127));
                expect (pumpUntil ([&] { return engine.getRecorder().getTrack (0).muted.load(); }, 2000),
                        "the surface still listens on the old port");
                engine.getRemotes().disableControlSurface();
                engine.clearAllStripOverrides();
            }
        }

        // ── NET-11 ─────────────────────────────────────────────────────────

        bool loseTcpLink (ConsoleLink& link)
        {
            juce::StreamingSocket desk;
            if (! desk.createListener (0, "127.0.0.1")) return false;
            if (! link.connect ("127.0.0.1", desk.getBoundPort())) return false;
            {
                std::unique_ptr<juce::StreamingSocket> wire (desk.waitForNextConnection());
            }
            desk.close();
            return pumpUntil ([&] { link.timerCallback(); return ! link.isConnected(); }, 4000)
                && link.wasLinkLost();
        }

        void aLostLinkCanBeCleared (MainComponent& main)
        {
            beginTest ("NET-11: CONSOLE LINK LOST clears on a deliberate disconnect or a profile change, not on a failed retry");
            {
                ConsoleLink link;
                link.setProfile (ConsoleProfile::Kind::Yamaha);
                expect (loseTcpLink (link), "fixture: the link was not lost");

                juce::StreamingSocket closed;
                expect (closed.createListener (0, "127.0.0.1"));
                const int deadPort = closed.getBoundPort();
                closed.close();
                expect (! link.connect ("127.0.0.1", deadPort), "fixture: a closed port took the link");
                expect (link.wasLinkLost(), "a failed retry cleared the loss");

                link.disconnect();
                expect (! link.wasLinkLost(), "a deliberate disconnect left CONSOLE LINK LOST up");

                expect (loseTcpLink (link), "fixture: the link was not lost again");
                link.setProfile (ConsoleProfile::Kind::BehringerX32);
                expect (! link.wasLinkLost(), "another console left the old one's loss up");
            }

            beginTest ("NET-11: the connect prompt can dismiss a lost link");
            {
                auto& link = main.consoleLink;
                link.setProfile (ConsoleProfile::Kind::Yamaha);
                expect (loseTcpLink (link), "fixture: the link was not lost");
                main.promptConsoleConnect();
                juce::AlertWindow* prompt = nullptr;
                auto& modals = *juce::ModalComponentManager::getInstance();
                for (int i = 0; i < modals.getNumModalComponents() && prompt == nullptr; ++i)
                    prompt = dynamic_cast<juce::AlertWindow*> (modals.getModalComponent (i));
                expect (prompt != nullptr, "fixture: the connect prompt did not open");
                juce::Button* dismiss = nullptr;
                if (prompt != nullptr)
                    for (int i = 0; i < prompt->getNumButtons() && dismiss == nullptr; ++i)
                        if (prompt->getButton (i)->getButtonText().containsIgnoreCase ("dismiss"))
                            dismiss = prompt->getButton (i);
                expect (dismiss != nullptr, "the prompt has no way to dismiss the lost link");
                if (dismiss != nullptr) dismiss->triggerClick();
                else modals.cancelAllModalComponents();
                pumpUntil ([&] { return ! link.wasLinkLost(); }, 2000);
                expect (! link.wasLinkLost(), "CONSOLE LINK LOST could not be dismissed");
                modals.cancelAllModalComponents();
                pumpUntil ([] { return false; }, 100);
                link.disconnect();
                link.setProfile (ConsoleProfile::Kind::BehringerX32);
            }
        }
    };

    static NetFix20261116Tests netFix20261116Tests;
}
