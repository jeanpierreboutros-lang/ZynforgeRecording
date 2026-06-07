#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // Popup-style "click away to dismiss" for NON-modal floating panels
    // (launchFloating). Construct one inside the panel's content component,
    // passing that component; it watches global mouse presses and hides the
    // panel's top-level window when a press lands outside it. MainComponent's
    // 10 Hz reaper (syncTab) then deletes the hidden window and clears the
    // matching header-tab light.
    //
    // NB: don't use this on panels that open their own popup menus /
    // combo-box dropdowns -- those land in a separate OS window outside the
    // panel bounds and would dismiss the panel. Meant for display-only
    // panels (e.g. the meterbridge).
    class DismissOnOutsideClick final : private juce::MouseListener
    {
    public:
        explicit DismissOnOutsideClick (juce::Component& panelContent)
            : owner (panelContent)
        {
            juce::Desktop::getInstance().addGlobalMouseListener (this);
        }

        ~DismissOnOutsideClick() override
        {
            juce::Desktop::getInstance().removeGlobalMouseListener (this);
        }

    private:
        void mouseDown (const juce::MouseEvent& e) override
        {
            auto* top = owner.getTopLevelComponent();
            if (top == nullptr || ! top->isVisible() || ! top->isOnDesktop())
                return;
            if (! top->getScreenBounds().contains (e.getScreenPosition()))
                top->setVisible (false);   // reaped + tab-light cleared by MainComponent
        }

        juce::Component& owner;
    };
}
