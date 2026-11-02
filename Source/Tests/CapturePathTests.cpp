// What the capture path owes a take: the right device in the daemon, every copy
// holding the channel its primary holds, live audio ahead of the history dump,
// and no helper's live session handed to recovery.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/PunchSplice.h"
#include "../Audio/TrackOrderLog.h"
#include "../Audio/WavRepair.h"
#include "../Capture/CaptureDaemon.h"
#include "../Capture/DeviceState.h"
#include "../UI/MainComponent.h"
#include "../UI/PreflightDialog.h"

#include <cmath>
#include <thread>
#include <vector>
#include <CoreFoundation/CoreFoundation.h>
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

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

        // On the system volume. A test home can sit on exFAT, which has neither
        // file modes nor sparse files.
        static juce::File systemTempDir (const juce::String& tag)
        {
            auto dir = juce::File ("/tmp").getChildFile ("zf-capture-path-" + tag + "-" + juce::Uuid().toString());
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

        // ── Pre-roll ───────────────────────────────────────────────────────
        static float rampAt (juce::int64 frame) { return 0.1f + 0.5f * (float) (frame % 1000) / 1000.0f; }

        struct Ramp
        {
            MultitrackRecorder& rec;
            juce::int64 frame { 0 };

            void block()
            {
                std::vector<float> samples ((size_t) kBlock);
                for (int i = 0; i < kBlock; ++i) samples[(size_t) i] = rampAt (frame + i);
                const float* inputs[] { samples.data() };
                rec.processBlock (inputs, 1, kBlock);
                frame += kBlock;
            }
        };

        // The file holds the ramp from `first`, unbroken to its last frame.
        void expectRamp (const juce::File& file, juce::int64 first, juce::int64 frames, const juce::String& what)
        {
            const auto audio = samplesOfTrack (file);
            expectEquals ((juce::int64) audio.size(), frames, what + ": wrong length");
            int wrong = 0;
            for (size_t i = 0; i < audio.size(); ++i)
                if (std::abs (audio[i] - rampAt (first + (juce::int64) i)) > 0.0002f) ++wrong;
            expectEquals (wrong, 0, what + ": frames differ from what was played");
        }

        void testPreRoll()
        {
            using Copy = MultitrackRecorder::CaptureCopy;
            constexpr int kHeard = 200 * kBlock;        // more than a second of history

            beginTest ("Live audio outlasts a pre-roll dump on a drive that only keeps real time");
            {
                const auto root = tempDir ("dump");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                const auto mirror = root.getChildFile ("mirror");
                auto entered = std::make_shared<juce::WaitableEvent> (true);
                auto released = std::make_shared<juce::WaitableEvent> (true);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                rec.copyRingFramesForTests = Copy::kChunkFrames;      // far less than the pre-roll
                rec.beforeCopyWriteForTests = [entered, released] (Copy&)
                {
                    entered->signal();
                    released->wait (-1);
                };
                Ramp ramp { rec };
                while (ramp.frame < kHeard) ramp.block();
                expect (rec.startRecording (session));
                expect (entered->wait (5000), "no copy reached its dump");

                // As long as the dump would take at exactly real time.
                while (ramp.frame < kHeard + (juce::int64) kRate)
                {
                    ramp.block();
                    rec.drainOnce();
                }
                expect (rec.getCopyProblems().isEmpty(), rec.getCopyProblems().joinIntoString (" | "));
                released->signal();
                rec.stopRecording();

                expectEquals (rec.getMissedSamples(), (juce::int64) 0, "live audio went to the dump");
                expect (! rec.hasPrimaryFailed());
                expect (! rec.anyMirrorFailed());
                const auto total = (juce::int64) kRate + ramp.frame - kHeard;
                expectRamp (session.getChildFile ("Audio Files/Track_01.wav"), kHeard - (juce::int64) kRate, total, "the primary");
                expectRamp (mirror.getChildFile ("session/Audio Files/Track_01.wav"), kHeard - (juce::int64) kRate, total, "the mirror");
            }

            beginTest ("At the memory limit the pre-roll gives way, newest kept, with a notice");
            {
                const auto root = tempDir ("limit");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                constexpr int kFits = 14400;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                rec.copyRingBytesForTests = (Copy::kChunkFrames + kFits) * (juce::int64) sizeof (float);
                expectWithinAbsoluteError (rec.preRollSecondsThatFit(), kFits / kRate, 0.001);
                Ramp ramp { rec };
                while (ramp.frame < kHeard) ramp.block();
                expect (rec.startRecording (session));
                expect (rec.getPreRollNotice().contains ("Pre-roll cut to 0.3 s of 1.0 s"), rec.getPreRollNotice());
                for (int b = 0; b < 40; ++b) { ramp.block(); rec.drainPendingForTests(); }
                rec.stopRecording();
                expectRamp (session.getChildFile ("Audio Files/Track_01.wav"), kHeard - kFits,
                            ramp.frame - kHeard + kFits, "the take");
                const auto report = juce::JSON::parse (session.getChildFile ("session.report.json"));
                expectEquals ((int) report["passPreRollSamples"], kFits);
                expectEquals ((int) report["passPreRollCutSamples"], (int) kRate - kFits);
            }

            beginTest ("A whole pre-roll is written with no notice when memory allows");
            {
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                expectEquals (rec.preRollFramesFitting (2, 56, 30 * (int) kRate), 30 * (int) kRate);
                const int fits = rec.preRollFramesFitting (6, 256, 30 * (int) kRate);
                expectLessThan (fits, 30 * (int) kRate);
                const auto ringBytes = (juce::int64) (rec.copyRingFrames (6, 256) + fits) * (juce::int64) sizeof (float) * 256 * 6;
                expect (ringBytes <= (juce::int64) 1 << 30, "the rings went past the memory limit");
                expectEquals (rec.getPreRollNotice(), juce::String());
            }

            beginTest ("A cut pre-roll is a quiet notice on the banner, and crosses from the daemon");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                const juce::ScopeGuard cleanUp { [] { MainComponent::s_testConstruct = false; } };
                MainComponent main;
                EngineStatus status;
                status.recording = true;
                status.preRollNotice = "Pre-roll cut to 2.0 s of 30.0 s -- this many channels and copies fill the capture memory";
                const auto crossed = EngineStatus::fromJson (status.toJson());
                expectEquals (crossed.preRollNotice, status.preRollNotice);
                main.updateCaptureWarnings (crossed);
                expect (main.alarmBanner.getText().startsWith ("Pre-roll cut"), main.alarmBanner.getText());
                expect (! main.alarmBanner.getText().containsChar ('!'), "a cut pre-roll is not an emergency");
            }

            beginTest ("Pre-flight says what the pre-roll will do");
            {
                using Status = PreflightRow::Status;
                expect (preRollRow (10, 10.0, 500.0, 100.0).status == Status::ok);
                const auto cut = preRollRow (30, 12.0, 500.0, 100.0);
                expect (cut.status == Status::warning);
                expect (cut.value.contains ("12.0 s of 30 s"), cut.value);
                const auto slow = preRollRow (10, 10.0, 90.0, 100.0);
                expect (slow.status == Status::warning);
                expect (slow.value.contains ("cannot write"), slow.value);
            }
        }

        // Rolling in the daemon, with this window knowing nothing of it.
        bool rollUnwatched (Rig& rig)
        {
            auto& main = *rig.main;
            juce::BigInteger arms; arms.setBit (0);
            expect (main.captureSupervisor.startRecording (rig.session, 1, (int) CaptureFormat::Wav24, arms));
            if (! rig.daemon.isRecording()) return false;
            main.captureSupervisor.disconnect();
            main.captureSupervisor.forgetRollingTake();
            main.engine.setExternalRecording (false);
            main.useCaptureDaemon = false;
            return true;
        }

        void testLiveHelperSessions()
        {
            using capture::identity::Endpoint;
            using capture::identity::TakeClaim;

            beginTest ("A session a helper is writing is never offered, with no link to that helper");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                rig.daemon.getRecorder().getTrack (0).armed.store (true);
                if (! rollUnwatched (rig)) { expect (false, "the daemon did not roll"); return; }
                expect (rig.session.getChildFile ("recording.session").existsAsFile());
                expect (AudioEngine::findIncompleteSessions (rig.root).contains (rig.session));

                expect (! main.sessionsToRecover (rig.root).contains (rig.session), "a live take is on the list");
                expect (main.removeRecoveredSession (rig.session).isNotEmpty());
                expect (rig.session.isDirectory(), "a live take was deleted");
                expectEquals (main.openSessionFolder (rig.session, true), -1, "a live take was opened and repaired");
                expect (rig.session.getChildFile ("recording.session").existsAsFile());
                rig.daemon.getRecorder().stopRecording();
            }

            beginTest ("The claim stays up through a long STOP and goes when the files are final");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                auto& recorder = rig.daemon.getRecorder();
                recorder.getTrack (0).armed.store (true);
                juce::WaitableEvent closing, finish;
                recorder.beforeFinalizationForTests = [&] { closing.signal(); finish.wait (5000); };
                juce::BigInteger arms; arms.setBit (0);
                expect (main.captureSupervisor.startRecording (rig.session, 1, (int) CaptureFormat::Wav24, arms));
                std::thread stopper ([&] { recorder.stopRecording(); });
                expect (closing.wait (5000));
                juce::Thread::sleep (250);      // several status ticks
                expect (TakeClaim::isHeld (rig.session), "a helper still closing files let go of its session");
                finish.signal();
                stopper.join();
                recorder.beforeFinalizationForTests = {};
                expect (pumpUntil ([&] { return ! TakeClaim::isHeld (rig.session); }, 3000),
                        "a finished take still reads as live");
            }

            beginTest ("A helper that dies stops holding its session, and recovery lists it");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                const auto root = tempDir ("crashed");
                const juce::ScopeGuard cleanUp { [&]
                {
                    MainComponent::s_testConstruct = false;
                    root.deleteRecursively();
                } };
                const auto session = root.getChildFile ("take");
                session.createDirectory();
                expect (session.getChildFile ("recording.session").replaceWithText ("{}"));
                MainComponent main;
                {
                    TakeClaim helper;
                    helper.hold (session);
                    expect (TakeClaim::isHeld (session));
                    expect (! main.sessionsToRecover (root).contains (session));
                }
                expect (main.sessionsToRecover (root).contains (session), "a crashed session stayed hidden");
                expectEquals (main.removeRecoveredSession (session), juce::String());
                expect (! session.exists());
            }

            beginTest ("A helper too old to say what it holds keeps recovery off everything");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                const auto root = tempDir ("older");
                const juce::ScopeGuard cleanUp { [&]
                {
                    MainComponent::s_testConstruct = false;
                    root.deleteRecursively();
                } };
                const auto session = root.getChildFile ("take");
                session.createDirectory();
                expect (session.getChildFile ("recording.session").replaceWithText ("{}"));
                MainComponent main;
                const int port = 40000 + juce::Random::getSystemRandom().nextInt (9000);
                main.captureDaemonPort = port;
                Endpoint helper;
                expect (helper.create (port));
                expect (main.sessionsToRecover (root).contains (session), "a helper that publishes claims hid a crash");
                const int claims = Endpoint::openSharedFile ((juce::String (port) + ".claims").toRawUTF8(), false);
                expect (claims >= 0);
                ::close (claims);
                expect (juce::File ("/tmp/zynforge-capture-" + juce::String ((int) ::geteuid()))
                            .getChildFile (juce::String (port) + ".claims").deleteFile());
                expect (main.sessionsToRecover (root).isEmpty(), "a helper with no claims could be writing this");
                expect (main.removeRecoveredSession (session).isNotEmpty());
                expect (session.isDirectory());
            }
        }

        // ── What a crash leaves ────────────────────────────────────────────
        // A finished WAV as a crash leaves it: the last header write older than
        // the audio behind it.
        static void ageHeader (const juce::File& file, juce::int64 framesInHeader)
        {
            juce::MemoryBlock bytes;
            file.loadFileAsData (bytes);
            auto* data = static_cast<char*> (bytes.getData());
            juce::int64 at = 12;
            int blockAlign = 0;
            while (at + 8 < (juce::int64) bytes.getSize())
            {
                const juce::String chunk (data + at, 4);
                const auto length = (juce::int64) (juce::uint32) juce::ByteOrder::littleEndianInt (data + at + 4);
                if (chunk == "fmt ") blockAlign = juce::ByteOrder::littleEndianShort (data + at + 8 + 12);
                if (chunk == "data")
                {
                    const auto audio = framesInHeader * blockAlign;
                    juce::MemoryOutputStream patch;
                    patch.writeInt ((int) audio);
                    std::memcpy (data + at + 4, patch.getData(), 4);
                    juce::MemoryOutputStream riff;
                    riff.writeInt ((int) (at + 8 - 8 + audio));
                    std::memcpy (data + 4, riff.getData(), 4);
                    break;
                }
                at += 8 + length + (length & 1);
            }
            file.replaceWithData (bytes.getData(), bytes.getSize());
        }

        static void writeWav (const juce::File& file, int channels, int bits, int frames)
        {
            std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (out.get(), kRate, (unsigned int) channels, bits, {}, 0));
            out.release();
            juce::AudioBuffer<float> audio (channels, frames);
            audio.clear();
            writer->writeFromAudioSampleBuffer (audio, 0, frames);
        }

        void testCrashRemains()
        {
            beginTest ("A lead-in that cannot be written leaves the session open and says what is wrong");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                const auto root = systemTempDir ("lead-in");
                const auto audio = root.getChildFile ("Audio Files");
                const juce::ScopeGuard cleanUp { [&]
                {
                    MainComponent::s_testConstruct = false;
                    audio.setReadOnly (false);
                    ::chmod (audio.getFullPathName().toRawUTF8(), 0755);
                    root.deleteRecursively();
                } };
                audio.createDirectory();
                writeWav (audio.getChildFile ("Track_01.wav"), 1, 24, 96000);
                writeWav (audio.getChildFile ("Track_02_part02.wav"), 1, 24, 48000);
                const auto markers = audio.getChildFile ("Pending Lead-ins");
                markers.createDirectory();
                const auto marker = markers.getChildFile ("Track_02.json");
                expect (marker.replaceWithText (R"({"track":"Track_02","ext":".wav","firstPart":1,"parts":1,"container":0,
                                                    "bitDepth":24,"channels":1,"sampleRate":48000,"priorLength":0,
                                                    "timelineStart":48000})"));
                ::chmod (audio.getFullPathName().toRawUTF8(), 0555);

                bool owed = false;
                int recovered = 0;
                expect (MultitrackRecorder::recoverInterruptedPunches (audio, &recovered, &owed),
                        "a lead-in was reported as a failed punch");
                expect (owed);
                {
                    MainComponent main;
                    expect (main.openSessionFolder (root, true) >= 0, "the session would not open");
                    main.engine.clearAllStripOverrides();
                }
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.armContinue (0);
                    expect (! rec.startRecording (root));
                    expect (rec.getStartError().contains ("lead-in"), rec.getStartError());
                    expect (! rec.getStartError().contains ("punch"), rec.getStartError());
                }

                {
                    AudioEngine engine;
                    engine.setActiveSessionDir (root);
                    engine.claimMixerFor (root);
                    engine.setStripCount (2);
                    expect (! engine.reorderTracks ({ 1, 0 }), "a strip moved under a lead-in that names it by number");
                    expect (audio.getChildFile ("Track_01.wav").existsAsFile());
                    engine.clearAllStripOverrides();
                }

                ::chmod (audio.getFullPathName().toRawUTF8(), 0755);
                owed = true;
                expect (MultitrackRecorder::recoverInterruptedPunches (audio, &recovered, &owed));
                expect (! owed);
                expect (! marker.existsAsFile());
                expect (audio.getChildFile ("Track_02.wav").existsAsFile());
            }

            beginTest ("A marker nothing can read is set aside and holds nothing up");
            {
                const auto root = tempDir ("bad-marker");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                const auto markers = root.getChildFile ("Pending Lead-ins");
                markers.createDirectory();
                expect (markers.getChildFile ("Track_01.json").replaceWithText ("not json at all"));
                bool owed = false;
                expect (MultitrackRecorder::recoverInterruptedPunches (root, nullptr, &owed));
                expect (owed);
                expect (markers.getChildFile ("Track_01.unreadable").existsAsFile());
                expect (MultitrackRecorder::recoverInterruptedPunches (root, nullptr, &owed));
                expect (! owed, "the same marker held the session up twice");
            }

            beginTest ("A WAV a crash left gets its last seconds back, on every copy, and only once");
            {
                const auto root = tempDir ("wav");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (0).armed.store (true);
                    rec.getTrack (1).armed.store (true);
                    rec.getTrack (0).isStereo.store (true);
                    expect (rec.startRecording (session));
                    Ramp ramp { rec };
                    std::vector<float> left ((size_t) kBlock, 0.25f), right ((size_t) kBlock, -0.25f);
                    const float* inputs[] { left.data(), right.data() };
                    for (int b = 0; b < 40; ++b) { rec.processBlock (inputs, 2, kBlock); rec.drainPendingForTests(); }
                    rec.stopRecording();
                }
                const auto file = session.getChildFile ("Audio Files/Track_01.wav");
                const auto whole = file.getSize();
                constexpr juce::int64 kFrames = 40 * kBlock;
                for (const juce::int64 inHeader : { (juce::int64) 0, kFrames / 2 })
                {
                    ageHeader (file, inHeader);
                    juce::AudioFormatManager fm; fm.registerBasicFormats();
                    {
                        std::unique_ptr<juce::AudioFormatReader> stale (fm.createReaderFor (file));
                        expect (stale != nullptr);
                        if (stale != nullptr) expectEquals (stale->lengthInSamples, inHeader);
                    }
                    expect (wavrepair::repairStaleHeader (file) == wavrepair::Result::repaired);
                    std::unique_ptr<juce::AudioFormatReader> fixed (fm.createReaderFor (file));
                    expect (fixed != nullptr);
                    if (fixed != nullptr)
                    {
                        expectEquals (fixed->lengthInSamples, kFrames, "the tail stayed hidden");
                        expectEquals ((int) fixed->numChannels, 2);
                    }
                    expectEquals (file.getSize(), whole, "repair changed the file's length");
                    expect (wavrepair::repairStaleHeader (file) == wavrepair::Result::untouched);
                }

                ageHeader (file, 0);
                expect (MultitrackRecorder::recoverInterruptedPunches (session.getChildFile ("Audio Files")));
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reopened (fm.createReaderFor (file));
                expect (reopened != nullptr);
                if (reopened != nullptr) expectEquals (reopened->lengthInSamples, kFrames, "reopening did not repair it");
            }

            beginTest ("WAV repair stops at the last whole frame and leaves other files alone");
            {
                const auto root = tempDir ("wav-shape");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                const auto file = root.getChildFile ("Track_01.wav");
                writeWav (file, 1, 24, 1000);
                ageHeader (file, 100);
                {
                    juce::FileOutputStream torn (file);
                    torn.writeByte (7);    // a frame cut off part way
                }
                expect (wavrepair::repairStaleHeader (file) == wavrepair::Result::repaired);
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                expect (reader != nullptr);
                if (reader != nullptr) expectEquals (reader->lengthInSamples, (juce::int64) 1000);

                const auto finished = root.getChildFile ("finished.wav");
                writeWav (finished, 2, 16, 500);
                juce::MemoryBlock before;
                finished.loadFileAsData (before);
                expect (wavrepair::repairStaleHeader (finished) == wavrepair::Result::untouched);
                juce::MemoryBlock after;
                finished.loadFileAsData (after);
                expect (before == after, "a finished take was rewritten");

                // Another writer's chunk after the audio is not this shape.
                const auto foreign = root.getChildFile ("foreign.wav");
                writeWav (foreign, 1, 16, 500);
                {
                    juce::FileOutputStream extra (foreign);
                    extra.write ("LIST", 4);
                    extra.writeInt (4);
                    extra.write ("abcd", 4);
                    extra.flush();
                    extra.setPosition (4);
                    extra.writeInt ((int) foreign.getSize() - 8);
                }
                expect (wavrepair::repairStaleHeader (foreign) == wavrepair::Result::untouched);
                const auto text = root.getChildFile ("Track_02.wav");
                expect (text.replaceWithText ("this is not audio, whatever its name says, and it is long enough to read"));
                expect (wavrepair::repairStaleHeader (text) == wavrepair::Result::untouched);
            }

            beginTest ("A take past 4 GiB whose header never said so comes back as RF64");
            {
                const auto root = systemTempDir ("rf64");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                const auto file = root.getChildFile ("Track_01.wav");
                writeWav (file, 2, 24, 1000);
                ageHeader (file, 1000);
                constexpr juce::int64 kPast = ((juce::int64) 1 << 32) + 6000;
                {
                    // Sparse: the length is what matters, not the bytes.
                    juce::FileOutputStream grown (file);
                    expect (grown.setPosition (kPast));
                    grown.write ("0123456", 6);
                }
                const auto size = file.getSize();
                expect (size > kPast);
                expect (wavrepair::repairStaleHeader (file) == wavrepair::Result::repaired);
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    expectGreaterThan (reader->lengthInSamples, (juce::int64) 700000000, "the RF64 sizes did not read back");
                    expectEquals ((int) reader->sampleRate, (int) kRate);
                }
                expect (wavrepair::repairStaleHeader (file) == wavrepair::Result::untouched);
            }

            beginTest ("StereoMix.wav shows its length while it records, so a crash leaves it readable");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                const auto root = tempDir ("mix");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                auto& track = engine.getRecorder().getTrack (0);
                track.armed.store (true);
                track.streamSend.store (true);
                engine.setStreamOutputs (-1, -1);
                engine.setRecordStereoMix (true);
                expect (engine.startRecording (root));
                std::vector<float> input ((size_t) kBlock, 0.25f), left ((size_t) kBlock), right ((size_t) kBlock);
                const float* inputs[] { input.data() };
                float* outputs[] { left.data(), right.data() };
                const juce::AudioIODeviceCallbackContext context {};
                for (int b = 0; b < (int) (6.0 * kRate) / kBlock; ++b)
                    engine.audioDeviceIOCallbackWithContext (inputs, 1, outputs, 2, kBlock, context);
                const auto mix = root.getChildFile ("Export Files/StereoMix.wav");
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                const auto length = [&]
                {
                    const std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (mix));
                    return reader != nullptr ? reader->lengthInSamples : (juce::int64) 0;
                };
                expect (pumpUntil ([&] { return length() > 0; }, 5000), "the mix read as empty while it recorded");
                engine.setRecordStereoMix (false);
                engine.stopRecording();
                engine.clearAllStripOverrides();
            }
        }

        void testHelperLifecycle()
        {
            beginTest ("A window can reach a helper while its device is still opening");
            {
                capture::CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                bool answered = false;
                int port = 0;
                daemon.registerCallbackForTests = [&]
                {
                    capture::CaptureClient client;
                    answered = client.connect ("127.0.0.1", port) && client.hello (1500).ok;
                    client.disconnect();
                };
                juce::Random random;
                bool started = false;
                for (int attempt = 0; attempt < 20 && ! started; ++attempt)
                {
                    port = 50000 + random.nextInt (9000);
                    started = daemon.start (port, 1);
                }
                expect (started);
                expect (answered, "the port opened only after the device did");
                daemon.stop();
            }

            beginTest ("A helper asked to exit closes down cleanly, where a kill would not");
            {
                const auto binary = capture::CaptureSupervisor::discoverBinary();
                if (binary.existsAsFile())
                {
                    const int port = 50000 + juce::Random::getSystemRandom().nextInt (9000);
                    juce::ChildProcess child;
                    expect (child.start (juce::StringArray { binary.getFullPathName(), "--port", juce::String (port),
                                                             "--no-device" }));
                    capture::CaptureClient client;
                    expect (pumpUntil ([&] { return client.connect ("127.0.0.1", port); }, 5000), "the helper never listened");
                    client.disconnect();
                    expectGreaterOrEqual (capture::CaptureSupervisor::askChildrenToExit (binary.getFileName()), 1);
                    expect (child.waitForProcessToFinish (5000), "the helper ignored SIGTERM");
                    expect (child.readAllProcessOutput().contains ("stopped cleanly"),
                            "the helper did not close down cleanly");
                }
            }
        }

        void testDiskFigures()
        {
            beginTest ("Two copies on one drive drain it twice as fast");
            {
                const auto root = tempDir ("volume");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                const auto session = root.getChildFile ("session");
                const auto mirror = root.getChildFile ("mirror");
                session.createDirectory();
                mirror.createDirectory();
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                const auto alone = rec.estimateMinutesRemaining (session, {});
                expect (rec.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                const auto twice = rec.estimateMinutesRemaining (session, {});
                expectGreaterThan (alone, 0);
                expectWithinAbsoluteError ((double) twice, alone / 2.0, alone * 0.1,
                                           "a mirror on the session's own drive was counted as its own drive");
            }

            beginTest ("The idle record time reads the open session's drive, not the last take's");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                const auto root = tempDir ("idle-time");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                AudioEngine engine;
                engine.prepareForTests (kRate, kBlock);
                engine.setStripCount (1);
                engine.getRecorder().getTrack (0).armed.store (true);
                engine.setActiveSessionDir (root);
                engine.refreshDiskMinutesRemaining();
                expectGreaterThan (engine.getEstimatedMinutesRemaining(), 0, "no take has run, and the figure said nothing");
                engine.clearAllStripOverrides();
            }
        }

        void testRefusedThreads()
        {
            using Copy = MultitrackRecorder::CaptureCopy;

            beginTest ("A copy the OS gives no thread leaves the take by name and the take goes on");
            {
                const auto root = tempDir ("no-thread");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { root.getChildFile ("mirror"), CaptureFormat::Wav24 } }));
                rec.copyThreadRefusedForTests = [] (Copy& copy, bool finishing) { return ! finishing && copy.copyIndex == 2; };
                expect (rec.startRecording (session));
                Ramp ramp { rec };
                for (int b = 0; b < 40; ++b) { ramp.block(); rec.drainPendingForTests(); }
                expect (rec.anyMirrorFailed());
                expect (rec.getCopyProblems().joinIntoString (" ").contains ("could not start"),
                        rec.getCopyProblems().joinIntoString (" | "));
                rec.stopRecording();
                expect (! rec.hasPrimaryFailed());
                expectRamp (session.getChildFile ("Audio Files/Track_01.wav"), 0, ramp.frame, "the primary");
            }

            beginTest ("A copy the OS gives no thread at STOP is reported, and STOP finishes");
            {
                const auto root = tempDir ("no-finish");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (root.getChildFile ("backup"));
                expect (rec.setMirrors ({ { root.getChildFile ("mirror"), CaptureFormat::Wav24 } }));
                rec.copyThreadRefusedForTests = [] (Copy&, bool finishing) { return finishing; };
                expect (rec.startRecording (session));
                rec.waitForCopiesOpenForTests();
                Ramp ramp { rec };
                for (int b = 0; b < 40; ++b) { ramp.block(); rec.drainPendingForTests(); }
                rec.stopRecording();
                expect (rec.hasBackupFailed());
                expect (rec.anyMirrorFailed());
                expect (! rec.hasPrimaryFailed());
                expectRamp (session.getChildFile ("Audio Files/Track_01.wav"), 0, ramp.frame, "the primary");
            }
        }

        // Files stop growing at `bytes` while `work` runs, as on a drive that fills.
        template <typename Work>
        static void withFileLimit (juce::int64 bytes, Work&& work)
        {
            rlimit before {};
            getrlimit (RLIMIT_FSIZE, &before);
            const auto previous = std::signal (SIGXFSZ, SIG_IGN);
            auto limited = before;
            limited.rlim_cur = (rlim_t) bytes;
            setrlimit (RLIMIT_FSIZE, &limited);
            work();
            setrlimit (RLIMIT_FSIZE, &before);
            std::signal (SIGXFSZ, previous);
        }

        void testSmallerThings()
        {
            using Copy = MultitrackRecorder::CaptureCopy;

            beginTest ("A status never races a device restart");
            {
                capture::CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                daemon.prepareForTests (kRate, kBlock, 2);
                std::atomic<bool> running { true };
                std::thread restarts ([&]
                {
                    while (running.load())
                    {
                        daemon.getRecorder().prepare (kRate, kBlock, 3);
                        daemon.getRecorder().prepare (kRate, kBlock, 2);
                    }
                });
                for (int i = 0; i < 2000; ++i) daemon.buildStatus();
                running.store (false);
                restarts.join();
                expectEquals (daemon.getRecorder().getNumTracks(), 2);
            }

            beginTest ("A backup that fails after STOP is failed in the report");
            {
                const auto root = systemTempDir ("late-backup");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                const auto backup = root.getChildFile ("backup");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 2);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (backup);
                expect (rec.startRecording (session));
                rec.waitForCopiesOpenForTests();
                Ramp ramp { rec };
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data(), samples.data() };
                for (int b = 0; b < 8; ++b) { rec.processBlock (inputs, 2, kBlock); rec.drainPendingForTests(); }
                rec.stopRecording();

                // A continue that newly arms a track owes the backup a lead-in,
                // and here it cannot write one.
                rec.getTrack (1).armed.store (true);
                rec.armContinue (0);
                rec.beforeCopyFinishForTests = [&backup] (Copy& copy)
                {
                    if (copy.copyIndex == 1)
                        ::chmod (backup.getChildFile ("session/Audio Files").getFullPathName().toRawUTF8(), 0555);
                };
                expect (rec.startRecording (session));
                rec.waitForCopiesOpenForTests();
                for (int b = 0; b < 8; ++b) { rec.processBlock (inputs, 2, kBlock); rec.drainPendingForTests(); }
                rec.stopRecording();
                ::chmod (backup.getChildFile ("session/Audio Files").getFullPathName().toRawUTF8(), 0755);

                const auto report = juce::JSON::parse (session.getChildFile ("session.report.json"));
                expect ((bool) report["backupFailed"], "the report called a failed backup clean");
                expect (juce::JSON::toString (report["copyProblems"]).contains ("lead-in"), juce::JSON::toString (report));
            }

            beginTest ("A status says which strips are stereo");
            {
                capture::CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                daemon.prepareForTests (kRate, kBlock, 3);
                daemon.getRecorder().getTrack (1).isStereo.store (true);
                const auto status = EngineStatus::fromJson (daemon.buildStatus().toJson());
                expect (! status.tracks[0].stereoLeft);
                expect (status.tracks[1].stereoLeft, "a pair came back as two mono strips");
            }

            beginTest ("A daemon START that never rolled leaves the window where it was");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                const auto before = rig.root.getChildFile ("before");
                before.createDirectory();
                main.engine.setActiveSessionDir (before);
                main.engine.claimMixerFor (before);
                main.engine.noteLastCapture (before);
                main.engine.getMarkers().setContext (before, kRate);

                std::atomic<bool> releaseStart { false };
                rig.daemon.beforeStartForTests = [&] { while (! releaseStart.load()) juce::Thread::sleep (5); };
                main.captureSupervisor.startReplyTimeoutMs = 50;
                main.requestDaemonTake (rig.session, 1, {}, 0, true);
                expect (main.daemonStartUnansweredMs != 0);
                expect (main.engine.isExternalRecording());
                releaseStart.store (true);      // nothing is armed, so the daemon refuses
                expect (pumpUntil ([&] { return main.captureSupervisor.lastStatusAtMs() != 0; }, 3000));

                main.daemonStartUnansweredMs = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter() - 20000);
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return ! main.engine.isExternalRecording();
                }, 3000));
                expectEquals (main.engine.getActiveSessionDir().getFullPathName(), before.getFullPathName());
                expectEquals (main.engine.getMixerSession().getFullPathName(), before.getFullPathName());
                expectEquals (main.engine.getLastCapture().getFullPathName(), before.getFullPathName());
                expectEquals (main.engine.getMarkers().getSessionDir().getFullPathName(), before.getFullPathName());
            }

            beginTest ("A take file that closes short is a failed write, not a clean STOP");
            {
                const auto root = tempDir ("short-close");
                const juce::ScopeGuard cleanUp { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (root); } };
                const auto session = root.getChildFile ("session");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (session));
                Ramp ramp { rec };
                for (int b = 0; b < 4; ++b) { ramp.block(); rec.drainPendingForTests(); }
                withFileLimit (1500, [&] { rec.stopRecording(); });
                expect (session.getChildFile ("Audio Files/Track_01.wav").getSize() < 4 * kBlock * 3,
                        "the limit did not bite, so this proved nothing");
                expect (rec.hasPrimaryFailed(), "STOP called a short file finished");
            }

            beginTest ("A punch that closes short never replaces the take");
            {
                const auto root = tempDir ("short-splice");
                const juce::ScopeGuard cleanUp { [&] { root.deleteRecursively(); } };
                const auto base = root.getChildFile ("base.wav");
                const auto insert = root.getChildFile ("insert.wav");
                const auto out = root.getChildFile ("out.wav");
                writeWav (base, 1, 24, 4000);
                writeWav (insert, 1, 24, 100);
                bool spliced = true;
                withFileLimit (9000, [&]
                {
                    juce::AudioFormatManager fm; fm.registerBasicFormats();
                    spliced = splicePunchFile (fm, base, insert, 100, out);
                });
                expect (! spliced, "a short splice was reported whole");
                expect (! out.exists());
            }

        }

        void runTest() override
        {
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };
            testSmallerThings();
            testRefusedThreads();
            testDiskFigures();
            testHelperLifecycle();
            testCrashRemains();
            testDaemonDevice();
            AudioEngine::setTestModeSkipAudioInit (true);
            testCopiesFollowStrips();
            testLiveHelperSessions();
            testPreRoll();
        }
    };

    static CapturePathTests capturePathTests;
}
