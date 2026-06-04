#include "MiniSpectrum.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    MiniSpectrum::MiniSpectrum (TrackState& s) : state (s)
    {
        startTimerHz (24);
    }

    MiniSpectrum::~MiniSpectrum() = default;

    void MiniSpectrum::timerCallback()
    {
        const bool blockReady = state.fftBlockReady.load (std::memory_order_acquire);

        // Idle-CPU gate. The audio callback keeps producing FFT snapshots
        // every block (even of silence) while the device runs, so without
        // this every strip would run a 1024-pt FFT + repaint at 24 Hz
        // forever -- the source of the high idle CPU. When the strip has no
        // meaningful signal we skip the (expensive) FFT entirely: drain any
        // ready block cheaply so the audio thread keeps cycling, decay the
        // bars toward zero, and repaint only while something is still
        // visible. Once the display reaches zero the component goes fully
        // idle until signal returns. `peak` is driven by BOTH live input
        // and playback, so this covers record and playback alike.
        constexpr float kSilenceFloor = 3.0e-4f;   // ~ -70 dBFS
        const float peak = state.peak.load (std::memory_order_relaxed);

        if (peak <= kSilenceFloor)
        {
            if (blockReady)                          // drain without hashing the FFT
                state.fftBlockReady.store (false, std::memory_order_release);

            bool anyVisible = false;
            for (auto& b : bins)
            {
                b *= 0.78f;
                if (b > 1.0e-3f) anyVisible = true;
                else             b = 0.0f;
            }
            if (displayActive)                       // keep painting the fade-out
            {
                displayActive = anyVisible;
                repaint();
            }
            return;
        }

        if (! blockReady) return;

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
        displayActive = true;
        repaint();
    }

    void MiniSpectrum::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.10f, 0.16f));
        g.fillRoundedRectangle (r, brand::radius::sm);

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
