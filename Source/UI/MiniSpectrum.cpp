#include "MiniSpectrum.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    MiniSpectrum::MiniSpectrum (TrackState& s) : state (s)
    {
        startTimerHz (24);
    }

    MiniSpectrum::~MiniSpectrum() = default;

    void MiniSpectrum::timerCallback()
    {
        if (! state.fftBlockReady.load (std::memory_order_acquire)) return;

        // Copy snapshot into the front half, zero the back half.
        std::memcpy (fftData.data(), state.fftSnapshot.data(),
                     sizeof (float) * kFftSize);
        std::memset (fftData.data() + kFftSize, 0,
                     sizeof (float) * kFftSize);

        // Now safe for audio thread to start filling a new snapshot.
        state.fftBlockReady.store (false, std::memory_order_release);

        window.multiplyWithWindowingTable (fftData.data(), kFftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        // Reduce to log-spaced visible bins. Bin 0 is DC; useful range is
        // 1..kFftSize/2.
        const int half = kFftSize / 2;
        for (int b = 0; b < kVisibleBins; ++b)
        {
            const float t0 = (float) b       / (float) kVisibleBins;
            const float t1 = (float) (b + 1) / (float) kVisibleBins;
            const int lo = juce::jmax (1,    (int) std::pow ((float) half, t0));
            const int hi = juce::jmin (half, (int) std::pow ((float) half, t1));
            float peak = 0.0f;
            for (int i = lo; i < hi; ++i) peak = juce::jmax (peak, fftData[(std::size_t) i]);

            const float dbNorm = juce::jlimit (0.0f, 1.0f,
                                  (juce::Decibels::gainToDecibels (peak, -80.0f) + 80.0f) / 80.0f);
            bins[(std::size_t) b] = juce::jmax (dbNorm, bins[(std::size_t) b] * 0.78f);
        }
        repaint();
    }

    void MiniSpectrum::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (brand::bgDeep);
        g.fillRoundedRectangle (r, 2.0f);

        if (r.getWidth() < 2.0f || r.getHeight() < 4.0f) return;

        const float w = r.getWidth() / (float) kVisibleBins;
        for (int i = 0; i < kVisibleBins; ++i)
        {
            const float v = juce::jlimit (0.0f, 1.0f, bins[(std::size_t) i]);
            const float h = juce::jmax (1.0f, r.getHeight() * v);
            juce::Rectangle<float> bar (r.getX() + i * w + 0.5f,
                                         r.getBottom() - h,
                                         juce::jmax (1.0f, w - 1.0f),
                                         h);
            const float t = (float) i / (float) (kVisibleBins - 1);
            const auto base = (t < 0.6f ? brand::meterGreen
                              : t < 0.85f ? brand::meterAmber
                                          : brand::meterRed);
            g.setColour (base.withAlpha (brand::alpha::prominent));
            g.fillRect (bar);
        }
    }
}
