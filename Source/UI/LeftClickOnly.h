#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    namespace leftclick
    {
        // A press that asks for a menu: the right (or middle) button, or
        // Ctrl-click on the Mac (isPopupMenu). Only a plain left press moves or
        // presses a control. A synthetic press with no button flags counts as left.
        inline bool isMenuPress (const juce::MouseEvent& e) noexcept
        {
            return e.mods.isPopupMenu() || e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown();
        }
    }

    // A control that acts on a left press only. The strip and the setlist bar
    // open their menus from a mouse listener on every child, and JUCE calls a
    // listener AFTER the control under the pointer has had the press: a Button
    // doesn't look at which button went down, so a right-click on M muted the
    // channel (and on Update overwrote the cue) before the menu opened, and a
    // snapping fader jumped to the pointer. Here a menu press, its drag, its
    // release and a double-click made of menu presses never reach the control;
    // the listener still sees them, so the menu still opens.
    template <typename Base>
    class LeftClickOnly : public Base
    {
    public:
        using Base::Base;

        void mouseDown (const juce::MouseEvent& e) override
        {
            menuPress = leftclick::isMenuPress (e);
            if (! menuPress) Base::mouseDown (e);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! menuPress) Base::mouseDrag (e);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! menuPress) Base::mouseUp (e);
        }

        // JUCE sends the double-click after the second release, so the press
        // that made it is still the one held here. A slider's double-click
        // return value (0 dB, pan centre) must not come from two right-clicks.
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (! menuPress && ! leftclick::isMenuPress (e)) Base::mouseDoubleClick (e);
        }

    private:
        bool menuPress { false };   // the press now held (or last held) asked for a menu
    };
}
