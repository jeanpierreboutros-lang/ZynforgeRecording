// Session-file jobs and the offline mix render.
//
// SessionJobRunner owns the one background worker for exports, bounces,
// imports and session copies, and makes releasing the session-I/O reservation
// structural. Before it, each of eight hand-written std::thread launches
// released its own reservation; the track export's cancel path returned
// before doing so, leaving the session busy for good after a quit-cancel.
//
// The stereo-mix render used to keep a TrackState pointer per track, so its
// UI caller held the recorder's structure lock for the whole bounce -- the
// lock the 10 Hz timer's captureStatus() also takes, which froze the app until
// the bounce ended. The render now snapshots strips under a brief lock.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "../Audio/AudioEngine.h"
#include "../UI/SessionJobRunner.h"

#include <stdexcept>
#include <thread>

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    class SessionJobsTests final : public juce::UnitTest
    {
    public:
        SessionJobsTests() : juce::UnitTest ("Session jobs", "zynforge") {}

        using Completion = SessionJobRunner::Completion;

        // Same message pump as AsyncStopAuditTests.
        static void pumpFor (int ms)
        {
           #if JUCE_MAC
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, ms / 1000.0, false);
           #elif JUCE_MODAL_LOOPS_PERMITTED
            juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
           #else
            juce::Thread::sleep (ms);
           #endif
        }

        static bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 3000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                pumpFor (5);
            return done();
        }

        static bool writeWav (const juce::File& f, juce::int64 len, float value)
        {
            f.getParentDirectory().createDirectory();
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            if (os == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0));
            if (w == nullptr) return false;
            os.release();
            juce::AudioBuffer<float> buf (1, 4096);
            for (juce::int64 done = 0; done < len;)
            {
                const int n = (int) juce::jmin ((juce::int64) 4096, len - done);
                juce::FloatVectorOperations::fill (buf.getWritePointer (0), value, n);
                if (! w->writeFromAudioSampleBuffer (buf, 0, n)) return false;
                done += n;
            }
            return true;
        }

        void runTest() override
        {
            beginTest ("A cancelled job that returns no completion still releases the reservation");
            {
                std::atomic<bool> busy { false };
                SessionJobRunner jobs (busy);
                juce::WaitableEvent started;
                expect (jobs.start ([&started] (const std::atomic<bool>& cancel) -> Completion
                {
                    started.signal();
                    while (! cancel.load()) juce::Thread::sleep (1);
                    return {};   // the shape of the old track-export early return
                }));
                expect (busy.load(), "a running job holds the reservation");
                expect (started.wait (2000));
                jobs.cancel();
                expect (pumpUntil ([&busy] { return ! busy.load(); }),
                        "cancelling must release the reservation even with no completion");
            }

            beginTest ("A job that throws still releases the reservation");
            {
                std::atomic<bool> busy { false };
                SessionJobRunner jobs (busy);
                expect (jobs.start ([] (const std::atomic<bool>&) -> Completion
                { throw std::runtime_error ("disk vanished"); }));
                expect (pumpUntil ([&busy] { return ! busy.load(); }));
            }

            beginTest ("The completion runs on the message thread after the release");
            {
                std::atomic<bool> busy { false };
                SessionJobRunner jobs (busy);
                bool ran = false, busyDuringCompletion = true, onMessageThread = false;
                expect (jobs.start ([&] (const std::atomic<bool>&) -> Completion
                {
                    return [&]
                    {
                        ran = true;
                        busyDuringCompletion = busy.load();
                        onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
                    };
                }));
                expect (pumpUntil ([&ran] { return ran; }));
                expect (! busyDuringCompletion,
                        "a completion that opens a session or starts the next job needs the reservation free");
                expect (onMessageThread);
            }

            beginTest ("A held reservation refuses a second job until released");
            {
                std::atomic<bool> busy { false };
                SessionJobRunner jobs (busy);
                expect (jobs.reserve());
                bool ran = false;
                expect (! jobs.start ([&ran] (const std::atomic<bool>&) -> Completion { ran = true; return {}; }));
                expect (! jobs.reserve());
                jobs.release();
                expect (! busy.load());
                expect (jobs.reserve());
                jobs.release();
                pumpFor (20);
                expect (! ran, "a refused job must never run");
            }

            beginTest ("A completion still queued when the runner is destroyed is dropped");
            {
                std::atomic<bool> busy { false };
                bool ran = false;
                {
                    SessionJobRunner jobs (busy);
                    expect (jobs.start ([&ran] (const std::atomic<bool>&) -> Completion
                    { return [&ran] { ran = true; }; }));
                    jobs.cancelAndJoin();   // posted, not yet delivered
                }
                pumpFor (50);
                expect (! ran, "the host is gone; its completion must not run");
            }

            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("A stereo-mix render holds no structure lock between windows and survives strips removed mid-render");
            {
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-mix-snapshot-" + juce::String (juce::Random::getSystemRandom().nextInt()));
                const auto audio = dir.getChildFile ("Audio Files");
                constexpr juce::int64 len = 200000;   // four 64k-sample windows
                for (int i = 1; i <= 4; ++i)
                    expect (writeWav (audio.getChildFile (juce::String::formatted ("Track_%02d.wav", i)), len, 0.1f));

                {
                    AudioEngine eng;
                    eng.clearAllStripOverrides();
                    eng.prepareForTests (48000.0, 512);
                    eng.setActiveSessionDir (dir);
                    expect (eng.loadSession (dir) >= 4);
                    eng.setStripCount (4);

                    juce::WaitableEvent inFirstWindow, resume;
                    std::atomic<int> windows { 0 };
                    bool ok = false;
                    std::thread render ([&]
                    {
                        ok = eng.forEachStereoMixWindow (len, [&] (const juce::AudioBuffer<float>&, juce::int64, int)
                        {
                            if (windows.fetch_add (1) == 0)
                            {
                                inFirstWindow.signal();
                                resume.wait (5000);
                            }
                            return true;
                        });
                    });
                    expect (inFirstWindow.wait (5000), "the render never reached its first window");
                    {
                        const juce::ScopedTryLock probe (eng.getRecorder().getStructureLock());
                        expect (probe.isLocked(),
                                "the render must not hold the structure lock while it renders windows");
                    }
                    // Frees TrackStates 1..3 while the render is mid-flight. With
                    // the old per-track TrackState pointers this is a use-after-free.
                    eng.setStripCount (1);
                    resume.signal();
                    render.join();
                    expect (ok, "the render must complete from its snapshot");
                    expectEquals (windows.load(), 4);
                    eng.clearAllStripOverrides();
                }
                dir.deleteRecursively();
            }

            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static SessionJobsTests sessionJobsTests;
}
