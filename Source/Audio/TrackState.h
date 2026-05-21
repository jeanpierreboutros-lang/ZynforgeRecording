#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

namespace zynforge
{
    struct TrackState
    {
        std::atomic<float> peak    { 0.0f };
        std::atomic<float> rms     { 0.0f };
        std::atomic<bool>  clipped { false };
        std::atomic<bool>  armed   { true };
        std::atomic<bool>  monitor { false };
        juce::String       name;

        void reset() noexcept
        {
            peak.store    (0.0f, std::memory_order_relaxed);
            rms.store     (0.0f, std::memory_order_relaxed);
            clipped.store (false, std::memory_order_relaxed);
        }
    };
}
