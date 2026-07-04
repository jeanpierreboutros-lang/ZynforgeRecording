#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>
#include <algorithm>

namespace zynforge
{
    // Energy-ratio onset detector. Operates on a single mono buffer
    // (or the L channel of a stereo file); two windowed RMS values
    // are tracked at every short hop:
    //   short window: ~10 ms — instantaneous energy
    //   long  window: ~100 ms — running noise floor
    // An onset is reported when the short/long ratio crosses
    // `threshold` AND the previous hop was below it (rising edge),
    // AND we're at least `minSeparationSec` after the previous
    // onset (refractory).
    //
    // Doesn't need to be RT-safe -- callers run it on a background
    // thread (or in the test harness on a synthesised buffer).
    class TransientDetector
    {
    public:
        struct Params
        {
            double shortWinSec       { 0.010 };  // 10 ms
            double longWinSec        { 0.100 };  // 100 ms
            double hopSec            { 0.005 };  // 5 ms
            float  threshold         { 2.5f };   // short/long ratio
            double minSeparationSec  { 0.025 };  // 25 ms refractory
        };

        // Returns a sorted list of sample positions, one per detected
        // onset. Returns empty if input is too short.
        static std::vector<juce::int64> detect (const float* samples,
                                                juce::int64 numSamples,
                                                double sampleRate)
        {
            return detect (samples, numSamples, sampleRate, Params{});
        }

        static std::vector<juce::int64> detect (const float* samples,
                                                juce::int64 numSamples,
                                                double sampleRate,
                                                const Params& p)
        {
            std::vector<juce::int64> onsets;
            if (samples == nullptr || sampleRate <= 0.0 || numSamples < 1024)
                return onsets;

            // Window lengths must be >= 1 or rms() divides by zero (NaN) at
            // pathologically low sample rates (e.g. sr < 100 → shortN == 0).
            const auto shortN = std::max ((juce::int64) 1, (juce::int64) (p.shortWinSec * sampleRate));
            const auto longN  = std::max ((juce::int64) 1, (juce::int64) (p.longWinSec  * sampleRate));
            const auto hopN   = (juce::int64) std::max ((juce::int64) 1, (juce::int64) (p.hopSec * sampleRate));
            const auto refractoryN = (juce::int64) (p.minSeparationSec * sampleRate);

            if (longN >= numSamples) return onsets;

            auto rms = [samples, numSamples] (juce::int64 start, juce::int64 n) -> double
            {
                if (n <= 0 || start < 0 || start + n > numSamples) return 0.0;
                double acc = 0.0;
                for (juce::int64 i = 0; i < n; ++i)
                    acc += (double) samples[start + i] * (double) samples[start + i];
                return std::sqrt (acc / (double) n);
            };

            float prevRatio = 0.0f;
            juce::int64 lastOnsetSample = -refractoryN - 1;

            for (juce::int64 pos = longN; pos + shortN < numSamples; pos += hopN)
            {
                const double sR = rms (pos, shortN);
                const double lR = rms (pos - longN, longN);
                const float ratio = lR > 1.0e-6 ? (float) (sR / lR) : 0.0f;

                if (ratio >= p.threshold
                    && prevRatio < p.threshold
                    && (pos - lastOnsetSample) >= refractoryN)
                {
                    onsets.push_back (pos);
                    lastOnsetSample = pos;
                }
                prevRatio = ratio;
            }
            return onsets;
        }

        // Loads an audio file off-thread, runs detect on the L channel,
        // returns the sample-position list. Returns empty on failure.
        // Reads in ~60 s chunks (with enough overlap for the long window
        // + rising-edge primer) so a long track never needs more than a
        // few MB of RAM; analysis still caps at the first 30 minutes.
        static std::vector<juce::int64> detectInFile (const juce::File& f)
        {
            std::vector<juce::int64> onsets;
            if (! f.existsAsFile()) return onsets;

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
            if (reader == nullptr) return onsets;

            const auto sr = reader->sampleRate;
            const auto n  = (juce::int64) reader->lengthInSamples;
            if (n < 1024 || sr <= 0.0) return onsets;

            const Params p;
            const auto longN       = (juce::int64) (p.longWinSec  * sr);
            const auto shortN      = (juce::int64) (p.shortWinSec * sr);
            const auto hopN        = (juce::int64) std::max ((juce::int64) 1, (juce::int64) (p.hopSec * sr));
            const auto refractoryN = (juce::int64) (p.minSeparationSec * sr);

            const juce::int64 analyseLen = juce::jmin ((juce::int64) (sr * 1800.0), n);
            const juce::int64 chunkCore  = (juce::int64) (sr * 60.0);

            juce::AudioBuffer<float> buf;
            for (juce::int64 coreStart = 0; coreStart < analyseLen; coreStart += chunkCore)
            {
                // Back up by the long window plus one hop so the first core
                // hop has real history and a primed previous-ratio.
                const juce::int64 readStart = juce::jmax ((juce::int64) 0, coreStart - longN - hopN);
                const juce::int64 readEnd   = juce::jmin (analyseLen, coreStart + chunkCore + shortN);
                const int nRead = (int) (readEnd - readStart);
                if (nRead < 1024) break;

                buf.setSize (1, nRead, false, false, true);
                reader->read (&buf, 0, nRead, readStart, true, false);

                const juce::int64 coreEnd = juce::jmin (analyseLen, coreStart + chunkCore);
                for (auto pos : detect (buf.getReadPointer (0), nRead, sr, p))
                {
                    const juce::int64 global = readStart + pos;
                    if (global < coreStart || global >= coreEnd) continue;     // another chunk's core
                    if (! onsets.empty() && global - onsets.back() < refractoryN) continue;
                    onsets.push_back (global);
                }
            }
            return onsets;
        }
    };
}
