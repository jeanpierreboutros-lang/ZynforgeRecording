// The main window's alarms and transport presses: what the banner must never
// lose, and how a press from somewhere else stays what it is. These run on a
// silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/PatchPage.h"
#include "../UI/SessionProjPath.h"
#include "../Audio/ChannelAlarm.h"
#include "../Audio/PreflightProbes.h"
#include "../Audio/SessionBackup.h"
#include "../Audio/VirtualSoundcheck.h"
#include "SilentTestDevice.h"

#include <functional>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-mainwin-" + tag + "-" + juce::Uuid().toString());
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
                audio.emplace_back ((size_t) kBlock, 0.1f * (float) (t + 1));
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        // Keeps a metadata save open until released, as a slow drive would.
        struct SaveHeldOpen
        {
            juce::WaitableEvent entered, release;

            SaveHeldOpen()
            {
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::make_shared<const std::function<void()>> ([this] { entered.signal(); release.wait (5000); }));
            }

            ~SaveHeldOpen() { finish(); }

            void finish()
            {
                release.signal();
                std::atomic_store (&sessionbackup::beforeSnapshotForTests, std::shared_ptr<const std::function<void()>>());
            }
        };

        std::vector<AlarmBanner::Alarm> alarms (std::initializer_list<AlarmBanner::Alarm> list) { return list; }
    }

    class MainWindowFixTests final : public juce::UnitTest
    {
    public:
        MainWindowFixTests() : juce::UnitTest ("Main window fixes", "zynforge") {}

        struct Window
        {
            static inline MainComponent* shared = nullptr;

            MainComponent& main;
            juce::File session;
            juce::Array<juce::File> dirs;

            explicit Window (int tracks = 2, int blocks = 200) : main (*shared)
            {
                session = tempDir ("session");
                dirs.add (session);
                recordBaseTake (session, tracks, blocks);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Window()
            {
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.punchSessionActive = false;
                main.manualPunchActive = false;
                main.punchWindowTakeActive = false;
                main.stopArmedAtMs = 0;
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.twinLink.stop();
                main.sampleRateWarning.clear();
                main.updateCaptureWarnings (EngineStatus());
                main.lastTrackCount = -1;
                for (const auto& d : dirs) d.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            SessionPlayer& player() { return main.engine.getPlayer(); }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }

            void arm (std::initializer_list<int> tracks)
            {
                for (int t = 0; t < recorder().getNumTracks(); ++t) engine().setTrackArmed (t, false);
                for (int t : tracks) engine().setTrackArmed (t, true);
            }

            bool stopped()
            {
                return pumpUntil ([this]
                {
                    return ! engine().isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
            }

            // A save runs until `hold` lets it go.
            bool saveRunning (SaveHeldOpen& hold)
            {
                main.enqueueMetadataSave (session, {}, {}, true, true);
                return hold.entered.wait (5000) && main.pendingMetadataSaves != 0;
            }

            void saveFinishes (SaveHeldOpen& hold)
            {
                hold.finish();
                pumpUntil ([this] { return main.pendingMetadataSaves == 0; });
            }
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
            useSilentDevice (main.engine);
            main.setSize (1320, 820);
            Window::shared = &main;

            heldPresses();
            bannerHoldsEverything();
            bannerTakesTurns();
            testTone();
            nothingStandsInFrontOfStop();
            cuesDuringATake();
            stripDeleteAsks();
            combosFollowTheDevice();
            pairsTakeTwoChannels();
            questionsKeepTheirSession();
            gridKeepsItsFaders();
            headerKeepsItsWarnings();
            recordTimeWarns();
            noDriveReadsDuringATake();
            preflightHasATimeLimit();
            backupButtonIsLive();
            smallRepairs();
        }

    private:
        using Action = AudioEngine::RemoteTransportAction;
        using Level = AlarmBanner::Level;

        void heldPresses()
        {
            beginTest ("A remote RECORD during a save never comes back as a local press");
            {
                const auto refused = [this] (std::function<void (Window&)> setUp)
                {
                    Window w;
                    w.arm ({ 0 });
                    setUp (w);
                    SaveHeldOpen hold;
                    expect (w.saveRunning (hold));
                    juce::String error;
                    expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                    expect (error.containsIgnoreCase ("saving"), error);
                    expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a remote press was held");
                    w.saveFinishes (hold);
                    expect (! w.engine().isRecording(), "the remote press ran after the save");
                    expect (! w.main.punchSessionActive, "the remote press rolled a punch");
                    expect (w.main.cursorPunchQuestion == nullptr, "the remote press asked this Mac a question");
                    w.engine().stopPlayback();
                };
                refused ([] (Window& w)
                {
                    w.player().setPositionSamples (20000);
                    w.engine().startPlayback();
                });
                refused ([] (Window& w) { w.player().selectRange (1000, 2000); });
                refused ([] (Window& w) { w.engine().setEditCursorSample (20000); });
            }

            beginTest ("A twin RECORD during a save is refused, and no take follows");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                juce::String refusal;
                expect (! w.main.twinStartRecord (refusal));
                expect (refusal.isNotEmpty());
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a link press was held");
                w.saveFinishes (hold);
                expect (! w.engine().isRecording(), "the follower took a take the leader was told it refused");
            }

            beginTest ("A remote press leaves a press made here alone");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                w.main.onRecordClicked();
                expect (w.main.recordPressHeldAtMs != 0);
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                expect (w.main.recordPressHeldAtMs != 0, "the local press was lost");
                w.saveFinishes (hold);
                expect (w.engine().isRecording(), "the local press did not run");
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("Ending a punch during a save never leaves a press behind");
            {
                const auto ends = [this] (std::function<void (Window&)> press)
                {
                    Window w;
                    w.arm ({ 0 });
                    w.player().setPositionSamples (20000);
                    w.engine().startPlayback();
                    w.main.punchSessionActive = true;
                    w.engine().setPunchModeOn (true);
                    SaveHeldOpen hold;
                    expect (w.saveRunning (hold));
                    press (w);
                    expectEquals ((int) w.main.recordPressHeldAtMs, 0, "the press that ends a punch was held");
                    expect (! w.main.punchSessionActive, "the press did not end the punch");
                    expect (! w.engine().isPlaying(), "the post-roll kept rolling");
                    w.saveFinishes (hold);
                    expect (! w.engine().isRecording(), "the press started a take");
                };
                ends ([] (Window& w) { w.main.onStopClicked(); });
                ends ([] (Window& w) { w.main.onRecordClicked(); });
                ends ([] (Window& w) { juce::String e; w.engine().performRemoteTransport (Action::StopAll, e); });
            }

            beginTest ("A held press goes when STOP, a session change or the transport says so");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                w.main.onRecordClicked();
                w.main.onStopClicked();
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "STOP left the press held");
                w.main.onRecordClicked();
                w.main.forgetSessionUiState();
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a press outlived its session");
                w.main.onRecordClicked();
                w.main.recordPressHeldRolling = true;   // it was pressed while rolling, and that has stopped
                w.saveFinishes (hold);
                expect (! w.engine().isRecording(), "a press meant as a punch ran as a new part");
                expect (w.main.statusLabel.getText().contains ("press RECORD again"), w.main.statusLabel.getText());
            }
        }

        static EngineStatus quietNotice()
        {
            EngineStatus status;
            status.recording = true;
            status.channelAlarms.push_back ({});
            status.channelAlarms.back().kind = channelalarm::kNoSignal;
            status.channelAlarms.back().name = "Spare 31";
            return status;
        }

        void bannerHoldsEverything()
        {
            beginTest ("The twin going down reaches the banner past a standing notice");
            {
                Window w;
                twin::Config config;
                config.role = twin::Role::Follower;
                config.port = 0;
                config.bindAddress = "127.0.0.1";
                config.pairingCode = "7K3M-Q2XD-9HVB-T4WN";
                juce::String error;
                expect (w.main.twinLink.start (config, error), error);
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                w.main.twinLink.tick();
                expectEquals (w.main.twinLink.snapshot().headline, juce::String ("TWIN NOT LINKED"));

                w.main.updateCaptureWarnings (quietNotice());
                expect (w.main.alarmBanner.getText().contains ("TWIN NOT LINKED"), w.main.alarmBanner.getText());
                expect (w.main.alarmBanner.getLevel() == Level::Danger, "a dropped twin read as a notice");
                expect (w.main.alarmBanner.getText().startsWith ("! TWIN"), "a notice sat ahead of the trouble");
                w.main.twinLink.stop();
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("Twin trouble is danger while either Mac rolls, and only then");
            {
                Window w;
                twin::Snapshot link;
                link.state = twin::LinkState::Linked;
                link.peer = twin::MacStatus();
                link.peer->recording = true;
                link.warnings = { "SESSION NAME differs: \"a\" here, \"b\" on FOLLOWER", "FOLLOWER MISSED 12 SAMPLES" };
                link.setupWarnings = 1;
                link.level = 1;

                std::vector<AlarmBanner::Alarm> list;
                w.main.collectTwinAlarms (link, list);
                expectEquals ((int) list.size(), 2, "with the peer rolling its trouble raises the banner");
                expect (list[0].level == Level::Danger && list[0].text.contains ("MISSED"));
                expect (list[1].level == Level::Notice && list[1].text.contains ("differs"));

                twin::Snapshot lost;
                lost.state = twin::LinkState::Lost;
                lost.headline = "TWIN LOST";
                lost.detail = "no frames for 3 s";
                lost.level = 2;
                list.clear();
                w.main.collectTwinAlarms (lost, list);
                expectEquals ((int) list.size(), 1, "the peer was rolling when the link dropped");
                expect (list[0].level == Level::Danger);
                expect (list[0].text.contains ("no frames"), list[0].text);

                link.peer->recording = false;
                link.warnings = { "SESSION NAME differs: \"a\" here, \"b\" on FOLLOWER" };
                list.clear();
                w.main.collectTwinAlarms (link, list);
                expect (list.empty(), "two setups that differ held the banner up at idle");

                list.clear();
                w.main.collectTwinAlarms (lost, list);
                expect (list.empty(), "with nothing rolling the pill says it alone");
                list.push_back ({ "Something else", Level::Notice });
                w.main.collectTwinAlarms (lost, list);
                expectEquals ((int) list.size(), 2, "a banner over the pill must carry what the pill said");
                expect (list[1].level == Level::Notice);
            }

            beginTest ("The rate warning stays reachable under every other alarm");
            {
                Window w;
                w.main.pendingSampleRate = 48000.0;
                w.main.checkDeviceSampleRate (44100.0);
                dismissModals();
                expect (w.main.sampleRateWarning.contains ("MISMATCH"));
                w.main.refreshAlarmBanner();
                expect (w.main.alarmBanner.getText().contains ("recording blocked"), "the whole warning did not reach the banner");
                expect (w.main.alarmBanner.getLevel() == Level::Notice, "no take is in trouble at idle");

                w.arm ({ 0 });
                w.main.lastCaptureFinalizationError = "Recording stopped, but finalization failed: backup audio write failed";
                w.main.refreshAlarmBanner();
                expect (w.main.alarmBanner.getText().contains ("finalization failed"));
                expect (w.main.alarmBanner.getText().contains ("SAMPLE-RATE MISMATCH"), "the failure hid the rate");
                w.main.lastCaptureFinalizationError.clear();
                w.main.checkDeviceSampleRate (48000.0);
                w.main.refreshAlarmBanner();
                expect (! w.main.alarmBanner.isVisible());
            }

            beginTest ("A refused press under a standing alarm shows at once");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                status.primaryFailed = status.backupFailed = status.mirrorFailed = true;
                status.recoveryMarkerFailed = status.diskStruggling = true;
                status.mirrorsSkipped = 2;
                w.main.setSize (1120, 700);
                w.main.resized();
                w.main.updateCaptureWarnings (status);
                expect (w.main.alarmBanner.getPageCount() > 1, "this list should not fit on one page");
                w.arm ({});
                w.main.onRecordClicked();
                expect (w.main.alarmBanner.getShownText().containsIgnoreCase ("arm"), w.main.alarmBanner.getShownText());
                expect (w.main.alarmBanner.getLevel() == Level::Danger, "the take's trouble must keep its colour");
                w.main.setSize (1320, 820);
                w.main.resized();
            }
        }

        void bannerTakesTurns()
        {
            const auto paged = [] (AlarmBanner& banner, std::vector<AlarmBanner::Alarm> list)
            {
                juce::uint32 now = 1000;
                banner.show (list, now);
                juce::StringArray seen;
                for (int i = 0; i < banner.getPageCount(); ++i)
                {
                    seen.add (banner.getShownText());
                    now += AlarmBanner::kPageMs;
                    banner.show (list, now);
                }
                return seen;
            };

            beginTest ("Danger sorts ahead of a notice, whatever order it arrives in");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                banner.show (alarms ({ { "No signal yet this take: A, B", Level::Notice },
                                       { "! BACKUP WRITE FAILED", Level::Danger },
                                       { "A refused press", Level::Notice },
                                       { "! MIRROR WRITE FAILED", Level::Danger } }), 1000);
                expect (banner.getText().startsWith ("! BACKUP WRITE FAILED  ! MIRROR WRITE FAILED"), banner.getText());
                expect (banner.getText().endsWith ("A refused press"), banner.getText());
                expect (banner.getLevel() == Level::Danger);
            }

            beginTest ("When everything does not fit, every alarm gets a turn");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                std::vector<AlarmBanner::Alarm> list;
                for (const juce::String text : { "!! DAEMON STATUS UNAVAILABLE -- take may still be rolling; reconnect and verify",
                                                 "! BACKUP WRITE FAILED -- primary recording continues",
                                                 "! MIRROR WRITE FAILED -- check mirror drive",
                                                 "! 2 MIRRORS NOT WRITING -- drive missing or folder unusable",
                                                 "! DISK STRUGGLING -- missed samples imminent" })
                    list.push_back ({ text, Level::Danger });
                const auto pages = paged (banner, list);
                expect (pages.size() > 1);
                expectEquals (pages[0].substring (0, 2), juce::String ("!!"), "the worst goes first");
                const auto joined = pages.joinIntoString ("  ");
                for (const auto& alarm : list)
                    expect (joined.contains (alarm.text), "never reached the screen: " + alarm.text);
            }

            beginTest ("One alarm too long for a page runs on; none of it is cut off");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                const juce::String failure ("Recording stopped, but finalization failed: primary audio write failed, "
                                            "capture audio device stopped during take, backup audio write failed, "
                                            "mirror audio write failed, configured mirror did not open, recovery "
                                            "marker failed, integrity report could not be saved, optional stereo "
                                            "mix is incomplete");
                const auto pages = paged (banner, { { failure, Level::Danger } });
                expect (pages.size() > 1);
                expectEquals (pages.joinIntoString (" "), failure, "words went missing between pages");
            }

            beginTest ("A page always fits the banner, with nothing lost to an ellipsis");
            {
                for (int width : { 400, 446, 640, 900 })
                {
                    const auto text = (float) (width - 2 * brand::space::lg);
                    const auto pages = AlarmBanner::paginate (
                        { "!! DAEMON STATUS UNAVAILABLE -- take may still be rolling; reconnect and verify",
                          "! CHANNEL FAULT -- input lost, digital zero: Kick In, Kick Out, Snare Top, Snare Bottom, Hat, Ride, OH L, OH R +12 more",
                          "! COPY /Volumes/Backup Drive 2/Sessions/2026 Tour/Night 3 fell a whole ring behind and lost audio",
                          "Recording stopped, but finalization failed: primary audio write failed, capture audio device "
                          "stopped during take, backup audio write failed, mirror audio write failed" },
                        text - 60.0f);
                    for (const auto& page : pages)
                    {
                        juce::GlyphArrangement glyphs;
                        const auto shown = typeset (page);
                        glyphs.addFittedText (brand::type::headline(), shown, 0.0f, 0.0f, text - 60.0f, 36.0f,
                                              juce::Justification::centredLeft, 2, 0.75f);
                        int drawn = 0, written = 0;
                        for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
                            if (! glyphs.getGlyph (i).isWhitespace()) ++drawn;
                        for (int i = 0; i < shown.length(); ++i)
                            if (! juce::CharacterFunctions::isWhitespace (shown[i])) ++written;
                        // "fi" draws as one glyph, so only a shortfall past those is lost text.
                        for (auto ligature : { "fi", "fl", "ff" })
                            for (int at = shown.indexOf (ligature); at >= 0; at = shown.indexOf (at + 2, ligature))
                                --written;
                        expect (drawn >= written, "cut off at width " + juce::String (width) + ": " + page);
                    }
                }
            }

            beginTest ("One short alarm stays put and shows no count");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                const auto list = alarms ({ { "! BACKUP WRITE FAILED -- primary recording continues", Level::Danger } });
                banner.show (list, 1000);
                banner.show (list, 90000);
                expectEquals (banner.getPageCount(), 1);
                expectEquals (banner.getPageIndex(), 0);
            }

            beginTest ("A new alarm starts the turns again; turns survive a changing count");
            {
                AlarmBanner banner;
                banner.setSize (446, 36);
                const auto line = [] (int missed)
                {
                    return std::vector<AlarmBanner::Alarm> {
                        { "! " + juce::String (missed) + " SAMPLES MISSED -- the take has gaps in it, check the drive", Level::Danger },
                        { "! BACKUP WRITE FAILED -- primary recording continues on its own", Level::Danger } };
                };
                banner.show (line (1), 1000);
                expectEquals (banner.getPageCount(), 2);
                banner.show (line (2), 1000 + AlarmBanner::kPageMs);
                expectEquals (banner.getPageIndex(), 1, "a counter going up must not stop the turns");
                auto more = line (3);
                more.push_back ({ "! PRIMARY WRITE FAILED -- recording on backup/mirror", Level::Danger });
                banner.show (more, 1100 + AlarmBanner::kPageMs);
                expectEquals (banner.getPageIndex(), 0);
            }

            beginTest ("Alarm text splits where updateCaptureWarnings joined it");
            {
                const auto parts = AlarmBanner::splitAlarms ("! A -- one  ! B  No signal yet this take: C  ");
                expectEquals (parts.size(), 3);
                expectEquals (parts[0], juce::String ("! A -- one"));
                expectEquals (parts[2], juce::String ("No signal yet this take: C"));
                expect (AlarmBanner::splitAlarms ({}).isEmpty());
            }

            beginTest ("Turning pages moves nothing and takes no click");
            {
                Window w;
                const auto strips = w.main.stripsViewport.getBounds();
                const auto bannerBounds = w.main.alarmBanner.getBounds();
                EngineStatus status;
                status.recording = true;
                status.primaryFailed = status.backupFailed = status.mirrorFailed = true;
                status.recoveryMarkerFailed = status.diskStruggling = true;
                status.mirrorsSkipped = 2;
                w.main.updateCaptureWarnings (status);
                w.main.resized();
                expect (w.main.stripsViewport.getBounds() == strips);
                expect (w.main.alarmBanner.getBounds() == bannerBounds);
                bool self = true, children = true;
                w.main.alarmBanner.getInterceptsMouseClicks (self, children);
                expect (! self && ! children);
            }
        }

        // The engine's callback on four outputs. A test tone is the only thing
        // that can sound here: no strip feeds an output.
        struct Outputs
        {
            juce::AudioBuffer<float> in { 2, kBlock }, out { 4, kBlock };

            void render (AudioEngine& engine)
            {
                in.clear();
                out.clear();
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 2,
                                                         out.getArrayOfWritePointers(), 4, kBlock, {});
            }

            float peak (int channel) const { return out.getMagnitude (channel, 0, kBlock); }

            float peakOver (AudioEngine& engine, int channel, int blocks)
            {
                float most = 0.0f;
                for (int b = 0; b < blocks; ++b) { render (engine); most = juce::jmax (most, peak (channel)); }
                return most;
            }
        };

        void testTone()
        {
            beginTest ("The test tone sounds on the monitor pair and nowhere else");
            {
                AudioEngine engine;
                engine.prepareForTests (kRate, kBlock);
                engine.setMasterOutputs (2, 3);
                engine.setMasterGainDb (0.0f);
                engine.setMasterMuted (false);
                expect (engine.playTestTone().isEmpty());
                Outputs outputs;
                float left = 0.0f, right = 0.0f, others = 0.0f;
                for (int b = 0; b < 40; ++b)
                {
                    outputs.render (engine);
                    left   = juce::jmax (left,  outputs.peak (2));
                    right  = juce::jmax (right, outputs.peak (3));
                    others = juce::jmax (others, outputs.peak (0), outputs.peak (1));
                }
                expect (left > 0.05f, "no tone on the left of the pair");
                expect (right > 0.05f, "no tone on the right of the pair");
                expect (left < 0.2f, "the tone is meant to be a line-up level");
                expectEquals (others, 0.0f, "the tone reached outputs that feed the desk");
                engine.setMasterOutputs (0, 1);
            }

            beginTest ("Master MUTE silences the tone, even one already sounding");
            {
                AudioEngine engine;
                engine.prepareForTests (kRate, kBlock);
                engine.setMasterOutputs (0, 1);
                engine.setMasterGainDb (0.0f);
                engine.setMasterMuted (false);
                expect (engine.playTestTone().isEmpty());
                Outputs outputs;
                expect (outputs.peakOver (engine, 0, 20) > 0.05f);
                engine.setMasterMuted (true);
                expectEquals (outputs.peakOver (engine, 0, 20), 0.0f, "the tone went past MUTE");
                expect (engine.playTestTone().containsIgnoreCase ("mute"));
                engine.setMasterMuted (false);
            }

            beginTest ("A take refuses the tone and takes the outputs back from one sounding");
            {
                Window w;
                w.arm ({ 0 });
                expect (w.engine().playTestTone().isEmpty());
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                expect (w.engine().playTestTone().containsIgnoreCase ("take"));
                Outputs outputs;
                expectEquals (outputs.peakOver (w.engine(), 0, 20), 0.0f, "the tone played on into the take");
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("With the monitor bus set to no output the tone has nowhere to go");
            {
                AudioEngine engine;
                engine.prepareForTests (kRate, kBlock);
                engine.setMasterOutputs (-1, -1);
                expect (engine.playTestTone().isNotEmpty());
                Outputs outputs;
                expectEquals (outputs.peakOver (engine, 0, 4), 0.0f);
                engine.setMasterOutputs (0, 1);
            }
        }

        static int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        static void feed (MultitrackRecorder& rec, int blocks)
        {
            const int tracks = rec.getNumTracks();
            std::vector<float> audio ((size_t) kBlock, 0.1f);
            std::vector<const float*> inputs ((size_t) tracks, audio.data());
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
        }

        void stopTake (Window& w)
        {
            w.main.onStopClicked();
            w.main.onStopClicked();
            expect (w.stopped());
        }

        void nothingStandsInFrontOfStop()
        {
            beginTest ("M, + Cue and the delete keys open no modal over a rolling take");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                const auto markers = w.engine().getMarkers().getCount();

                w.main.dropMarkerAndPromptName();
                expectEquals (modalCount(), 0, "M opened a prompt in front of STOP");
                expectEquals (w.engine().getMarkers().getCount(), markers + 1, "the marker went missing");

                w.main.addCueAtTransport();
                expectEquals (modalCount(), 0, "+ Cue opened a prompt in front of STOP");
                expectEquals ((int) w.main.cues.size(), 1, "the cue went missing");
                expectEquals (w.main.cues.back().name, juce::String ("Song 1"));

                w.main.renameCurrentCue();
                expectEquals (modalCount(), 0, "Rename opened a prompt in front of STOP");

                bool asked = false;
                w.main.confirmForTests = [&asked] (std::function<void()>) { asked = true; };
                w.main.confirmDeleteRecording (false, [] {});
                w.main.selectedLogical.insert (0);
                w.main.confirmDeleteChannels ([] {});
                w.main.confirmForTests = {};
                w.main.selectedLogical.clear();
                expect (! asked, "a delete asked over a take it would refuse");

                w.main.showBatchRenameDialog();
                w.main.showBatchColourDialog();
                expectEquals (modalCount(), 0, "a batch dialog opened over the take");
                stopTake (w);
            }

            beginTest ("Between takes M and + Cue still ask for a name");
            {
                Window w;
                w.main.dropMarkerAndPromptName();
                expectEquals (modalCount(), 1, "the marker prompt is gone at idle");
                dismissModals();
                w.main.addCueAtTransport();
                expectEquals (modalCount(), 1, "the cue prompt is gone at idle");
                dismissModals();
            }
        }

        void cuesDuringATake()
        {
            beginTest ("A cue during a take lands where the take is, not where the player parked");
            {
                Window w;
                w.arm ({ 0 });
                w.player().setPositionSamples (0);
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                feed (w.recorder(), 8);
                const auto at = w.engine().getRecordTimelineSamples();
                expect (at > 0);
                w.main.addCueAtTransport();
                expectEquals (w.main.cues.back().samplePos, at, "+ Cue stored the parked playhead");

                feed (w.recorder(), 8);
                const auto later = w.engine().getRecordTimelineSamples();
                w.main.updateCueAtTransport();
                expectEquals (w.main.cues.back().samplePos, later, "Update stored the parked playhead");
                stopTake (w);
            }

            beginTest ("Cue edits during a take stay off the drive and arrive with STOP's save");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                const auto backups = w.session.getChildFile ("Session File Backups");
                const auto before = backups.getNumberOfChildFiles (juce::File::findDirectories);
                w.main.addCueAtTransport();
                w.main.updateCueAtTransport();
                w.main.setlistBar.onMoveCue (1);
                expectEquals (w.main.pendingMetadataSaves, 0, "a cue edit started a save during the take");
                expect (! w.main.sessionIoBusy.load(), "a cue edit raised the session-busy flag during the take");
                expectEquals (backups.getNumberOfChildFiles (juce::File::findDirectories), before);
                stopTake (w);
                const auto saved = findSessionProj (w.session).loadFileAsString();
                expect (saved.contains ("Song 1"), "STOP's save left the cue behind");
            }
        }

        void stripDeleteAsks()
        {
            beginTest ("The strip menu's Delete asks first, like every other delete");
            {
                Window w (3);
                w.main.rebuildStrips();
                std::function<void()> answer;
                w.main.confirmForTests = [&answer] (std::function<void()> yes) { answer = std::move (yes); };
                w.main.strips[1]->requestDelete();
                expect ((bool) answer, "the strip went without a question");
                expectEquals (w.recorder().getNumTracks(), 3, "the strip went before the answer");
                answer();
                w.main.confirmForTests = {};
                expectEquals (w.recorder().getNumTracks(), 2, "Delete did not delete");
            }

            beginTest ("Delete answered after the layout changed removes nothing");
            {
                Window w (3);
                w.main.rebuildStrips();
                std::function<void()> answer;
                w.main.confirmForTests = [&answer] (std::function<void()> yes) { answer = std::move (yes); };
                w.main.strips[1]->requestDelete();
                w.main.confirmForTests = {};
                w.recorder().bumpTrackGeneration();
                answer();
                expectEquals (w.recorder().getNumTracks(), 3, "the answer hit another strip");
            }
        }

        static juce::String comboText (zynforge::ChannelStrip& strip, int whichCombo)
        {
            int seen = 0;
            for (auto* child : strip.getChildren())
                if (auto* combo = dynamic_cast<juce::ComboBox*> (child))
                    if (seen++ == whichCombo) return combo->getText();
            return {};
        }

        void combosFollowTheDevice()
        {
            beginTest ("MIXER's combos follow a device change that leaves the strip count alone");
            {
                Window w (8);
                auto& manager = w.engine().getDeviceManager();
                manager.closeAudioDevice();
                const juce::ScopeGuard deviceBack { [&]
                {
                    w.engine().setDeviceInputCountForTests (0);
                    manager.restartLastAudioDevice();
                } };
                for (int t = 0; t < 8; ++t) w.engine().setTrackInputRouting (t, t);
                w.engine().setDeviceInputCountForTests (8);
                w.main.rebuildStrips();
                expectEquals (comboText (*w.main.strips[6], 0), juce::String ("In 7"));

                w.engine().setDeviceInputCountForTests (4);
                w.main.syncStripsWithEngine();
                expectEquals (w.engine().getEffectiveInputRouting (6), 2);
                expectEquals (comboText (*w.main.strips[6], 0), juce::String ("In 3"),
                              "MIXER named another input than the strip hears");

                w.engine().setDeviceInputCountForTests (8);
                w.main.syncStripsWithEngine();
                expect (! comboText (*w.main.strips[6], 0).contains ("(off)"), "a live channel kept its (off)");
            }
        }

        void pairsTakeTwoChannels()
        {
            beginTest ("A pair sent to the last channel lands on the last two, in every view");
            {
                Window w (4);
                w.engine().setTrackStereo (0, true);
                auto& left = w.recorder().getTrack (0);
                auto& right = w.recorder().getTrack (1);
                w.engine().setStripLinkedRouting (0, true, 7, 8);
                expectEquals (left.inputRouting.load(), 6);
                expectEquals (right.inputRouting.load(), 7, "R has to have a channel, or RECORD picks one for it");
                expectEquals (right.outputRouting.load(), 7);

                w.engine().setStripLinkedRouting (0, true, 3, 8);
                expectEquals (right.inputRouting.load(), 4);
                w.engine().setStripLinkedRouting (0, true, -1, 8);
                expectEquals (left.inputRouting.load(), -1);
                expectEquals (right.inputRouting.load(), -1);

                // Fewer outputs than inputs: the output count speaks for both sides.
                w.engine().setStripLinkedRouting (0, true, 1, 2);
                expectEquals (left.inputRouting.load(), 0);
                expectEquals (right.inputRouting.load(), 1, "an output click cleared a free input");
            }

            beginTest ("A mono strip keeps the last channel, and a one-channel device still patches");
            {
                Window w (2);
                w.engine().setStripLinkedRouting (1, false, 7, 8);
                expectEquals (w.recorder().getTrack (1).inputRouting.load(), 7);
                expectEquals (pairStartChannel (0, 1), 0);
                expectEquals (pairStartChannel (-1, 8), -1);
            }

            beginTest ("A link that runs off the device says what R really hears");
            {
                Window w (4);
                w.engine().setTrackInputRouting (2, 7);      // the silent device has 8 inputs
                expect (w.main.setStereoLink (2, true));
                const auto notice = w.engine().pairWrapNotice (2);
                expect (notice.contains ("In 8") && notice.contains ("In 4"), notice);
                expect (w.main.statusLabel.getText().contains ("no input after"), w.main.statusLabel.getText());
                expect (w.engine().pairWrapNotice (0).isEmpty());
            }

            beginTest ("A link the engine refuses moves no pan, finalizing included");
            {
                Window w (2);
                w.arm ({ 0 });
                w.engine().setTrackPan (0, 0.25f);
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                expect (! w.engine().linkStereoPair (0, true));
                expectWithinAbsoluteError (w.recorder().getTrack (0).pan.load(), 0.25f, 1.0e-4f);

                struct Gate { juce::WaitableEvent entered, release; };
                auto gate = std::make_shared<Gate>();
                w.recorder().beforeFinalizationForTests = [gate] { gate->entered.signal(); gate->release.wait (5000); };
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (gate->entered.wait (3000));
                expect (w.engine().isCaptureFinalizing());
                expect (! w.engine().linkStereoPair (0, true), "the engine let a link through finalization");
                expectWithinAbsoluteError (w.recorder().getTrack (0).pan.load(), 0.25f, 1.0e-4f,
                                           "a refused link moved the pan");
                gate->release.signal();
                expect (w.stopped());
                w.recorder().beforeFinalizationForTests = {};
            }
        }

        // The modal count falls before the answer's callback runs, so wait for
        // the status line the callback leaves.
        static void answerModal (Window& w, int answer)
        {
            const juce::String waiting ("waiting for the answer");
            w.main.statusLabel.setText (waiting, juce::dontSendNotification);
            if (auto* modal = juce::Component::getCurrentlyModalComponent()) modal->exitModalState (answer);
            pumpUntil ([&] { return w.main.statusLabel.getText() != waiting; }, 2000);
        }

        void nameStrips (Window& w, const juce::String& prefix)
        {
            for (int t = 0; t < w.recorder().getNumTracks(); ++t)
                w.engine().setTrackName (t, prefix + juce::String (t + 1));
        }

        // Opens another session with the question still up, as the menu bar and
        // a Finder open can.
        juce::File swapSession (Window& w, int tracks)
        {
            auto other = tempDir ("other");
            w.dirs.add (other);
            recordBaseTake (other, tracks, 50);
            w.main.openSessionFolder (other);
            w.main.engine.prepareForTests (kRate, kBlock);
            return other;
        }

        void questionsKeepTheirSession()
        {
            beginTest ("A soundcheck question answered in another session repatches nothing");
            {
                Window w (2);
                std::function<void()> answer;
                w.main.confirmForTests = [&answer] (std::function<void()> yes) { answer = std::move (yes); };
                w.main.onVscClicked();
                w.main.confirmForTests = {};
                expect ((bool) answer);
                swapSession (w, 2);
                answer();
                expect (! vsc::hasSavedPatch (w.engine()), "the new session's strips were repatched");
                expect (w.main.statusLabel.getText().contains ("session changed"), w.main.statusLabel.getText());
            }

            beginTest ("Replace From Cursor answered in another session starts nothing");
            {
                Window w (2);
                w.arm ({ 0 });
                MainComponent::RecordStartPlan plan;
                plan.punchAt = 4800;
                plan.armed = 1;
                w.main.askBeforeCursorPunch (plan);
                swapSession (w, 2);
                w.arm ({ 0 });
                w.main.answerCursorPunch (2);
                expect (! w.engine().isRecording(), "the answer punched the new session");
                dismissModals();
            }

            beginTest ("Batch Rename answered in another session writes no names");
            {
                Window w (2);
                nameStrips (w, "Old ");
                w.main.showBatchRenameDialog();
                expectEquals (modalCount(), 1);
                swapSession (w, 2);
                nameStrips (w, "New ");
                answerModal (w, 1);
                expectEquals (w.recorder().getTrack (0).name, juce::String ("New 1"),
                              "the old session's names landed on the new strips");
            }

            beginTest ("Batch Colour stands down once its strips are gone");
            {
                Window w (6);
                const auto keep = juce::Colour (0xff336699);
                w.main.showBatchColourDialog();
                expectEquals (modalCount(), 1);
                auto* dialog = juce::Component::getCurrentlyModalComponent();
                swapSession (w, 2);
                w.engine().setTrackColour (0, keep);

                // The last row's swatch, for a strip this session doesn't have.
                juce::Button* last = nullptr;
                std::function<void (juce::Component&)> findLast = [&] (juce::Component& c)
                {
                    for (auto* child : c.getChildren())
                    {
                        if (auto* button = dynamic_cast<juce::TextButton*> (child))
                            if (button->getButtonText().isEmpty()) last = button;
                        findLast (*child);
                    }
                };
                findLast (*dialog);
                expect (last != nullptr);
                if (last != nullptr) last->onClick();
                expectEquals (modalCount(), 1, "a picker opened for a strip that is gone");

                answerModal (w, 0);
                expect (w.recorder().getTrack (0).colourARGB.load() == keep.getARGB(),
                        "Cancel painted the old session's colours over this one");
            }

            beginTest ("Add Tracks refuses by name when the moment has passed");
            {
                Window w (2);
                const auto stale = w.main.questionScope();
                AddTracksDialog::Entry entry;
                entry.count = 2;
                swapSession (w, 2);
                w.main.addTracks ({ entry }, stale);
                expectEquals (w.recorder().getNumTracks(), 2, "tracks landed in the new session");

                w.arm ({ 0 });
                const auto scope = w.main.questionScope();
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                w.main.addTracks ({ entry }, scope);
                expectEquals (w.recorder().getNumTracks(), 2);
                expect (w.main.statusLabel.getText().contains ("No tracks were added"), w.main.statusLabel.getText());
                expect (! w.main.statusLabel.getText().startsWith ("Added"));
                stopTake (w);
            }

            beginTest ("The empty mixer's Add tracks obeys LOCK");
            {
                Window w (2);
                w.main.onLockToggled();
                w.main.addChannelButton.onClick();
                expectEquals (modalCount(), 0, "Add tracks opened under LOCK");
                w.main.onLockToggled();
            }
        }

        static juce::Slider* faderOf (zynforge::ChannelStrip& strip)
        {
            for (auto* child : strip.getChildren())
                if (auto* slider = dynamic_cast<juce::Slider*> (child))
                    if (slider->isVertical()) return slider;
            return nullptr;
        }

        static juce::Button* muteOf (zynforge::ChannelStrip& strip)
        {
            for (auto* child : strip.getChildren())
                if (auto* button = dynamic_cast<juce::Button*> (child))
                    if (button->getButtonText() == "M" || button->getButtonText() == "MUTE") return button;
            return nullptr;
        }

        void gridKeepsItsFaders()
        {
            beginTest ("GRID keeps a fader and a Mute button at every window size");
            {
                Window w (24);
                const auto gridWas = w.main.mixerGridView;
                w.main.mixerGridView = true;
                w.main.rebuildStrips();
                for (const auto& [width, height] : { std::pair (MainComponent::kMinWidth, MainComponent::kMinHeight),
                                                     std::pair (1320, 820), std::pair (1120, 700) })
                {
                    w.main.setSize (width, height);
                    w.main.resized();
                    for (auto& strip : w.main.strips)
                    {
                        auto* fader = faderOf (*strip);
                        expect (fader != nullptr);
                        if (fader != nullptr)
                            expectGreaterOrEqual (fader->getHeight(), 100, "no fader to move at width " + juce::String (width));
                        auto* mute = muteOf (*strip);
                        expect (mute != nullptr);
                        if (mute != nullptr)
                            expectGreaterOrEqual (mute->getHeight(), 20, "the Mute button is a sliver at width " + juce::String (width));
                    }
                    expect (w.main.stripsContainer.getHeight() >= w.main.strips.back()->getBottom(),
                            "the last row can't be scrolled to");
                }
                w.main.mixerGridView = gridWas;
                w.main.setSize (1320, 820);
                w.main.resized();
            }
        }

        void headerKeepsItsWarnings()
        {
            beginTest ("Both pills keep their text at the smallest window");
            {
                Window w;
                w.main.setSize (MainComponent::kMinWidth, MainComponent::kMinHeight);
                w.main.punchPill.setText ("RECORD PUNCHES AT CURSOR", juce::dontSendNotification);
                w.main.punchPill.setVisible (true);
                w.main.soundcheckPill.setVisible (true);
                w.main.resized();
                const auto punch = w.main.punchPill.getBounds();
                const auto desk  = w.main.soundcheckPill.getBounds();
                expectGreaterOrEqual (punch.getWidth(), 170);
                expectGreaterOrEqual (desk.getWidth(), 170, "the soundcheck pill was squeezed out");
                expectGreaterOrEqual (desk.getHeight(), 18);
                expect (! punch.intersects (desk), "the pills overlap");
                expect (punch.getBottom() <= w.main.bigClock.getY() && desk.getBottom() <= w.main.bigClock.getY());
                expect (! desk.intersects (w.main.sessionLabel.getBounds()));

                w.main.punchPill.setVisible (false);
                w.main.resized();
                expectGreaterOrEqual (w.main.soundcheckPill.getHeight(), 24, "a pill alone shrank");
                w.main.soundcheckPill.setVisible (false);
                w.main.punchPill.setText ({}, juce::dontSendNotification);
                w.main.setSize (1320, 820);
                w.main.resized();
            }

            beginTest ("The status line keeps room for a sentence at the smallest window");
            {
                Window w;
                w.main.setSize (MainComponent::kMinWidth, MainComponent::kMinHeight);
                w.main.resized();
                expectGreaterOrEqual (w.main.statusLabel.getWidth(), 280);
                expectGreaterOrEqual (w.main.alarmBanner.getWidth(), 500);
                w.main.setSize (1320, 820);
                w.main.resized();
            }
        }

        void recordTimeWarns()
        {
            beginTest ("Record time running out reaches the banner: amber at 30 minutes, danger at 10");
            {
                Window w;
                const auto withMinutes = [&w] (int minutes, bool rolling = true)
                {
                    EngineStatus status;
                    status.recording = rolling;
                    status.minutesRemaining = minutes;
                    w.main.updateCaptureWarnings (status);
                };
                withMinutes (31);
                expect (! w.main.alarmBanner.isVisible(), "31 minutes is not a warning yet");
                withMinutes (30);
                expect (w.main.alarmBanner.isVisible());
                expect (w.main.alarmBanner.getLevel() == Level::Notice, "30 minutes is a notice");
                expect (w.main.alarmBanner.getText().contains ("0h30m"), w.main.alarmBanner.getText());
                withMinutes (11);
                expect (w.main.alarmBanner.getLevel() == Level::Notice);
                withMinutes (10);
                expect (w.main.alarmBanner.getLevel() == Level::Danger, "10 minutes is a danger");
                withMinutes (1);
                expect (w.main.alarmBanner.getLevel() == Level::Danger);
                withMinutes (0);
                expect (! w.main.alarmBanner.isVisible(), "no estimate is not an alarm");
                withMinutes (5, false);
                expect (! w.main.alarmBanner.isVisible(), "this is about a take, not an idle drive");
            }

            beginTest ("The last minute still reads as a time, not as no estimate");
            {
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.volumeProbePeriodMs = 10;
                rec.volumeReaderForTests = [] (const juce::File&)
                {
                    VolumeProbe::Reading reading;
                    reading.freeBytes = 1000;
                    reading.totalBytes = 1 << 30;
                    reading.volumeId = 1;
                    return reading;
                };
                auto dir = tempDir ("last-minute");
                expect (rec.startRecording (dir));
                expect (pumpUntil ([&rec] { return rec.readDiskEstimate().primaryFreeBytes >= 0; }));
                expectEquals (rec.readDiskEstimate().minutes, 1);
                rec.stopRecording();
                dir.deleteRecursively();
            }
        }

        void noDriveReadsDuringATake()
        {
            beginTest ("Session Info and console state keep off the take's drive");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording());
                w.main.showSessionInfo();
                expectEquals (modalCount(), 0, "Session Info opened during the take");
                expect (w.main.statusLabel.getText().contains ("after the take"), w.main.statusLabel.getText());

                w.session.getChildFile ("console_state.json").replaceWithText (R"({"deskPatch":"stage"})");
                w.main.statusLabel.setText ("Recording", juce::dontSendNotification);
                w.main.loadConsoleStateAfterConnect ("10.0.0.1");
                expect (! w.main.statusLabel.getText().contains ("loaded saved patch"), "the read ran during the take");
                stopTake (w);

                expect (w.engine().hasSessionFolder());
                juce::PopupMenu menu = w.main.getMenuForIndex (3, "Session");
                bool found = false, enabled = false;
                for (juce::PopupMenu::MenuItemIterator item (menu, true); item.next();)
                    if (item.getItem().itemID == 251) { found = true; enabled = item.getItem().isEnabled; }
                expect (found && enabled, "Session Info stayed grey after the take");
            }
        }

        void preflightHasATimeLimit()
        {
            beginTest ("A drive that never answers probes as gone, in the time allowed");
            {
                auto held = std::make_shared<juce::WaitableEvent> (true);
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto answer = preflight::withinTime (150, [held] { held->wait (5000); return true; });
                expect (! answer.has_value());
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 2000.0, "the wait ran past its limit");
                held->signal();
                expect (preflight::withinTime (2000, [] { return 7; }).value_or (0) == 7);
            }

            beginTest ("Pre-flight opens in bounded time over a backup that has stopped answering");
            {
                Window w;
                auto held = std::make_shared<juce::WaitableEvent> (true);
                const auto before = w.recorder().getBackupDirectory();
                const auto backup = tempDir ("stalled-backup");
                w.dirs.add (backup);
                expect (backup.createDirectory().wasOk());
                expect (w.engine().setBackupDirectory (backup));
                w.main.preflightStallForTests = [held] { held->wait (20000); };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                w.main.showPreflightChecklist();
                const auto took = juce::Time::getMillisecondCounterHiRes() - started;
                w.main.preflightStallForTests = {};
                held->signal();
                expectLessThan (took, 12000.0, "pre-flight hung on the dead drive");
                expectEquals (modalCount(), 1, "pre-flight never opened");
                dismissModals();
                w.engine().setBackupDirectory (before);
            }
        }

        void backupButtonIsLive()
        {
            beginTest ("The BACKUP button says what is true now");
            {
                Window w;
                const auto before = w.recorder().getBackupDirectory();
                const auto backup = tempDir ("button");
                w.dirs.add (backup);
                expect (backup.createDirectory().wasOk());
                expect (w.engine().setBackupDirectory (backup));

                const auto textOf = [&w] (const EngineStatus& status, const juce::String& wanted)
                {
                    pumpUntil ([&] { w.main.updateBackupButton (status); return w.main.backupButton.getButtonText() == wanted; }, 8000);
                    return w.main.backupButton.getButtonText();
                };
                expectEquals (textOf ({}, "BACKUP OK"), juce::String ("BACKUP OK"));

                EngineStatus rolling;
                rolling.recording = true;
                rolling.backupActive = true;
                expectEquals (textOf (rolling, "BACKUP OK"), juce::String ("BACKUP OK"));
                rolling.backupFailed = true;
                expectEquals (textOf (rolling, "BACKUP !"), juce::String ("BACKUP !"), "OK stood beside BACKUP WRITE FAILED");
                rolling.backupFailed = false;
                rolling.backupActive = false;
                expectEquals (textOf (rolling, "BACKUP !"), juce::String ("BACKUP !"), "a take with no backup writing said OK");

                EngineStatus failed;
                failed.backupFailed = true;
                expectEquals (textOf (failed, "BACKUP !"), juce::String ("BACKUP !"), "a failed take's backup reads OK after STOP");

                expect (backup.deleteRecursively());
                expectEquals (textOf ({}, "BACKUP !"), juce::String ("BACKUP !"), "a folder that went still read OK");

                w.engine().setBackupDirectory ({});
                expectEquals (textOf ({}, "BACKUP"), juce::String ("BACKUP"));
                w.engine().setBackupDirectory (before);
            }
        }

        void smallRepairs()
        {

            beginTest ("A PATCH drag gives every strip its own channels");
            {
                using Targets = std::vector<std::pair<int, int>>;
                expect (PatchPage::dragTargets ({ false, false, false }, 0, 0, 2, 8) == Targets ({ { 0, 0 }, { 1, 1 }, { 2, 2 } }));
                expect (PatchPage::dragTargets ({ true, true, true }, 0, 0, 4, 8) == Targets ({ { 0, 0 }, { 1, 2 }, { 2, 4 } }),
                        "a pair's R and the next pair's L landed on one input");
                expect (PatchPage::dragTargets ({ false, true, false }, 2, 5, -3, 8) == Targets ({ { 2, 5 }, { 1, 3 }, { 0, 2 } }));
                expect (PatchPage::dragTargets ({ true, true }, 0, 5, 3, 8) == Targets ({ { 0, 5 } }),
                        "a pair ran off the last row");
            }

            beginTest ("LOCK stops a Finder open");
            {
                Window w;
                auto other = tempDir ("locked-open");
                w.dirs.add (other);
                recordBaseTake (other, 2, 50);
                w.main.onLockToggled();
                expect (! w.main.openSessionDocument (other), "a double-click swapped the session under LOCK");
                expect (w.engine().getActiveSessionDir() == w.session);
                w.main.onLockToggled();
            }

            beginTest ("LOCK reaches the Meterbridge");
            {
                Window w;
                w.main.openMetersWindow();
                expect (w.main.metersDialog != nullptr);
                w.main.onLockToggled();
                expect (w.main.metersDialog != nullptr && ! w.main.metersDialog->isEnabled(),
                        "a click on a meter could clear clip evidence under LOCK");
                w.main.onLockToggled();
                expect (w.main.metersDialog->isEnabled());
                delete w.main.metersDialog.getComponent();
            }

            beginTest ("A click leaves no keyboard focus on the transport");
            {
                Window w;
                int buttons = 0;
                for (auto* child : w.main.transportBar->getChildren())
                    if (dynamic_cast<juce::Button*> (child) != nullptr)
                    {
                        ++buttons;
                        expect (! child->getMouseClickGrabsKeyboardFocus(), "Return would press " + child->getName());
                    }
                expectGreaterThan (buttons, 0);
            }

            beginTest ("The next-cue countdown keeps a name in any script");
            {
                Window w;
                const auto name = juce::String::fromUTF8 ("Caf\xc3\xa9 \xd8\xb3\xd9\x84\xd8\xa7\xd9\x85");
                SetlistBar::Cue cue;
                cue.name = name;
                cue.samplePos = 40000;
                w.main.cues.push_back (cue);
                w.player().setPositionSamples (0);
                w.engine().startPlayback();
                w.main.updateNextCueCountdown();
                w.engine().stopPlayback();
                expect (w.main.nextCueLabel.getText().contains (name), w.main.nextCueLabel.getText());
            }

            beginTest ("Space and PLAY start from the same place and tell the truth");
            {
                Window w;
                const auto total = w.player().getTotalLengthSamples();
                w.player().setPositionSamples (total);
                w.main.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), nullptr);
                expect (! w.engine().isPlaying());
                expect (! w.main.statusLabel.getText().startsWith ("Playing"), w.main.statusLabel.getText());

                w.engine().setEditCursorSample (2000);
                w.main.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), nullptr);
                expect (w.engine().isPlaying(), w.main.statusLabel.getText());
                expect (w.player().getPositionSamples() < 40000, "Space ignored the cursor PLAY obeys");
                w.engine().stopPlayback();
                w.engine().setEditCursorSample (-1);
            }

        }

        static void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0; }, 2000);
        }
    };

    static MainWindowFixTests mainWindowFixTests;
}
