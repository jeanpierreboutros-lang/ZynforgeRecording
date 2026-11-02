// The main window's guards around a take: what RECORD may do to existing audio,
// PUNCH mode, and what the window refuses or shows while a take rolls. RECORD's
// whole path runs here, pre-flight included, on a silent fake device.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/PreflightDialog.h"
#include "../Audio/VirtualSoundcheck.h"
#include "../UI/AudioDeviceDialog.h"
#include "../UI/ControlSurfacesDialog.h"
#include "../UI/LedMeter.h"
#include "../UI/Meterbridge.h"
#include "../UI/RecordPosition.h"
#include "../UI/SessionSettingsDialog.h"
#include "../Audio/ChannelAlarm.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SongDetector.h"

#include <functional>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        // Gives pre-flight a device to read. Nothing here reaches CoreAudio,
        // so no mic-consent prompt and no real interface is touched.
        class SilentDevice final : public juce::AudioIODevice
        {
        public:
            SilentDevice (double rateIn, int inputsIn)
                : juce::AudioIODevice ("Silent", "Silent"), rate (rateIn), inputs (inputsIn) {}

            juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
            juce::StringArray getInputChannelNames() override
            {
                juce::StringArray names;
                for (int i = 0; i < inputs; ++i) names.add (juce::String (i + 1));
                return names;
            }
            juce::Array<double> getAvailableSampleRates() override { return { rate }; }
            juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
            int getDefaultBufferSize() override { return kBlock; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override
            { opened = true; return {}; }
            void close() override { opened = false; }
            bool isOpen() override { return opened; }
            void start (juce::AudioIODeviceCallback*) override { started = true; }
            void stop() override { started = false; }
            bool isPlaying() override { return started; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return kBlock; }
            double getCurrentSampleRate() override { return rate; }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return 3; }
            juce::BigInteger getActiveInputChannels() const override
            {
                juce::BigInteger bits;
                bits.setRange (0, inputs, true);
                return bits;
            }

        private:
            double rate;
            int inputs;
            bool opened { false }, started { false };
        };

        class SilentDeviceType final : public juce::AudioIODeviceType
        {
        public:
            SilentDeviceType (double rateIn, int inputsIn)
                : juce::AudioIODeviceType ("Silent"), rate (rateIn), inputs (inputsIn) {}

            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool) const override { return { "Silent" }; }
            int getDefaultDeviceIndex (bool) const override { return 0; }
            int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
            bool hasSeparateInputsAndOutputs() const override { return false; }
            juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override
            { return new SilentDevice (rate, inputs); }

        private:
            double rate;
            int inputs;
        };

        void useSilentDevice (AudioEngine& engine, double rate = kRate, int inputs = 8)
        {
            auto& manager = engine.getDeviceManager();
            manager.addAudioDeviceType (std::make_unique<SilentDeviceType> (rate, inputs));
            manager.setCurrentAudioDeviceType ("Silent", true);
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = "Silent";
            setup.inputDeviceName = "Silent";
            setup.sampleRate = rate;
            setup.bufferSize = kBlock;
            manager.setAudioDeviceSetup (setup, false);
        }

        juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-window-" + tag + "-" + juce::Uuid().toString());
        }

        float levelOf (int track) { return 0.1f * (float) (track + 1); }

        // Every track holds its own DC level, so a file shows what replaced it.
        void recordBaseTake (const juce::File& dir, int tracks, int blocks)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, levelOf (t));
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        juce::File trackFile (const juce::File& dir, int track)
        {
            return dir.getChildFile ("Audio Files")
                      .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1));
        }

        // Mean of [start, start + length), read as playback reads a whole take.
        float meanOf (const juce::File& file, juce::int64 start, juce::int64 length)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            auto reader = ConcatReader::create (formats, findTakeParts (file));
            if (reader == nullptr || start + length > reader->lengthInSamples) return 999.0f;
            juce::AudioBuffer<float> buffer (1, (int) length);
            reader->read (&buffer, 0, (int) length, start, true, false);
            double sum = 0.0;
            for (int i = 0; i < (int) length; ++i) sum += buffer.getSample (0, i);
            return (float) (sum / (double) length);
        }

        juce::int64 lengthOf (const juce::File& file)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            auto reader = ConcatReader::create (formats, findTakeParts (file));
            return reader != nullptr ? reader->lengthInSamples : -1;
        }

        void feedAll (MultitrackRecorder& rec, float value, int blocks, juce::int64 playhead = -1)
        {
            const int tracks = rec.getNumTracks();
            std::vector<float> audio ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) tracks, audio.data());
            for (int b = 0; b < blocks; ++b)
                rec.processBlock (inputs.data(), tracks, kBlock,
                                  playhead >= 0 ? playhead + (juce::int64) b * kBlock : -1);
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }
    }

    class MainWindowGuardTests final : public juce::UnitTest
    {
    public:
        MainWindowGuardTests() : juce::UnitTest ("Main window guards", "zynforge") {}

        // One session open on the suite's main window. The window itself is
        // shared: its waveform threads run at background priority, and tearing
        // dozens of them down on a loaded machine is what could fail here.
        struct Window
        {
            static inline MainComponent* shared = nullptr;

            MainComponent& main;
            juce::File session;
            juce::Array<juce::File> dirs;

            explicit Window (int tracks = 2, int blocks = 200) : main (*shared)
            {
                session = newSession (tracks, blocks);
                open (session);
            }

            ~Window()
            {
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.punchSessionActive = false;
                main.manualPunchActive = false;
                main.punchWindowTakeActive = false;    // the timer's job, and it isn't running
                main.stopArmedAtMs = 0;
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.updateCaptureWarnings (EngineStatus());
                main.lastTrackCount = -1;
                for (const auto& d : dirs) d.deleteRecursively();
            }

            juce::File newSession (int tracks, int blocks)
            {
                auto dir = tempDir ("session");
                dirs.add (dir);
                recordBaseTake (dir, tracks, blocks);
                return dir;
            }

            void open (const juce::File& dir)
            {
                main.openSessionFolder (dir, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            AudioEngine& engine() { return main.engine; }
            SessionPlayer& player() { return main.engine.getPlayer(); }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }

            void arm (std::initializer_list<int> tracks)
            {
                for (int t = 0; t < recorder().getNumTracks(); ++t) engine().setTrackArmed (t, false);
                for (int t : tracks) engine().setTrackArmed (t, true);
            }

            juce::Button* loopButton()
            {
                for (auto* child : main.transportBar->getChildren())
                    if (auto* button = dynamic_cast<juce::Button*> (child))
                        if (button->getTitle() == "Loop region") return button;
                return nullptr;
            }

            void clickLoop() { if (auto* b = loopButton()) b->onClick(); }

            bool stopped()
            {
                return pumpUntil ([this]
                {
                    return ! engine().isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
            }

            MainComponent::RecordStartPlan plan()
            {
                MainComponent::RecordStartPlan p;
                main.planRecordPosition (p);
                return p;
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
            dismissModals();   // an earlier suite may have left a dialog up

            loopNeverArmsAPunch();
            savedRegions();
            oneSelectionOnePunch();
            parkedPlayheadAppends();
            punchModeUsesLiveArms();
            punchModeLeavesWithTheSession();
            remoteNeverPunches();
            lockHolds();
            alarmsStandOut();
            sessionLeaving();
            statusPanel();
            soundcheckIsVisibleAndReversible();
            screensAgree();
            soundcheckReport();
            analysisStaysOutOfTheTake();
            consoleWaitsForTheTake();
            mainWindowRepairs();
        }

    private:
        static juce::MouseEvent rightClick (juce::Component& target)
        {
            const juce::Point<float> at (8.0f, 8.0f);
            const juce::ModifierKeys mods (juce::ModifierKeys::rightButtonModifier
                                           | juce::ModifierKeys::popupMenuClickModifier);
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods,
                                     1, 0, 0, 0, 0, &target, &target, juce::Time::getCurrentTime(), at,
                                     juce::Time::getCurrentTime(), 1, false);
        }

        // True when a right-click on `target` opened a menu.
        static bool rightClickOpensMenu (juce::Component& target)
        {
            juce::PopupMenu::dismissAllActiveMenus();
            target.mouseDown (rightClick (target));
            const bool opened = juce::PopupMenu::dismissAllActiveMenus();
            pumpUntil ([] { return false; }, 30);
            return opened;
        }

        void loopNeverArmsAPunch()
        {
            beginTest ("Loop with nothing selected makes no region");
            {
                Window w;
                w.arm ({ 0 });
                w.clickLoop();
                expect (w.player().isLoopEnabled());
                expect (! w.player().hasLoopRegion(), "the Loop button made a selection");
                w.clickLoop();
                expect (! w.player().isLoopEnabled());
                expect (! w.player().hasLoopRegion());
            }

            beginTest ("A click on Loop leaves RECORD appending, and the take intact");
            {
                Window w;
                w.arm ({ 0, 1 });
                const auto total = w.player().getTotalLengthSamples();
                w.clickLoop();
                w.clickLoop();

                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                expect (! w.main.punchSessionActive, "Loop turned RECORD into a selection punch");
                expect (! w.main.manualPunchActive);
                expect (! w.player().isPlaying(), "an append rolled the old take to the outputs");
                feedAll (w.recorder(), -0.3f, 20);
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
                expectEquals (lengthOf (trackFile (w.session, 0)), total + 20 * kBlock,
                              "the new audio did not land after the take");
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 0), 0, total),
                                           levelOf (0), 1.0e-3f, "the take was overwritten");
            }

            beginTest ("Loop with no range wraps the whole session");
            {
                Window w;
                auto& player = w.player();
                player.setPositionSamples (player.getTotalLengthSamples() - kBlock / 2);
                player.setLoopEnabled (true);
                expect (player.hasAudioAhead());
                player.start();
                std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
                float* outs[] { left.data(), right.data() };
                // Starting is a jump, and a jump waits for every track's audio,
                // so the first blocks may hold the playhead where it is.
                const auto start = player.getPositionSamples();
                for (int i = 0; i < 400 && player.getPositionSamples() == start; ++i)
                {
                    player.processBlock (outs, 2, kBlock);
                    if (player.getPositionSamples() == start) juce::Thread::sleep (5);
                }
                expect (player.isPlaying(), "playback stopped at the session end");
                expectEquals (player.getPositionSamples(), (juce::int64) (kBlock - kBlock / 2));
                player.stop();
                player.setLoopEnabled (false);
            }

            beginTest ("A loop already on never wraps a punch take");
            {
                Window w;
                auto& player = w.player();
                player.setLoopRegion (0, 1000);
                player.setLoopEnabled (true);
                player.setPositionSamples (900);
                player.setSeekLocked (true);
                player.start();
                std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
                float* outs[] { left.data(), right.data() };
                player.processBlock (outs, 2, kBlock);
                expectEquals (player.getPositionSamples(), (juce::int64) (900 + kBlock),
                              "the punch clock wrapped");
                player.stop();
                player.setSeekLocked (false);
                player.setLoopEnabled (false);
            }
        }

        void writeSavedRegion (const juce::File& session, juce::int64 start, juce::int64 end)
        {
            juce::DynamicObject::Ptr root (new juce::DynamicObject());
            root->setProperty ("loopStart", start);
            root->setProperty ("loopEnd", end);
            session.getChildFile ("session_settings.json")
                   .replaceWithText (juce::JSON::toString (juce::var (root.get())));
        }

        void savedRegions()
        {
            beginTest ("A saved range loops and edits, and never arms a punch");
            {
                Window w;
                const auto total = w.player().getTotalLengthSamples();
                writeSavedRegion (w.session, 1000, 20000);
                w.open (w.session);
                expect (w.player().hasLoopRegion(), "the saved range did not come back");
                expect (! w.player().isRangeArmed(), "a saved range armed a punch");
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                expect (! w.main.punchSessionActive, "a saved range punched");
                feedAll (w.recorder(), -0.3f, 4);
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 0), 0, total),
                                           levelOf (0), 1.0e-3f);
            }

            beginTest ("Touching a saved range arms it");
            {
                Window w;
                writeSavedRegion (w.session, 1000, 20000);
                w.open (w.session);
                w.player().setPositionSamples (5000);
                w.main.editStartRange();
                expect (w.player().isRangeArmed(), "moving an edge of the range did not arm it");
            }

            beginTest ("A whole-timeline region from an older build is dropped and not saved back");
            {
                Window w;
                const auto total = w.player().getTotalLengthSamples();
                writeSavedRegion (w.session, 0, total);
                w.open (w.session);
                expect (! w.player().hasLoopRegion(), "the implicit region came back");
                expect (w.main.saveSessionStateTo (w.session));
                const auto saved = juce::JSON::parse (w.session.getChildFile ("session_settings.json"));
                expect (saved.isObject() && ! saved.hasProperty ("loopStart"));
            }
        }

        void oneSelectionOnePunch()
        {
            beginTest ("One selection arms one punch; the range then stays for Loop and edits");
            {
                Window w;
                w.arm ({ 0 });
                const auto in = (juce::int64) 150 * kBlock, out = (juce::int64) 160 * kBlock;
                w.player().selectRange (in, out);
                expect (w.player().isRangeArmed());
                w.main.onRecordClicked();
                expect (w.main.punchSessionActive, w.main.statusLabel.getText());
                expect (w.engine().isRecording());
                expect (! w.player().isRangeArmed(), "the range stayed armed for the next RECORD");
                expect (w.player().hasLoopRegion(), "the punch took the selection away");

                feedAll (w.recorder(), -0.3f, 10, in);
                w.main.onRecordClicked();
                expect (w.stopped());
                expect (! w.main.punchSessionActive);
                w.engine().stopPlayback();

                const auto next = w.plan();
                expect (next.continueAppend, "the next RECORD punches again");
                expect (! w.main.recordPlanPunches (next));
            }

            beginTest ("A refused punch keeps the selection armed");
            {
                Window w;
                w.arm ({ 0 });
                w.player().selectRange (1000, 2000);
                w.engine().setSessionTransitionActive (true);
                auto plan = w.plan();
                plan.dir = w.session;
                plan.numTracks = 2;
                plan.armed = 1;
                w.main.runRecordPlan (plan);
                w.engine().setSessionTransitionActive (false);
                expect (! w.engine().isRecording());
                expect (w.player().isRangeArmed(), "a refused start used up the selection");
                expect (! w.engine().isPunchModeOn(), "a refused start left PUNCH mode on");
            }

            beginTest ("Half a range is a default and arms nothing");
            {
                Window w;
                w.player().setPositionSamples (5000);
                w.main.editStartRange();
                expect (w.player().hasLoopRegion());
                expect (! w.player().isRangeArmed(), "Range In alone armed a punch");
                w.player().setPositionSamples (9000);
                w.main.editFinishRange();
                expect (w.player().isRangeArmed(), "In and Out together are a selection");
                w.player().clearLoopRegion();
                expect (! w.player().isRangeArmed());

                w.player().setPositionSamples (9000);
                w.main.editFinishRange();
                expect (w.player().hasLoopRegion());
                expect (! w.player().isRangeArmed(), "Range Out alone armed a punch from zero");

                w.player().clearLoopRegion();
                w.main.editSetRangeToLoopRange();
                expect (w.player().hasLoopRegion());
                expect (! w.player().isRangeArmed(), "the default loop range armed a punch");
            }
        }

        void dismissQuestion (Window& w)
        {
            expect (w.main.cursorPunchQuestion != nullptr, "no question to dismiss");
            if (w.main.cursorPunchQuestion == nullptr) return;
            w.main.cursorPunchQuestion->exitModalState (0);
            expect (pumpUntil ([&w] { return w.main.cursorPunchQuestion == nullptr; }), "the question stayed open");
        }

        void parkedPlayheadAppends()
        {
            beginTest ("Only a chosen position punches");
            {
                const juce::int64 capture = 48000, timeline = 96000;
                expect (! manualRecordPunches (false, -1, 24000, capture, timeline), "a parked playhead punched");
                expect (! manualRecordPunches (false, -1, 0, capture, timeline));
                expect (manualRecordPunches (true, -1, 24000, capture, timeline));
                expect (manualRecordPunches (false, 24000, 24000, capture, timeline));
                expect (manualRecordPunches (false, 0, 0, capture, timeline), "a cursor at 0 is deliberate");
                expect (manualRecordPunches (false, 60000, 60000, capture, timeline), "a cursor in the bed is deliberate");
                expect (! manualRecordPunches (true, -1, 96000, capture, timeline));
            }

            beginTest ("A paused or located playhead appends; a placed cursor and a rolling one punch");
            {
                Window w;
                w.player().setPositionSamples (20000);
                auto parked = w.plan();
                expect (parked.continueTake && parked.continueAppend, "a parked playhead punched");
                expectEquals (parked.punchAt, (juce::int64) -1);

                w.engine().setEditCursorSample (20000);
                auto placed = w.plan();
                expectEquals (placed.punchAt, (juce::int64) 20000);
                w.engine().clearEditCursor();

                w.player().start();
                auto rolling = w.plan();
                expectEquals (rolling.punchAt, (juce::int64) 20000);
                w.player().stop();
            }
        }

        void punchModeUsesLiveArms()
        {
            beginTest ("PUNCH mode punches the tracks the R buttons show at punch-in");
            {
                Window w (3);
                const auto in = (juce::int64) 100 * kBlock, out = (juce::int64) 120 * kBlock;
                w.arm ({ 0, 1 });
                w.main.togglePunchMode();
                // After the mode went on: track 2 protected, track 3 added.
                w.arm ({ 0, 2 });
                w.player().selectRange (in, out);
                w.player().setPositionSamples (in - 10 * kBlock);
                w.engine().startPlayback();
                w.main.servicePunch();
                expect (! w.engine().isRecording(), "a punch opened before the range");

                w.player().setPositionSamples (in + 8);
                w.main.servicePunch();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                expect (w.recorder().getTrack (0).armed.load());
                expect (! w.recorder().getTrack (1).armed.load(), "punch-in re-armed a disarmed strip");
                expect (w.recorder().getTrack (2).armed.load(), "punch-in disarmed an armed strip");
                expect (! w.engine().setTrackArmed (1, true) || ! w.recorder().getTrack (1).armed.load(),
                        "arms moved while the punch rolled");

                feedAll (w.recorder(), -0.3f, 10, in + 8);
                w.engine().stopPlayback();
                w.main.servicePunch();
                expect (w.stopped());
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 0), in + 8, 5 * kBlock),
                                           -0.3f, 1.0e-3f, "an armed strip was not punched");
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 1), in + 8, 5 * kBlock),
                                           levelOf (1), 1.0e-3f, "a disarmed strip was overwritten");
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 2), in + 8, 5 * kBlock),
                                           -0.3f, 1.0e-3f, "a strip armed after PUNCH went on was left out");
                expect (w.recorder().getTrack (0).armed.load());
                expect (! w.recorder().getTrack (1).armed.load(), "the punch changed the arm layout");
                expect (w.recorder().getTrack (2).armed.load());
                w.main.togglePunchMode();
            }

            beginTest ("Turning PUNCH on inside the range opens nothing until a pass crosses in");
            {
                Window w;
                w.arm ({ 0 });
                w.player().selectRange (1000, 90000);
                w.player().setPositionSamples (20000);
                w.engine().startPlayback();
                w.main.togglePunchMode();
                w.main.servicePunch();
                expect (! w.engine().isRecording(), "turning the mode on opened a punch");
                w.main.togglePunchMode();
                w.engine().stopPlayback();
            }

            beginTest ("PUNCH mode never punches a saved range");
            {
                Window w;
                w.arm ({ 0 });
                writeSavedRegion (w.session, 1000, 20000);
                w.open (w.session);
                w.arm ({ 0 });
                w.main.togglePunchMode();
                expect (w.engine().isPunchModeOn());
                w.player().setPositionSamples (0);
                w.engine().startPlayback();
                w.main.servicePunch();
                w.player().setPositionSamples (5000);
                w.main.servicePunch();
                expect (! w.engine().isRecording(), "a saved range punched in PUNCH mode");
                w.main.togglePunchMode();
                w.engine().stopPlayback();
            }

            beginTest ("PUNCH mode leaves a live take to its two-tap STOP");
            {
                Window w;
                w.arm ({ 0 });
                w.main.togglePunchMode();
                w.player().selectRange (1000, 2000);
                w.player().setPositionSamples (1500);
                w.main.servicePunch();              // parked inside, not rolling
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.player().setPositionSamples (5000);
                w.main.servicePunch();
                expect (w.engine().isRecording(), "the punch window ended a take it did not open");
                w.main.togglePunchMode();
                expect (w.engine().isRecording(), "the PUNCH menu item ended a live take in one click");
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("LOCK holds a punch-in back");
            {
                Window w;
                w.arm ({ 0 });
                w.main.togglePunchMode();
                w.player().selectRange (1000, 90000);
                w.player().setPositionSamples (0);
                w.engine().startPlayback();
                w.main.servicePunch();
                w.main.onLockToggled();
                w.player().setPositionSamples (5000);
                w.main.servicePunch();
                expect (! w.engine().isRecording(), "a punch opened under LOCK");
                w.main.onLockToggled();
                w.main.togglePunchMode();
                w.engine().stopPlayback();
            }
        }

        void punchModeLeavesWithTheSession()
        {
            beginTest ("Opening another session turns PUNCH off and keeps the next session's loop off");
            {
                Window w;
                w.arm ({ 0 });
                w.player().setLoopEnabled (true);
                w.main.togglePunchMode();
                expect (w.engine().isPunchModeOn());
                expect (! w.player().isLoopEnabled(), "PUNCH mode left loop-play on");
                const auto other = w.newSession (2, 200);
                w.open (other);
                expect (! w.engine().isPunchModeOn(), "PUNCH mode followed the engineer into the next session");
                expect (! w.player().isLoopEnabled());
                w.main.togglePunchMode();
                w.main.togglePunchMode();
                expect (! w.player().isLoopEnabled(), "the old session's loop came back");
            }

            beginTest ("A new session turns PUNCH off too");
            {
                Window w;
                w.main.togglePunchMode();
                NewSessionDialog::Result r;
                r.name = "Next";
                r.location = tempDir ("next-root");
                w.dirs.add (r.location);
                expect (w.main.createSessionFolderStructure (r).isDirectory());
                expect (! w.engine().isPunchModeOn());
            }

            beginTest ("Closing the session turns PUNCH off");
            {
                Window w;
                w.main.togglePunchMode();
                w.engine().clearSessionState();
                expect (! w.engine().isPunchModeOn());
            }

            beginTest ("A moved strip keeps its punch");
            {
                Window w;
                w.arm ({ 0 });
                w.main.togglePunchMode();
                expect (w.engine().reorderTracks ({ 1, 0 }));
                expect (! w.recorder().getTrack (0).armed.load());
                expect (w.recorder().getTrack (1).armed.load(), "the arm did not move with the strip");
                const auto in = (juce::int64) 100 * kBlock;
                w.player().selectRange (in, in + 20 * kBlock);
                w.player().setPositionSamples (in - 4 * kBlock);
                w.engine().startPlayback();
                w.main.servicePunch();
                w.player().setPositionSamples (in + 8);
                w.main.servicePunch();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                feedAll (w.recorder(), -0.3f, 10, in + 8);
                w.engine().stopPlayback();
                w.main.servicePunch();
                expect (w.stopped());
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 0), in + 8, 5 * kBlock),
                                           levelOf (1), 1.0e-3f, "the strip now at the old index was punched");
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 1), in + 8, 5 * kBlock),
                                           -0.3f, 1.0e-3f, "the moved strip was not punched");
                w.main.togglePunchMode();
            }

            beginTest ("PUNCH shows in the main window, and so does a RECORD that would punch");
            {
                Window w;
                w.main.updatePunchPill();
                expect (! w.main.punchPill.isVisible());
                w.main.togglePunchMode();
                w.main.updatePunchPill();
                expect (w.main.punchPill.isVisible());
                expect (w.main.punchPill.getText().contains ("PUNCH MODE"));
                w.main.togglePunchMode();
                w.main.updatePunchPill();
                expect (! w.main.punchPill.isVisible());

                w.player().selectRange (1000, 2000);
                w.main.updatePunchPill();
                expect (w.main.punchPill.getText().containsIgnoreCase ("range"));
                w.player().clearLoopRegion();
                w.engine().setEditCursorSample (1000);
                w.main.updatePunchPill();
                expect (w.main.punchPill.getText().containsIgnoreCase ("cursor"));
                w.engine().clearEditCursor();
                w.main.updatePunchPill();
                expect (! w.main.punchPill.isVisible());

                w.main.togglePunchMode();
                w.main.updatePunchPill();
                w.open (w.session);
                w.main.updatePunchPill();
                expect (! w.main.punchPill.isVisible(), "the pill outlived the session");
            }
        }

        void lockHolds()
        {
            beginTest ("LOCK survives a strip rebuild");
            {
                Window w;
                w.main.rebuildStrips();
                expect (! w.main.strips.empty());
                w.main.onLockToggled();
                w.main.rebuildStrips();
                for (auto& s : w.main.strips) expect (! s->isEnabled(), "a rebuilt strip came back live under LOCK");
                w.main.gridButton.onClick();
                w.main.rebuildStrips();
                for (auto& s : w.main.strips) expect (! s->isEnabled());
                w.main.gridButton.onClick();
                w.main.onLockToggled();
                for (auto& s : w.main.strips) expect (s->isEnabled(), "UNLOCK left a strip dead");
                w.main.rebuildStrips();
                for (auto& s : w.main.strips) expect (s->isEnabled());
            }

            beginTest ("LOCK stops the right-click menus on strips and the setlist");
            {
                Window w;
                w.main.rebuildStrips();
                auto& strip = *w.main.strips.front();
                expect (rightClickOpensMenu (strip), "the fixture never opened a strip menu");
                expect (rightClickOpensMenu (w.main.setlistBar), "the fixture never opened a setlist menu");
                w.main.onLockToggled();
                expect (! rightClickOpensMenu (strip), "a strip menu opened under LOCK");
                expect (! rightClickOpensMenu (w.main.setlistBar), "a setlist menu opened under LOCK");
                w.main.onLockToggled();
            }

            beginTest ("LOCK refuses a cue action from a menu that was already open");
            {
                Window w;
                w.main.cues.push_back ({});
                w.main.cues.back().name = "Keep";
                w.main.currentCueIndex = 0;
                w.main.onLockToggled();
                w.main.setlistBar.onDeleteCue();
                expectEquals ((int) w.main.cues.size(), 1, "a cue was deleted under LOCK");
                w.main.onLockToggled();
            }

            beginTest ("LOCK makes EDIT rows click-through, as a take does");
            {
                Window w;
                w.main.switchView (MainComponent::View::Edit);
                expect (pumpUntil ([&] { return w.main.pendingMetadataSaves == 0; }), "the layout save never ended");
                expect (w.main.editPage != nullptr);
                expect (! w.main.editPage->isTimelineReadOnly(), "the fixture starts read-only");
                w.main.onLockToggled();
                expect (w.main.editPage->isTimelineReadOnly(), "EDIT rows took clicks under LOCK");
                w.main.onLockToggled();
                expect (! w.main.editPage->isTimelineReadOnly());
                w.main.switchView (MainComponent::View::Mix);
            }
        }

        void alarmsStandOut()
        {
            beginTest ("A failing take shows in the danger banner until it clears, and never blocks STOP");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                status.backupFailed = true;
                w.main.updateCaptureWarnings (status);
                expect (w.main.alarmBanner.isVisible(), "a failed backup write stayed in the header text");
                expect (w.main.alarmBanner.getText().contains ("BACKUP WRITE FAILED"), w.main.alarmBanner.getText());
                expect (w.main.alarmBanner.getLevel() == AlarmBanner::Level::Danger);
                expect (w.main.alarmBanner.getHeight() >= 36, "the banner is too small to read from a distance");
                expect (w.main.alarmBanner.getBottom() <= w.main.bigClock.getY(), "the banner covers the clock");
                bool childClicks = true, selfClicks = true;
                w.main.alarmBanner.getInterceptsMouseClicks (selfClicks, childClicks);
                expect (! selfClicks && ! childClicks, "the banner can take a click meant for STOP");
                expect (w.main.alarmBanner.getBottom() <= w.main.transportBar->getY(), "the banner covers the transport");

                status.primaryFailed = true;
                status.channelAlarms.push_back ({});
                status.channelAlarms.back().kind = channelalarm::kStream;
                status.channelAlarms.back().channels = 8;
                status.channelAlarms.back().name = "Dante";
                w.main.updateCaptureWarnings (status);
                expect (w.main.alarmBanner.getText().contains ("INPUT STREAM LOST"));
                expect (w.main.alarmBanner.getText().contains ("PRIMARY WRITE FAILED"));

                w.main.updateCaptureWarnings (EngineStatus());
                expect (! w.main.alarmBanner.isVisible(), "the banner outlived the condition");
            }

            beginTest ("An alarm moves nothing and covers no header button, at any window size");
            {
                Window w;
                for (const auto& [width, height] : { std::pair (1120, 700), std::pair (1440, 900), std::pair (2200, 1200) })
                {
                    w.main.setSize (width, height);
                    w.main.resized();
                    const auto strips = w.main.stripsViewport.getBounds();
                    const auto clock  = w.main.bigClock.getBounds();

                    EngineStatus status;
                    status.recording = true;
                    status.backupFailed = true;
                    w.main.updateCaptureWarnings (status);
                    w.main.resized();
                    expect (w.main.alarmBanner.isVisible());
                    expect (w.main.stripsViewport.getBounds() == strips, "the strips jumped when the alarm came up");
                    expect (w.main.bigClock.getBounds() == clock, "the clock jumped when the alarm came up");
                    expect (w.main.alarmBanner.getWidth() >= 400, "the banner is too short at width " + juce::String (width));
                    for (auto* button : std::initializer_list<juce::Component*>
                             { &w.main.metersButton, &w.main.backupButton, &w.main.lockButton,
                               &w.main.addChannelButton, &w.main.vscButton, &w.main.deviceButton })
                        expect (! w.main.alarmBanner.getBounds().intersects (button->getBounds()),
                                "the banner covers a header button at width " + juce::String (width));
                    w.main.updateCaptureWarnings (EngineStatus());
                }
            }

            beginTest ("Device loss, a dead channel and a struggling disk all reach the banner");
            {
                Window w;
                const auto raises = [&] (std::function<void (EngineStatus&)> condition)
                {
                    EngineStatus status;
                    status.recording = true;
                    condition (status);
                    w.main.updateCaptureWarnings (status);
                    const bool shown = w.main.alarmBanner.isVisible();
                    w.main.updateCaptureWarnings (EngineStatus());
                    return shown;
                };
                expect (raises ([] (EngineStatus& s) { s.captureDeviceLost = true; }));
                expect (raises ([] (EngineStatus& s) { s.diskStruggling = true; }));
                expect (raises ([] (EngineStatus& s) { s.mirrorFailed = true; }));
                expect (raises ([] (EngineStatus& s) { s.mirrorsSkipped = 1; }));
                expect (raises ([] (EngineStatus& s) { s.recoveryMarkerFailed = true; }));
                expect (raises ([] (EngineStatus& s) { s.source = "daemon-unavailable"; }));
                expect (raises ([] (EngineStatus& s)
                {
                    s.channelAlarms.push_back ({});
                    s.channelAlarms.back().kind = channelalarm::kChannel;
                    s.channelAlarms.back().name = "Vox";
                }));
            }

            beginTest ("A channel with no signal yet is a notice, not a danger");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                status.channelAlarms.push_back ({});
                status.channelAlarms.back().kind = channelalarm::kNoSignal;
                status.channelAlarms.back().name = "Vox";
                w.main.updateCaptureWarnings (status);
                expect (w.main.alarmBanner.isVisible());
                expect (w.main.alarmBanner.getLevel() == AlarmBanner::Level::Notice);
                w.main.updateCaptureWarnings (EngineStatus());
            }

            beginTest ("A failed finalization stands after STOP until the next take or session");
            {
                Window w;
                w.main.lastCaptureFinalizationError = "Recording stopped, but finalization failed: backup audio write failed";
                w.main.refreshAlarmBanner();
                expect (w.main.alarmBanner.getText().contains ("finalization failed"));
                w.open (w.session);
                w.main.refreshAlarmBanner();
                expect (! w.main.alarmBanner.isVisible(), "a failed take followed the engineer into the next session");
            }

            beginTest ("A refused RECORD shows in the banner, and the banner then clears");
            {
                Window w;
                w.arm ({});
                w.main.onRecordClicked();
                expect (! w.engine().isRecording());
                expect (w.main.alarmBanner.isVisible(), "a refused RECORD stayed in the header text");
                expect (w.main.alarmBanner.getText().containsIgnoreCase ("arm"), w.main.alarmBanner.getText());
                expect (w.main.alarmBanner.getLevel() == AlarmBanner::Level::Notice,
                        "a refused press used the red that means a take is in trouble");
                w.main.refreshAlarmBanner();
                expect (w.main.alarmBanner.isVisible(), "the refusal vanished at once");
                w.main.recordRefusalShownMs -= 60000;
                w.main.refreshAlarmBanner();
                expect (! w.main.alarmBanner.isVisible(), "the refusal never cleared");

                w.main.onRecordClicked();
                expect (w.main.alarmBanner.isVisible());
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                w.main.refreshAlarmBanner();
                expect (! w.main.alarmBanner.isVisible(), "the refusal stayed up over a running take");
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("A refusal further down pre-flight reaches the banner too");
            {
                Window w;
                w.arm ({ 0 });
                auto& manager = w.engine().getDeviceManager();
                manager.closeAudioDevice();
                w.main.onRecordClicked();
                expect (w.main.alarmBanner.getText().containsIgnoreCase ("device"), w.main.alarmBanner.getText());
                manager.restartLastAudioDevice();
                expect (manager.getCurrentAudioDevice() != nullptr, "the fixture lost its device");
            }
        }

        void statusPanel()
        {
            beginTest ("Master loudness stays on screen in both views at every window size");
            {
                Window w;
                for (const auto view : { MainComponent::View::Mix, MainComponent::View::Edit })
                {
                    w.main.switchView (view);
                    for (const auto& [width, height] : { std::pair (1120, 700), std::pair (1320, 820), std::pair (2200, 1200) })
                    {
                        w.main.setSize (width, height);
                        w.main.resized();
                        expect (w.main.perfDashboard.isVisible());
                        expect (w.main.perfDashboard.loudnessFits(), "the readout does not fit at width " + juce::String (width));
                    }
                }
                w.main.switchView (MainComponent::View::Mix);
            }

            beginTest ("The loudness line reads what the master measures and marks a hot true peak");
            {
                PerfDashboard panel;
                expectEquals (panel.getLoudnessText(), juce::String ("LUFS M -- I -- TP --"));
                expect (! panel.truePeakIsHot());

                panel.setLoudness (-23.14f, -18.26f, -3.0f);
                expectEquals (panel.getLoudnessText(), juce::String ("LUFS M -18.3 I -23.1 TP -3.0"));
                expect (! panel.truePeakIsHot());

                panel.setLoudness (-23.1f, -18.3f, -0.9f);
                expect (panel.truePeakIsHot(), "true peak within 1 dB of full scale stayed calm");
                expectEquals (panel.getTruePeakText(), juce::String ("TP -0.9!"), "a hot peak rested on colour alone");
            }
        }

        void soundcheckIsVisibleAndReversible()
        {
            beginTest ("Virtual Soundcheck asks first, shows it is on, and gives the old patch back");
            {
                Window w;
                auto& track = w.recorder().getTrack (0);
                w.engine().setTrackInputRouting (0, 3);
                w.engine().setTrackOutputRouting (0, 7);

                std::function<void()> pending;
                w.main.confirmForTests = [&pending] (std::function<void()> go) { pending = std::move (go); };
                w.main.onVscClicked();
                expectEquals (track.outputRouting.load(), 7, "VSC repatched before the engineer agreed");
                expect (pending != nullptr, "VSC did not ask");

                pending();
                pending = nullptr;
                expectEquals (track.outputRouting.load(), 3, "the output did not move onto the input's channel");
                w.main.updateSoundcheckState();
                expect (w.main.vscButton.getToggleState(), "VSC looks off with the outputs repatched");
                expectEquals (w.main.vscButton.getButtonText(), juce::String ("VSC ON"));

                expectEquals (vsc::repatch (w.engine()), 0, "a second press would save the soundcheck patch as the show patch");
                expectEquals (track.outputBeforeVsc.load(), 7);

                expect (w.engine().saveSessionMixTo (w.session));
                track.outputBeforeVsc.store (TrackState::kNoSavedOutput);
                expect (w.engine().loadSessionMixFrom (w.session));
                expectEquals (track.outputBeforeVsc.load(), 7, "the way back did not survive a reopen");

                w.main.onVscClicked();
                expect (pending != nullptr, "restoring did not ask");
                pending();
                pending = nullptr;
                expectEquals (track.outputRouting.load(), 7, "the old patch did not come back");
                w.main.updateSoundcheckState();
                expect (! w.main.vscButton.getToggleState(), "VSC stayed lit after the restore");
                expectEquals (w.main.vscButton.getButtonText(), juce::String ("VSC"));

                expect (w.main.undoManager.undo(), "the restore is not on the undo stack");
                expectEquals (track.outputRouting.load(), 3);
                expect (vsc::hasSavedPatch (w.engine()), "undo lost the saved patch");
                w.main.confirmForTests = nullptr;
            }

            beginTest ("Clearing the strips forgets the soundcheck, so a new session starts with VSC off");
            {
                Window w;
                vsc::repatch (w.engine());
                expect (vsc::hasSavedPatch (w.engine()));
                w.engine().clearAllStripOverrides();
                expect (! vsc::hasSavedPatch (w.engine()), "a new session inherited the last one's VSC state");

                vsc::repatch (w.engine());
                w.engine().clearSessionState();
                expect (! vsc::hasSavedPatch (w.engine()), "another session inherited this one's VSC state");
            }

            beginTest ("A strip added after the soundcheck keeps its own output on restore");
            {
                Window w;
                w.engine().setTrackOutputRouting (0, 7);
                vsc::repatch (w.engine());
                auto& later = w.recorder().getTrack (1);
                later.outputBeforeVsc.store (TrackState::kNoSavedOutput);
                w.engine().setTrackOutputRouting (1, 4);
                vsc::restore (w.engine());
                expectEquals (w.recorder().getTrack (0).outputRouting.load(), 7);
                expectEquals (later.outputRouting.load(), 4);
            }

            beginTest ("VSC sits with the actions, not the view tabs");
            {
                Window w;
                expect (w.main.vscButton.getBottom() <= w.main.transportBar->getY());
                expect (w.main.mixViewButton.getY() >= w.main.transportBar->getY());
            }

            beginTest ("A desk left on the soundcheck patch stays on screen");
            {
                Window w;
                w.main.updateSoundcheckState();
                expect (! w.main.soundcheckPill.isVisible());

                juce::DynamicObject::Ptr state (new juce::DynamicObject());
                juce::Array<juce::var> blocks;
                for (int block = 0; block < w.main.consoleLink.getProfile().numInBlocks; ++block)
                    blocks.add (block);
                state->setProperty ("stagePatch", blocks);
                state->setProperty ("deskPatch", "soundcheck");
                w.main.consoleLink.loadJson (juce::var (state.get()));
                expect (w.main.consoleLink.isOnSoundcheckPatch());

                w.main.updateSoundcheckState();
                expect (w.main.soundcheckPill.isVisible(), "the desk's patch is only in a menu");
                expect (w.main.soundcheckPill.getWidth() > 100, "the chip has no room");

                // A desk on card returns outlives every session, and this window is shared.
                w.main.consoleLink.setProfile (ConsoleProfile::Kind::GenericOsc);
                w.main.consoleLink.setProfile (ConsoleProfile::Kind::BehringerX32);
                w.main.updateSoundcheckState();
            }

            beginTest ("A pre-flight problem offers the control that deals with it");
            {
                int fixed = 0;
                const auto fix = [&fixed] { ++fixed; };
                PreflightRow backup { PreflightRow::Status::failed, "Backup", "not configured" };
                backup.fixLabel = "Backup";
                backup.fix = fix;
                PreflightRow healthy { PreflightRow::Status::ok, "Audio device", "Silent" };
                healthy.fixLabel = "Device";
                healthy.fix = fix;
                PreflightRow slowDrive { PreflightRow::Status::warning, "Disk write speed", "slow" };

                PreflightDialog dialog ({ healthy, backup, slowDrive });
                dialog.resized();
                expectEquals (dialog.getNumFixes(), 1, "only a problem with a control gets a button");
                expect (dialog.getFixButton (0)->getWidth() > 0);
                expectEquals (dialog.getFixButton (0)->getButtonText(), juce::String ("Backup"));
                dialog.getFixButton (0)->onClick();
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
                expectEquals (fixed, 1);
            }
        }

        void screensAgree()
        {
            beginTest ("Strip width and GRID show only where they do something");
            {
                Window w;
                w.main.switchView (MainComponent::View::Edit);
                w.main.resized();
                for (auto* button : { &w.main.gridButton, &w.main.stripXsButton, &w.main.stripLButton })
                    expect (! button->isVisible(), "a mixer control sits dead in EDIT");
                w.main.switchView (MainComponent::View::Mix);
                w.main.resized();
                for (auto* button : { &w.main.gridButton, &w.main.stripXsButton, &w.main.stripLButton })
                    expect (button->isVisible());
            }

            beginTest ("The small clock follows a take instead of sitting frozen beside the big one");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                feedAll (w.recorder(), 0.1f, 8);
                w.main.updateTransportLabels();
                expect (! w.main.transportLabel.getText().contains ("/"), w.main.transportLabel.getText());
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
                w.main.updateTransportLabels();
                expect (w.main.transportLabel.getText().contains ("/"), w.main.transportLabel.getText());
            }

            beginTest ("Every fader on the page starts and ends level, stereo or mono, at every strip width");
            {
                Window w;
                w.engine().setTrackStereo (2, true);
                w.main.lastTrackCount = -1;
                w.main.syncStripsWithEngine();
                const auto fader = [] (ChannelStrip& strip)
                {
                    for (auto* child : strip.getChildren())
                        if (auto* slider = dynamic_cast<juce::Slider*> (child))
                            if (slider->getSliderStyle() == juce::Slider::LinearVertical)
                                return slider->getBounds();
                    return juce::Rectangle<int>();
                };
                for (auto width : { MainComponent::StripWidth::XS, MainComponent::StripWidth::S,
                                    MainComponent::StripWidth::M,  MainComponent::StripWidth::L })
                {
                    w.main.setStripWidthPreset (width);
                    w.main.resized();
                    expect (w.main.strips.size() >= 2);
                    const auto first = fader (*w.main.strips[0]);
                    expect (first.getHeight() > 0);
                    for (auto& strip : w.main.strips)
                    {
                        expectEquals (fader (*strip).getY(), first.getY(), "0 dB sits at two heights");
                        expectEquals (fader (*strip).getHeight(), first.getHeight());
                    }
                }
            }
        }

        void sessionLeaving()
        {
            using Action = AudioEngine::RemoteTransportAction;

            beginTest ("A failed take never stops a remote STOP from stopping playback");
            {
                Window w;
                w.main.lastCaptureFinalizationError = "Recording stopped, but finalization failed: mirror audio write failed";
                w.player().setPositionSamples (20000);
                w.engine().startPlayback();
                expect (w.player().isPlaying());
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StopAll, error), "the failure was not reported");
                expect (error.containsIgnoreCase ("finalization"), error);
                expect (! w.player().isPlaying(), "playback kept rolling");
            }

            beginTest ("An idle local STOP no longer repeats an old failure");
            {
                Window w;
                w.main.lastCaptureFinalizationError = "Recording stopped, but finalization failed: mirror audio write failed";
                w.main.statusLabel.setText ({}, juce::dontSendNotification);
                w.main.onStopClicked();
                expect (! w.main.statusLabel.getText().containsIgnoreCase ("finalization"), w.main.statusLabel.getText());
            }

            beginTest ("The setlist bar ends with the session");
            {
                Window w;
                w.main.cues.push_back ({});
                w.main.cues.back().name = "Old";
                w.main.currentCueIndex = 0;
                w.main.setlistBar.setCues (w.main.cues, 0);
                w.main.engine.clearSessionState();
                w.main.forgetSessionUiState();
                expect (w.main.cues.empty());
                for (auto* child : w.main.setlistBar.getChildren())
                    if (auto* combo = dynamic_cast<juce::ComboBox*> (child))
                        expectEquals (combo->getNumItems(), 0, "the setlist bar kept the closed session's cues");
            }

            beginTest ("A new session from any path starts without the old cues");
            {
                Window w;
                w.main.cues.push_back ({});
                w.main.cues.back().name = "Old";
                w.main.currentCueIndex = 0;
                NewSessionDialog::Result r;
                r.name = "Fresh";
                r.location = tempDir ("fresh-root");
                w.dirs.add (r.location);
                expect (w.main.createSessionFolderStructure (r).isDirectory());
                expect (w.main.cues.empty(), "the new session inherited the old cues");
            }
        }

        void soundcheckReport()
        {
            beginTest ("The soundcheck report keeps its levels and bands, and no longer guesses");
            {
                Window w;
                auto& track = w.recorder().getTrack (0);
                std::fill (track.fftSnapshot.begin(), track.fftSnapshot.end(), 0.5f);
                track.fftBlockReady.store (true, std::memory_order_release);
                w.main.writeSoundcheckReport();
                const auto report = juce::JSON::parse (w.session.getChildFile ("soundcheck.report.json"));
                const auto strip = report["strips"][0];
                expect (strip.hasProperty ("name") && strip.hasProperty ("peak") && strip.hasProperty ("clipCount"));
                expect ((double) strip["bandSub"] > 0.0, "the bands left the report");
                expect (! strip.hasProperty ("guess"), "the report still guesses a name");
            }

            beginTest ("A strip releases the FFT snapshot, so the next report is current");
            {
                Window w;
                w.main.rebuildStrips();
                auto& track = w.recorder().getTrack (0);
                track.fftBlockReady.store (true, std::memory_order_release);
                expect (pumpUntil ([&] { return ! track.fftBlockReady.load (std::memory_order_acquire); }),
                        "nothing lets the audio thread publish another snapshot");
            }
        }

        void analysisStaysOutOfTheTake()
        {
            beginTest ("A cancelled scan stops and writes no report");
            {
                Window w;
                const std::atomic<bool> cancelled { true };
                expectEquals (qc::analyzeFile (trackFile (w.session, 0), &cancelled).clipEventCount, 0);
                double rate = 0.0;
                expect (songs::envelopeFlags (trackFile (w.session, 0), -40.0f, 1.0, rate, &cancelled).empty());
            }

            beginTest ("A second scan refuses, and RECORD never waits for one");
            {
                Window w;
                w.arm ({ 0 });
                w.main.analysisBusy.store (true);
                w.main.runQcAnalysis();
                expect (w.main.statusLabel.getText().containsIgnoreCase ("already"), w.main.statusLabel.getText());
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                w.main.analysisBusy.store (false);
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("A take cancels a running scan, and no report opens over it");
            {
                Window w;
                w.arm ({ 0 });
                w.main.runQcAnalysis();
                expect (w.main.analysisBusy.load());
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.main.cancelAnalysisForTake();
                expect (pumpUntil ([&] { return ! w.main.analysisBusy.load(); }), "the scan never ended");
                expectEquals (juce::ModalComponentManager::getInstance()->getNumModalComponents(), 0,
                              "a report dialog opened over the take");
                expect (w.main.statusLabel.getText().containsIgnoreCase ("stopped"), w.main.statusLabel.getText());
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("A scan's result is held back from a take and from another session");
            {
                Window w;
                expect (w.main.analysisResultWanted (w.session));
                expect (! w.main.analysisResultWanted (juce::File ("/tmp/zf-other-session")),
                        "a report opened for the wrong session");
                w.main.captureMetadataPending = true;
                expect (! w.main.analysisResultWanted (w.session), "a report opened over finalization");
                w.main.captureMetadataPending = false;
            }
        }

        void consoleWaitsForTheTake()
        {
            beginTest ("Console repatch and head-amp restore wait for the take to end");
            {
                Window w;
                std::vector<juce::OSCMessage> sent;
                w.main.consoleLink.setSendHook ([&] (const auto& m) { sent.push_back (m); });
                w.arm ({ 0 });
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.main.consoleToggleSoundcheck();
                expect (sent.empty(), "the desk was repatched under a live take");
                w.main.consoleRestoreGains();
                expect (sent.empty(), "the head-amps were rewritten under a live take");
                expect (w.main.statusLabel.getText().containsIgnoreCase ("take"), w.main.statusLabel.getText());
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
                w.main.consoleToggleSoundcheck();
                expect (! sent.empty(), "the repatch stayed refused after the take");
                w.main.consoleLink.setSendHook (nullptr);
            }
        }

        static int modalCount()
        {
            return juce::ModalComponentManager::getInstance()->getNumModalComponents();
        }

        // Ends every open modal by its Cancel-style result and lets it clean up.
        static void dismissModals()
        {
            auto* modals = juce::ModalComponentManager::getInstance();
            for (int i = modals->getNumModalComponents(); --i >= 0;)
                if (auto* c = modals->getModalComponent (i)) c->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        void mainWindowRepairs()
        {
            beginTest ("LOCK keeps a locked take from Cmd+Q then Return");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.main.onLockToggled();
                w.main.confirmAndQuit();
                expectEquals (modalCount(), 0, "the Stop & Quit dialog opened under LOCK");
                expect (w.main.statusLabel.getText().containsIgnoreCase ("unlock"), w.main.statusLabel.getText());
                w.main.onLockToggled();
                dismissModals();
            }

            beginTest ("The no-session Quit dialog rechecks for a take");
            {
                Window w;
                w.engine().clearSessionState();
                w.main.confirmAndQuit();
                expectEquals (modalCount(), 1, "no quit dialog");
                w.recorder().getTrack (0).armed.store (true);
                const auto fresh = tempDir ("quit-take");
                w.dirs.add (fresh);
                expect (w.recorder().startRecording (fresh), w.recorder().getStartError());
                if (auto* quit = juce::ModalComponentManager::getInstance()->getModalComponent (0))
                    quit->exitModalState (1);
                expect (pumpUntil ([&]
                {
                    auto* top = juce::ModalComponentManager::getInstance()->getModalComponent (0);
                    return top != nullptr && dynamic_cast<juce::AlertWindow*> (top) != nullptr
                        && top->getName() == "Recording is still rolling";
                }, 2000), "the dialog quit past a live take");
                dismissModals();
            }

            beginTest ("Session > Patch and Meterbridge reuse one tracked window, and LOCK reaches it");
            {
                Window w;
                const auto& commands = w.main.getMenuCommands();
                const int before = juce::TopLevelWindow::getNumTopLevelWindows();
                commands.dispatch (50);
                commands.dispatch (50);
                commands.dispatch (51);
                commands.dispatch (51);
                expectEquals (juce::TopLevelWindow::getNumTopLevelWindows(), before + 2, "a second window opened");
                expect (w.main.patchDialog != nullptr, "the menu's Patch window is untracked");
                expect (w.main.metersDialog != nullptr, "the menu's Meterbridge is untracked");
                w.main.onLockToggled();
                expect (w.main.patchDialog != nullptr && ! w.main.patchDialog->isEnabled(), "LOCK did not reach it");
                w.main.onLockToggled();
                delete w.main.patchDialog.getComponent();
                delete w.main.metersDialog.getComponent();
            }

            beginTest ("Pre-flight never calls a disk fine that RECORD would refuse");
            {
                const auto want = recordHeadroomBytes (48000.0, 56, CaptureFormat::Wav24);
                expectEquals (want, (juce::int64) 48000 * 56 * 3 * 60 * 30);
                expectEquals (recordHeadroomBytes (48000.0, 1, CaptureFormat::Wav32Float),
                              (juce::int64) 48000 * 4 * 60 * 30);
                expectEquals (recordHeadroomBytes (48000.0, 1, CaptureFormat::Flac16),
                              (juce::int64) 48000 * 2 * 60 * 30);
                expect (! preflightDiskOk (10.0, want, 56), "10 GB for 56 tracks is 20 minutes, and RECORD refuses it");
                expect (preflightDiskOk (20.0, want, 56));
                expect (preflightDiskOk (10.0, 0, 0));
            }

            beginTest ("Welcome's Create takes the session's rate even when the device is already there");
            {
                Window w;
                w.main.pendingSampleRate = 44100.0;
                WelcomeDialog::NewResult r;
                r.name = "Rate";
                r.location = tempDir ("welcome-root");
                r.sampleRate = kRate;
                r.captureFormat = CaptureFormat::Wav24;
                w.dirs.add (r.location);
                w.main.createSessionFromWelcome (r);
                expectWithinAbsoluteError (w.main.pendingSampleRate, kRate, 0.5, "Create left the old rate behind");
                w.engine().setStripCount (2);
                w.main.engine.prepareForTests (kRate, kBlock);
            }

            beginTest ("A session with no audio opened is a session that opened");
            {
                Window w;
                const auto empty = tempDir ("empty");
                w.dirs.add (empty);
                empty.getChildFile ("Audio Files").createDirectory();
                w.main.openSessionFromWelcome (empty);
                expect (w.main.statusLabel.getText().contains ("Opened empty"), w.main.statusLabel.getText());
                w.main.showStartupWelcome();
                expectEquals (modalCount(), 0, "Welcome opened over a session that reopened empty");
                dismissModals();
                w.main.openSessionFromWelcome (tempDir ("missing"));
                expect (w.main.statusLabel.getText().contains ("Failed"), w.main.statusLabel.getText());
            }

            beginTest ("The tour runs first, and Welcome waits for it");
            {
                Window w;
                bool finished = false;
                w.main.showFirstRunTutorial ([&finished] { finished = true; });
                expectEquals (modalCount(), 1);
                expect (! finished, "Welcome came up under the tour");
                dismissModals();
                expect (pumpUntil ([&finished] { return finished; }, 2000), "Welcome never followed the tour");
            }

            beginTest ("Session Info shows the format in use, not the one at creation");
            {
                Window w;
                w.recorder().setCaptureFormat (CaptureFormat::Flac16);
                w.main.showSessionInfo();
                juce::StringArray texts;
                if (auto* dialog = juce::ModalComponentManager::getInstance()->getModalComponent (0))
                {
                    const std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                    {
                        if (auto* label = dynamic_cast<juce::Label*> (&c)) texts.add (label->getText());
                        for (auto* child : c.getChildren()) walk (*child);
                    };
                    walk (*dialog);
                }
                expect (texts.joinIntoString (" ").contains ("FLAC"), texts.joinIntoString (" | "));
                expect (texts.contains ("16-bit"));
                dismissModals();
                w.recorder().setCaptureFormat (CaptureFormat::Wav24);
            }

            beginTest ("The meterbridge follows renames and a same-count session swap");
            {
                Window w;
                w.main.openMetersWindow();
                w.engine().setTrackName (0, "Renamed");
                const auto labelled = [&] (const juce::String& text)
                {
                    const std::function<bool (juce::Component&)> walk = [&] (juce::Component& c)
                    {
                        if (auto* label = dynamic_cast<juce::Label*> (&c))
                            if (label->getText() == text) return true;
                        for (auto* child : c.getChildren()) if (walk (*child)) return true;
                        return false;
                    };
                    return w.main.metersDialog != nullptr && walk (*w.main.metersDialog);
                };
                expect (pumpUntil ([&] { return labelled ("Renamed"); }, 2000), "the bridge kept the old name");

                const auto firstMeter = [&]() -> juce::Component*
                {
                    const std::function<juce::Component* (juce::Component&)> walk = [&] (juce::Component& c) -> juce::Component*
                    {
                        if (dynamic_cast<LedMeter*> (&c) != nullptr) return &c;
                        for (auto* child : c.getChildren()) if (auto* hit = walk (*child)) return hit;
                        return nullptr;
                    };
                    return w.main.metersDialog != nullptr ? walk (*w.main.metersDialog) : nullptr;
                };
                juce::Component::SafePointer<juce::Component> before (firstMeter());
                expect (before != nullptr, "no meter to watch");
                Meterbridge::condemnAllMeters();
                expect (pumpUntil ([&] { return before == nullptr; }, 2000),
                        "detached meters were never rebound, so the bridge stayed dead");
                delete w.main.metersDialog.getComponent();
            }

            beginTest ("Pre-flight names the backup's format");
            {
                Window w;
                const auto folder = tempDir ("backup-format");
                w.dirs.add (folder);
                const auto saved = w.recorder().getBackupDirectory();
                expect (folder.createDirectory().wasOk());
                expect (w.engine().setBackupDirectory (folder));
                expect (w.engine().setBackupFormat (CaptureFormat::Flac24));
                expect (w.main.backupChecklistRow().contains ("FLAC/24"), w.main.backupChecklistRow());
                expect (w.engine().setBackupDirectory (saved));
                expect (w.engine().setBackupFormat (CaptureFormat::Wav24));
                updateSettings (w.engine().getAppProps(), [] (juce::PropertiesFile& p) { p.removeValue ("backupFormat"); });
            }

            beginTest ("A refused backup folder is neither claimed nor saved");
            {
                Window w;
                const auto before = w.recorder().getBackupDirectory();
                const auto saved = w.engine().getAppProps()->getValue ("backupDirectory");
                const auto other = tempDir ("backup");
                w.dirs.add (other);
                w.arm ({ 0 });
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.main.backupButton.setButtonText ("BACKUP");
                w.main.applyBackupFolder (other);
                expect (w.main.backupButton.getButtonText() == "BACKUP", "the header claimed success");
                expect (w.recorder().getBackupDirectory() == before);
                expectEquals (w.engine().getAppProps()->getValue ("backupDirectory"), saved,
                              "a refused folder was saved for the next launch");
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("With no session there is no rate to disagree with");
            {
                Window w;
                w.engine().clearSessionState();
                w.main.pendingSampleRate = 48000.0;
                w.main.checkDeviceSampleRate (44100.0);
                expect (w.main.srWarnLabel.getText().isEmpty(), "a false alarm with no session");
                expectEquals (modalCount(), 0, "a false modal with no session");
                expectWithinAbsoluteError (w.main.pendingSampleRate, 44100.0, 0.5);
                dismissModals();
                w.main.pendingSampleRate = 48000.0;
                w.main.srMismatchWarned = 0.0;
            }

            beginTest ("A real rate mismatch in a session still warns");
            {
                Window w;
                w.main.pendingSampleRate = 48000.0;
                w.main.checkDeviceSampleRate (44100.0);
                expect (w.main.srWarnLabel.getText().contains ("MISMATCH"));
                dismissModals();
                w.main.checkDeviceSampleRate (48000.0);
                expect (w.main.srWarnLabel.getText().isEmpty());
            }

            beginTest ("A session saved before the flag still knows its reference bed");
            {
                Window w;
                w.engine().setTrackName (1, "Click");
                w.engine().setTrackInputRouting (1, -1);
                expect (w.engine().saveSessionMixTo (w.session));
                const auto mix = w.session.getChildFile ("session_mix.json");
                auto text = mix.loadFileAsString();
                expect (text.contains ("referenceMedia"));
                auto stripped = text.replace ("\"referenceMedia\": false,", "").replace ("\"referenceMedia\": true,", "");
                mix.replaceWithText (stripped);
                w.open (w.session);
                expect (w.recorder().getTrack (1).referenceMedia.load(), "an old session lost its click strip");
                expect (! w.recorder().getTrack (0).referenceMedia.load());
            }

            beginTest ("Pre-roll and the Generic OSC token have a real home");
            {
                Window w;
                w.recorder().setPreRollSeconds (0);
                SessionSettingsDialog::launch (w.engine(), {}, {});
                juce::ComboBox* preRoll = nullptr;
                juce::Button* apply = nullptr;
                if (auto* dialog = juce::ModalComponentManager::getInstance()->getModalComponent (0))
                {
                    const std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
                    {
                        if (auto* box = dynamic_cast<juce::ComboBox*> (&c))
                            if (box->getTitle() == "Pre-roll") preRoll = box;
                        if (auto* b = dynamic_cast<juce::Button*> (&c))
                            if (b->getButtonText() == "Apply" || b->getButtonText() == "OK") apply = b;
                        for (auto* child : c.getChildren()) walk (*child);
                    };
                    walk (*dialog);
                }
                expect (preRoll != nullptr && preRoll->getWidth() > 0, "no pre-roll control");
                expect (apply != nullptr);
                if (preRoll != nullptr && apply != nullptr)
                {
                    preRoll->setSelectedId (11, juce::dontSendNotification);
                    apply->onClick();
                    expectEquals (w.recorder().getPreRollSeconds(), 10);
                }
                dismissModals();
                w.recorder().setPreRollSeconds (0);

                cs_detail::ProtoInfo generic { "Open Sound Control (OSC)", true, 0, "osc", "/zynforge/" };
                cs_detail::OscSettings panel (w.engine(), generic, {});
                juce::Label* token = nullptr;
                for (auto* child : panel.getChildren())
                    if (auto* label = dynamic_cast<juce::Label*> (child))
                        if (label->getTitle() == "OSC access token") token = label;
                expect (token != nullptr && token->getWidth() > 0, "the Generic OSC token is shown nowhere");
                if (token != nullptr)
                {
                    expect (w.engine().getRemotes().startOsc (49391, 0));
                    panel.refreshToken();
                    expectEquals (token->getText(), w.engine().getRemotes().getOscAccessToken());
                    expect (token->getText().isNotEmpty());
                    w.engine().getRemotes().stopOsc();
                }
            }

            beginTest ("Tap STOP again shows at once, ahead of anything queued");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                w.main.toast.show ("Earlier");
                w.main.toast.show ("Waiting");
                w.main.onStopClicked();
                expect (w.main.toast.getCurrentMessage().contains ("Tap STOP"), w.main.toast.getCurrentMessage());
                feedAll (w.recorder(), 0.1f, 4);
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("VSC and the report writers wait for the take");
            {
                Window w;
                w.engine().setTrackOutputRouting (0, 5);
                w.arm ({ 0 });
                w.engine().armContinue (w.engine().getCaptureLengthSamples());
                expect (w.engine().startRecording (w.session), w.recorder().getStartError());
                w.main.onVscClicked();
                expectEquals (w.recorder().getTrack (0).outputRouting.load(), 5, "VSC repatched under a live take");
                const auto listed = w.session.findChildFiles (juce::File::findFiles, true, "*eport*").size();
                w.main.writeSoundcheckReport();
                expectEquals (w.session.findChildFiles (juce::File::findFiles, true, "*eport*").size(), listed,
                              "a report was written to the take volume");
                w.main.cues.push_back ({});
                w.main.printSetlist();
                expect (! w.session.getChildFile ("setlist.html").existsAsFile(), "the setlist was written to the take volume");
                feedAll (w.recorder(), 0.1f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("A self-owned device dialog is gone before the engine is");
            {
                Window w;
                AudioDeviceDialog::launch (w.engine(), true);
                expectEquals (modalCount(), 1);
                AudioDeviceDialog::closeSelfOwned();
                expect (pumpUntil ([] { return modalCount() == 0; }, 2000), "the dialog outlived its close");
            }
        }

        void remoteNeverPunches()
        {
            using Action = AudioEngine::RemoteTransportAction;

            beginTest ("A remote RECORD refuses to punch at a placed cursor");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                expect (! w.engine().isRecording(), "a remote RECORD punched");
                expect (! w.main.manualPunchActive);
                expect (w.main.alarmBanner.getText().containsIgnoreCase ("punch"), w.main.alarmBanner.getText());
                expect (error.containsIgnoreCase ("punch"), error);
            }

            beginTest ("A remote RECORD refuses to punch at a rolling playhead");
            {
                Window w;
                w.arm ({ 0 });
                w.player().setPositionSamples (20000);
                w.engine().startPlayback();
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::ToggleRecord, error));
                expect (! w.engine().isRecording(), "a remote RECORD punched while rolling");
                w.engine().stopPlayback();
            }

            beginTest ("A remote RECORD refuses to punch a selected range, and leaves it armed");
            {
                Window w;
                w.arm ({ 0 });
                w.player().selectRange (1000, 2000);
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                expect (! w.engine().isRecording());
                expect (! w.main.punchSessionActive, "a remote RECORD rolled a selection punch");
                expect (w.player().isRangeArmed());
            }

            beginTest ("A remote RECORD at a parked playhead appends");
            {
                Window w;
                w.arm ({ 0 });
                const auto total = w.player().getTotalLengthSamples();
                w.player().setPositionSamples (20000);
                juce::String error;
                expect (w.engine().performRemoteTransport (Action::StartRecord, error), error);
                expect (w.engine().isRecording());
                feedAll (w.recorder(), -0.3f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
                expectWithinAbsoluteError (meanOf (trackFile (w.session, 0), 0, total),
                                           levelOf (0), 1.0e-3f, "a parked remote RECORD overwrote the take");
            }

            beginTest ("A stopped RECORD at a placed cursor asks and starts nothing");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                w.main.onRecordClicked();
                expect (w.main.cursorPunchQuestion != nullptr, "RECORD did not ask");
                expect (! w.engine().isRecording(), "a stray cursor started a take");
                expect (! w.main.manualPunchActive, "a stray cursor punched");

                w.main.onRecordClicked();   // a second press does not stack a second question
                expectEquals (juce::ModalComponentManager::getInstance()->getNumModalComponents(), 1);
                dismissQuestion (w);
                w.main.answerCursorPunch (0);
                expect (! w.engine().isRecording(), "Cancel started a take");
                expectEquals (w.engine().getEditCursorSample(), (juce::int64) 20000, "Cancel moved the cursor");
            }

            beginTest ("The safe answer records after the take and leaves it alone");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                w.main.onRecordClicked();
                dismissQuestion (w);
                w.main.answerCursorPunch (1);
                expect (w.engine().isRecording(), w.main.statusLabel.getText());
                expect (! w.main.manualPunchActive, "the safe answer punched");
                expectEquals (w.engine().getEditCursorSample(), (juce::int64) -1, "the cursor stayed armed");
                feedAll (w.recorder(), -0.3f, 4);
                w.main.stopActiveCaptureAsync (false);
                expect (w.stopped());
            }

            beginTest ("A local press may still punch, through the same path");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                w.main.onRecordClicked();
                dismissQuestion (w);
                w.main.answerCursorPunch (2);
                expect (w.main.manualPunchActive, w.main.statusLabel.getText());
                expect (! w.main.cursorPunchAnswered, "the answer outlived its press");
                feedAll (w.recorder(), -0.3f, 4);
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("A punch during playback does not ask");
            {
                Window w;
                w.arm ({ 0 });
                w.player().setPositionSamples (20000);
                w.player().start();
                w.main.onRecordClicked();
                expect (w.main.cursorPunchQuestion == nullptr, "a deliberate rolling punch asked");
                expect (w.main.manualPunchActive, w.main.statusLabel.getText());
                feedAll (w.recorder(), -0.3f, 4);
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("An answer that arrives after playback started does nothing");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                w.main.onRecordClicked();
                dismissQuestion (w);
                w.player().start();
                for (const int answer : { 1, 2 })
                {
                    w.main.answerCursorPunch (answer);
                    expect (! w.engine().isRecording(), "a stale answer started a take");
                    expect (! w.main.manualPunchActive, "a stale answer punched");
                }
                expectEquals (w.engine().getEditCursorSample(), (juce::int64) 20000);
                w.player().stop();
            }

            beginTest ("No remote PLAY rolls into a PUNCH window");
            {
                Window w;
                w.arm ({ 0 });
                w.main.togglePunchMode();
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartPlay, error));
                expect (error.containsIgnoreCase ("punch"), error);
                expect (! w.engine().isPlaying());
                w.main.togglePunchMode();
                expect (w.engine().performRemoteTransport (Action::StartPlay, error), error);
                w.engine().stopPlayback();
            }

            beginTest ("A remote RECORD during a punch's post-roll leaves the punch alone");
            {
                Window w;
                w.main.punchSessionActive = true;
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                expect (error.containsIgnoreCase ("punch"), error);
                expect (w.main.punchSessionActive);
                w.main.punchSessionActive = false;
            }
        }
    };

    static MainWindowGuardTests mainWindowGuardTests;
}
