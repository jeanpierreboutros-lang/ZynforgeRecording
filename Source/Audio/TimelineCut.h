#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace zynforge::timelinecut
{
    // What a Crop or a Ripple Delete does to a position on the timeline. Audio,
    // automation, markers, the tempo map and cues all follow this one rule, so
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

    // The tempo map is a step function, so the tempo in force where playback
    // resumes after the removed span has to survive at the join.
    template <class Change>
    std::vector<Change> cutTempoMap (const std::vector<Change>& map, const Cut& cut)
    {
        const auto resume   = cut.crop ? cut.start : cut.end;
        const auto joinedAt = cut.crop ? juce::int64 (0) : cut.start;
        std::vector<Change> out;
        std::optional<float> inForce;
        bool changeAtJoin = false;
        for (const auto& change : map)
        {
            if (const auto pos = cut.map (change.samplePos))
            {
                changeAtJoin = changeAtJoin || *pos == joinedAt;
                out.push_back ({ *pos, change.bpm });
            }
            else if (change.samplePos < resume)
            {
                inForce = change.bpm;
            }
        }
        if (inForce && ! changeAtJoin)
        {
            const auto at = std::lower_bound (out.begin(), out.end(), joinedAt,
                [] (const Change& c, juce::int64 pos) { return c.samplePos < pos; });
            out.insert (at, { joinedAt, *inForce });
        }
        return out;
    }
}
