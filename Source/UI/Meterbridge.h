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
        static juce::DialogWindow* launch (AudioEngine&);

        // Detach every OPEN meterbridge's meters from their TrackStates.
        // Third instance of the cached-TrackState& hazard: the bridge's
        // LedMeters hold a TrackState& on their own timers and it only
        // rebinds on its own 12 Hz tick, so a recorder-vector shrink left it
        // sampling freed memory for up to ~83 ms -- wider than the window
        // condemnAllStrips() closes for the MIXER and EDIT. The bridge is a
        // floating window with two launch paths (one stores no pointer), so
        // it self-registers rather than being reached through an owner.
        // Safe to call with no bridge open. Message thread only.
        static void condemnAllMeters();
    };
}
