#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace zynforge
{
    // A fader that moves ONLY by a direct click + drag on the cap -- never by
    // the scroll wheel / trackpad. A stray two-finger swipe over the mixer must
    // not nudge a channel / master / VCA / bus level mid-show. We skip Slider's
    // wheel handling entirely and hand the event to Component, which propagates
    // it up the parent chain so the mixer viewport can still SCROLL when the
    // pointer happens to be over a fader. Drag behaviour is unchanged.
    //
    // (Was a fine-trim fader; the engineer asked for click-drag-only, so the
    // wheel-step logic is gone.)
    class FineFader : public juce::Slider
    {
    public:
        using juce::Slider::Slider;

        void mouseWheelMove (const juce::MouseEvent& e,
                             const juce::MouseWheelDetails& wheel) override
        {
            juce::Component::mouseWheelMove (e, wheel);   // ignore + propagate
        }
    };
}
