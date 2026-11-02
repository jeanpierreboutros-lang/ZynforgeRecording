// What the app believes about things it reaches over a wire: the soundcheck
// patch of the strips and of the desk, and what a remote may do to the
// transport. Most of it comes down to one rule: the state on screen follows
// what the desk or the engine actually did, not what was asked of it.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/PreflightDialog.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/MidiControlSurface.h"
#include "../Audio/VirtualSoundcheck.h"
#include "../Network/ConsoleLink.h"
#include "../Network/DeskMacro.h"

#include <CoreFoundation/CoreFoundation.h>
#include <functional>
#include <map>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        const char* const kInBlocks[] { "/config/routing/IN/1-8",  "/config/routing/IN/9-16",
                                        "/config/routing/IN/17-24", "/config/routing/IN/25-32" };

        juce::File scratchDir()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-netstate-" + juce::Uuid().toString());
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

        // Plays an X32 as the link hears one: routing and head-amps that writes
        // change, and answers that arrive later, as they do on a network.
        struct FakeDesk
        {
            std::vector<int> routing { 4, 5, 6, 7 };
            std::map<int, float> headamps;
            bool deaf { false };         // takes no write, still answers
            int  lostWriteBlock { -1 };  // this one write never arrived
            bool silent { false };       // answers nothing, as a wrong address
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
                    if (m.hasArgs())
                    {
                        if (! deaf && block != lostWriteBlock) routing[(size_t) block] = m.intArg();
                    }
                    else if (! silent)
                        replies.push_back (ConsoleMessage (m.address, { routing[(size_t) block] }));
                }
                else if (const int amp = headampOf (m.address); amp >= 0)
                {
                    if (m.hasArgs())
                    {
                        if (! deaf) headamps[amp] = m.floatArg();
                    }
                    else if (! silent)
                        replies.push_back (ConsoleMessage (m.address, { (double) headamps[amp] }));
                }
            }

            void answer()
            {
                auto pending = std::move (replies);
                replies.clear();
                for (const auto& r : pending) link->injectReply (r);
            }

            std::vector<int> writesSeen() const
            {
                std::vector<int> values;
                for (const auto& m : wire)
                    if (blockOf (m.address) >= 0 && m.hasArgs()) values.push_back (m.intArg());
                return values;
            }
        };

        // A deliberately false file: what a session saved by the broken build
        // or a hand edit could hold.
        juce::var stateFile (const juce::String& deskPatch, std::initializer_list<int> blocks)
        {
            juce::DynamicObject::Ptr state (new juce::DynamicObject());
            juce::Array<juce::var> patch;
            for (int value : blocks) patch.add (value);
            state->setProperty ("stagePatch", patch);
            if (deskPatch.isNotEmpty()) state->setProperty ("deskPatch", deskPatch);
            return juce::var (state.get());
        }

        juce::String fileDeskPatch (const juce::File& dir)
        {
            return juce::JSON::parse (dir.getChildFile (ConsoleLink::kStateFileName))
                       .getProperty ("deskPatch", "").toString();
        }
    }

    class NetworkStateTests final : public juce::UnitTest
    {
    public:
        NetworkStateTests() : juce::UnitTest ("Network state", "zynforge") {}

        using Patch = ConsoleLink::Patch;

        struct Bench
        {
            MainComponent& main;
            juce::File session { scratchDir() };

            explicit Bench (MainComponent& m, int tracks = 3) : main (m)
            {
                recordBaseTake (session, tracks, 200);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Bench()
            {
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                pumpUntil ([this] { return ! engine().isRecording() && ! main.captureMetadataPending
                                           && main.pendingMetadataSaves == 0; });
                main.consoleLink.setMessageHook (nullptr);
                main.consoleLink.disconnect();
                engine().stopPlayback();
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
            TrackState& track (int i) { return recorder().getTrack (i); }
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

            cuesKeepTheShowPatch (main);
            aRemoteCannotStartOrLandInAPunch (main);
            aDeskThatGoesAway (main);
            deskWithNoSoundcheckState (main);
            restoreIsConfirmedByTheDesk();
            gainsAreConfirmedByTheDesk();
            desksStateOutlivesLinkAndSession();
            checkTheDesk();
            damagedPatchFile();
            aSoundcheckReadUnderATake();
            aCaptureLeftOpen();
        }

        // ── Cues and Virtual Soundcheck ────────────────────────────────────

        void cuesKeepTheShowPatch (MainComponent& main)
        {
            beginTest ("A cue recalled during VSC keeps the strips on their input channels");
            {
                Bench b (main);
                for (int t = 0; t < 3; ++t)
                {
                    b.engine().setTrackInputRouting (t, t + 3);
                    b.engine().setTrackOutputRouting (t, -1);
                }
                main.currentCueIndex = -1;
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                vsc::repatch (b.engine());
                for (int t = 0; t < 3; ++t) expectEquals (b.track (t).outputRouting.load(), t + 3);

                main.jumpToCue (0);
                for (int t = 0; t < 3; ++t)
                    expectEquals (b.track (t).outputRouting.load(), t + 3,
                                  "a recall threw the strip off its input channel with VSC still lit");
                expect (vsc::hasSavedPatch (b.engine()));

                vsc::restore (b.engine());
                for (int t = 0; t < 3; ++t)
                    expectEquals (b.track (t).outputRouting.load(), -1, "Restore did not give the cue's patch");
            }

            beginTest ("A cue recalled during VSC replaces what Restore puts back");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                b.engine().setTrackOutputRouting (0, 5);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                b.engine().setTrackOutputRouting (0, 6);
                vsc::repatch (b.engine());
                expectEquals (b.track (0).outputBeforeVsc.load(), 6);
                main.jumpToCue (0);
                expectEquals (b.track (0).outputBeforeVsc.load(), 5);
                vsc::restore (b.engine());
                expectEquals (b.track (0).outputRouting.load(), 5);
            }

            beginTest ("A cue taken during VSC stores the show patch, so a later recall is clean");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                b.engine().setTrackOutputRouting (0, 5);
                vsc::repatch (b.engine());

                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();
                expectEquals (main.cues[0].strips[0].outputRouting, 5, "the soundcheck patch was kept as the show's");

                vsc::restore (b.engine());
                b.engine().setTrackOutputRouting (0, 7);
                main.jumpToCue (0);
                expectEquals (b.track (0).outputRouting.load(), 5);
                expect (! vsc::hasSavedPatch (b.engine()));
            }

            beginTest ("During VSC a strip follows a cue's input, so outputs still match inputs");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                b.engine().setTrackInputRouting (0, 6);
                vsc::repatch (b.engine());
                expectEquals (b.track (0).outputRouting.load(), 6);
                main.jumpToCue (0);
                expectEquals (b.track (0).inputRouting.load(), 3);
                expectEquals (b.track (0).outputRouting.load(), 3, "VSC reads on, but the output left the input");
            }

            beginTest ("A strip added after the soundcheck takes a cue as usual");
            {
                Bench b (main);
                b.engine().setTrackOutputRouting (0, 5);
                b.engine().setTrackOutputRouting (1, 6);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                vsc::repatch (b.engine());
                b.track (1).outputBeforeVsc.store (TrackState::kNoSavedOutput);
                b.engine().setTrackOutputRouting (1, 2);
                main.jumpToCue (0);
                expectEquals (b.track (1).outputRouting.load(), 6);
            }
        }

        // ── A remote and a punch ───────────────────────────────────────────

        static constexpr juce::int64 kRangeIn = 20000, kRangeOut = 40000;

        static void armPunch (Bench& b)
        {
            b.engine().setTrackArmed (0, true);
            b.engine().setPunchModeOn (true);
            b.engine().getPlayer().selectRange (kRangeIn, kRangeOut);
            b.engine().getPlayer().setPositionSamples (1000);
        }

        void aRemoteCannotStartOrLandInAPunch (MainComponent& main)
        {
            const auto press = [] (MidiControlSurface& surface, int note)
            { surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::noteOn (1, note, (juce::uint8) 127)); };

            beginTest ("Surface PLAY obeys PUNCH mode like every other remote");
            {
                Bench b (main);
                armPunch (b);
                MidiControlSurface surface (b.engine());
                press (surface, mcu::Play);
                expect (! b.engine().isPlaying(), "the surface rolled toward a punch");
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());

                b.engine().setPunchModeOn (false);
                press (surface, mcu::Play);
                expect (b.engine().isPlaying(), "the surface can't start playback at all");
                press (surface, mcu::Play);
                expect (! b.engine().isPlaying(), "PLAY on the surface stopped toggling");
            }

            beginTest ("Under PUNCH mode the surface can still stop what the host started");
            {
                Bench b (main);
                armPunch (b);
                b.engine().startPlayback();
                MidiControlSurface surface (b.engine());
                press (surface, mcu::Play);
                expect (! b.engine().isPlaying());
            }

            beginTest ("The jog wheel can't land inside the PUNCH range");
            {
                Bench b (main);
                armPunch (b);
                auto& player = b.engine().getPlayer();
                MidiControlSurface surface (b.engine());
                const auto jog = [&]
                { surface.handleIncomingMidiMessage (nullptr, juce::MidiMessage::controllerEvent (1, mcu::kJogCc, 1)); };

                player.setPositionSamples (kRangeIn - 100);
                jog();
                expectEquals (player.getPositionSamples(), kRangeIn - 100, "the wheel landed in the range");
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());

                player.setPositionSamples (1000);
                jog();
                expect (player.getPositionSamples() > 1000, "the wheel stopped working outside the range");
            }

            beginTest ("A desk macro marker jump can't land inside the PUNCH range");
            {
                Bench b (main);
                armPunch (b);
                auto& player = b.engine().getPlayer();
                b.engine().getMarkers().drop (kRangeIn + 500, "in the range");
                b.engine().getMarkers().drop (kRangeOut + 500, "after it");
                DeskMacroListener macro (b.engine());
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/next"), 10000.0);
                expectEquals (player.getPositionSamples(), (juce::int64) 1000, "a macro landed in the range");
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());

                player.setPositionSamples (kRangeOut);
                macro.dispatchForTest (juce::OSCMessage ("/zynforge/macro/marker/next"), 20000.0);
                expectEquals (player.getPositionSamples(), kRangeOut + 500, "a jump outside the range was refused");
            }

            beginTest ("Scene Follow can't land inside the PUNCH range");
            {
                Bench b (main);
                armPunch (b);
                auto& player = b.engine().getPlayer();
                b.engine().getMarkers().drop (kRangeIn + 500, "Verse");
                b.engine().setSceneFollowEnabled (true);
                const auto action = b.engine().getRemotes().onConsoleScene (1, "Verse", scenefollow::Path::ConsoleLink);
                b.engine().setSceneFollowEnabled (false);
                expect (action == scenefollow::Action::Refused);
                expectEquals (player.getPositionSamples(), (juce::int64) 1000, "Follow landed in the range");
            }

            beginTest ("The rule itself: a range only counts in PUNCH mode, and a marker past it follows");
            {
                using namespace scenefollow;
                Marker verse;
                verse.sampleOffset = 500;
                verse.name = "Verse";
                std::vector<Marker> markers { verse };
                Facts f;
                f.followOn = true;
                f.playerLoaded = true;
                expect (decide (f, markers, "Verse", 0).action == Action::Locate);
                f.punchRange = juce::Range<juce::int64> (400, 600);
                expect (decide (f, markers, "Verse", 0).action == Action::Refused);
                f.punchRange = juce::Range<juce::int64> (600, 800);
                expect (decide (f, markers, "Verse", 0).action == Action::Locate);
            }

            beginTest ("Timecode chase holds in PUNCH mode, and says so");
            {
                Bench b (main);
                armPunch (b);
                auto& e = b.engine();
                e.setChaseMode (AudioEngine::ChaseMode::Mtc);
                e.setChaseStartTimecode ("00:00:00:00");
                e.getTimecodeChase().feedMtcFullFrame (0, 0, 0, 12, 25.0f);
                expect (e.isChaseLive(), "fixture: chase is not live");
                main.serviceTimecodeChase();
                expect (! e.isPlaying(), "chase rolled toward a punch");
                expectEquals (e.getPlayer().getPositionSamples(), (juce::int64) 1000, "chase moved the playhead");
                expect (main.statusLabel.getText().contains ("PUNCH"), main.statusLabel.getText());
                e.setChaseMode (AudioEngine::ChaseMode::Off);
                main.serviceTimecodeChase();
            }
        }

        // ── Is anyone there ────────────────────────────────────────────────

        void aDeskThatGoesAway (MainComponent& main)
        {
            using Health = ConsoleLink::Health;
            const auto answer = [] (ConsoleLink& link) { link.injectReply (ConsoleMessage ("/info", { juce::String ("X32") })); };

            beginTest ("A desk that stops answering is reported once, and again when it returns");
            {
                ConsoleLink link;
                juce::StringArray said;
                link.onStatus = [&] (const juce::String& s) { said.add (s); };
                link.setMessageHook ([] (const ConsoleMessage&) {});
                expect (link.getHealth() == Health::NotAnswered);
                answer (link);
                expect (link.getHealth() == Health::Answering);
                link.timerCallback();

                said.clear();
                link.lastAnswerMs -= ConsoleLink::kDeskSilentMs + 1000;
                link.timerCallback();
                expect (link.getHealth() == Health::Silent);
                expect (link.isConnected(), "a desk may come back, so the link stays");
                expectEquals (said.size(), 1);
                expect (said[0].containsIgnoreCase ("stopped answering"), said[0]);

                link.timerCallback();
                expectEquals (said.size(), 1, "the notice repeats every second");

                answer (link);
                link.timerCallback();
                expect (link.getHealth() == Health::Answering);
                expect (said.joinIntoString (" ").containsIgnoreCase ("answering again"), said.joinIntoString (" "));
            }

            beginTest ("The probe goes out again while the link is up, and only the probe");
            {
                ConsoleLink link;
                std::vector<ConsoleMessage> sent;
                link.setMessageHook ([&] (const ConsoleMessage& m) { sent.push_back (m); });
                link.nextProbeMs = 1;
                link.timerCallback();
                expectEquals ((int) sent.size(), 1);
                if (! sent.empty()) expectEquals (sent[0].address, juce::String ("/info"));
                link.timerCallback();
                expectEquals ((int) sent.size(), 1, "the probe floods the desk");
            }

            beginTest ("A desk that never answered is called that, not lost");
            {
                ConsoleLink link;
                juce::StringArray said;
                link.onStatus = [&] (const juce::String& s) { said.add (s); };
                link.setMessageHook ([] (const ConsoleMessage&) {});
                link.connectedAtMs = juce::Time::currentTimeMillis() - ConsoleLink::kDeskSilentMs - 1000;
                link.timerCallback();
                link.timerCallback();
                expect (link.getHealth() == Health::NotAnswered);
                expectEquals (said.size(), 1);
                expect (said[0].containsIgnoreCase ("has not answered"), said[0]);
            }

            beginTest ("A link that reads nothing says it can't tell, on every such profile");
            {
                int checked = 0;
                for (const auto& p : consoleProfiles())
                    if (! p.dialect.probe)
                    {
                        ++checked;
                        ConsoleLink link;
                        link.setProfile (p.kind);
                        link.setMessageHook ([] (const ConsoleMessage&) {});
                        expect (link.getHealth() == Health::CannotTell, p.displayName);
                        expect (link.healthText().containsIgnoreCase ("can't tell"), link.healthText());
                    }
                expect (checked >= 2, "SSL and Generic have no probe");
            }

            beginTest ("A desk gone during a take raises a notice, not a danger, and pre-flight shows it");
            {
                Bench b (main);
                auto& link = main.consoleLink;
                link.setMessageHook ([] (const ConsoleMessage&) {});
                link.injectReply (ConsoleMessage ("/info", { juce::String ("X32") }));
                EngineStatus rolling;
                rolling.recording = true;

                main.updateCaptureWarnings (rolling);
                expect (! main.alarmBanner.getText().contains ("CONSOLE"), "a healthy desk raised an alarm");
                expect (main.deskLinkRow().status == PreflightRow::Status::ok);

                link.lastAnswerMs -= ConsoleLink::kDeskSilentMs + 1000;
                main.updateCaptureWarnings (rolling);
                expect (main.alarmBanner.getText().contains ("CONSOLE NOT ANSWERING"), main.alarmBanner.getText());
                expect (main.alarmBanner.getLevel() == AlarmBanner::Level::Notice, "a console can't threaten the take");
                expect (main.deskLinkRow().isProblem());

                main.updateCaptureWarnings (EngineStatus());
            }

            beginTest ("A desk that never answered raises nothing during a take");
            {
                Bench b (main);
                main.consoleLink.setProfile (ConsoleProfile::Kind::DiGiCo);
                main.consoleLink.setMessageHook ([] (const ConsoleMessage&) {});
                main.consoleLink.connectedAtMs = 1;
                EngineStatus rolling;
                rolling.recording = true;
                main.updateCaptureWarnings (rolling);
                expect (! main.alarmBanner.getText().contains ("CONSOLE"), "a false alarm for the whole show");
                main.updateCaptureWarnings (EngineStatus());
                main.consoleLink.setMessageHook (nullptr);
                main.consoleLink.disconnect();
                main.consoleLink.setProfile (ConsoleProfile::Kind::BehringerX32);
            }
        }

        // ── A desk that cannot repatch ─────────────────────────────────────

        void deskWithNoSoundcheckState (MainComponent& main)
        {
            const auto cannotRepatch = []
            {
                std::vector<ConsoleProfile> profiles;
                for (const auto& p : consoleProfiles())
                    if (! p.canRepatch) profiles.push_back (p);
                return profiles;
            }();

            beginTest ("Every profile that can't repatch has no soundcheck state");
            {
                expect (cannotRepatch.size() >= 6, "the test lost a profile");
                for (const auto& p : cannotRepatch)
                {
                    const auto dir = scratchDir();
                    ConsoleLink link;
                    link.setProfile (p.kind);
                    link.setMessageHook ([] (const ConsoleMessage&) {});
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " lit on connect");
                    expect (! link.hasStagePatch(), p.displayName + " claims a saved patch");

                    // Whatever a file says.
                    link.loadJson (stateFile ("soundcheck", {}));
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " believed a file");
                    expect (link.getPatch() == Patch::Unknown, p.displayName);

                    expect (atomicfile::writeText (dir.getChildFile (ConsoleLink::kStateFileName),
                                                   juce::JSON::toString (stateFile ("soundcheck", { 1, 2, 3, 4 }))));
                    link.setSessionDir (dir);
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " believed a session file");
                    link.confirmDialectForTests();
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " lit on the handshake");
                    link.disconnect();
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " lit on disconnect");

                    link.setSessionDir ({});
                    expect (! link.isOnSoundcheckPatch(), p.displayName + " lit leaving a session");
                    dir.deleteRecursively();
                }
            }

            beginTest ("A connect to a desk with no repatch leaves the chip dark and says what can't be seen");
            {
                Bench b (main);
                for (const auto& p : cannotRepatch)
                {
                    main.consoleLink.setProfile (p.kind);
                    main.consoleLink.setMessageHook ([] (const ConsoleMessage&) {});
                    main.updateSoundcheckState();
                    expect (! main.soundcheckPill.isVisible(), "DESK ON SOUNDCHECK lit for " + p.displayName);

                    const auto row = main.deskPatchRow();
                    expect (! row.isProblem(), p.displayName + " shows a problem in pre-flight");
                    expect (row.status != PreflightRow::Status::ok, p.displayName + " gets a tick nothing proved");
                    expect (row.value.containsIgnoreCase ("not visible"), row.value);
                    main.consoleLink.setMessageHook (nullptr);
                    main.consoleLink.disconnect();
                }
                main.consoleLink.setProfile (ConsoleProfile::Kind::BehringerX32);
            }

            beginTest ("A desk that can repatch says it has not been read until it has");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                expect (! link.isOnSoundcheckPatch());
                expect (link.deskPatchText().containsIgnoreCase ("not read"), link.deskPatchText());
            }
        }

        // ── STAGE needs the desk's word ────────────────────────────────────

        static void toSoundcheck (ConsoleLink& link, FakeDesk& desk)
        {
            link.enterSoundcheck();
            desk.answer();
        }

        void restoreIsConfirmedByTheDesk()
        {
            beginTest ("STAGE clears only when every block reads back the show patch");
            {
                const auto dir = scratchDir();
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.setSessionDir (dir);
                toSoundcheck (link, desk);
                expect (link.isOnSoundcheckPatch());
                expect (desk.routing == std::vector<int> ({ 16, 17, 18, 19 }));

                link.exitSoundcheck();
                expect (link.isOnSoundcheckPatch(), "the chip cleared before the desk answered");
                expectEquals (fileDeskPatch (dir), juce::String ("soundcheck"), "saved as restored on a send");
                desk.answer();
                expect (! link.isOnSoundcheckPatch());
                expect (link.getPatch() == Patch::Stage);
                expectEquals (fileDeskPatch (dir), juce::String ("stage"));
                dir.deleteRecursively();
            }

            beginTest ("A desk that never answers leaves SOUNDCHECK standing, and says so");
            {
                const auto dir = scratchDir();
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                link.setSessionDir (dir);
                toSoundcheck (link, desk);

                desk.silent = true;
                link.exitSoundcheck();
                desk.answer();
                link.patchDeadlineMs = 1;
                link.timerCallback();
                expect (link.isOnSoundcheckPatch(), "the record of the desk is gone");
                expectEquals (fileDeskPatch (dir), juce::String ("soundcheck"));
                expect (status.containsIgnoreCase ("did not confirm"), status);
                expect (status.contains ("SOUNDCHECK"), status);
                dir.deleteRecursively();
            }

            beginTest ("One lost write leaves SOUNDCHECK standing");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                toSoundcheck (link, desk);

                desk.lostWriteBlock = 2;
                link.exitSoundcheck();
                desk.answer();
                expect (link.isOnSoundcheckPatch(), "one block is still on card returns");
                expect (status.contains ("1 of 4"), status);

                // Pressing it again is the way out.
                desk.lostWriteBlock = -1;
                link.exitSoundcheck();
                desk.answer();
                expect (! link.isOnSoundcheckPatch());
                expect (desk.routing == std::vector<int> ({ 4, 5, 6, 7 }));
            }

            beginTest ("A desk that ignores the writes leaves SOUNDCHECK standing");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                toSoundcheck (link, desk);
                desk.deaf = true;
                link.exitSoundcheck();
                desk.answer();
                expect (link.isOnSoundcheckPatch());
            }

            beginTest ("A partial read-back never confirms, even with a repeat of one block");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                toSoundcheck (link, desk);
                link.exitSoundcheck();
                desk.replies.clear();
                for (int i = 0; i < 4; ++i)
                    link.injectReply (ConsoleMessage (kInBlocks[0], { 4 }));
                expect (link.isOnSoundcheckPatch(), "a repeated block finished the read");
            }
        }

        void gainsAreConfirmedByTheDesk()
        {
            const auto captured = [] (ConsoleLink& link, FakeDesk& desk)
            {
                desk.headamps = { { 0, 0.25f }, { 1, 0.5f } };
                link.captureGains (2);
                desk.answer();
            };

            beginTest ("Restore Gains reports success only when the desk reads the gains back");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                captured (link, desk);
                desk.headamps = { { 0, 0.75f }, { 1, 0.75f } };

                link.restoreGains();
                expect (! status.containsIgnoreCase ("restored"), status);
                desk.answer();
                expect (status.containsIgnoreCase ("confirmed"), status);
                expect (status.contains ("2 head-amp gains"), status);
            }

            beginTest ("A desk that keeps its old gains is reported, by count");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                captured (link, desk);
                desk.headamps = { { 0, 0.75f }, { 1, 0.75f } };
                desk.deaf = true;
                link.restoreGains();
                desk.answer();
                expect (status.contains ("did not take 2 of 2"), status);
                expect (! status.containsIgnoreCase ("restored"), status);
            }

            beginTest ("A desk that says nothing leaves Restore Gains unconfirmed");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                captured (link, desk);
                desk.silent = true;
                link.restoreGains();
                link.gainDeadlineMs = 1;
                link.timerCallback();
                expect (status.containsIgnoreCase ("did not confirm"), status);
            }
        }

        // ── One state across links, sessions and relaunches ────────────────

        void desksStateOutlivesLinkAndSession()
        {
            beginTest ("A clean return to STAGE stays clean when the link closes");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                toSoundcheck (link, desk);
                link.exitSoundcheck();
                desk.answer();
                link.disconnect();
                expect (! link.isOnSoundcheckPatch(), "disconnect lit SOUNDCHECK over a desk on stage");
                expect (link.getPatch() == Patch::Stage);
            }

            beginTest ("A dropped link does not forget a desk on card returns");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                toSoundcheck (link, desk);
                link.disconnect();
                expect (link.isOnSoundcheckPatch());
            }

            beginTest ("Another session's file cannot clear a desk on card returns");
            {
                const auto a = scratchDir(), b = scratchDir();
                {
                    ConsoleLink earlier;
                    FakeDesk desk;
                    desk.attach (earlier);
                    earlier.setSessionDir (b);
                    toSoundcheck (earlier, desk);
                    earlier.exitSoundcheck();
                    desk.answer();
                    expectEquals (fileDeskPatch (b), juce::String ("stage"));
                }
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.setSessionDir (a);
                toSoundcheck (link, desk);
                link.disconnect();
                link.setSessionDir (b);
                desk.attach (link);
                expect (link.isOnSoundcheckPatch(), "the chip went out with the desk on card returns");

                desk.wire.clear();
                link.enterSoundcheck();
                expect (desk.wire.empty(), "a second soundcheck read the card returns as the show patch");

                link.exitSoundcheck();
                expect (desk.writesSeen() == std::vector<int> ({ 4, 5, 6, 7 }), "the way back was lost");
                a.deleteRecursively();
                b.deleteRecursively();
            }

            beginTest ("A relaunch keeps a desk on card returns through a session change");
            {
                const auto a = scratchDir(), b = scratchDir();
                {
                    ConsoleLink earlier;
                    FakeDesk desk;
                    desk.attach (earlier);
                    earlier.setSessionDir (a);
                    toSoundcheck (earlier, desk);
                }
                ConsoleLink relaunched;
                relaunched.setSessionDir (a);
                expect (relaunched.isOnSoundcheckPatch());
                relaunched.setSessionDir (b);
                expect (relaunched.isOnSoundcheckPatch(), "New Session forgot the desk");
                relaunched.setSessionDir ({});
                expect (relaunched.isOnSoundcheckPatch(), "closing the session forgot the desk");

                // And the session open at the next relaunch knows too.
                ConsoleLink again;
                again.setSessionDir (b);
                expect (again.isOnSoundcheckPatch(), "a relaunch in the second session forgot the desk");
                a.deleteRecursively();
                b.deleteRecursively();
            }

            beginTest ("STAGE restored from another session reaches the file that said SOUNDCHECK");
            {
                const auto a = scratchDir(), b = scratchDir();
                ConsoleLink link;
                FakeDesk desk;
                desk.attach (link);
                link.setSessionDir (a);
                toSoundcheck (link, desk);
                link.setSessionDir (b);
                link.exitSoundcheck();
                desk.answer();
                expectEquals (fileDeskPatch (b), juce::String ("stage"));
                expectEquals (fileDeskPatch (a), juce::String ("stage"), "session A still says soundcheck");

                ConsoleLink reopened;
                reopened.setSessionDir (a);
                expect (! reopened.isOnSoundcheckPatch(), "reopening A lit the chip over a desk on stage");
                a.deleteRecursively();
                b.deleteRecursively();
            }

            beginTest ("A file from before the state was saved counts as a soundcheck not known to have ended");
            {
                ConsoleLink link;
                expect (link.loadJson (stateFile ({}, { 4, 5, 6, 7 })));
                expect (link.isOnSoundcheckPatch());
            }
        }

        void checkTheDesk()
        {
            beginTest ("A desk put back by hand clears SOUNDCHECK with nothing written to it");
            {
                const auto dir = scratchDir();
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                link.setSessionDir (dir);
                toSoundcheck (link, desk);

                desk.routing = { 0, 1, 2, 3 };   // today's patch, not the saved one
                desk.wire.clear();
                link.checkDeskPatch();
                desk.answer();
                expect (! link.isOnSoundcheckPatch());
                expect (desk.writesSeen().empty(), "the check overwrote today's routing");
                expect (desk.routing == std::vector<int> ({ 0, 1, 2, 3 }));
                expectEquals (fileDeskPatch (dir), juce::String ("stage"));
                expect (status.containsIgnoreCase ("nothing was written"), status);
                dir.deleteRecursively();
            }

            beginTest ("A desk still partly on card returns stays on SOUNDCHECK");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                toSoundcheck (link, desk);
                desk.routing = { 4, 5, 6, 19 };
                link.checkDeskPatch();
                desk.answer();
                expect (link.isOnSoundcheckPatch());
                expect (status.contains ("1 of 4"), status);
            }

            beginTest ("A show patch that uses the card is not read as a soundcheck");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.routing = { 16, 5, 6, 7 };   // block 1 plays from the card at the show
                desk.attach (link);
                toSoundcheck (link, desk);
                link.exitSoundcheck();
                desk.answer();
                link.checkDeskPatch();
                desk.answer();
                expect (! link.isOnSoundcheckPatch());
            }

            beginTest ("A desk that doesn't answer the check leaves the state alone");
            {
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                toSoundcheck (link, desk);
                desk.silent = true;
                link.checkDeskPatch();
                link.patchDeadlineMs = 1;
                link.timerCallback();
                expect (link.isOnSoundcheckPatch());
                expect (status.containsIgnoreCase ("did not answer"), status);
            }

            beginTest ("A saved SOUNDCHECK is put to the desk once when the link is up");
            {
                const auto dir = scratchDir();
                {
                    ConsoleLink earlier;
                    FakeDesk desk;
                    desk.attach (earlier);
                    earlier.setSessionDir (dir);
                    toSoundcheck (earlier, desk);
                }
                ConsoleLink link;
                FakeDesk desk;
                desk.routing = { 4, 5, 6, 7 };
                desk.attach (link);
                link.setSessionDir (dir);
                expect (link.isOnSoundcheckPatch());
                link.timerCallback();
                desk.answer();
                expect (! link.isOnSoundcheckPatch(), "the desk said it was on stage");
                expectEquals (fileDeskPatch (dir), juce::String ("stage"));

                desk.wire.clear();
                link.timerCallback();
                expect (desk.wire.empty(), "the claim is asked about once");
                dir.deleteRecursively();
            }

            beginTest ("The check needs a desk that can answer");
            {
                ConsoleLink link;
                link.setProfile (ConsoleProfile::Kind::DiGiCo);
                juce::String status;
                int sent = 0;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                link.setMessageHook ([&] (const ConsoleMessage&) { ++sent; });
                link.checkDeskPatch();
                expectEquals (sent, 0);
                expect (status.containsIgnoreCase ("cannot report"), status);
            }
        }

        void damagedPatchFile()
        {
            beginTest ("A saved patch outside the desk's range never reaches the desk");
            {
                for (const auto& bad : { std::vector<int> { 4, 5, 6, 999 }, std::vector<int> { 4, 5, 6, 20 },
                                         std::vector<int> { -7, 5, 6, 7 } })
                {
                    ConsoleLink link;
                    FakeDesk desk;
                    juce::String status;
                    link.onStatus = [&] (const juce::String& s) { status = s; };
                    desk.attach (link);
                    juce::DynamicObject::Ptr state (new juce::DynamicObject());
                    juce::Array<juce::var> patch;
                    for (int value : bad) patch.add (value);
                    state->setProperty ("stagePatch", patch);
                    state->setProperty ("deskPatch", "soundcheck");
                    link.loadJson (juce::var (state.get()));

                    expect (! link.hasStagePatch(), "a bad value became the way back");
                    expect (status.containsIgnoreCase ("damaged"), status);
                    link.exitSoundcheck();
                    expect (desk.writesSeen().empty(), "a bad value went to the desk");
                }
            }

            beginTest ("A value that isn't a number is damage too, not routing 0");
            {
                ConsoleLink link;
                juce::DynamicObject::Ptr state (new juce::DynamicObject());
                juce::Array<juce::var> patch { 4, 5, 6, juce::var ("seven") };
                state->setProperty ("stagePatch", patch);
                link.loadJson (juce::var (state.get()));
                expect (! link.hasStagePatch());
            }

            beginTest ("A routing value the file could not reload is refused off the wire too");
            {
                ConsoleLink link;
                FakeDesk desk;
                desk.routing = { 4, 5, 6, 99 };
                desk.attach (link);
                toSoundcheck (link, desk);
                expect (! link.isOnSoundcheckPatch());
                expect (desk.routing[3] == 99, "the desk moved on a way back that can't be reloaded");
            }
        }

        void aSoundcheckReadUnderATake()
        {
            beginTest ("A SOUNDCHECK read that lands after RECORD moves nothing");
            {
                const auto dir = scratchDir();
                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                bool rolling = false;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                link.takeHoldsDesk = [&] { return rolling; };
                desk.attach (link);
                link.setSessionDir (dir);

                link.enterSoundcheck();
                rolling = true;
                desk.answer();

                expect (desk.writesSeen().empty(), "the desk moved under a take");
                expect (! link.isOnSoundcheckPatch());
                expect (! dir.getChildFile (ConsoleLink::kStateFileName).existsAsFile(),
                        "the session's drive was written during the take");
                expect (status.containsIgnoreCase ("take"), status);
                dir.deleteRecursively();
            }

            beginTest ("Every write refuses at the link during a take, whoever asks");
            {
                ConsoleLink link;
                FakeDesk desk;
                bool rolling = false;
                link.takeHoldsDesk = [&] { return rolling; };
                desk.attach (link);
                toSoundcheck (link, desk);
                desk.headamps = { { 0, 0.25f } };
                link.captureGains (1);
                desk.answer();

                rolling = true;
                desk.wire.clear();
                link.exitSoundcheck();
                link.restoreGains();
                expect (desk.wire.empty(), "a write reached the desk under a take");

                link.captureGains (1);
                expect (! desk.wire.empty(), "capturing gains is a read and stays available");
            }
        }

        void aCaptureLeftOpen()
        {
            beginTest ("Closing the link ends a head-amp capture");
            {
                const auto dir = scratchDir();
                {
                    ConsoleLink earlier;
                    FakeDesk desk;
                    desk.attach (earlier);
                    desk.headamps = { { 0, 0.5f } };
                    earlier.captureGains (1);
                    desk.answer();
                    expect (earlier.saveTo (dir));
                }

                ConsoleLink link;
                FakeDesk desk;
                juce::String status;
                link.onStatus = [&] (const juce::String& s) { status = s; };
                desk.attach (link);
                link.captureGains (2);
                link.disconnect();

                desk.attach (link);
                expect (link.loadFrom (dir));
                expectEquals ((int) link.getCapturedGains().size(), 1, "the saved capture was not read back");

                status.clear();
                link.injectReply (ConsoleMessage ("/headamp/001/gain", { 0.9 }));
                expect (! status.containsIgnoreCase ("captured"), "a pushed update finished a closed capture");
                expectEquals ((int) link.getCapturedGains().size(), 1);
                dir.deleteRecursively();
            }
        }
    };

    static NetworkStateTests networkStateTests;
}
