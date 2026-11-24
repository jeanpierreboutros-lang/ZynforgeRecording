// The capture helper: the fixes from the 2026-11-24 audit. Each test names
// what it guards and fails on the code from before the fix. The helper tests
// launch the real helper binary on real ports.
//
//   1. A helper of another build replaced by this build's: the window waits
//      until the old one has let go of its port's key before it launches the
//      new one, and a new one that still can't take the port says the old one
//      is still shutting down, not "another program".
//   2. A launch that attached to another helper (one already on the port)
//      does not take it for the one it started, and the one it started, which
//      lost the port, is not reported later as a helper that died.
//   3. A window's instant look at a key lock does not stop a helper taking it.
//   4. A device restart at a new rate while STOP closes the take's files is
//      applied before the next take: its files carry the new rate.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "../Capture/CaptureDaemon.h"
#include "../Capture/CaptureSupervisor.h"
#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureLink.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace zynforge
{
    namespace
    {
        constexpr double kRate    = 48000.0;
        constexpr double kNewRate = 96000.0;
        constexpr int    kBlock   = 256;

        // The message thread runs what it was posted only while this pumps.
        bool pumpUntil (const std::function<bool()>& done, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return done();
        }

        juce::File scratchDir (const juce::String& name)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-fix1124-" + name + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool startOnSomePort (capture::CaptureDaemon& daemon, int inputs = 1)
        {
            juce::Random random;
            for (int attempt = 0; attempt < 20; ++attempt)
                if (daemon.start (50000 + random.nextInt (9000), inputs)) return true;
            return false;
        }

        // No capture helper answers on the port any more.
        bool portGoesQuiet (int port, int timeoutMs)
        {
            return pumpUntil ([port]
            {
                capture::CaptureClient probe;
                const bool up = probe.connect ("127.0.0.1", port) && probe.hello (300).ok;
                probe.disconnect();
                return ! up;
            }, timeoutMs);
        }

        // ── An interface that can change its rate by itself ─────────────────

        class RateRigType;

        class RateRigDevice final : public juce::AudioIODevice
        {
        public:
            explicit RateRigDevice (RateRigType& owner) : juce::AudioIODevice ("Interface", "RateRig"), type (owner) {}
            ~RateRigDevice() override { close(); }

            juce::StringArray getOutputChannelNames() override { return {}; }
            juce::StringArray getInputChannelNames() override  { return { "1", "2" }; }
            juce::Array<double> getAvailableSampleRates() override { return { kRate, kNewRate }; }
            juce::Array<int> getAvailableBufferSizes() override    { return { kBlock }; }
            int getDefaultBufferSize() override                    { return kBlock; }
            juce::String open (const juce::BigInteger& ins, const juce::BigInteger&, double sampleRate, int) override
            {
                activeInputs = ins;
                if (sampleRate > 0.0) rate = sampleRate;
                opened = true;
                return {};
            }
            void close() override;
            bool isOpen() override { return opened; }
            void start (juce::AudioIODeviceCallback*) override;
            void stop() override;
            bool isPlaying() override { return callback != nullptr; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return kBlock; }
            double getCurrentSampleRate() override { return rate; }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return {}; }
            juce::BigInteger getActiveInputChannels() const override  { return activeInputs; }

            // The interface moved to another rate by itself (its front panel,
            // or another app): JUCE stops the device and starts it again.
            void restartAt (double newRate)
            {
                auto* cb = callback;
                stop();
                rate = newRate;
                start (cb);
            }

            RateRigType& type;
            double rate { kRate };
            bool opened { false };
            juce::BigInteger activeInputs;
            juce::AudioIODeviceCallback* callback { nullptr };
        };

        class RateRigType final : public juce::AudioIODeviceType
        {
        public:
            RateRigType() : juce::AudioIODeviceType ("RateRig") {}

            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool wantInputs) const override
            { return wantInputs ? juce::StringArray { "Interface" } : juce::StringArray(); }
            int getDefaultDeviceIndex (bool) const override { return 0; }
            int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
            { return device == nullptr ? -1 : getDeviceNames (asInput).indexOf (device->getName()); }
            bool hasSeparateInputsAndOutputs() const override { return true; }
            juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
            {
                if (output.isEmpty() && input.isEmpty()) return nullptr;
                return new RateRigDevice (*this);
            }

            // Blocks from the running device, as its own thread would deliver them.
            void feed (int blocks, float value)
            {
                std::vector<float> samples ((size_t) kBlock, value);
                const float* inputs[] { samples.data(), samples.data() };
                for (int i = 0; i < blocks && live != nullptr && live->callback != nullptr; ++i)
                    live->callback->audioDeviceIOCallbackWithContext (inputs, 2, nullptr, 0, kBlock, {});
            }

            RateRigDevice* live { nullptr };
        };

        void RateRigDevice::close()
        {
            stop();
            opened = false;
            if (type.live == this) type.live = nullptr;
        }

        void RateRigDevice::start (juce::AudioIODeviceCallback* cb)
        {
            if (cb == nullptr || callback != nullptr) return;
            type.live = this;
            cb->audioDeviceAboutToStart (this);
            callback = cb;
        }

        void RateRigDevice::stop()
        {
            if (auto* cb = std::exchange (callback, nullptr))
                cb->audioDeviceStopped();
        }

        capture::Command rigConfigure()
        {
            juce::XmlElement state ("DEVICESETUP");
            state.setAttribute ("deviceType", "RateRig");
            state.setAttribute ("audioInputDeviceName", "Interface");
            state.setAttribute ("audioDeviceRate", kRate);
            state.setAttribute ("audioDeviceBufferSize", kBlock);

            auto* track = new juce::DynamicObject();
            track->setProperty ("name", "Vox");
            track->setProperty ("uid", "strip-1");
            track->setProperty ("input", 0);
            track->setProperty ("armed", true);
            track->setProperty ("stereo", false);
            juce::Array<juce::var> tracks;
            tracks.add (juce::var (track));

            auto* config = new juce::DynamicObject();
            config->setProperty ("deviceState", state.toString());
            config->setProperty ("sampleRate", kRate);
            config->setProperty ("captureDevice", "Interface");
            config->setProperty ("preRoll", 0);
            config->setProperty ("backup", "");
            config->setProperty ("backupFormat", 1);
            config->setProperty ("tracks", tracks);
            config->setProperty ("mirrors", juce::Array<juce::var>());

            capture::Command c;
            c.action = capture::Action::ConfigureCapture;
            c.configuration = juce::var (config);
            return c;
        }

        // The rate in the header of the first track file a take wrote; 0 if none.
        double takeRate (const juce::File& session)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (const auto& file : session.getChildFile ("Audio Files").findChildFiles (juce::File::findFiles, false))
                if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (file) })
                    return reader->sampleRate;
            return 0.0;
        }
    }

    class Fix20261124HelperTests final : public juce::UnitTest
    {
    public:
        Fix20261124HelperTests() : juce::UnitTest ("Fixes 2026-11-24 helper", "zynforge") {}

        void runTest() override
        {
            anOldHelperSlowToLetGoIsReplaced();                // 1
            anOldHelperThatKeepsItsKeyIsNamed();               // 1
            aTwinAttachIsNotTakenForTheLaunchedHelper();       // 2
            anInstantLookAtTheKeyDoesNotStopAHelper();         // 3
            aRestartDuringStopReachesTheNextTake();            // 4
        }

        // ── 1. Replacing a helper of another build ─────────────────────────

        // A helper of another build, idle, that closes its port at once when
        // told to quit and lets go of its key only after keyHoldMs (or once
        // letGo is signalled): one whose last threads take that long to end.
        struct SlowToLetGo
        {
            capture::CaptureDaemon older;
            juce::WaitableEvent quitEvent, letGo;
            std::atomic<bool> quitAsked { false }, keyHeldPastPort { false };
            std::thread stopper;
            int port { -1 };

            bool start (int keyHoldMs)
            {
                older.setTestModeNoDevice (true);
                older.setBuildId ("an-older-build");
                // Quit stops this helper as the real one exits: from another
                // thread, never the reader the Quit arrived on.
                older.onQuitRequest = [this] { quitAsked = true; quitEvent.signal(); };
                stopper = std::thread ([this] { quitEvent.wait (120000); older.stop(); });
                if (! startOnSomePort (older)) return false;
                port = older.getPort();
                older.prepareForTests (kRate, kBlock, 1);
                older.server.beforeIdentityReleaseForTests = [this, keyHoldMs]
                {
                    keyHeldPastPort = true;
                    letGo.wait (keyHoldMs);
                };
                return true;
            }

            ~SlowToLetGo()
            {
                letGo.signal();
                quitEvent.signal();
                if (stopper.joinable()) stopper.join();
            }
        };

        void anOldHelperSlowToLetGoIsReplaced()
        {
            using namespace capture;
            beginTest ("Helper: an idle one of another build that keeps its key after its port closes is replaced once it lets go");

            const auto binary = CaptureSupervisor::discoverBinary();
            expect (binary.existsAsFile(), "no helper binary next to the tests");
            if (! binary.existsAsFile()) return;

            SlowToLetGo old;
            expect (old.start (3000), "fixture: the older helper did not listen");
            if (old.port <= 0) return;

            CaptureSupervisor window;
            window.setBinaryPath (binary);
            const bool attached = window.connectOrLaunch (old.port, { "--no-device", "--inputs", "1" });
            expect (old.quitAsked.load(), "fixture: the older helper was never asked to quit");
            expect (old.keyHeldPastPort.load(), "fixture: the older helper let go of its key with its port");
            expect (attached, "this build's helper did not take the older one's place: " + window.lastConnectError);
            expect (! window.lastConnectError.containsIgnoreCase ("another program"), window.lastConnectError);
            if (attached)
            {
                expect (window.didLaunch(), "the window does not hold the helper it started");
                expect (window.didReplaceOtherBuild());
                expect (! window.keepsOtherBuild());
                expectEquals (window.helperBuild(), window.expectedHelperBuild(),
                              "the helper now attached is not this build's");
            }
            window.shutdown();
            expect (portGoesQuiet (old.port, 5000), "the launched helper did not quit");
        }

        void anOldHelperThatKeepsItsKeyIsNamed()
        {
            using namespace capture;
            beginTest ("Helper: a replacement that can't start while the old one still holds its key says so, and blames no other program");

            const auto binary = CaptureSupervisor::discoverBinary();
            if (! binary.existsAsFile()) return;   // said by the test above

            SlowToLetGo old;
            expect (old.start (60000), "fixture: the older helper did not listen");
            if (old.port <= 0) return;

            CaptureSupervisor window;
            window.setBinaryPath (binary);
            const bool attached = window.connectOrLaunch (old.port, { "--no-device", "--inputs", "1" });
            expect (old.keyHeldPastPort.load(), "fixture: the older helper let go of its key with its port");
            expect (! attached, "fixture: a helper started while the old one held the port's key");
            expect (! window.lastConnectError.containsIgnoreCase ("another program"),
                    "our own helper still shutting down was blamed on another program: " + window.lastConnectError);
            expect (window.lastConnectError.containsIgnoreCase ("still shutting down"), window.lastConnectError);
            old.letGo.signal();
            window.shutdown();
        }

        // ── 2. A launch that attached to another helper ────────────────────

        void aTwinAttachIsNotTakenForTheLaunchedHelper()
        {
            using namespace capture;
            beginTest ("Helper: a launch that attached to another helper doesn't take it for its own, or report its own one dead");

            const auto binary = CaptureSupervisor::discoverBinary();
            if (! binary.existsAsFile()) return;

            // One already on the port (another window launched it a moment
            // earlier); the one this window starts is slow to start.
            CaptureDaemon other;
            other.setTestModeNoDevice (true);
            expect (startOnSomePort (other), "fixture: the other helper did not listen");
            const int port = other.getPort();
            other.prepareForTests (kRate, kBlock, 1);

            CaptureSupervisor window;
            window.setBinaryPath (binary);
            window.port = port;
            window.launchArgs = { "--no-device", "--inputs", "1", "--start-delay-ms", "1500" };
            std::atomic<int> deaths { 0 };
            window.onDaemonDied = [&] (bool) { ++deaths; };
            expect (window.launchAndAttach(), "the window attached to nothing: " + window.lastConnectError);
            expect (window.isAttached());
            expect (! window.didLaunch(), "a helper this window did not start was taken for its own");

            // The one it started finds the port taken and exits.
            const auto end = juce::Time::getMillisecondCounterHiRes() + 8000.0;
            while (window.process.isRunning() && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep (20);
            expect (! window.process.isRunning(), "fixture: the slow helper never gave up the port");
            for (int i = 0; i < 5; ++i)
            {
                window.tick();
                juce::Thread::sleep (20);
            }
            expectEquals (deaths.load(), 0, "the helper this window started, which lost the port, was reported as a dead helper");
            expect (window.isAttached(), "the link to the helper that answered was lost");
            window.shutdown();
            other.stop();
        }

        // ── 3. A look at the key lock ──────────────────────────────────────

        void anInstantLookAtTheKeyDoesNotStopAHelper()
        {
            using namespace capture;
            beginTest ("Helper: a window's instant look at a port's key lock doesn't stop a helper from taking it");

            juce::Random random;
            int port = 0;
            for (int attempt = 0; attempt < 50 && port == 0; ++attempt)
            {
                const int candidate = 50000 + random.nextInt (9000);
                if (! identity::Endpoint::lockIsHeld ((juce::String (candidate) + ".key").toRawUTF8())) port = candidate;
            }
            expect (port > 0, "fixture: no free port key");
            if (port == 0) return;

            // A window asks whether the key is held (Endpoint::lockIsHeld) just
            // as the helper takes it: the look holds the lock shared, briefly.
            const int look = identity::Endpoint::openSharedFile ((juce::String (port) + ".key").toRawUTF8());
            expect (look >= 0, "fixture: the key file could not be opened");
            if (look < 0) return;
            expect (::flock (look, LOCK_SH | LOCK_NB) == 0, "fixture: the look could not hold the lock");
            std::thread lookDone ([look] { juce::Thread::sleep (15); ::flock (look, LOCK_UN); });
            identity::Endpoint endpoint;
            const bool created = endpoint.create (port);
            lookDone.join();
            ::close (look);
            expect (created, "a helper could not take its port's key because a window looked at it");
            endpoint.reset();
            if (! created)   // the endpoint removes only a key it took
                juce::File ("/tmp/zynforge-capture-" + juce::String ((int) ::geteuid()))
                    .getChildFile (juce::String (port) + ".key").deleteFile();
        }

        // ── 4. A device restart while STOP closes the files ────────────────

        void aRestartDuringStopReachesTheNextTake()
        {
            using namespace capture;
            beginTest ("Helper: a device restart at a new rate while STOP closes the files is applied before the next take");

            CaptureDaemon daemon;
            auto owned = std::make_unique<RateRigType>();
            auto* type = owned.get();
            daemon.useDeviceTypeForTests (std::move (owned));
            expect (startOnSomePort (daemon), "fixture: the helper did not listen");
            daemon.handleCommand (rigConfigure());
            auto* device = dynamic_cast<RateRigDevice*> (daemon.deviceManager.getCurrentAudioDevice());
            expect (device != nullptr, "fixture: the configured device did not open");
            if (device == nullptr) { daemon.stop(); return; }
            expectEquals (device->getCurrentSampleRate(), kRate);

            const auto first = scratchDir ("restart-first"), second = scratchDir ("restart-second");
            const juce::ScopeGuard removeSessions { [&] { first.deleteRecursively(); second.deleteRecursively(); } };

            Command start; start.action = Action::StartRecording; start.sessionDir = first.getFullPathName();
            daemon.handleCommand (start);
            expect (daemon.isRecording(), "fixture: the first take did not roll");
            type->feed (40, 0.25f);

            // A window's STOP closes the files on the reader thread; the
            // interface comes back at 96 kHz meanwhile, on the helper's main
            // thread.
            juce::WaitableEvent closing, resume;
            daemon.duringStopFinalizationForTests = [&] { closing.signal(); resume.wait (10000); };
            std::atomic<bool> stopped { false };
            std::thread stopper ([&]
            {
                Command stop; stop.action = Action::StopRecording;
                daemon.handleCommand (stop);
                stopped = true;
            });
            const bool held = closing.wait (5000);
            expect (held, "fixture: STOP never began closing the files");
            if (held) device->restartAt (kNewRate);
            resume.signal();
            expect (pumpUntil ([&] { return stopped.load(); }, 20000), "STOP never finished");
            stopper.join();
            daemon.duringStopFinalizationForTests = nullptr;
            expect (! daemon.isRecording());

            Command next; next.action = Action::StartRecording; next.sessionDir = second.getFullPathName();
            daemon.handleCommand (next);
            expect (daemon.isRecording(), "the take after the restart did not roll");
            type->feed (40, 0.25f);
            Command stop; stop.action = Action::StopRecording;
            daemon.handleCommand (stop);
            expect (! daemon.isRecording());

            expectEquals (takeRate (first), kRate, "the take before the restart lost its rate");
            expectEquals (takeRate (second), kNewRate,
                          "the take after a restart during STOP was written with the old rate in its header");
            daemon.stop();
        }
    };

    static Fix20261124HelperTests fix20261124HelperTests;
}
