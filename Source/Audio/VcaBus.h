#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

namespace zynforge
{
    // VCA bus — a "ghost fader" the engineer can ride to scale a group
    // of strips at once. Strips assign themselves to a VCA via
    // TrackState::vcaGroup; the audio thread sums vca[group].gainDb
    // into the per-strip gain at the monitor / output / stream summing
    // stages. The VCA's muted flag gates every assigned strip's
    // output.
    //
    // Soft-takeover (rampSamplesRemaining) lets the audio thread step
    // gainDb toward rampTarget over a fixed duration, so a cue jump or
    // a fader move doesn't click.
    struct VcaBus
    {
        std::atomic<float>  gainDb           { 0.0f };   // -60..+12
        std::atomic<bool>   muted            { false };
        std::atomic<bool>   soloed           { false };
        std::atomic<juce::uint32> colourARGB { 0 };

        std::atomic<float>       rampTargetGainDb     { 0.0f };
        std::atomic<juce::int64> rampSamplesRemaining { 0 };

        juce::String name;
    };
}
