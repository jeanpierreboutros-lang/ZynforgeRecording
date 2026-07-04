#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
#include "TrackState.h"

namespace zynforge
{
    // Lightweight on-device channel classifier. Runs an FFT over the
    // most recent TrackState::fftSnapshot (already populated by the
    // audio thread) and buckets the energy into 5 broad bands:
    //   sub  20-80    Hz   (kick, bass)
    //   low  80-250   Hz   (snare bottom, low toms)
    //   mid  250-2k   Hz   (vocal, guitar body, snare top)
    //   high 2k-6k    Hz   (cymbal pickup, vocal sibilance)
    //   air  6k-20k   Hz   (hi-hat, cymbal shimmer)
    //
    // Heuristic → guess label (kick / bass / snare / vocal / guitar /
    // hat / cymbal / keys / other). Not a CoreML model -- but good
    // enough to seed the engineer's channel names during soundcheck.
    class SpectralClassifier
    {
    public:
        struct EnergyBands { float sub{}, low{}, mid{}, high{}, air{}; float peak{}, rms{}; };
        struct Result      { juce::String name; EnergyBands bands; };

        // Returns the best-guess label + raw band energies for the
        // strip, given its FFT snapshot + the device sample rate.
        static Result classify (const TrackState& t, double sampleRate)
        {
            // Before the device starts sampleRate is 0; binHz would collapse
            // every bin to 0 Hz, dumping all energy into b.sub and forcing a
            // bogus Kick/Bass guess. Return a neutral "other" instead.
            if (sampleRate <= 0.0)
                return { "other", {} };

            // Window + FFT in place using a temp buffer (kFftSize is 1024).
            constexpr int kSize = TrackState::kFftSize;
            const int order = 10;   // 2^10 = 1024
            juce::dsp::FFT fft (order);

            std::vector<float> buf (kSize * 2, 0.0f);
            for (int i = 0; i < kSize; ++i)
            {
                // Hann window
                const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi
                                                        * (float) i / (float) (kSize - 1));
                buf[(size_t) i] = t.fftSnapshot[(size_t) i] * w;
            }
            fft.performFrequencyOnlyForwardTransform (buf.data());

            EnergyBands b{};
            const double binHz = sampleRate / (double) kSize;
            for (int k = 0; k < kSize / 2; ++k)
            {
                const float hz  = (float) (k * binHz);
                const float mag = buf[(size_t) k];
                if      (hz <   80.0f) b.sub  += mag;
                else if (hz <  250.0f) b.low  += mag;
                else if (hz < 2000.0f) b.mid  += mag;
                else if (hz < 6000.0f) b.high += mag;
                else                   b.air  += mag;
            }
            b.peak = t.peak.load (std::memory_order_relaxed);
            b.rms  = t.rms .load (std::memory_order_relaxed);

            // Tiny rule engine. Returns 'other' when nothing matches --
            // engineer can confirm / override.
            const float total = b.sub + b.low + b.mid + b.high + b.air + 1e-6f;
            const float subR  = b.sub  / total;
            const float lowR  = b.low  / total;
            const float midR  = b.mid  / total;
            const float highR = b.high / total;
            const float airR  = b.air  / total;

            juce::String guess = "other";
            if (subR > 0.50f && b.peak > 0.05f)              guess = "Kick";
            else if (subR + lowR > 0.55f && midR < 0.20f)    guess = "Bass";
            else if (lowR > 0.30f && midR > 0.20f
                     && b.peak > 0.10f)                       guess = "Snare";
            else if (airR > 0.45f)                            guess = "Hi-hat";
            else if (highR + airR > 0.50f && b.peak < 0.30f)  guess = "Cymbal";
            else if (midR > 0.45f && airR < 0.20f)            guess = "Vocal";
            else if (midR > 0.30f && highR > 0.20f)           guess = "Guitar";
            else if (lowR + midR > 0.55f && airR < 0.10f)     guess = "Keys";

            return { guess, b };
        }
    };
}
