#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

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
        static void launch (AudioEngine& engine);
    };
}
