#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace zynforge::timelinecut
{
    // What a Crop or a Ripple Delete does to a position on the timeline. Audio,
    // markers and cues all follow this one rule, so
    // nothing can land on different audio after the edit.
    struct Cut
    {
        juce::int64 start { 0 }, end { 0 };
        bool crop { false };   // true: keep [start, end) and move it to zero.
                               // false: remove [start, end) and close the gap.

        // Where a position lands, or nothing when its audio is gone. A crop
        // keeps its end: a marker there still marks the end of the kept audio.
        std::optional<juce::int64> map (juce::int64 pos) const noexcept
        {
            if (crop)
            {
                if (pos < start || pos > end) return std::nullopt;
                return pos - start;
            }
            if (pos < start) return pos;
            if (pos < end)   return std::nullopt;
            return pos - (end - start);
        }
    };

}
