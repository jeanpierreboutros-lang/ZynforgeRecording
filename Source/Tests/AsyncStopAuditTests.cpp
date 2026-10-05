#include "../Audio/AudioEngine.h"

#include <atomic>
#include <cmath>
#include <memory>
#include <thread>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
    class AsyncStopAuditTests final : public juce::UnitTest
    {
    public:
        AsyncStopAuditTests() : UnitTest ("Async STOP telemetry ownership", "zynforge") {}

        struct Gate
        {
            juce::WaitableEvent entered, release, pollerStarted;
            std::atomic<bool> stopPolling { false }, completed { false };
            std::atomic<int> snapshots { 0 }, malformed { 0 };
            bool completionOk = false; // message-thread only
        };

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
               #else
                juce::Thread::sleep (5);
               #endif
            }
            return predicate();
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            for (int pass = 0; pass < 3; ++pass)
            {
                beginTest ("Companion status and GUI disk metrics remain safe through mirrored asynchronous STOP "
                           + juce::String (pass + 1));
                const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-async-stop-metrics-" + juce::Uuid().toString());
                const auto session = root.getChildFile ("session");
                const auto backup = root.getChildFile ("backup");
                const auto mirror = root.getChildFile ("mirror");
                const juce::ScopeGuard removeFixture { [&]
                {
                    MultitrackRecorder::deleteSessionAfterCancellingReports (session);
                    root.deleteRecursively();
                } };
                expect (session.createDirectory().wasOk());
                expect (backup.createDirectory().wasOk());
                expect (mirror.createDirectory().wasOk());
                AudioEngine engine;
                engine.clearAllStripOverrides();
                const juce::ScopeGuard clearOverrides { [&] { engine.clearAllStripOverrides(); } };
                constexpr int channels = 27, block = 256;
                engine.setStripCount (channels);
                engine.prepareForTests (48000, block);
                auto& recorder = engine.getRecorder();
                recorder.setBackupDirectory (backup);
                expect (recorder.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                std::vector<float> audio ((size_t) channels * block, 0.125f);
                std::vector<const float*> input;
                for (int i = 0; i < channels; ++i)
                {
                    recorder.getTrack (i).armed.store (true);
                    input.push_back (audio.data() + (size_t) i * block);
                }
                const bool started = engine.startRecording (session);
                expect (started);
                if (! started) continue;
                expect (recorder.isBackupActive());
                expectEquals (recorder.getMirrorsSkippedAtStart(), 0);
                for (int i = 0; i < 8; ++i) recorder.processBlock (input.data(), channels, block);

                auto gate = std::make_shared<Gate>();
                recorder.beforeFinalizationForTests = [gate]
                {
                    gate->entered.signal();
                    gate->release.wait (5000);
                };
                const juce::ScopeGuard releaseWorker { [gate] { gate->release.signal(); } };
                const bool accepted = engine.stopRecordingAsync ([gate] (bool ok)
                {
                    gate->completionOk = ok;
                    gate->completed.store (true, std::memory_order_release);
                });
                expect (accepted);
                if (! accepted) continue;
                const bool entered = gate->entered.wait (2000);
                expect (entered, "STOP never reached the finalization ownership gate");
                if (! entered) continue;
                expect (! recorder.isRecording());
                expect (engine.isCaptureFinalizing());

                // Start real status/telemetry reads while the STOP worker is
                // held at its existing scheduling seam. The start event is
                // signalled BEFORE the first read: there is deliberately no
                // test-created happens-before edge ordering subsequent reads
                // against the worker's writer/shard retirement. TSan therefore
                // checks production ownership, not an artificial test lock.
                std::thread observer ([&engine, gate]
                {
                    gate->pollerStarted.signal();
                    while (! gate->stopPolling.load (std::memory_order_acquire))
                    {
                        const auto status = engine.captureStatus();
                        const auto disk = engine.getDiskMBPerSec();
                        const auto ring = engine.getRingFillPct();
                        const bool mirrorFailure = engine.getRecorder().anyMirrorFailed();
                        if (status.numTracks != channels || (int) status.tracks.size() != channels
                            || ! std::isfinite (disk) || ! std::isfinite (ring)
                            || status.primaryFailed || status.backupFailed || status.mirrorFailed || mirrorFailure)
                            gate->malformed.fetch_add (1, std::memory_order_relaxed);
                        gate->snapshots.fetch_add (1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                });
                const juce::ScopeGuard stopObserver { [&]
                {
                    gate->stopPolling.store (true, std::memory_order_release);
                    if (observer.joinable()) observer.join();
                } };
                expect (gate->pollerStarted.wait (1000));
                gate->release.signal();
                expect (pumpUntil ([gate] { return gate->completed.load (std::memory_order_acquire); }, 10000),
                        "mirrored STOP did not complete while telemetry was being polled");
                gate->stopPolling.store (true, std::memory_order_release);
                observer.join();
                expect (gate->snapshots.load() > 0, "fixture did not exercise live telemetry");
                expectEquals (gate->malformed.load(), 0);
                expect (gate->completionOk);
                expect (! engine.isCaptureFinalizing());
                expect (engine.getPlayer().isLoaded());
                expectEquals (engine.getPlayer().getTotalLengthSamples(), (juce::int64) (8 * block));
            }
        }
    };
    static AsyncStopAuditTests asyncStopAuditTests;
}
