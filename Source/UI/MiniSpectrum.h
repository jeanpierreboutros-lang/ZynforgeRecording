#pragma once

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

#include "../Audio/TrackState.h"

namespace zynforge
{
    // Small log-frequency magnitude bar display fed from TrackState's
    // FFT FIFO. UI thread polls fftBlockReady, runs the FFT, reduces to
    // visible bins, repaints.
    class MiniSpectrum final : public juce::Component,
                               public juce::SettableTooltipClient,
                               private juce::Timer
    {
    public:
        explicit MiniSpectrum (TrackState& s);
        ~MiniSpectrum() override;

        void paint (juce::Graphics&) override;

        // Stop reading the (about-to-be-freed) TrackState + its FFT snapshot.
        // Called from ChannelStrip::invalidate() when the owning strip is
        // condemned but not yet destroyed. paint() reads only `bins`, so it
        // stays safe after detach.
        void detach() noexcept { detached = true; stopTimer(); }

    private:
        void timerCallback() override;

        static constexpr int kVisibleBins = 28;
        static constexpr int kFftOrder    = 10;                // 2^10 = 1024
        static constexpr int kFftSize     = 1 << kFftOrder;

        TrackState& state;

        juce::dsp::FFT                            fft   { kFftOrder };
        juce::dsp::WindowingFunction<float>       window { kFftSize, juce::dsp::WindowingFunction<float>::hann };
        std::array<float, kFftSize * 2>           fftData {};
        std::array<float, kVisibleBins>           bins   {};

        // True while anything is still visible. Lets the component go fully
        // idle (no FFT, no repaint) once the bars have decayed to zero on a
        // silent strip -- this is the per-strip idle-CPU saver.
        bool displayActive { false };
        bool detached      { false };
    };
}
