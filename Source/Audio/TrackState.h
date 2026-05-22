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
        std::atomic<bool>  muted   { false };
        std::atomic<bool>  soloed  { false };

        // Playback / monitor only. Recording is always pre-fader.
        std::atomic<float> gainDb  { 0.0f };           // -60 .. +12
        std::atomic<float> pan     { 0.0f };           // -1 = L, 0 = C, +1 = R

        // Routing. -1 = unrouted (no input captured / no output played).
        // Default of -2 means "use identity routing" (resolved at init).
        std::atomic<int> inputRouting  { -2 };
        std::atomic<int> outputRouting { -2 };

        std::atomic<int>          clipCount        { 0 };
        std::atomic<juce::int64>  lastClipSample   { -1 };

        // 0 means "use default personality colour for this index".
        std::atomic<juce::uint32> colourARGB       { 0 };

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
