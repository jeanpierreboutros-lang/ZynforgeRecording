// What the capture path owes a take: the right device in the daemon, every copy
// holding the channel its primary holds, live audio ahead of the history dump,
// and no helper's live session handed to recovery.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
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

        void runTest() override
        {
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };
            testDaemonDevice();
        }
    };

    static CapturePathTests capturePathTests;
}
