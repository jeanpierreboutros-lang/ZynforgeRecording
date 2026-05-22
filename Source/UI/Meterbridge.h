#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Floating meterbridge window: a wide row of big LedMeters + names,
    // each tracking one strip's TrackState. Engineers can drag it onto
    // a second display.
    class Meterbridge
    {
    public:
        static void launch (AudioEngine&);
    };
}
