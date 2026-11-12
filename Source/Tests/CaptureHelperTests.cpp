// The capture helper's own rules about its device and its exit: it opens the
// input alone, leaves a running device alone when only the tracks change, does
// its device work on its main thread, and stays up until a STOP's reply is out.
// A device type of its own stands in for CoreAudio, so the helper's real device
// paths run and nothing reaches the hardware.

#include "../Capture/CaptureDaemon.h"
#include "../Capture/HelperExit.h"

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zynforge
{
    namespace
    {
        constexpr double kRate  = 48000.0;
        constexpr int    kBlock = 256;

        class RigType;

        // A device the test can feed and stop, the way a real one calls back.
        class RigDevice final : public juce::AudioIODevice
        {
        public:
            RigDevice (RigType& owner, const juce::String& deviceName, bool inputs, bool outputs)
                : juce::AudioIODevice (deviceName, "Rig"), type (owner), hasInputs (inputs), hasOutputs (outputs) {}
            ~RigDevice() override { close(); }

            juce::StringArray getOutputChannelNames() override
            { return hasOutputs ? juce::StringArray { "1", "2" } : juce::StringArray(); }
            juce::StringArray getInputChannelNames() override
            { return hasInputs ? juce::StringArray { "1", "2", "3", "4" } : juce::StringArray(); }
            juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
            juce::Array<int> getAvailableBufferSizes() override    { return { kBlock }; }
            int getDefaultBufferSize() override                    { return kBlock; }
            juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int) override;
            void close() override;
            bool isOpen() override { return opened; }
            void start (juce::AudioIODeviceCallback*) override;
            void stop() override;
            bool isPlaying() override { return callback != nullptr; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return kBlock; }
            double getCurrentSampleRate() override { return kRate; }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
            juce::BigInteger getActiveInputChannels() const override  { return activeInputs; }

            RigType& type;
            const bool hasInputs, hasOutputs;
            bool opened { false };
            juce::BigInteger activeInputs, activeOutputs;
            juce::AudioIODeviceCallback* callback { nullptr };
        };

        // An interface with inputs and outputs, and a headphone output of its
        // own. A device takes its output's name when it has one, as CoreAudio
        // names the pair it combines.
        class RigType final : public juce::AudioIODeviceType
        {
        public:
            RigType() : juce::AudioIODeviceType ("Rig") {}

            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool wantInputs) const override
            { return wantInputs ? juce::StringArray { "Interface" } : outputs; }
            int getDefaultDeviceIndex (bool) const override { return 0; }
            int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
            { return device == nullptr ? -1 : getDeviceNames (asInput).indexOf (device->getName()); }
            bool hasSeparateInputsAndOutputs() const override { return true; }
            juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
            {
                if (output.isEmpty() && input.isEmpty()) return nullptr;
                return new RigDevice (*this, output.isEmpty() ? input : output, input.isNotEmpty(), output.isNotEmpty());
            }

            // Leaves the list, as headphones pulled out or a display asleep do.
            void unplug (const juce::String& name)
            {
                outputs.removeString (name);
                callDeviceChangeListeners();
            }

            // Blocks from the running device, as its own thread would deliver them.
            void feed (int blocks, float value)
            {
                std::vector<float> samples ((size_t) kBlock, value);
                const float* inputs[] { samples.data(), samples.data(), samples.data(), samples.data() };
                for (int i = 0; i < blocks && live != nullptr && live->callback != nullptr; ++i)
                    live->callback->audioDeviceIOCallbackWithContext (inputs, 4, nullptr, 0, kBlock, {});
            }

            std::vector<std::thread::id> openThreadsSeen()
            {
                const std::lock_guard<std::mutex> g (threadLock);
                return openThreads;
            }

            juce::StringArray outputs { "Interface", "Headphones" };
            std::atomic<int> opens { 0 }, starts { 0 };
            // Every open fails, as when another app hogs the Mac's default input.
            std::atomic<bool> refuseOpens { false };
            std::mutex threadLock;
            std::vector<std::thread::id> openThreads;
            RigDevice* live { nullptr };
        };

        juce::String RigDevice::open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int)
        {
            {
                const std::lock_guard<std::mutex> g (type.threadLock);
                type.openThreads.push_back (std::this_thread::get_id());
            }
            if (type.refuseOpens.load()) return "the device is in use by another application";
            ++type.opens;
            activeInputs = hasInputs ? ins : juce::BigInteger();
            activeOutputs = hasOutputs ? outs : juce::BigInteger();
            opened = true;
            return {};
        }

        void RigDevice::close()
        {
            stop();
            opened = false;
            if (type.live == this) type.live = nullptr;
        }

        void RigDevice::start (juce::AudioIODeviceCallback* cb)
        {
            if (cb == nullptr || callback != nullptr) return;
            ++type.starts;
            type.live = this;
            cb->audioDeviceAboutToStart (this);
            callback = cb;
        }

        void RigDevice::stop()
        {
            if (auto* cb = std::exchange (callback, nullptr))
                cb->audioDeviceStopped();
        }

        // The window's device as it sends it: recording from the interface and
        // monitoring on the headphones.
        juce::String windowDeviceState()
        {
            juce::XmlElement state ("DEVICESETUP");
            state.setAttribute ("deviceType", "Rig");
            state.setAttribute ("audioOutputDeviceName", "Headphones");
            state.setAttribute ("audioInputDeviceName", "Interface");
            state.setAttribute ("audioDeviceRate", kRate);
            state.setAttribute ("audioDeviceBufferSize", kBlock);
            state.setAttribute ("audioDeviceOutChans", "11");
            return state.toString();
        }

        capture::Command configureCommand (bool armed, int preRollSeconds)
        {
            auto* track = new juce::DynamicObject();
            track->setProperty ("name", "Vox");
            track->setProperty ("uid", "strip-1");
            track->setProperty ("input", 0);
            track->setProperty ("armed", armed);
            track->setProperty ("stereo", false);
            juce::Array<juce::var> tracks;
            tracks.add (juce::var (track));

            auto* config = new juce::DynamicObject();
            config->setProperty ("deviceState", windowDeviceState());
            config->setProperty ("sampleRate", kRate);
            config->setProperty ("captureDevice", "Interface");
            config->setProperty ("preRoll", preRollSeconds);
            config->setProperty ("backup", "");
            config->setProperty ("backupFormat", 1);
            config->setProperty ("tracks", tracks);
            config->setProperty ("mirrors", juce::Array<juce::var>());

            capture::Command c;
            c.action = capture::Action::ConfigureCapture;
            c.configuration = juce::var (config);
            return c;
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        juce::File tempSession (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-helper-" + tag + "-" + juce::Uuid().toString());
        }
    }

    class CaptureHelperTests final : public juce::UnitTest
    {
    public:
        CaptureHelperTests() : juce::UnitTest ("Capture helper device and exit", "zynforge") {}

        // A helper on the rig's devices, listening on a free port.
        bool bringUp (capture::CaptureDaemon& daemon, RigType*& type)
        {
            auto owned = std::make_unique<RigType>();
            type = owned.get();
            daemon.useDeviceTypeForTests (std::move (owned));
            juce::Random random;
            for (int attempt = 0; attempt < 20; ++attempt)
                if (daemon.start (50000 + random.nextInt (9000), 1)) return true;
            expect (false, "the helper never listened");
            return false;
        }

        void runTest() override
        {
            using namespace capture;

            beginTest ("The helper starts when the Mac's default input won't open, and opens the real one when told");
            {
                CaptureDaemon daemon;
                auto owned = std::make_unique<RigType>();
                auto* type = owned.get();
                type->refuseOpens.store (true);
                daemon.useDeviceTypeForTests (std::move (owned));
                juce::Random random;
                bool listening = false;
                for (int attempt = 0; attempt < 5 && ! listening; ++attempt)
                    listening = daemon.start (50000 + random.nextInt (9000), 1);
                expect (listening, "a default input that would not open kept the helper from starting");
                if (! listening) return;

                // The window's ConfigureCapture names the show interface.
                type->refuseOpens.store (false);
                daemon.handleCommand (configureCommand (true, 0));
                auto* device = daemon.deviceManager.getCurrentAudioDevice();
                expect (device != nullptr, "the configured device never opened");
                if (device != nullptr) expectEquals (device->getName(), juce::String ("Interface"));
                const auto session = tempSession ("no-default");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                daemon.handleCommand (start);
                expect (daemon.isRecording(), "the helper can't record once configured");
                Command stop; stop.action = Action::StopRecording;
                daemon.handleCommand (stop);
                daemon.stop();
                session.deleteRecursively();
            }

            beginTest ("The helper records from the input alone, so losing the window's monitor output leaves its take rolling");
            {
                CaptureDaemon daemon;
                RigType* type = nullptr;
                if (! bringUp (daemon, type)) return;
                daemon.handleCommand (configureCommand (true, 0));
                auto* device = daemon.deviceManager.getCurrentAudioDevice();
                expect (device != nullptr);
                if (device == nullptr) return;
                expectEquals (device->getName(), juce::String ("Interface"),
                              "the helper holds the window's monitor output");
                expect (daemon.deviceManager.getAudioDeviceSetup().outputDeviceName.isEmpty());

                const auto session = tempSession ("output-lost");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                daemon.handleCommand (start);
                expect (daemon.isRecording());
                type->feed (20, 0.25f);

                type->unplug ("Headphones");
                type->feed (20, 0.25f);
                expect (daemon.isRecording(), "pulling the headphones ended the take");
                expect (! daemon.captureDeviceLost.load());

                Command stop; stop.action = Action::StopRecording;
                daemon.handleCommand (stop);
                expect (! daemon.isRecording());
                daemon.stop();
                session.deleteRecursively();
            }

            beginTest ("An arm change reaches the helper without restarting its device, so its pre-roll survives");
            {
                CaptureDaemon daemon;
                RigType* type = nullptr;
                if (! bringUp (daemon, type)) return;
                daemon.handleCommand (configureCommand (false, 1));
                type->feed (200, 0.25f);   // a little over the 1 s of pre-roll
                const int starts = type->starts.load();

                daemon.handleCommand (configureCommand (true, 1));
                expectEquals (type->starts.load(), starts, "a changed arm restarted the helper's device");
                expect (daemon.getRecorder().getTrack (0).armed.load(), "the arm never reached the helper");

                const auto session = tempSession ("pre-roll");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                daemon.handleCommand (start);
                expect (daemon.isRecording());
                expectEquals (daemon.getRecorder().getSamplesSinceStart(), (juce::int64) kRate,
                              "the take started without the pre-roll it was set to keep");
                Command stop; stop.action = Action::StopRecording;
                daemon.handleCommand (stop);
                daemon.stop();
                session.deleteRecursively();
            }

            beginTest ("The helper opens and changes its device on its main thread, whichever thread asked");
            {
                CaptureDaemon daemon;
                RigType* type = nullptr;
                if (! bringUp (daemon, type)) return;
                const auto mainThread = std::this_thread::get_id();
                const auto opensBefore = type->openThreadsSeen().size();

                std::atomic<bool> finished { false }, ok { false };
                std::thread asker ([&]
                {
                    CaptureClient client;
                    if (client.connect ("127.0.0.1", daemon.getPort()) && client.hello (2000).ok)
                        ok = client.request (configureCommand (true, 0), 8000).ok;
                    client.disconnect();
                    finished = true;
                });
                expect (pumpUntil ([&] { return finished.load(); }, 10000), "the configure never finished");
                asker.join();
                expect (ok.load(), "the helper refused a configure it was asked for over the wire");
                const auto threads = type->openThreadsSeen();
                expect (threads.size() > opensBefore, "the configure opened no device");
                for (size_t i = opensBefore; i < threads.size(); ++i)
                    expect (threads[i] == mainThread, "the helper opened its device off its main thread");
                daemon.stop();
            }

            beginTest ("A helper stays up through STOP until STOP's reply has gone out");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                bool listening = false;
                juce::Random random;
                for (int attempt = 0; attempt < 20 && ! listening; ++attempt)
                    listening = daemon.start (50000 + random.nextInt (9000), 1);
                expect (listening);
                if (! listening) return;
                daemon.prepareForTests (kRate, kBlock, 1);
                daemon.getRecorder().getTrack (0).armed.store (true);

                CaptureClient client;
                expect (client.connect ("127.0.0.1", daemon.getPort()));
                expect (client.hello (2000).ok);
                const auto session = tempSession ("stop-reply");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                expect (client.request (start, 3000).ok);
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                for (int i = 0; i < 20; ++i) daemon.processTestBlock (inputs, 1, kBlock);

                std::atomic<bool> rollingAtReply { true }, heldAtReply { false };
                daemon.beforeStopReplyForTests = [&]
                {
                    rollingAtReply = daemon.isRecording();
                    heldAtReply = daemon.holdsTake();
                };
                Command stop; stop.action = Action::StopRecording;
                const auto reply = client.request (stop, 5000);
                expect (reply.completed, "STOP's reply never reached the window");
                expect (! rollingAtReply.load());
                expect (heldAtReply.load(), "the helper could exit before STOP's reply went out");
                const auto end = juce::Time::getMillisecondCounterHiRes() + 3000;
                while (daemon.holdsTake() && juce::Time::getMillisecondCounterHiRes() < end)
                    juce::Thread::sleep (5);
                expect (! daemon.holdsTake(), "a finished STOP kept the helper waiting");
                daemon.beforeStopReplyForTests = nullptr;
                client.disconnect();
                daemon.stop();
                session.deleteRecursively();
            }

            beginTest ("A status built before STOP never reaches the window after STOP's reply");
            {
                // The window takes a rolling status after STOP for a take it has
                // not adopted, takes it up again and re-links a twin follower.
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                // Installed before the status thread starts; armed later.
                juce::WaitableEvent built, release;
                std::atomic<bool> armed { false }, holding { false };
                daemon.afterStatusBuiltForTests = [&] (const EngineStatus& s)
                {
                    if (! armed.load() || ! s.recording || holding.exchange (true)) return;
                    built.signal();
                    release.wait (5000);
                };
                bool listening = false;
                juce::Random random;
                for (int attempt = 0; attempt < 20 && ! listening; ++attempt)
                    listening = daemon.start (50000 + random.nextInt (9000), 1);
                expect (listening);
                if (! listening) return;
                daemon.prepareForTests (kRate, kBlock, 1);
                daemon.getRecorder().getTrack (0).armed.store (true);

                CaptureClient client;
                std::atomic<bool> replied { false }, staleAfterReply { false };
                client.onStatus = [&] (const EngineStatus& s)
                {
                    if (replied.load() && s.recording) staleAfterReply = true;
                };
                expect (client.connect ("127.0.0.1", daemon.getPort()));
                expect (client.hello (2000).ok);
                const auto session = tempSession ("stale-status");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                expect (client.request (start, 3000).ok);
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                for (int i = 0; i < 20; ++i) daemon.processTestBlock (inputs, 1, kBlock);

                // Hold one rolling status between its build and its send.
                armed = true;
                expect (built.wait (3000), "no rolling status was built");
                Command stop; stop.action = Action::StopRecording;
                const auto reply = client.request (stop, 5000);
                expect (reply.completed && reply.ok, "STOP's reply never reached the window");
                replied = true;
                release.signal();
                juce::Thread::sleep (400);   // the held status and the next ones go out
                expect (! staleAfterReply.load(), "a status built before STOP arrived after STOP's reply");
                client.disconnect();
                daemon.stop();
                daemon.afterStatusBuiltForTests = nullptr;
                client.onStatus = nullptr;
                session.deleteRecursively();
            }

            beginTest ("A helper finishes its take at once only when the system is ending, or on a second signal");
            {
                expect (! helperexit::finishTakeNow (1, false), "one signal from a person ended the take");
                expect (helperexit::finishTakeNow (2, false));
                expect (helperexit::finishTakeNow (1, true), "a logout or shutdown left the take to be killed");
                expect (helperexit::fromSystem (1));
                expect (! helperexit::fromSystem (0));
                expect (! helperexit::fromSystem (4242));
            }
        }
    };

    static CaptureHelperTests captureHelperTests;
}
