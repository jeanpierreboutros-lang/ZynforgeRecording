// The main window's guards around a take: what RECORD may do to existing audio,
// PUNCH mode, and what the window refuses or shows while a take rolls. RECORD's
// whole path runs here, pre-flight included, on a silent fake device.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/RecordPosition.h"
#include "../Audio/MultiPartReader.h"

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

        // A main window on the silent device, with one session open.
        struct Window
        {
            MainComponent main;
            juce::File session;
            juce::Array<juce::File> dirs;

            explicit Window (int tracks = 2, int blocks = 200, double rate = kRate)
            {
                useSilentDevice (main.engine, rate);
                session = newSession (tracks, blocks);
                open (session);
            }

            ~Window()
            {
                main.engine.clearAllStripOverrides();
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
                main.openSessionFolder (dir);
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

            loopNeverArmsAPunch();
            savedRegions();
            oneSelectionOnePunch();
            parkedPlayheadAppends();
            punchModeUsesLiveArms();
            punchModeLeavesWithTheSession();
        }

    private:
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
                player.processBlock (outs, 2, kBlock);
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
    };

    static MainWindowGuardTests mainWindowGuardTests;
}
