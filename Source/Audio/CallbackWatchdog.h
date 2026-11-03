#pragma once

#include <juce_core/juce_core.h>

namespace zynforge
{
    // Every sign of life a take has runs inside the audio callback, so a device
    // that just stops calling back stops them all with it, and the take goes on
    // reading as recording. This watches the callback from outside: its counter
    // against wall time. It only reports, and never stops a take: the device may
    // come back, and a stopped take can't. One non-audio thread updates it.
    class CallbackWatchdog
    {
    public:
        static constexpr juce::uint32 kStallMs = 2000;
        // A device error is a reason to look sooner, not proof: JUCE passes on
        // any failed HAL call, and the device can run straight through one.
        static constexpr juce::uint32 kStallAfterErrorMs = 300;

        // True while no block has arrived for too long.
        bool update (bool rolling, juce::uint32 callbacks, juce::uint32 nowMs, bool errorReported) noexcept
        {
            if (! rolling)
            {
                watching = stalled = false;
                return false;
            }
            if (! watching || callbacks != lastCallbacks)
            {
                if (! watching) closedStallsMs = 0;
                else if (stalled) closedStallsMs += nowMs - lastProgressMs;
                watching = true;
                stalled = false;
                lastCallbacks = callbacks;
                lastProgressMs = nowMs;
                return false;
            }
            stalled = nowMs - lastProgressMs >= (errorReported ? kStallAfterErrorMs : kStallMs);
            return stalled;
        }

        // Time this take has had no audio, a stall still running included.
        juce::uint32 stalledMs (juce::uint32 nowMs) const noexcept
        {
            return closedStallsMs + (stalled ? nowMs - lastProgressMs : 0);
        }

    private:
        bool watching { false }, stalled { false };
        juce::uint32 lastCallbacks { 0 }, lastProgressMs { 0 }, closedStallsMs { 0 };
    };
}
