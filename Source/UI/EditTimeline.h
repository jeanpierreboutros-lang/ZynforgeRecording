#pragma once

// Shared EDIT-timeline helpers. These live in their own header (rather than
// inside EditPage / TrackRow) so the ruler, the lanes and the unit tests all
// read the SAME definition -- the 2026-08-12 audit found the ruler drawing a
// 300 s empty session while the automation lane hit-tested against 60 s, and a
// hardcoded 48 kHz in the tempo lane on top of that.

#include <juce_core/juce_core.h>
#include <limits>
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

    inline juce::int64 recordingTimelineSpanSamples (juce::int64 recordHead,
                                                     juce::int64 loadedTake,
                                                     double sampleRate) noexcept
    {
        return juce::jmax (notionalEmptyLaneSamples (sampleRate),
                           juce::jmax (recordHead, loadedTake));
    }

    // Keep a fixed time scale as a fresh take grows. At 1x, one viewport
    // initially spans five minutes; after that, content widens instead of
    // squeezing the entire take into the same pixels and pinning the head at
    // the right edge. The viewport can then genuinely follow a long take.
    inline int recordingContentWidth (int viewportWidth, float zoom,
                                      juce::int64 timelineSpan,
                                      juce::int64 initialSpan) noexcept
    {
        const double ratio = (double) juce::jmax (timelineSpan, initialSpan)
                           / (double) juce::jmax ((juce::int64) 1, initialSpan);
        return (int) juce::jlimit ((double) juce::jmax (1, viewportWidth),
                                  (double) (std::numeric_limits<int>::max() / 4),
                                  (double) viewportWidth * (double) zoom * ratio);
    }

    inline float steppedTimelineZoom (float current, bool zoomIn) noexcept
    {
        constexpr float step = 1.41f;
        return juce::jlimit (1.0f, 16.0f, zoomIn ? current * step : current / step);
    }

    // Auto-follow must yield to an engineer browsing a rolling take. Keep this
    // small state machine independent of the viewport so the distinction
    // between a manual pan and our own page-scroll can be regression-tested.
    class TimelineFollowState
    {
    public:
        void transportChanged (bool active) noexcept
        {
            if (active && ! transportActive)
                following = true;  // each new play/record pass starts at the live edge
            transportActive = active;
        }

        void horizontalScroll (int beforeX, int afterX, bool programmatic) noexcept
        {
            if (transportActive && ! programmatic && beforeX != afterX)
                following = false;
        }

        void userZoom() noexcept
        {
            if (transportActive) following = false;
        }

        void setFollowing (bool shouldFollow) noexcept { following = shouldFollow; }
        bool isFollowing() const noexcept { return following; }
        bool isTransportActive() const noexcept { return transportActive; }

    private:
        bool following { true };
        bool transportActive { false };
    };

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
