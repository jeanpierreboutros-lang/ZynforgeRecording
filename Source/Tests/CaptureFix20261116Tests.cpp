// Regression tests for the capture helper, its link and the capture core
// (2026-11-16 audit): a window relaunched during a long helper STOP, the
// helper's files in /tmp, an attached helper of another build, the helper's
// configure refusal, and the capture boundary the audio callback took.

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Capture/CaptureDaemon.h"
#include "../Capture/CaptureSupervisor.h"
#include "../Network/CaptureIdentity.h"
#include "../Network/CaptureLink.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class CaptureFix20261116Tests final : public juce::UnitTest
    {
    public:
        CaptureFix20261116Tests() : juce::UnitTest ("Capture fixes 2026-11-16", "zynforge") {}

        static constexpr double kRate  = 48000.0;
        static constexpr int    kBlock = 256;

        static bool pumpUntil (const std::function<bool()>& done, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
            return done();
        }

        static juce::File tempSession (const juce::String& name)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-capfix-" + name + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        static bool startOnSomePort (capture::CaptureDaemon& daemon, int inputs = 1)
        {
            juce::Random random;
            for (int attempt = 0; attempt < 20; ++attempt)
                if (daemon.start (50000 + random.nextInt (9000), inputs)) return true;
            return false;
        }

        static juce::File identityFolder()
        {
            return juce::File ("/tmp/zynforge-capture-" + juce::String ((int) ::geteuid()));
        }

        static juce::File takeLockFor (const juce::File& session)
        {
            std::error_code error;
            const auto path = std::filesystem::weakly_canonical (session.getFullPathName().toStdString(), error);
            const auto text = error ? session.getFullPathName() : juce::String (path.string());
            return identityFolder().getChildFile ("take-" + juce::SHA256 (text.toUTF8()).toHexString().substring (0, 32)
                                                  + ".lock");
        }

        // As the macOS /tmp cleaner sees a file nobody touched for four days.
        static bool ageFile (const juce::File& file)
        {
            const auto old = (time_t) (juce::Time::currentTimeMillis() / 1000 - 4 * 24 * 3600);
            struct timeval times[2] {};
            times[0].tv_sec = times[1].tv_sec = old;
            return ::utimes (file.getFullPathName().toRawUTF8(), times) == 0;
        }

        static double ageSeconds (const juce::File& file)
        {
            struct stat status {};
            if (::stat (file.getFullPathName().toRawUTF8(), &status) != 0) return -1.0;
            return (double) (juce::Time::currentTimeMillis() / 1000 - (juce::int64) status.st_mtime);
        }

        static juce::var trackConfig (int tracks)
        {
            auto* o = new juce::DynamicObject();
            juce::var config (o);
            juce::Array<juce::var> list;
            for (int i = 0; i < tracks; ++i)
            {
                auto* t = new juce::DynamicObject();
                t->setProperty ("name", "In " + juce::String (i + 1));
                t->setProperty ("uid", "");
                t->setProperty ("input", i);
                t->setProperty ("armed", true);
                t->setProperty ("stereo", false);
                list.add (juce::var (t));
            }
            o->setProperty ("tracks", list);
            o->setProperty ("preRoll", 0);
            o->setProperty ("backup", "");
            o->setProperty ("backupFormat", 0);
            o->setProperty ("mirrors", juce::Array<juce::var>());
            return config;
        }

        // A helper rolling a take, then closing it slowly: its STOP holds the
        // command lock until `release` is signalled.
        struct SlowStopRig
        {
            capture::CaptureDaemon daemon;
            capture::CaptureClient first;
            juce::File session;
            juce::WaitableEvent stopHeld, release;
            bool ready { false };

            explicit SlowStopRig (const juce::String& name)
            {
                using namespace capture;
                daemon.setTestModeNoDevice (true);
                if (! startOnSomePort (daemon)) return;
                daemon.prepareForTests (kRate, kBlock, 1);
                daemon.getRecorder().getTrack (0).armed.store (true);
                session = tempSession (name);
                if (! first.connect ("127.0.0.1", daemon.getPort()) || ! first.hello (2000).ok) return;
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                if (! first.request (start, 3000).ok) return;
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                for (int i = 0; i < 20; ++i) daemon.processTestBlock (inputs, 1, kBlock);
                daemon.beforeStopReplyForTests = [this] { stopHeld.signal(); release.wait (15000); };
                Command stop; stop.action = Action::StopRecording;
                if (! first.send (stop) || ! stopHeld.wait (3000)) return;
                first.disconnect();   // the window crashed during STOP
                ready = true;
            }

            ~SlowStopRig()
            {
                release.signal();
                daemon.stop();
                daemon.beforeStopReplyForTests = nullptr;
                session.deleteRecursively();
            }
        };

        void runTest() override
        {
            using namespace capture;
            using namespace capture::identity;

            // ── HELP-01 ────────────────────────────────────────────────────
            beginTest ("HELP-01: a helper in a long STOP answers a new window's Hello without waiting for it");
            {
                SlowStopRig rig ("hello");
                expect (rig.ready, "the slow STOP rig did not come up");
                if (! rig.ready) return;
                std::atomic<bool> answered { false };
                std::thread window ([&]
                {
                    Command hello; hello.action = Action::Hello;
                    rig.daemon.handleCommand (hello);
                    answered = true;
                });
                expect (pumpUntil ([&] { return answered.load(); }, 500),
                        "the Hello waited on the STOP's command lock");
                rig.release.signal();
                window.join();
            }

            beginTest ("HELP-01: a window attached during a long helper STOP never blocks on configuring it");
            {
                SlowStopRig rig ("configure");
                expect (rig.ready, "the slow STOP rig did not come up");
                if (! rig.ready) return;
                CaptureSupervisor window;
                expect (window.connectOrLaunch (rig.daemon.getPort()), "the relaunched window could not attach");
                const auto before = juce::Time::getMillisecondCounterHiRes();
                juce::String refusal;
                const bool configured = window.configureCapture (trackConfig (1), &refusal);
                const auto took = juce::Time::getMillisecondCounterHiRes() - before;
                expect (took < 1500.0, "configuring a helper busy with STOP held the window "
                                       + juce::String (took, 0) + " ms");
                expect (! configured, "a configure counted as done while the helper was still in STOP");
                rig.release.signal();
                expect (pumpUntil ([&] { return window.hasStatus() && ! window.isDaemonRecording(); }, 5000),
                        "no status after the STOP finished");
                expect (window.configureCapture (trackConfig (1), &refusal), "the helper took no configure after its STOP");
                window.shutdown();
            }

            // ── HELP-06 ────────────────────────────────────────────────────
            beginTest ("HELP-06: the helper's configure refusal is kept, also while a retry waits");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                expect (startOnSomePort (daemon, 2));
                daemon.prepareForTests (kRate, kBlock, 2);
                CaptureSupervisor window;
                expect (window.connectOrLaunch (daemon.getPort()));
                expect (pumpUntil ([&] { return window.hasStatus(); }, 3000));
                auto invalid = trackConfig (1);
                invalid.getDynamicObject()->setProperty ("tracks", juce::Array<juce::var>());
                juce::String refusal;
                expect (! window.configureCapture (invalid, &refusal));
                expectEquals (refusal, juce::String ("invalid track configuration"));
                refusal.clear();
                expect (! window.configureCapture (invalid, &refusal), "a refused configure counted as done");
                expectEquals (refusal, juce::String ("invalid track configuration"),
                              "the reason was lost while the retry waits");
                expect (window.configureCapture (trackConfig (2), &refusal));
                expect (refusal.isEmpty(), "an accepted configure still carried a refusal");
                window.disconnect();
                daemon.stop();
            }

            // ── HELP-02 ────────────────────────────────────────────────────
            beginTest ("HELP-02: a helper writes its key file again when the /tmp cleaner deletes it");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                expect (startOnSomePort (daemon));
                const auto key = identityFolder().getChildFile (juce::String (daemon.getPort()) + ".key");
                expect (key.existsAsFile());
                expect (key.deleteFile());
                expect (pumpUntil ([&] { return key.existsAsFile(); }, 4000), "the deleted key file never came back");
                CaptureClient window;
                expect (window.connect ("127.0.0.1", daemon.getPort()));
                const auto greeting = window.hello (2000);
                expect (greeting.ok, "a window could not prove itself after the cleaner ran: " + greeting.error);
                window.disconnect();
                daemon.stop();
            }

            beginTest ("HELP-02: a helper keeps its key and claims files fresh for the /tmp cleaner");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                expect (startOnSomePort (daemon));
                const auto port = juce::String (daemon.getPort());
                const auto key = identityFolder().getChildFile (port + ".key");
                const auto claims = identityFolder().getChildFile (port + ".claims");
                expect (ageFile (key));
                expect (ageSeconds (key) > 3 * 24 * 3600, "the test could not age the key file");
                expect (claims.deleteFile());
                expect (Endpoint::helperWithoutClaims (daemon.getPort()), "the setup did not remove the claims file");
                expect (pumpUntil ([&] { return ageSeconds (key) >= 0.0 && ageSeconds (key) < 600.0; }, 4000),
                        "the helper never freshened its key file");
                expect (pumpUntil ([&] { return claims.existsAsFile(); }, 4000),
                        "the helper never put its claims file back");
                expect (! Endpoint::helperWithoutClaims (daemon.getPort()),
                        "a current helper reads as one too old to publish claims");
                daemon.stop();
            }

            beginTest ("HELP-02: holding a take claim freshens an old lock file");
            {
                const auto session = tempSession ("old-lock");
                const auto lock = takeLockFor (session);
                {
                    TakeClaim first;
                    expect (first.hold (session));
                }
                expect (lock.existsAsFile(), "the lock file is not where the test looks");
                expect (ageFile (lock));
                TakeClaim second;
                expect (second.hold (session));
                expect (ageSeconds (lock) >= 0.0 && ageSeconds (lock) < 600.0,
                        "a held take lock kept a four-day-old date for the cleaner");
                second.release();
                session.deleteRecursively();
            }

            beginTest ("HELP-02: a take lock deleted while the helper holds it comes back held");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                expect (startOnSomePort (daemon));
                daemon.prepareForTests (kRate, kBlock, 1);
                daemon.getRecorder().getTrack (0).armed.store (true);
                CaptureClient window;
                expect (window.connect ("127.0.0.1", daemon.getPort()) && window.hello (2000).ok);
                const auto session = tempSession ("deleted-lock");
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                expect (window.request (start, 3000).ok);
                expect (TakeClaim::isHeld (session));
                expect (takeLockFor (session).deleteFile());
                expect (! TakeClaim::isHeld (session), "the setup did not remove the lock");
                expect (pumpUntil ([&] { return TakeClaim::isHeld (session); }, 4000),
                        "a rolling take's claim stayed gone after the cleaner removed its file");
                Command stop; stop.action = Action::StopRecording;
                expect (window.request (stop, 10000).completed);
                window.disconnect();
                daemon.stop();
                session.deleteRecursively();
            }

            // ── HELP-03 ────────────────────────────────────────────────────
            beginTest ("HELP-03: an idle helper the window attached to is asked to quit at shutdown");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                std::atomic<bool> quitAsked { false };
                daemon.onQuitRequest = [&] { quitAsked = true; };
                expect (startOnSomePort (daemon));
                daemon.prepareForTests (kRate, kBlock, 1);
                CaptureSupervisor window;
                expect (window.connectOrLaunch (daemon.getPort()));
                expect (! window.didLaunch());
                expect (pumpUntil ([&] { return window.hasStatus(); }, 3000));
                window.shutdown();
                expect (quitAsked.load(), "an attached idle helper outlived the window that used it");
                daemon.stop();
            }

            beginTest ("HELP-03: a helper rolling a take is never asked to quit at shutdown");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                std::atomic<bool> quitAsked { false };
                daemon.onQuitRequest = [&] { quitAsked = true; };
                expect (startOnSomePort (daemon));
                daemon.prepareForTests (kRate, kBlock, 1);
                const auto session = tempSession ("rolling-quit");
                CaptureSupervisor window;
                expect (window.connectOrLaunch (daemon.getPort()));
                juce::BigInteger arms; arms.setBit (0);
                expect (window.startRecording (session, 1, 0, arms));
                expect (pumpUntil ([&] { return window.isDaemonRecording(); }, 3000));
                window.shutdown();
                expect (! quitAsked.load(), "a rolling helper was asked to quit");
                expect (daemon.isRecording(), "shutdown ended the take");
                CaptureClient after;
                expect (after.connect ("127.0.0.1", daemon.getPort()) && after.hello (2000).ok);
                Command stop; stop.action = Action::StopRecording;
                expect (after.request (stop, 10000).completed);
                after.disconnect();
                daemon.stop();
                session.deleteRecursively();
            }

            beginTest ("HELP-03: an idle helper of another build is replaced by this build's helper");
            {
                const auto binary = CaptureSupervisor::discoverBinary();
                expect (binary.existsAsFile(), "no helper binary next to the tests");
                if (! binary.existsAsFile()) return;
                CaptureDaemon older;
                older.setTestModeNoDevice (true);
                older.setBuildId ("an-older-build");
                // Quit stops this helper as the real one exits: from another
                // thread, never the reader the Quit arrived on.
                juce::WaitableEvent quitEvent;
                std::atomic<bool> quitAsked { false };
                std::thread stopper ([&] { quitEvent.wait (30000); older.stop(); });
                older.onQuitRequest = [&] { quitAsked = true; quitEvent.signal(); };
                expect (startOnSomePort (older, 2));
                older.prepareForTests (kRate, kBlock, 2);
                const int port = older.getPort();

                CaptureSupervisor window;
                window.setBinaryPath (binary);
                expect (window.connectOrLaunch (port, { "--no-device", "--inputs", "2" }), window.lastConnectError);
                expect (quitAsked.load(), "the older idle helper was never asked to quit");
                expect (window.didLaunch(), "the window kept driving a helper of another build");
                expect (window.expectedHelperBuild().isNotEmpty(), "this build names no helper build");
                expectEquals (window.helperBuild(), window.expectedHelperBuild(),
                              "the helper now attached is not this build's");
                window.shutdown();
                quitEvent.signal();
                stopper.join();
                expect (pumpUntil ([&]
                {
                    CaptureClient probe;
                    const bool up = probe.connect ("127.0.0.1", port) && probe.hello (300).ok;
                    probe.disconnect();
                    return ! up;
                }, 5000), "the launched helper did not quit");
            }

            beginTest ("HELP-03: a helper of another build with a take rolling is kept");
            {
                CaptureDaemon older;
                older.setTestModeNoDevice (true);
                older.setBuildId ("an-older-build");
                std::atomic<bool> quitAsked { false };
                older.onQuitRequest = [&] { quitAsked = true; };
                expect (startOnSomePort (older));
                older.prepareForTests (kRate, kBlock, 1);
                older.getRecorder().getTrack (0).armed.store (true);
                const auto session = tempSession ("older-rolling");
                CaptureClient first;
                expect (first.connect ("127.0.0.1", older.getPort()) && first.hello (2000).ok);
                Command start; start.action = Action::StartRecording; start.sessionDir = session.getFullPathName();
                expect (first.request (start, 3000).ok);
                first.disconnect();

                CaptureSupervisor window;
                window.setBinaryPath (CaptureSupervisor::discoverBinary());
                expect (window.connectOrLaunch (older.getPort()));
                expect (! window.didLaunch(), "a helper with a take rolling was replaced");
                expect (! quitAsked.load(), "a rolling helper was asked to quit");
                expect (older.isRecording());
                expect (pumpUntil ([&] { return window.isDaemonRecording(); }, 3000));
                expect (window.stopRecording());
                window.disconnect();
                older.stop();
                session.deleteRecursively();
            }

            // ── CAP-05 ─────────────────────────────────────────────────────
            beginTest ("CAP-05: the audio callback never waits on the capture boundary");
            {
                MultitrackRecorder recorder;
                recorder.prepare (kRate, kBlock, 2);
                for (int ch = 0; ch < 2; ++ch) recorder.getTrack (ch).armed.store (true);
                const auto session = tempSession ("boundary-audio");
                expect (recorder.startRecording (session));
                std::vector<float> samples ((size_t) kBlock, 0.1f);
                const float* inputs[] { samples.data(), samples.data() };
                std::atomic<int> blocks { 0 };
                {
                    const juce::SpinLock::ScopedLockType held (recorder.captureBoundary);
                    std::thread audio ([&]
                    {
                        for (int i = 0; i < 8; ++i) { recorder.processBlock (inputs, 2, kBlock); ++blocks; }
                    });
                    const bool ran = pumpUntil ([&] { return blocks.load() == 8; }, 500);
                    expect (ran, "processBlock waited on a lock another thread held");
                    // Let a waiting callback through before the lock goes out of scope.
                    if (! ran) { recorder.captureBoundary.exit(); audio.join(); recorder.captureBoundary.enter(); }
                    else audio.join();
                }
                recorder.stopRecording();
                session.deleteRecursively();
            }

            beginTest ("CAP-05: the writer drains a take without the capture boundary");
            {
                MultitrackRecorder recorder;
                recorder.prepare (kRate, kBlock, 2);
                for (int ch = 0; ch < 2; ++ch) recorder.getTrack (ch).armed.store (true);
                const auto session = tempSession ("boundary-drain");
                expect (recorder.startRecording (session));
                std::vector<float> samples ((size_t) kBlock, 0.1f);
                const float* inputs[] { samples.data(), samples.data() };
                for (int i = 0; i < 40; ++i) recorder.processBlock (inputs, 2, kBlock);
                std::atomic<bool> drained { false };
                {
                    const juce::SpinLock::ScopedLockType held (recorder.captureBoundary);
                    std::thread writer ([&] { recorder.drainPendingForTests(); drained = true; });
                    const bool ran = pumpUntil ([&] { return drained.load(); }, 1000);
                    expect (ran, "the drain waited on the lock the audio callback shares");
                    if (! ran) { recorder.captureBoundary.exit(); writer.join(); recorder.captureBoundary.enter(); }
                    else writer.join();
                }
                recorder.stopRecording();
                session.deleteRecursively();
            }

            beginTest ("CAP-05: takes started and stopped beside a running callback keep every sample once");
            {
                // The audio thread feeds a counter signal the whole time; the
                // control side starts and stops takes with pre-roll on. A
                // block lost or doubled at a start, a stop or the pre-roll
                // boundary breaks the count in the file.
                MultitrackRecorder recorder;
                recorder.setPreRollSeconds (1);
                recorder.prepare (kRate, kBlock, 2);
                for (int ch = 0; ch < 2; ++ch) recorder.getTrack (ch).armed.store (true);
                std::atomic<bool> running { true };
                std::atomic<juce::int64> fed { 0 };
                std::thread audio ([&]
                {
                    std::vector<float> left ((size_t) kBlock), right ((size_t) kBlock);
                    const float* inputs[] { left.data(), right.data() };
                    juce::int64 n = 0;
                    while (running.load())
                    {
                        for (int i = 0; i < kBlock; ++i, ++n)
                        {
                            left[(size_t) i]  = (float) ((n % 4096) - 2048) / 4096.0f;
                            right[(size_t) i] = -left[(size_t) i];
                        }
                        recorder.processBlock (inputs, 2, kBlock);
                        fed.store (n);
                        std::this_thread::sleep_for (std::chrono::microseconds (900));
                    }
                });
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                for (int take = 0; take < 4; ++take)
                {
                    const auto session = tempSession ("counter-" + juce::String (take));
                    const auto startAt = fed.load();
                    pumpUntil ([&] { return fed.load() > startAt + (juce::int64) kRate / 2; }, 3000);
                    expect (recorder.startRecording (session), recorder.getStartError());
                    const auto rolledAt = fed.load();
                    pumpUntil ([&] { return fed.load() > rolledAt + (juce::int64) kRate / 4; }, 3000);
                    recorder.stopRecording();
                    expectEquals (recorder.getMissedSamples(), (juce::int64) 0);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const auto files = session.getChildFile ("Audio Files")
                                               .findChildFiles (juce::File::findFiles, false, "*.wav");
                        juce::File file;
                        for (const auto& f : files)
                            if (f.getFileName().startsWith ("Track_0" + juce::String (ch + 1))
                                || f.getFileName().startsWith ("0" + juce::String (ch + 1) + " "))
                                file = f;
                        expect (file.existsAsFile(), "take " + juce::String (take) + " has no file for track "
                                                     + juce::String (ch + 1));
                        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                        expect (reader != nullptr);
                        if (reader == nullptr) continue;
                        const int length = (int) reader->lengthInSamples;
                        expectGreaterThan (length, (int) kRate / 4, "take " + juce::String (take) + " is too short");
                        juce::AudioBuffer<float> audio (1, juce::jmax (1, length));
                        reader->read (&audio, 0, length, 0, true, false);
                        const auto* d = audio.getReadPointer (0);
                        int breaks = 0;
                        const float step = (ch == 0 ? 1.0f : -1.0f) / 4096.0f;
                        for (int i = 1; i < length; ++i)
                        {
                            const float diff = d[i] - d[i - 1];
                            const bool wrap = std::abs (std::abs (diff) - 4095.0f / 4096.0f) < 1.0e-5f;
                            if (std::abs (diff - step) > 1.0e-5f && ! wrap) ++breaks;
                        }
                        expectEquals (breaks, 0, "take " + juce::String (take) + " track " + juce::String (ch + 1)
                                                 + " lost or doubled audio");
                    }
                    session.deleteRecursively();
                }
                running = false;
                audio.join();
            }
        }
    };

    static CaptureFix20261116Tests captureFix20261116Tests;
}
