#include "../Audio/AudioEngine.h"
#include "../Audio/NoiseAnalyzer.h"
#include "../Audio/SettingsFile.h"
#include "../Audio/SpectralClassifier.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Capture/CaptureDaemon.h"
#include <thread>

namespace zynforge
{
class ReviewCaptureRepairTests final : public juce::UnitTest
{
public:
    ReviewCaptureRepairTests() : UnitTest ("Review capture repairs", "zynforge") {}

    static juce::File temporaryDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-review-capture-" + juce::Uuid().toString());
    }

    void testGapOrder (bool stereo, bool unequal = false)
    {
        beginTest (unequal ? "Unequal stereo FIFO drops preserve both channels' capture timeline"
                         : stereo ? "Stereo overflow silence follows all audio queued before the gap"
                         : "Mono overflow silence follows all audio queued before the gap");
        const auto dir = temporaryDirectory();
        const juce::ScopeGuard cleanup { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); } };
        MultitrackRecorder recorder;
        const int channels = stereo ? 2 : 1;
        recorder.prepare (48000, 16, channels);
        for (int c = 0; c < channels; ++c)
        {
            recorder.getTrack (c).armed.store (true);
            recorder.fifos[(size_t) c]->resize (unequal && c == 1 ? 12 : 16);
        }
        recorder.getTrack (0).isStereo.store (stereo);
        expect (recorder.startRecording (dir));
        if (! recorder.isRecording()) return;
        // Deterministic consumer scheduling; production writer objects and
        // processBlock are unchanged, only automatic scheduling is detached.
        for (auto& shard : recorder.shards)
            for (auto& thread : recorder.writerThreads)
                thread->removeTimeSliceClient (shard.get());
        auto push = [&] (float value, int count)
        {
            float block[16]; std::fill (std::begin (block), std::end (block), value);
            const float* inputs[] { block, block };
            recorder.processBlock (inputs, channels, count);
        };
        push (0.25f, 4); // A: consumer reserves this first.
        bool injected = false;
        recorder.afterFifoReservationForTests = [&]
        {
            if (injected) return;
            injected = true;
            push (0.5f, 11); // B fits behind the reserved A.
            push (0.75f, 4); // C overflows while A's disk write is in flight.
        };
        recorder.drainPendingForTests();
        push (-0.25f, 4); // D must remain AFTER the missing C on each timeline.
        recorder.drainPendingForTests();
        recorder.afterFifoReservationForTests = {};
        expect (injected);
        expectEquals (recorder.getMissedSamples(), (juce::int64) (4 * channels + (unequal ? 4 : 0)));
        recorder.stopRecording();
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (
            formats.createReaderFor (dir.getChildFile ("Audio Files/Track_01.wav")));
        expect (reader != nullptr);
        if (! reader) return;
        expectEquals (reader->lengthInSamples, (juce::int64) 23);
        juce::AudioBuffer<float> result (channels, 23);
        expect (reader->read (&result, 0, 23, 0, true, stereo));
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < 23; ++i)
                expectWithinAbsoluteError (result.getSample (c, i),
                    i < 4 ? 0.25f : (i < (unequal && c == 1 ? 11 : 15) ? 0.5f : (i < 19 ? 0.0f : -0.25f)), 0.00001f,
                    "retained audio moved around the dropout at sample " + juce::String (i));
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        testGapOrder (false);
        testGapOrder (true);
        testGapOrder (true, true);

        beginTest ("A saturated gap descriptor ring stays bounded, counts extra loss, and resumes in order");
        {
            const auto dir = temporaryDirectory();
            const juce::ScopeGuard cleanup { [&] { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); } };
            MultitrackRecorder recorder;
            recorder.prepare (48000, 16, 1);
            recorder.getTrack (0).armed.store (true);
            expect (recorder.startRecording (dir));
            for (auto& shard : recorder.shards)
                for (auto& thread : recorder.writerThreads) thread->removeTimeSliceClient (shard.get());
            auto& fifo = *recorder.fifos[0];
            // Construct the valid worst-case queue state directly: 256 retained
            // one-frame spans alternating with 256 one-frame dropouts. This
            // reaches metadata saturation without relying on scheduler timing.
            {
                const auto write = fifo.fifo.write ((int) fifo.gapCapacity);
                for (int i = 0; i < (int) fifo.gapCapacity; ++i)
                {
                    fifo.data[(size_t) (write.startIndex1 + i)] = 0.25f;
                    fifo.accountCapture (1, 2);
                }
            }
            recorder.missedSamples.store ((juce::int64) fifo.gapCapacity);
            recorder.samplesSinceStart.store ((juce::int64) (2 * fifo.gapCapacity));
            float audio[4] { -0.5f, -0.5f, -0.5f, -0.5f };
            const float* inputs[] { audio };
            const auto before = recorder.getMissedSamples();
            recorder.processBlock (inputs, 1, 4);
            expectEquals (recorder.getMissedSamples() - before, (juce::int64) 4);
            expectEquals ((int) fifo.gapCount, (int) fifo.gapCapacity);
            recorder.drainPendingForTests();
            expectEquals ((int) fifo.gapCount, 0);
            recorder.processBlock (inputs, 1, 4); // capacity recovered: keep D.
            recorder.stopRecording();
            juce::AudioFormatManager formats; formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (
                formats.createReaderFor (dir.getChildFile ("Audio Files/Track_01.wav")));
            expect (reader != nullptr);
            if (reader)
            {
                expectEquals (reader->lengthInSamples, (juce::int64) 520);
                juce::AudioBuffer<float> result (1, 520);
                expect (reader->read (&result, 0, 520, 0, true, false));
                for (int i = 0; i < 520; ++i)
                    expectWithinAbsoluteError (result.getSample (0, i),
                        i < 512 ? (i % 2 == 0 ? 0.25f : 0.0f) : (i < 516 ? 0.0f : -0.5f), 0.00001f);
            }
            reader.reset();
            recorder.armContinue (520);
            expect (recorder.startRecording (dir));
            expectEquals (fifo.producedFrames, (juce::int64) 0);
            expectEquals (fifo.consumedFrames, (juce::int64) 0);
            expectEquals ((int) fifo.gapCount, 0);
            recorder.processBlock (inputs, 1, 4);
            recorder.stopRecording();
            expectEquals (recorder.getMissedSamples(), (juce::int64) 0);
            std::unique_ptr<juce::AudioFormatReader> continued (
                formats.createReaderFor (dir.getChildFile ("Audio Files/Track_01_part02.wav")));
            expect (continued != nullptr);
            if (continued) expectEquals (continued->lengthInSamples, (juce::int64) 4);
        }

        for (int destination = 0; destination < 3; ++destination)
        {
            beginTest ("Capture reports float-AIFF final header failure for destination " + juce::String (destination));
            const auto root = temporaryDirectory();
            const auto dir = root.getChildFile ("session");
            const auto backup = root.getChildFile ("backup");
            const auto mirror = root.getChildFile ("mirror");
            expect (backup.createDirectory().wasOk()); expect (mirror.createDirectory().wasOk());
            const juce::ScopeGuard cleanup { [&]
            { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); root.deleteRecursively(); } };
            auto fail = std::make_shared<std::atomic<bool>> (false);
            struct HeaderFailureStream final : juce::OutputStream
            {
                explicit HeaderFailureStream (std::shared_ptr<std::atomic<bool>> state) : failure (std::move (state)) {}
                bool setPosition (juce::int64 position) override
                { return ! (position == 0 && failure->load()) && storage.setPosition (position); }
                juce::int64 getPosition() override { return storage.getPosition(); }
                bool write (const void* data, size_t size) override { return storage.write (data, size); }
                void flush() override {}
                juce::MemoryOutputStream storage;
                std::shared_ptr<std::atomic<bool>> failure;
            };
            MultitrackRecorder recorder;
            recorder.prepare (48000, 16, 1);
            recorder.setCaptureFormat (CaptureFormat::Aiff32Float);
            recorder.setBackupCaptureFormat (CaptureFormat::Aiff32Float);
            recorder.setBackupDirectory (backup);
            expect (recorder.setMirrors ({ { mirror, CaptureFormat::Aiff32Float } }));
            recorder.getTrack (0).armed.store (true);
            expect (recorder.startRecording (dir));
            if (! recorder.isRecording()) continue;
            for (auto& shard : recorder.shards)
                for (auto& thread : recorder.writerThreads) thread->removeTimeSliceClient (shard.get());
            auto* writer = FloatAiffWriter::create (new HeaderFailureStream (fail), 48000, 1);
            expect (writer != nullptr);
            auto& channel = recorder.writers[0];
            if (destination == 0) channel.writer.reset (writer);
            else if (destination == 1) channel.backupWriter.reset (writer);
            else
            {
                expectEquals ((int) channel.mirrors.size(), 1);
                if (channel.mirrors.empty()) { delete writer; continue; }
                channel.mirrors[0].writer.reset (writer);
            }
            float data[4] { 0.25f, 0.5f, -0.25f, -0.5f };
            const float* inputs[] { data };
            recorder.processBlock (inputs, 1, 4);
            recorder.drainPendingForTests();
            fail->store (true);
            recorder.stopRecording();
            const auto key = destination == 0 ? "primaryFailed" : destination == 1 ? "backupFailed" : "mirrorFailed";
            expect (destination == 0 ? recorder.hasPrimaryFailed()
                     : destination == 1 ? recorder.hasBackupFailed() : recorder.anyMirrorFailed(),
                    "STOP discarded the final header failure");
            const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
            expect ((bool) report[key], "capture report omitted the final header failure");
        }

        beginTest ("Session-state reset batches settings and preserves unrelated preferences");
        {
            AudioEngine engine;
            engine.setStripCount (27);
            engine.setTrackName (0, "old"); engine.setTrackGainDb (0, -17.0f);
            engine.setTrackPan (0, 0.7f); engine.setTrackInputRouting (0, 9);
            engine.setTrackOutputRouting (0, 7);
            auto* props = engine.getAppProps();
            props->setValue ("review_unknown_preference", "preserve");
            props->setValue ("strip_gain_40", -21.0);
            expect (props->saveIfNeeded());
            const auto before = settingsReloadCountForTests.load();
            engine.resetAllStripState();
            const auto reloads = settingsReloadCountForTests.load() - before;
            logMessage ("Session-state reset reloads: " + juce::String (reloads));
            expect (reloads <= 5, "session reset still performs per-field XML I/O");
            props->reload();
            expectEquals (props->getValue ("review_unknown_preference"), juce::String ("preserve"));
            expectWithinAbsoluteError (props->getDoubleValue ("strip_gain_40"), -21.0, 0.0001);
            const auto& track = engine.getRecorder().getTrack (0);
            expectWithinAbsoluteError (track.gainDb.load(), 0.0f, 0.0001f);
            expectWithinAbsoluteError (track.pan.load(), 0.0f, 0.0001f);
            expectEquals (track.inputRouting.load(), 0);
            expectEquals (track.outputRouting.load(), -1);
            expectEquals (track.getNameThreadSafe(), juce::String ("1"));
        }

        beginTest ("Noise analysis includes a native stereo recording's right channel");
        {
            const auto dir = temporaryDirectory();
            expect (dir.createDirectory().wasOk());
            const juce::ScopeGuard cleanup { [&] { dir.deleteRecursively(); } };
            const auto file = dir.getChildFile ("Track_01.wav");
            juce::AudioBuffer<float> audio (2, 96000); audio.clear();
            for (int i = 0; i < audio.getNumSamples(); ++i)
                audio.setSample (1, i, (i % 2 == 0) ? 0.25f : -0.25f);
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
                new juce::FileOutputStream (file), 48000, 2, 24, {}, 0));
            expect (writer != nullptr);
            if (writer) expect (writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()));
            writer.reset();
            const auto finding = NoiseAnalyzer::analyseFile (file, 0, "Stereo");
            expect (finding.noiseFloorDbFS > -30.0f, "right-channel noise was omitted");
            expect (! NoiseAnalyzer::summaryLine (finding).contains (": clean"));
        }

        beginTest ("Unreadable noise-analysis source is reported as a failure, never clean");
        {
            const auto dir = temporaryDirectory();
            expect (dir.createDirectory().wasOk());
            const juce::ScopeGuard cleanup { [&] { dir.deleteRecursively(); } };
            const auto file = dir.getChildFile ("Track_01.wav");
            expect (file.replaceWithText ("not an audio file"));
            const auto finding = NoiseAnalyzer::analyseFile (file, 0, "Broken");
            expect (! NoiseAnalyzer::summaryLine (finding).contains (": clean"),
                    "invalid media was represented as a successful clean analysis");
            const auto findings = NoiseAnalyzer::analyseSession (dir, {});
            expectEquals ((int) findings.size(), 1);
            const auto report = juce::JSON::parse (dir.getChildFile ("noise_report.json"));
            const auto* rows = report["tracks"].getArray();
            expect (rows != nullptr && rows->size() == 1);
            if (rows != nullptr && rows->size() == 1)
                expect ((*rows)[0]["error"].toString().isNotEmpty(), "persisted report omitted analysis failure");
        }

        beginTest ("Spectral classification reads only a published FFT snapshot");
        {
            TrackState track;
            track.peak.store (0.5f);
            std::fill (track.fftSnapshot.begin(), track.fftSnapshot.end(), 0.5f);
            track.fftBlockReady.store (false);
            expectEquals (SpectralClassifier::classify (track, 48000).name, juce::String ("other"),
                          "classifier read a snapshot owned by the producer");
            track.fftBlockReady.store (true, std::memory_order_release);
            expect (SpectralClassifier::classify (track, 48000).name != "other");
            expect (track.fftBlockReady.load(), "classification must leave publication available to the spectrum display");
        }

        beginTest ("Noise-analysis read failure is distinct from silence");
        {
            struct FailedReader final : juce::AudioFormatReader
            {
                FailedReader() : AudioFormatReader (nullptr, "failed-read fixture")
                { sampleRate = 48000; numChannels = 1; bitsPerSample = 32; usesFloatingPointData = true; lengthInSamples = 96000; }
                bool readSamples (int* const*, int, int, juce::int64, int) override { return false; }
            } reader;
            const auto finding = NoiseAnalyzer::analyseReader (&reader, 0, "Failed read");
            expect (! NoiseAnalyzer::summaryLine (finding).contains (": clean"));
        }

        beginTest ("FFT classification and audio publication remain race-free under repeated consumption");
        {
            MultitrackRecorder recorder;
            recorder.prepare (48000, 256, 1);
            recorder.getTrack (0).monitor.store (true);
            std::atomic<bool> stop { false };
            std::thread producer ([&]
            {
                float block[256]; std::fill (std::begin (block), std::end (block), 0.25f);
                const float* inputs[] { block };
                while (! stop.load (std::memory_order_acquire))
                    recorder.processBlock (inputs, 1, 256);
            });
            auto& track = recorder.getTrack (0);
            for (int i = 0; i < 2000; ++i)
            {
                const auto result = SpectralClassifier::classify (track, 48000);
                expect (std::isfinite (result.bands.sub));
                track.fftBlockReady.store (false, std::memory_order_release);
            }
            stop.store (true, std::memory_order_release);
            producer.join();
        }
    }
};
static ReviewCaptureRepairTests reviewCaptureRepairTests;
}

namespace zynforge::capture
{
class ReviewCaptureStartupTests final : public juce::UnitTest
{
public:
    ReviewCaptureStartupTests() : UnitTest ("Review capture startup", "zynforge") {}
    void runTest() override
    {
        beginTest ("Requested track storage exists before the first callback becomes callable");
        CaptureDaemon daemon;
        daemon.setTestModeNoDevice (true);
        int tracksAtRegistration = -1;
        daemon.registerCallbackForTests = [&]
        {
            tracksAtRegistration = daemon.getRecorder().getNumTracks();
            // Simulate a default mono device starting its callback. The
            // production callback preserves an already-created logical count.
            daemon.prepareForTests (48000, 64, tracksAtRegistration > 0 ? tracksAtRegistration : 1);
        };
        expect (daemon.start (0, 32));
        expectEquals (tracksAtRegistration, 32,
                      "callback was enabled before the requested track vector was stable");
        expectEquals (daemon.getRecorder().getNumTracks(), 32);
        daemon.stop();
    }
};
static ReviewCaptureStartupTests reviewCaptureStartupTests;
}
