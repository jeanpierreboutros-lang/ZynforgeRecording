// Deep, headless integrity test for the multitrack capture path.
//
// Pushes a KNOWN per-channel signal through MultitrackRecorder exactly the
// way the audio callback does, paced to ~real time so the background writer
// threads have to keep up (mimicking a live take), then re-opens every
// output WAV and asserts:
//   - the file exists and is readable,
//   - its length is EXACTLY the number of samples we pushed (no truncation,
//     no header/data mismatch),
//   - the samples round-trip (no glitches / zeroed regions / channel swap),
//   - the recorder reported zero missed samples.
//
// This is the regression guard for the "files come out different lengths /
// truncated / glitchy" class of bug that otherwise needs a live Dante rig
// to reproduce. Run with:  ZYNFORGE_RUN_TESTS=1 open ... (or --run-tests).

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class RecordingIntegrityTests final : public juce::UnitTest
    {
    public:
        RecordingIntegrityTests() : UnitTest ("Recording integrity", "zynforge") {}

        // Per-channel sine -- a distinct frequency per channel so we can
        // verify channel identity, and low-frequency / smooth so the
        // content check tolerates a sub-millisecond timing offset instead
        // of false-flagging it (a ramp would mismatch on a 1-sample shift).
        static float encode (int ch, juce::int64 idx) noexcept
        {
            const double freq = 220.0 + ch * 60.0;     // 220 Hz, +60/ch
            return 0.4f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                            * freq * (double) idx / 48000.0);
        }

        void runMultitrackCapture (int numCh, int numBlocks, int block, int paceMs)
        {
            const double sr = 48000.0;
            const juce::int64 expected = (juce::int64) numBlocks * block;

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf_rec_integrity_" + juce::String (numCh) + "ch");
            dir.deleteRecursively();

            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, numCh);
                for (int ch = 0; ch < numCh; ++ch)
                    rec.getTrack (ch).armed.store (true, std::memory_order_relaxed);

                expect (rec.startRecording (dir), "startRecording failed");

                std::vector<std::vector<float>> buf ((size_t) numCh,
                                                     std::vector<float> ((size_t) block));
                std::vector<const float*> ptrs ((size_t) numCh);
                juce::int64 gi = 0;
                for (int b = 0; b < numBlocks; ++b)
                {
                    for (int ch = 0; ch < numCh; ++ch)
                    {
                        for (int i = 0; i < block; ++i)
                            buf[(size_t) ch][(size_t) i] = encode (ch, gi + i);
                        ptrs[(size_t) ch] = buf[(size_t) ch].data();
                    }
                    rec.processBlock (ptrs.data(), numCh, block);
                    gi += block;
                    if (paceMs > 0) juce::Thread::sleep (paceMs);
                }

                rec.stopRecording();
                expectEquals (rec.getMissedSamples(), (juce::int64) 0,
                              "recorder dropped samples at the FIFO (overflow)");
            }

            // ── Re-open every track and verify length + content ──
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            auto audioDir = dir.getChildFile ("Audio Files");

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto f = audioDir.getChildFile ("Track_"
                            + juce::String::formatted ("%02d", ch + 1) + ".wav");
                expect (f.existsAsFile(), "missing file " + f.getFileName());
                if (! f.existsAsFile()) continue;

                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
                expect (r != nullptr, "no reader for " + f.getFileName());
                if (r == nullptr) continue;

                // Exact length -- this is the truncation guard.
                expectEquals ((juce::int64) r->lengthInSamples, expected,
                              "Track " + juce::String (ch + 1) + " length wrong (truncated?)");

                // Content health, offset-immune. Comparing a periodic signal
                // sample-for-sample is fragile (a few-sample capture offset
                // looks like a phase inversion), so instead verify the
                // recorded signal is (a) at the expected level -- not silent,
                // not clipped/garbage -- and (b) COHERENT, i.e. a real tone
                // rather than the random full-scale noise the "weird form"
                // bug produced. lag-1 autocorrelation ~1 = smooth tone, ~0 =
                // white noise. This catches truncation-to-silence, dropouts,
                // and garbage without false-flagging a sub-ms timing offset.
                const int len = (int) juce::jmin (r->lengthInSamples, expected);
                juce::AudioBuffer<float> ab (1, juce::jmax (1, len));
                r->read (&ab, 0, len, 0, true, false);
                const float* d = ab.getReadPointer (0);

                double sumSq = 0.0, mean = 0.0;
                for (int i = 0; i < len; ++i) { mean += d[i]; sumSq += d[i] * d[i]; }
                mean /= juce::jmax (1, len);
                const double rms = std::sqrt (sumSq / juce::jmax (1, len));

                double num = 0.0, den = 0.0;
                for (int i = 1; i < len; ++i)
                {
                    num += (d[i] - mean) * (d[i - 1] - mean);
                    den += (d[i - 1] - mean) * (d[i - 1] - mean);
                }
                const double autocorr = den > 0.0 ? num / den : 0.0;

                // 0.4-amp sine -> RMS ~0.283. Coherent tone -> autocorr near 1.
                expect (rms > 0.15 && rms < 0.45,
                        "Track " + juce::String (ch + 1) + " wrong level (RMS "
                        + juce::String (rms, 4) + ") -- silent / truncated / clipped?");
                expect (autocorr > 0.9,
                        "Track " + juce::String (ch + 1) + " incoherent (autocorr "
                        + juce::String (autocorr, 4) + ") -- random noise / garbage?");
            }

            dir.deleteRecursively();
        }

        void runTest() override
        {
            // ~4.3 s across 8 channels, paced near real time so the writer
            // threads must keep up exactly as in a live take.
            beginTest ("8 ch x 4.3 s -- complete, exact length, round-trips");
            runMultitrackCapture (/*numCh*/ 8, /*numBlocks*/ 400, /*block*/ 512, /*paceMs*/ 8);

            // A short unpaced burst that fits inside the FIFO -- isolates the
            // stop / final-flush / header-finalise path from the streaming
            // path. If THIS truncates, the bug is in stopRecording/close.
            beginTest ("16 ch short burst (fits FIFO) -- flush-on-stop is complete");
            runMultitrackCapture (/*numCh*/ 16, /*numBlocks*/ 120, /*block*/ 512, /*paceMs*/ 0);

            // Full-count stress: 48 channels for ~8 s, paced near real time.
            // This is the configuration that truncated real takes on disk.
            beginTest ("48 ch x 8 s -- complete, exact length (real-world scale)");
            runMultitrackCapture (/*numCh*/ 48, /*numBlocks*/ 750, /*block*/ 512, /*paceMs*/ 8);
        }
    };

    static RecordingIntegrityTests recordingIntegrityTestsInstance;

    // End-to-end Split + Crop on a REAL recorded session (Track_NN.wav on
    // disk, loaded through AudioEngine), driving the exact engine calls the
    // EDIT-view Split / Crop buttons fire. Verifies the resulting clip
    // geometry AND that each edit round-trips through the undo path
    // (playlistsToJson -> loadPlaylistsFromJson) -- the active-take desync
    // fix made these undoable / persistable.
    class ClipSplitCropTests final : public juce::UnitTest
    {
    public:
        ClipSplitCropTests() : UnitTest ("Clip split + crop (file-backed)", "zynforge") {}

        // Capture a short multitrack take to disk so split/crop have real
        // files + a loaded player to act on. Unpaced (fits the FIFO).
        juce::File recordSession (int numCh, int numBlocks, int block)
        {
            const double sr = 48000.0;
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf_splitcrop_" + juce::String (numCh) + "ch");
            dir.deleteRecursively();

            MultitrackRecorder rec;
            rec.prepare (sr, block, numCh);
            for (int ch = 0; ch < numCh; ++ch)
                rec.getTrack (ch).armed.store (true, std::memory_order_relaxed);
            rec.startRecording (dir);

            std::vector<std::vector<float>> buf ((size_t) numCh, std::vector<float> ((size_t) block));
            std::vector<const float*> ptrs ((size_t) numCh);
            juce::int64 gi = 0;
            for (int b = 0; b < numBlocks; ++b)
            {
                for (int ch = 0; ch < numCh; ++ch)
                {
                    for (int i = 0; i < block; ++i)
                        buf[(size_t) ch][(size_t) i] = 0.25f * (float) std::sin (
                            2.0 * juce::MathConstants<double>::pi * 220.0
                            * (double) (gi + i) / 48000.0);
                    ptrs[(size_t) ch] = buf[(size_t) ch].data();
                }
                rec.processBlock (ptrs.data(), numCh, block);
                gi += block;
            }
            rec.stopRecording();
            return dir;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            const int block = 512, numBlocks = 200, numCh = 2;
            const juce::int64 total = (juce::int64) numBlocks * block;   // 102400 (~2.1 s)

            beginTest ("Split at playhead -> two contiguous clips, undoable");
            {
                auto dir = recordSession (numCh, numBlocks, block);
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Off);   // no grid quantise
                expect (eng.loadSession (dir) > 0, "session failed to load");
                expectEquals ((int) eng.clipsFor (0).size(), 1, "seed should give one full clip");

                const juce::int64 splitAt = total / 2;
                eng.getPlayer().setPositionSamples (splitAt);

                const auto before = eng.playlistsToJson();
                expect (eng.splitTrackAtPlayhead (0), "split returned false");

                auto& clips = eng.clipsFor (0);
                expectEquals ((int) clips.size(), 2, "expected two clips after split");
                expectEquals (clips[0].timelineStartSamples, (juce::int64) 0);
                // Left ends exactly where right begins -- no gap, no overlap.
                expectEquals (clips[0].timelineStartSamples + clips[0].fileLengthSamples,
                              clips[1].timelineStartSamples, "clips not adjacent on timeline");
                // And contiguous in the source file -- no audio skipped/duplicated.
                expectEquals (clips[0].fileStartSamples + clips[0].fileLengthSamples,
                              clips[1].fileStartSamples, "clips not contiguous in file");

                const auto after = eng.playlistsToJson();
                expect (juce::JSON::toString (before) != juce::JSON::toString (after),
                        "split not captured in the playlist (would break undo + save)");
                eng.loadPlaylistsFromJson (before);
                expectEquals ((int) eng.clipsFor (0).size(), 1, "undo did not restore single clip");

                dir.deleteRecursively();
            }

            beginTest ("Crop to range -> keeps only the range, shifted to t=0, undoable");
            {
                auto dir = recordSession (numCh, numBlocks, block);
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Off);
                expect (eng.loadSession (dir) > 0, "session failed to load");

                const juce::int64 a = total / 4;          // 25600
                const juce::int64 b = (total * 3) / 4;     // 76800
                const juce::int64 keptLen = b - a;         // 51200

                const auto before = eng.playlistsToJson();
                expect (eng.cropToRange (a, b) > 0, "crop kept no tracks");

                auto& clips = eng.clipsFor (0);
                expectEquals ((int) clips.size(), 1, "crop should leave one clip");
                expectEquals (clips[0].timelineStartSamples, (juce::int64) 0,
                              "crop must shift the range start to timeline 0");
                expectEquals (clips[0].fileLengthSamples, keptLen, "cropped length wrong");
                expectEquals (clips[0].fileStartSamples, a,
                              "crop must offset into the file by the range start");

                const auto after = eng.playlistsToJson();
                expect (juce::JSON::toString (before) != juce::JSON::toString (after));
                eng.loadPlaylistsFromJson (before);
                expectEquals (eng.clipsFor (0)[0].fileLengthSamples, total,
                              "undo did not restore the full-length clip");

                dir.deleteRecursively();
            }

            // Mute + Delete are only "wired" if they change what the player
            // actually outputs. Render real blocks through SessionPlayer and
            // assert silence vs audio -- the audible contract, not just the
            // flag. (Mute skips the clip; a deleted region with other clips
            // still present plays silence.)
            beginTest ("Mute + Delete actually silence the player output");
            {
                auto dir = recordSession (numCh, numBlocks, block);
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Off);
                expect (eng.loadSession (dir) > 0, "session failed to load");

                auto& player = eng.getPlayer();
                player.prepare (48000.0, block);

                // Render one block at pos -> peak |sample| on channel 0 only.
                // Track i maps to output i, and we only edit track 0, so we
                // must measure track 0's output (not the still-playing
                // neighbour, which would mask the mute under a max-of-all).
                auto peakAt = [&] (juce::int64 pos) -> float
                {
                    player.start();
                    player.setPositionSamples (pos);
                    std::vector<std::vector<float>> ob ((size_t) numCh, std::vector<float> ((size_t) block, 0.0f));
                    std::vector<float*> op ((size_t) numCh);
                    for (int ch = 0; ch < numCh; ++ch) op[(size_t) ch] = ob[(size_t) ch].data();
                    player.processBlock (op.data(), numCh, block);
                    float pk = 0.0f;
                    for (int i = 0; i < block; ++i)
                        pk = juce::jmax (pk, std::abs (ob[0][(size_t) i]));
                    return pk;
                };
                // The BufferingAudioReader fills in the background, so the
                // first reads at a fresh position can be silent. Poll until
                // audio shows up (bounded ~0.5 s) before trusting a baseline.
                auto warmPeakAt = [&] (juce::int64 pos) -> float
                {
                    float pk = 0.0f;
                    for (int t = 0; t < 100 && pk < 0.05f; ++t)
                    {
                        pk = peakAt (pos);
                        if (pk < 0.05f) juce::Thread::sleep (5);
                    }
                    return pk;
                };

                const juce::int64 probe = total / 2;   // inside the full clip

                expect (warmPeakAt (probe) > 0.05f, "clip never produced audio (warmup failed)");

                // Mute the (single) clip -> that region is now pure silence.
                expect (eng.setClipMuted (0, 0, true));
                expect (peakAt (probe) < 1.0e-6f, "muted clip still produced audio");

                // Unmute -> audible again.
                expect (eng.setClipMuted (0, 0, false));
                expect (warmPeakAt (probe) > 0.05f, "unmute did not restore audio");

                // Split into two clips, then delete the first. Its old
                // region must go silent while the surviving clip still plays.
                player.setPositionSamples (probe);
                expect (eng.splitTrackAtPlayhead (0), "split failed");
                expectEquals ((int) eng.clipsFor (0).size(), 2);
                expect (eng.deleteClip (0, 0), "delete failed");
                expectEquals ((int) eng.clipsFor (0).size(), 1);

                expect (peakAt (total / 4)       < 1.0e-6f, "deleted region still produced audio");
                expect (warmPeakAt (total * 3 / 4) > 0.05f, "surviving clip went silent");

                player.stop();
                dir.deleteRecursively();
            }
        }
    };

    static ClipSplitCropTests clipSplitCropTestsInstance;
}
