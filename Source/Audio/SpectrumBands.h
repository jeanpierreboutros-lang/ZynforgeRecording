#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_core/juce_core.h>
#include "TrackState.h"

namespace zynforge::spectrumbands
{
    // The soundcheck report's five broad bands, from the strip's latest
    // TrackState::fftSnapshot:
    //   sub  20-80    Hz
    //   low  80-250   Hz
    //   mid  250-2k   Hz
    //   high 2k-6k    Hz
    //   air  6k-20k   Hz
    struct Bands { float sub{}, low{}, mid{}, high{}, air{}; };

    // Message thread. A published snapshot stays still until ChannelStrip's
    // timer, on this same thread, releases it. Never read the producer-owned
    // false state. Before the device starts sampleRate is 0, and binHz would
    // collapse every bin to 0 Hz.
    inline Bands measure (const TrackState& t, double sampleRate)
    {
        if (sampleRate <= 0.0 || ! t.fftBlockReady.load (std::memory_order_acquire))
            return {};

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

        Bands b;
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
        return b;
    }
}
