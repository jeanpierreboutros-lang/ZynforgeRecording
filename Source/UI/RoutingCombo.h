#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/BrandColors.h"

namespace zynforge
{
    // The short form of a strip's routing entry, for a box too narrow for the
    // whole text: the arrow and the In / Out word go, and Master is Mst.
    //   "-> Master" -> "Mst"    "In 9-10" -> "9-10"    "Out 3" -> "3"
    //   "In 33 (off)" -> "33 off"    "(unrouted)" -> "none"
    // Anything else comes back as it was.
    inline juce::String shortRoutingLabel (const juce::String& full)
    {
        auto t = full.trim();
        const auto arrow = juce::String::fromUTF8 ("\xe2\x86\x92");
        if (t.startsWith (arrow)) t = t.substring (arrow.length()).trimStart();
        else if (t.startsWith ("->")) t = t.substring (2).trimStart();

        const bool off = t.endsWithIgnoreCase ("(off)");
        if (off) t = t.dropLastCharacters (5).trimEnd();

        juce::String s;
        if      (t.equalsIgnoreCase ("Master"))        s = "Mst";
        else if (t.equalsIgnoreCase ("(unrouted)"))    s = "none";
        else if (t.startsWithIgnoreCase ("In "))       s = t.substring (3).trim();
        else if (t.startsWithIgnoreCase ("Out "))      s = t.substring (4).trim();
        else return full;
        return off ? s + " off" : s;
    }

    // A strip's routing box. When the selected entry doesn't fit the closed
    // box, it shows shortRoutingLabel's form instead of a clipped "-> ...".
    // The items, the popup, getText() and the accessible name keep the whole
    // text, so selection and every caller see what they always saw: only the
    // drawing changes. The box's own label is given transparent ink and the
    // text is drawn over it here.
    class RoutingCombo final : public juce::ComboBox
    {
    public:
        // The text colour (stands in for ComboBox::textColourId).
        void setInk (juce::Colour c)
        {
            ink = c;
            setColour (juce::ComboBox::textColourId, juce::Colours::transparentBlack);
            repaint();
        }

        // What the closed box draws now: the whole entry, or its short form.
        juce::String shownText() const
        {
            const auto* box = textBox();
            if (box == nullptr) return {};
            const auto full = box->getText();
            const auto area = box->getBorderSize().subtractedFrom (box->getLocalBounds());
            if (juce::GlyphArrangement::getStringWidth (box->getFont(), full) <= (float) area.getWidth())
                return full;
            return shortRoutingLabel (full);
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            const auto* box = textBox();
            if (box == nullptr || box->isBeingEdited()) return;
            const auto area = box->getBorderSize().subtractedFrom (box->getLocalBounds())
                                  + box->getPosition();
            g.setColour (isEnabled() ? ink : ink.withMultipliedAlpha (brand::alpha::half));   // as drawLabel dims
            g.setFont (box->getFont());
            g.drawFittedText (shownText(), area, box->getJustificationType(), 1,
                              box->getMinimumHorizontalScale());
        }

    private:
        const juce::Label* textBox() const
        {
            for (auto* child : getChildren())
                if (auto* l = dynamic_cast<const juce::Label*> (child)) return l;
            return nullptr;
        }

        juce::Colour ink;
    };
}
