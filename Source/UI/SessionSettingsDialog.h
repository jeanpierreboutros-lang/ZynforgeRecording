#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Modal session settings dialog: pick Audio Format, Sample Rate, and
    // Bit Depth, then Apply commits everything to the recorder and
    // device manager and closes the box. Cancel closes without changes.
    class SessionSettingsDialog
    {
    public:
        static void launch (AudioEngine&);
    };
}
