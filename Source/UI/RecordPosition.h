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
}
