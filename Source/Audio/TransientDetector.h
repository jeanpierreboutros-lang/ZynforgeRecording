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

            const auto shortN = (juce::int64) (p.shortWinSec * sampleRate);
            const auto longN  = (juce::int64) (p.longWinSec  * sampleRate);
            const auto hopN   = (juce::int64) std::max ((juce::int64) 1, (juce::int64) (p.hopSec * sampleRate));
            const auto refractoryN = (juce::int64) (p.minSeparationSec * sampleRate);

            if (longN >= numSamples) return onsets;

            auto rms = [samples, numSamples] (juce::int64 start, juce::int64 n) -> double
            {
                if (start < 0 || start + n > numSamples) return 0.0;
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
            if (n < 1024) return onsets;

            juce::AudioBuffer<float> buf (1, (int) juce::jmin ((juce::int64) (sr * 1800.0), n));
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, false);
            return detect (buf.getReadPointer (0), buf.getNumSamples(), sr);
        }
    };
}
