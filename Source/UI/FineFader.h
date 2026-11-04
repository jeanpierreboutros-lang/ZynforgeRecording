#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

namespace zynforge
{
    // A fader that moves ONLY by a direct click + drag on the cap -- never by
    // the scroll wheel / trackpad. A stray two-finger swipe over the mixer must
    // not nudge a channel / master level mid-show. We skip Slider's
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

        // A typed level. JUCE reads any text with no leading digit as 0, so
        // "-inf", "off" or a stray Space put an off strip at unity, into the PA.
        // The off words go to the bottom of the range; anything else that is
        // not a number keeps the level the fader has.
        double getValueFromText (const juce::String& text) override
        {
            auto typed = text.trim();
            const auto suffix = getTextValueSuffix().trim();
            if (suffix.isNotEmpty() && typed.endsWithIgnoreCase (suffix))
                typed = typed.dropLastCharacters (suffix.length()).trim();
            if (isOffWord (typed)) return getMinimum();
            while (typed.startsWithChar ('+')) typed = typed.substring (1).trimStart();
            const auto number = typed.initialSectionContainingOnly ("0123456789.,-");
            if (! number.containsAnyOf ("0123456789")) return getValue();
            return number.getDoubleValue();
        }

        // The level readout opens on a double-click, and a click elsewhere
        // cancels what was typed. One click while reaching for the fader opened
        // it with its text selected, the next Space replaced it, and the click
        // after that committed the blank. Slider rebuilds its box on a look
        // change and resets it on enable, so both put this back.
        void lookAndFeelChanged() override { juce::Slider::lookAndFeelChanged(); editValueOnDoubleClick(); }
        void enablementChanged() override  { juce::Slider::enablementChanged();  editValueOnDoubleClick(); }

        // The same for a readout the fader does not own (a strip's level label).
        static void editOnDoubleClick (juce::Label& box) { box.setEditable (false, true, true); }

    private:
        static bool isOffWord (const juce::String& t)
        {
            return t.equalsIgnoreCase ("-inf") || t.equalsIgnoreCase ("inf") || t.equalsIgnoreCase ("off")
                || t == juce::String::fromUTF8 ("-\xe2\x88\x9e") || t == juce::String::fromUTF8 ("\xe2\x88\x9e");
        }

        void editValueOnDoubleClick()
        {
            for (auto* child : getChildren())
                if (auto* box = dynamic_cast<juce::Label*> (child))
                    if (box->isEditable()) editOnDoubleClick (*box);
        }
    };
}
