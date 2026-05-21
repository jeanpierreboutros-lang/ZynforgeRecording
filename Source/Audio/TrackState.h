#pragma once

#include <array>
#include <atomic>
#include <juce_core/juce_core.h>

namespace zynforge
{
    struct TrackState
    {
        static constexpr int kFftSize = 1024;

        std::atomic<float> peak    { 0.0f };
        std::atomic<float> rms     { 0.0f };
        std::atomic<bool>  clipped { false };
        std::atomic<bool>  armed   { true };
        std::atomic<bool>  monitor { false };

        std::atomic<int>          clipCount        { 0 };
        std::atomic<juce::int64>  lastClipSample   { -1 };

        // FFT FIFO — audio thread fills `fftFifo`, snapshots into
        // `fftSnapshot` when full and sets `fftBlockReady`. UI thread
        // reads the snapshot, FFTs it, then clears the flag.
        std::array<float, kFftSize> fftFifo     {};
        std::array<float, kFftSize> fftSnapshot {};
        std::atomic<int>            fftIndex      { 0 };
        std::atomic<bool>           fftBlockReady { false };

        juce::String       name;

        void reset() noexcept
        {
            peak.store    (0.0f, std::memory_order_relaxed);
            rms.store     (0.0f, std::memory_order_relaxed);
            clipped.store (false, std::memory_order_relaxed);
        }
    };
}
