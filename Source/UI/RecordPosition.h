#pragma once

#include <juce_core/juce_core.h>

namespace zynforge
{
    // A rolling manual punch starts where playback is NOW. The edit cursor is
    // only an insertion point when transport is stopped; otherwise an old
    // cursor (often at zero) turns a punch into an unrelated continuation.
    inline juce::int64 manualRecordPosition (bool playbackRolling,
                                             juce::int64 playhead,
                                             juce::int64 editCursor) noexcept
    {
        return (! playbackRolling && editCursor >= 0) ? editCursor : playhead;
    }

    // Only a position the engineer chose can punch: a rolling playhead or a
    // placed cursor. A playhead merely parked inside the take (pause, marker
    // jump, remote locate, the end of an earlier punch) appends, which never
    // touches the take. A cursor at zero is a deliberate punch from the start;
    // a deliberate position may also sit in a reference bed past the capture.
    inline bool manualRecordPunches (bool playbackRolling, juce::int64 editCursor, juce::int64 position,
                                     juce::int64 captureLength, juce::int64 timelineLength) noexcept
    {
        return (playbackRolling || editCursor >= 0)
            && position < juce::jmax (captureLength, timelineLength);
    }

    // The installed builds' Loop button stored a whole-timeline region the
    // engineer never selected, so a saved one that spans everything means
    // nothing: it is dropped on open rather than restored.
    inline bool savedRegionSpansTimeline (juce::int64 start, juce::int64 end, juce::int64 timelineLength) noexcept
    {
        return start <= 0 && end >= timelineLength;
    }
}
