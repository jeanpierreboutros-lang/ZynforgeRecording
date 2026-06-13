#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace zynforge
{
    // Gig-one field report: fader scroll either jumped (JUCE's default
    // proportional wheel step on the VCA faders) or did nothing (channel +
    // master had scroll disabled to avoid those jumps). A live engineer
    // expects the wheel to TRIM: this slider steps a fixed, predictable
    // 0.5 dB per wheel notch (Shift = 0.1 dB fine trim), clamped to the
    // range. Drag behaviour is unchanged.
    class FineFader : public juce::Slider
    {
    public:
        using juce::Slider::Slider;

        void mouseWheelMove (const juce::MouseEvent& e,
                             const juce::MouseWheelDetails& wheel) override
        {
            if (! isEnabled()) return;
            const double notch = e.mods.isShiftDown() ? 0.1 : 0.5;   // dB
            // Trackpads stream small deltas; accumulate so a slow two-finger
            // drag still trims smoothly instead of waiting for a full notch.
            accum += wheel.isReversed ? -wheel.deltaY : wheel.deltaY;
            const double kNotchPerDelta = 0.4;   // one notch ~ a classic wheel click
            while (std::abs (accum) >= kNotchPerDelta)
            {
                const double dir = accum > 0 ? 1.0 : -1.0;
                accum -= dir * kNotchPerDelta;
                setValue (juce::jlimit (getMinimum(), getMaximum(),
                                        getValue() + dir * notch),
                          juce::sendNotificationSync);
            }
        }

    private:
        double accum { 0.0 };
    };
}
