// The main window's alarms and transport presses: what the banner must never
// lose, and how a press from somewhere else stays what it is. These run on a
// silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/SessionProjPath.h"
#include "../Audio/ChannelAlarm.h"
#include "../Audio/SessionBackup.h"
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

        static void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0; }, 2000);
        }
    };

    static MainWindowFixTests mainWindowFixTests;
}
