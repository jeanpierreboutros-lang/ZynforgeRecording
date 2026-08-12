#pragma once

// Shared EDIT-timeline helpers. These live in their own header (rather than
// inside EditPage / TrackRow) so the ruler, the lanes and the unit tests all
// read the SAME definition -- the 2026-08-12 audit found the ruler drawing a
// 300 s empty session while the automation lane hit-tested against 60 s, and a
// hardcoded 48 kHz in the tempo lane on top of that.

#include <juce_core/juce_core.h>
#include <vector>

#include "../Audio/ClipModel.h"

namespace zynforge
{
    // ── Shared EDIT-timeline helpers ───────────────────────────────────────
    // Both live here (rather than inside TrackRow) so the ruler, the lanes and
    // the tests all read the SAME definition -- the 2026-08-12 audit found the
    // ruler drawing a 300 s empty session while the automation lane hit-tested
    // against 60 s, and a hardcoded 48 kHz in the tempo lane on top of that.

    // Timeline span an EDIT lane (and the ruler) covers when the session has no
    // audio yet -- 5 notional minutes, so the engineer can place the edit
    // cursor, markers, tempo and automation before the first take.
    inline constexpr double kNotionalEmptyLaneSec = 300.0;

    inline juce::int64 notionalEmptyLaneSamples (double sampleRate) noexcept
    {
        return (juce::int64) (juce::jmax (8000.0, sampleRate) * kNotionalEmptyLaneSec);
    }

    // Which clip on `dst` corresponds to the clip [start, start+len) on another
    // track. Clip INDICES are per-track, so an edit-group broadcast that reuses
    // the source index edits whichever clip happens to sit at that position on
    // the peer. Match by timeline overlap instead: the peer clip containing the
    // source clip's midpoint. -1 = the peer has nothing there (skip it).
    inline int clipIndexAtMidpoint (const std::vector<Clip>& dst,
                                    juce::int64 srcStart, juce::int64 srcLen) noexcept
    {
        if (srcLen <= 0) return -1;
        const juce::int64 mid = srcStart + srcLen / 2;
        for (int i = 0; i < (int) dst.size(); ++i)
        {
            const auto& c = dst[(size_t) i];
            if (mid >= c.timelineStartSamples
                && mid <  c.timelineStartSamples + c.fileLengthSamples)
                return i;
        }
        return -1;
    }}
