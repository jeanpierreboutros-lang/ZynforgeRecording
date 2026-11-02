#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

#include <utility>
#include <vector>

namespace zynforge
{
    // Modal patch page with INPUT PATCH / OUTPUT PATCH tabs. Rows are the
    // device's hardware inputs (or outputs); columns are the channel
    // strips. Clicking a circle assigns that hardware channel to the
    // strip; clicking the same circle again clears it. Each strip can be
    // patched to one hardware channel at a time (column-radio behaviour).
    class PatchPage
    {
    public:
        static juce::DialogWindow* launch (AudioEngine& engine);

        // Column and row of every strip a drag lays down, from where it began.
        // A pair takes two rows, so the strip after it starts two rows on and
        // no two strips share a channel. A pair that would run off the last
        // row ends the drag.
        static std::vector<std::pair<int, int>> dragTargets (const std::vector<bool>& stripIsStereo,
                                                             int startCol, int startRow,
                                                             int rowsDragged, int numRows);
    };
}
