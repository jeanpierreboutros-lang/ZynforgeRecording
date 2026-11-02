// What the capture path owes a take: the right device in the daemon, every copy
// holding the channel its primary holds, live audio ahead of the history dump,
// and no helper's live session handed to recovery.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/TrackOrderLog.h"
#include "../Capture/CaptureDaemon.h"
#include "../Capture/DeviceState.h"
#include "../UI/MainComponent.h"

#include <cmath>
#include <vector>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class CapturePathTests final : public juce::UnitTest
    {
    public:
        CapturePathTests() : UnitTest ("Capture path", "zynforge") {}

        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 256;

        class SilentDevice final : public juce::AudioIODevice
        {
        public:
            SilentDevice() : juce::AudioIODevice ("Silent", "Silent") {}

            juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
            juce::StringArray getInputChannelNames() override { return { "1", "2", "3", "4" }; }
            juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
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
            double getCurrentSampleRate() override { return kRate; }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return 3; }
            juce::BigInteger getActiveInputChannels() const override { return 15; }

        private:
            bool opened { false }, started { false };
        };

        class SilentDeviceType final : public juce::AudioIODeviceType
        {
        public:
            SilentDeviceType() : juce::AudioIODeviceType ("Silent") {}
            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool) const override { return { "Silent" }; }
            int getDefaultDeviceIndex (bool) const override { return 0; }
            int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
            bool hasSeparateInputsAndOutputs() const override { return false; }
            juce::AudioIODevice* createDevice (const juce::String&, const juce::String&) override
            { return new SilentDevice(); }
        };

        static void useSilentDevice (juce::AudioDeviceManager& manager)
        {
            manager.addAudioDeviceType (std::make_unique<SilentDeviceType>());
            manager.setCurrentAudioDeviceType ("Silent", false);
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName = "Silent";
            setup.inputDeviceName = "Silent";
            setup.sampleRate = kRate;
            setup.bufferSize = kBlock;
            manager.setAudioDeviceSetup (setup, false);
        }

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return predicate();
        }

        static juce::File tempDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-capture-path-" + tag + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        struct Rig
        {
            capture::CaptureDaemon daemon;
            int port { 0 };
            juce::File root = tempDir ("daemon");
            juce::File session = root.getChildFile ("take");
            std::unique_ptr<MainComponent> main;

            ~Rig()
            {
                if (main != nullptr) main->engine.clearAllStripOverrides();
                main.reset();
                daemon.stop();
                MainComponent::s_testConstruct = false;
                root.deleteRecursively();
            }
        };

        bool bringUp (Rig& rig)
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            rig.daemon.setTestModeNoDevice (true);
            juce::Random random;
            for (int attempt = 0; attempt < 20 && rig.port == 0; ++attempt)
            {
                const int candidate = 50000 + random.nextInt (9000);
                if (rig.daemon.start (candidate, 1)) rig.port = candidate;
            }
            expect (rig.port != 0);
            if (rig.port == 0) return false;
            rig.daemon.prepareForTests (kRate, kBlock, 1);
            rig.main = std::make_unique<MainComponent>();
            rig.main->engine.clearAllStripOverrides();
            rig.main->engine.setStripCount (1);
            rig.main->engine.prepareForTests (kRate, kBlock);
            rig.main->useCaptureDaemon = true;
            rig.main->captureDaemonPort = rig.port;
            rig.main->connectCaptureDaemon (false);
            expect (rig.main->captureSupervisor.isAttached());
            return rig.main->captureSupervisor.isAttached();
        }

        // The window has seen the daemon holding this device.
        bool daemonHolds (Rig& rig, const juce::String& device)
        {
            rig.daemon.setDeviceNameForTests (device);
            return pumpUntil ([&] { return rig.main->captureSupervisor.lastStatus().captureDevice == device; }, 3000);
        }

        void testDaemonDevice()
        {
            beginTest ("RECORD is refused while the daemon holds another device, and the daemon is told again");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                useSilentDevice (main.engine.getDeviceManager());
                main.engine.getRecorder().getTrack (0).armed.store (true);
                expect (daemonHolds (rig, "Silent"));
                expect (main.configureCaptureDaemon());

                // The window's configuration does not change: only the daemon moved.
                expect (daemonHolds (rig, "Built-in Microphone"));
                const auto sentBefore = rig.daemon.configuresForTests.load();
                main.captureSupervisor.retryCaptureAtMs = 0;
                MainComponent::RecordStartPlan plan;
                plan.dir = rig.session;
                plan.numTracks = 1;
                plan.armed = 1;
                main.startDaemonTake (plan);
                expect (! rig.daemon.isRecording(), "the take rolled on the wrong device");
                expect (main.alarmBanner.getText().contains ("Built-in Microphone"), main.alarmBanner.getText());
                expect (main.alarmBanner.getText().contains ("Silent"), main.alarmBanner.getText());
                expectEquals (rig.daemon.configuresForTests.load(), sentBefore + 1,
                              "the daemon was not told which device to hold");

                expect (daemonHolds (rig, {}));
                main.startDaemonTake (plan);
                expect (! rig.daemon.isRecording(), "the take rolled with no device in the daemon");
                expect (main.alarmBanner.getText().contains ("no audio device"), main.alarmBanner.getText());

                expect (daemonHolds (rig, "Silent"));
                main.startDaemonTake (plan);
                expect (rig.daemon.isRecording(), "the take did not roll once the devices agreed");
                bool stopped = false;
                juce::String error;
                main.captureSupervisor.stopRecording (&stopped, &error);
            }

            beginTest ("A daemon RECORD works with no device chosen since launch");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                useSilentDevice (main.engine.getDeviceManager());
                expect (main.engine.getDeviceManager().createStateXml() == nullptr);
                expect (daemonHolds (rig, "Silent"));
                expect (main.configureCaptureDaemon(), "nothing was chosen, so nothing was sent");
            }

            beginTest ("The daemon checks the device again when it is asked to roll");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                expect (daemonHolds (rig, "Built-in Microphone"));
                juce::BigInteger arms; arms.setBit (0);
                juce::String refusal;
                bool unanswered = false;
                expect (! main.captureSupervisor.startRecording (rig.session, 1, (int) CaptureFormat::Wav24, arms, {},
                                                                 &refusal, &unanswered, "Silent"));
                expect (! unanswered);
                expect (refusal.contains ("Built-in Microphone"), refusal);
                expect (! rig.daemon.isRecording());
            }

            beginTest ("A refused configuration forgets what the daemon accepted before it");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                auto good = main.captureDaemonSettings();
                good.getDynamicObject()->setProperty ("sampleRate", kRate);
                expect (main.captureSupervisor.configureCapture (good));
                const auto sent = rig.daemon.configuresForTests.load();

                auto* bad = new juce::DynamicObject();
                bad->setProperty ("tracks", juce::Array<juce::var>());
                expect (! main.captureSupervisor.configureCapture (juce::var (bad)));

                expect (main.captureSupervisor.configureCapture (good));
                expectEquals (rig.daemon.configuresForTests.load(), sent + 2,
                              "the daemon may hold nothing now, and it was not told again");
            }

            beginTest ("The device state sent to a daemon names what is running, and a manager can open it");
            {
                juce::AudioDeviceManager running;
                useSilentDevice (running);
                expect (running.createStateXml() == nullptr);
                auto* device = running.getCurrentAudioDevice();
                expect (device != nullptr);
                if (device == nullptr) return;
                const auto state = capture::liveDeviceState (running, *device);
                expectEquals (state->getStringAttribute ("audioInputDeviceName"), juce::String ("Silent"));
                expectEquals (state->getStringAttribute ("deviceType"), juce::String ("Silent"));
                expectEquals (state->getIntAttribute ("audioDeviceBufferSize"), kBlock);

                juce::AudioDeviceManager daemon;
                daemon.addAudioDeviceType (std::make_unique<SilentDeviceType>());
                expectEquals (daemon.initialise (256, 0, state.get(), false), juce::String());
                expectEquals (daemon.getAudioDeviceSetup().inputDeviceName, juce::String ("Silent"));
            }
        }

        // ── Copies follow their strips ─────────────────────────────────────
        struct CopyRig
        {
            juce::File root = tempDir ("copies");
            juce::File session = root.getChildFile ("Show");
            juce::File backup = root.getChildFile ("backup"), mirror = root.getChildFile ("mirror");
            AudioEngine engine;
            std::vector<float> levels;    // each strip's own signal, which moves with it
            juce::UnitTest& test;

            CopyRig (juce::UnitTest& owner, int strips) : test (owner)
            {
                session.createDirectory();
                engine.setActiveSessionDir (session);
                engine.claimMixerFor (session);
                engine.setStripCount (strips);
                engine.prepareForTests (kRate, kBlock);
                for (int i = 0; i < strips; ++i) levels.push_back (0.1f * (float) (i + 1));
                auto& rec = engine.getRecorder();
                rec.setBackupDirectory (backup);
                rec.setBackupCaptureFormat (CaptureFormat::Flac24);
                (void) rec.setMirrors ({ { mirror, CaptureFormat::Aiff24 } });
            }

            ~CopyRig()
            {
                engine.clearAllStripOverrides();
                root.deleteRecursively();
            }

            void take (bool continuing, int blocks = 40)
            {
                auto& rec = engine.getRecorder();
                const int n = rec.getNumTracks();
                std::vector<std::vector<float>> audio;
                std::vector<const float*> inputs;
                for (int t = 0; t < n; ++t)
                {
                    rec.getTrack (t).armed.store (true);
                    audio.emplace_back ((size_t) kBlock, levels[(size_t) t]);
                }
                for (auto& channel : audio) inputs.push_back (channel.data());
                if (continuing) rec.armContinue (0);
                test.expect (rec.startRecording (session), rec.getStartError());
                rec.waitForCopiesOpenForTests();
                for (int b = 0; b < blocks; ++b)
                {
                    rec.processBlock (inputs.data(), n, kBlock);
                    rec.drainPendingForTests();
                }
                rec.stopRecording();
            }

            void moveStrips (const std::vector<int>& order)
            {
                std::vector<float> next;
                for (const int old : order) next.push_back (levels[(size_t) old]);
                test.expect (engine.reorderTracks (order), "the strip move was refused");
                levels = std::move (next);
            }

            juce::File takeFolder (const juce::File& copyRoot) const
            {
                return copyRoot.getChildFile ("Show").getChildFile ("Audio Files");
            }
        };

        // Track_NN on one copy, read through every part: one strip's signal
        // from its first sample to its last.
        void expectEveryTrackWhole (const CopyRig& rig, const juce::File& audioDir, const juce::String& who,
                                    int expectedFrames, const juce::String& ext)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            for (int t = 0; t < (int) rig.levels.size(); ++t)
            {
                const auto main = audioDir.getChildFile (juce::String::formatted ("Track_%02d", t + 1) + ext);
                const auto reader = ConcatReader::create (fm, findTakeParts (main));
                expect (reader != nullptr, who + " has no " + main.getFileName());
                if (reader == nullptr) continue;
                expectEquals ((int) reader->lengthInSamples, expectedFrames, who + " " + main.getFileName());
                juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
                reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false);
                juce::Range<float> range = buffer.findMinMax (0, 0, buffer.getNumSamples());
                expectWithinAbsoluteError (range.getStart(), rig.levels[(size_t) t], 0.001f,
                                           who + " " + main.getFileName() + " holds another channel");
                expectWithinAbsoluteError (range.getEnd(), rig.levels[(size_t) t], 0.001f,
                                           who + " " + main.getFileName() + " holds another channel");
            }
        }

        void expectEveryCopyWhole (const CopyRig& rig, int frames)
        {
            expectEveryTrackWhole (rig, rig.session.getChildFile ("Audio Files"), "the primary", frames, ".wav");
            expectEveryTrackWhole (rig, rig.takeFolder (rig.backup), "the backup", frames, ".flac");
            expectEveryTrackWhole (rig, rig.takeFolder (rig.mirror), "the mirror", frames, ".aif");
        }

        static juce::var ownerOf (const CopyRig& rig, const juce::File& copyRoot)
        {
            return juce::JSON::parse (copyRoot.getChildFile ("Show").getChildFile (".zynforge-primary"));
        }

        void testCopiesFollowStrips()
        {
            constexpr int kPass = 40 * kBlock;

            beginTest ("A moved strip takes its audio along on every copy");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                rig.moveStrips ({ 1, 2, 0 });
                rig.take (true);
                expectEveryCopyWhole (rig, 2 * kPass);
            }

            beginTest ("A deleted strip leaves every copy, and its audio is kept there");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                rig.moveStrips ({ 1, 2 });
                rig.take (true);
                expectEveryCopyWhole (rig, 2 * kPass);
                expect (! rig.backup.getChildFile ("Show/Removed Tracks")
                             .findChildFiles (juce::File::findFiles, true, "Track_01.flac").isEmpty(),
                        "the backup threw a deleted strip's audio away");
            }

            beginTest ("A copy away when strips move catches up on its return, however far behind");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                const auto away = rig.root.getChildFile ("away");
                expect (rig.backup.moveFileTo (away));
                rig.moveStrips ({ 1, 2, 0 });
                rig.moveStrips ({ 2, 0, 1 });
                rig.moveStrips ({ 1, 0, 2 });
                expect (away.moveFileTo (rig.backup));
                rig.take (true);
                expectEveryCopyWhole (rig, 2 * kPass);
                expect (rig.engine.getRecorder().getCopyProblems().isEmpty(),
                        rig.engine.getRecorder().getCopyProblems().joinIntoString (" | "));
            }

            beginTest ("A copy that cannot follow sits out the take, untouched, and says so");
            {
                CopyRig rig (*this, 2);
                rig.take (false);
                const auto journalHome = rig.backup.getChildFile ("Show/Session File Backups");
                journalHome.deleteRecursively();
                expect (journalHome.replaceWithText ("not a folder"));
                rig.moveStrips ({ 1, 0 });

                const auto backupAudio = rig.takeFolder (rig.backup);
                const auto before = samplesOfTrack (backupAudio.getChildFile ("Track_01.flac"));
                rig.take (true);
                expect (rig.engine.getRecorder().getCopyProblems().joinIntoString (" ").contains ("older order"),
                        rig.engine.getRecorder().getCopyProblems().joinIntoString (" | "));
                expect (rig.engine.getRecorder().hasBackupFailed());
                expect (samplesOfTrack (backupAudio.getChildFile ("Track_01.flac")) == before,
                        "a copy that could not follow was changed anyway");
                expect (! backupAudio.getChildFile ("Track_01_part02.flac").exists(),
                        "a copy in the old order took a new pass");
                expectEveryTrackWhole (rig, rig.session.getChildFile ("Audio Files"), "the primary", 2 * kPass, ".wav");

                // Once it can, it follows, and the audio it missed is a lead-in.
                expect (journalHome.deleteFile());
                rig.take (true);
                expect (rig.engine.getRecorder().getCopyProblems().isEmpty(),
                        rig.engine.getRecorder().getCopyProblems().joinIntoString (" | "));
                expectEquals (ownerOf (rig, rig.backup)["trackOrder"].toString(),
                              trackorder::read (rig.session).current);
            }

            beginTest ("A copy in an order the session has no record of never joins");
            {
                CopyRig rig (*this, 2);
                rig.take (false);
                rig.moveStrips ({ 1, 0 });
                const auto owner = rig.backup.getChildFile ("Show/.zynforge-primary");
                auto saved = ownerOf (rig, rig.backup);
                saved.getDynamicObject()->setProperty ("trackOrder", "somewhere else");
                expect (owner.replaceWithText (juce::JSON::toString (saved)));
                rig.take (true);
                expect (rig.engine.getRecorder().getCopyProblems().joinIntoString (" ").contains ("older order"));
                expect (! rig.takeFolder (rig.backup).getChildFile ("Track_01_part02.flac").exists());
            }

            beginTest ("A crash halfway through a copy's move is undone and made once");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                rig.moveStrips ({ 1, 2, 0 });

                // The state a crash leaves: files moved, position written, no commit.
                const auto copyDir = rig.backup.getChildFile ("Show");
                const auto log = trackorder::read (rig.session);
                TrackFileTransaction interrupted;
                expect (interrupted.begin (copyDir, trackorder::takeMoves (copyDir.getChildFile ("Audio Files"),
                                                                            copyDir.getChildFile ("Removed Tracks/x"),
                                                                            log.steps[0].destination)));
                auto owner = ownerOf (rig, rig.backup);
                owner.getDynamicObject()->setProperty ("trackOrder", log.current);
                expect (copyDir.getChildFile (".zynforge-primary").replaceWithText (juce::JSON::toString (owner)));

                rig.take (true);
                expectEveryCopyWhole (rig, 2 * kPass);
            }

            beginTest ("A move a copy finished is never made again");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                rig.moveStrips ({ 1, 2, 0 });
                rig.take (true);
                rig.take (true);
                expectEveryCopyWhole (rig, 3 * kPass);
            }

            beginTest ("A strip move the primary rolled back leaves no step for a copy to make");
            {
                CopyRig rig (*this, 2);
                rig.take (false);
                rig.moveStrips ({ 1, 0 });
                const auto before = trackorder::read (rig.session);
                expect (rig.session.getChildFile ("Show.zfproj").replaceWithText ("not a project"));
                expect (! rig.engine.reorderTracks ({ 1, 0 }));
                rig.session.getChildFile ("Show.zfproj").deleteFile();
                const auto after = trackorder::read (rig.session);
                expectEquals (after.current, before.current);
                expectEquals ((int) after.steps.size(), (int) before.steps.size());

                // And the same when a crash cuts the move short after its step.
                TrackFileTransaction interrupted;
                expect (interrupted.begin (rig.session, {}));
                expect (trackorder::append (rig.session, { 1, 0 }));
                expect (TrackFileTransaction::recover (rig.session));
                expectEquals (trackorder::read (rig.session).current, before.current);
            }

            beginTest ("A first strip move that never committed leaves nothing behind");
            {
                CopyRig rig (*this, 2);
                rig.take (false);
                expect (trackorder::ensureLog (rig.session));
                TrackFileTransaction interrupted;
                expect (interrupted.begin (rig.session, {}));
                expect (trackorder::append (rig.session, { 1, 0 }));
                expect (TrackFileTransaction::recover (rig.session));
                expectEquals ((int) trackorder::read (rig.session).steps.size(), 0);
            }

            beginTest ("A copy added after strips moved joins at the current order");
            {
                CopyRig rig (*this, 2);
                (void) rig.engine.getRecorder().setMirrors ({});
                rig.engine.getRecorder().setBackupDirectory ({});
                rig.take (false);
                rig.moveStrips ({ 1, 0 });
                rig.engine.getRecorder().setBackupDirectory (rig.backup);
                rig.take (true);
                expect (rig.engine.getRecorder().getCopyProblems().isEmpty(),
                        rig.engine.getRecorder().getCopyProblems().joinIntoString (" | "));
                expectEquals (ownerOf (rig, rig.backup)["trackOrder"].toString(),
                              trackorder::read (rig.session).current);
            }

            beginTest ("A copy from before the log began makes every step from its start");
            {
                CopyRig rig (*this, 3);
                rig.take (false);
                const auto owner = rig.backup.getChildFile ("Show/.zynforge-primary");
                auto saved = ownerOf (rig, rig.backup);
                saved.getDynamicObject()->removeProperty ("trackOrder");
                expect (owner.replaceWithText (juce::JSON::toString (saved)));
                rig.moveStrips ({ 1, 2, 0 });
                rig.take (true);
                expectEveryCopyWhole (rig, 2 * kPass);
            }
        }

        static std::vector<float> samplesOfTrack (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr) return {};
            juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
            reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false);
            return { buffer.getReadPointer (0), buffer.getReadPointer (0) + buffer.getNumSamples() };
        }

        void runTest() override
        {
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };
            testDaemonDevice();
            AudioEngine::setTestModeSkipAudioInit (true);
            testCopiesFollowStrips();
        }
    };

    static CapturePathTests capturePathTests;
}
