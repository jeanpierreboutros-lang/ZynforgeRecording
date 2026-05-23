#include "SetlistBar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    SetlistBar::SetlistBar()
    {
        titleLabel.setText ("SETLIST", juce::dontSendNotification);
        titleLabel.setFont (brand::type::uiLabel());
        titleLabel.setColour (juce::Label::textColourId, brand::textMuted);
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (titleLabel);

        auto styleArrow = [] (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
            b.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        };
        prevButton.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x82"));   // ◂
        nextButton.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb8"));   // ▸
        styleArrow (prevButton);
        styleArrow (nextButton);
        prevButton.setTooltip ("Jump to previous cue");
        nextButton.setTooltip ("Jump to next cue");
        prevButton.onClick = [this] { if (onPrev) onPrev(); };
        nextButton.onClick = [this] { if (onNext) onNext(); };
        addAndMakeVisible (prevButton);
        addAndMakeVisible (nextButton);

        cueCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff000000));
        cueCombo.setColour (juce::ComboBox::outlineColourId,    brand::edge);
        cueCombo.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
        cueCombo.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
        cueCombo.setTextWhenNothingSelected ("(empty setlist)");
        cueCombo.setTooltip ("Pick a cue to jump to. Use + Cue to add at the transport position.");
        cueCombo.onChange = [this]
        {
            if (suppressComboCallback) return;
            const int idx = cueCombo.getSelectedId() - 1;     // 1-based IDs
            if (idx >= 0 && onPick) onPick (idx);
        };
        addAndMakeVisible (cueCombo);

        addCueButton.setButtonText ("+ Cue");
        addCueButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus);
        addCueButton.setColour (juce::TextButton::textColourOffId, brand::onSignal (brand::accentStatus));
        addCueButton.setTooltip ("Drop a new cue at the current transport position");
        addCueButton.onClick = [this] { if (onAddCue) onAddCue(); };
        addAndMakeVisible (addCueButton);

        updateButton.setButtonText ("Update");
        updateButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
        updateButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        updateButton.setTooltip ("Overwrite the current cue's position with the live transport. "
                                 "Right-click for Rename / Delete.");
        updateButton.onClick = [this] { if (onUpdateCue) onUpdateCue(); };
        addAndMakeVisible (updateButton);

        // Listen for mouse events bubbling up from every child so a
        // right-click anywhere on the bar — combo, arrow, button —
        // opens the same context menu.
        addMouseListener (this, true);
    }

    void SetlistBar::mouseDown (const juce::MouseEvent& e)
    {
        if (! (e.mods.isPopupMenu() || e.mods.isRightButtonDown()))
            return;

        // Don't fight the combo's own dropdown — it owns the left-click
        // popup. Right-click on it still opens our menu.
        juce::PopupMenu menu;
        menu.addItem (1, "Rename cue...", onRenameCue != nullptr);
        menu.addItem (2, "Delete cue",            onDeleteCue != nullptr);
        menu.addSeparator();
        menu.addItem (3, "Move cue up",            onMoveCue != nullptr);
        menu.addItem (4, "Move cue down",          onMoveCue != nullptr);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                                juce::Rectangle<int> (e.getScreenPosition(), e.getScreenPosition())),
                            [this] (int chosen)
        {
            if      (chosen == 1 && onRenameCue) onRenameCue();
            else if (chosen == 2 && onDeleteCue) onDeleteCue();
            else if (chosen == 3 && onMoveCue)   onMoveCue (-1);
            else if (chosen == 4 && onMoveCue)   onMoveCue (+1);
        });
    }

    void SetlistBar::setCues (const std::vector<Cue>& cues, int selectedIndex)
    {
        suppressComboCallback = true;
        cueCombo.clear (juce::dontSendNotification);
        for (size_t i = 0; i < cues.size(); ++i)
        {
            // Numbered prefix so the engineer can read the order even on
            // a busy show: '01  Intro', '02  Opener', …
            const auto labelNum = juce::String ((int) i + 1).paddedLeft ('0', 2);
            cueCombo.addItem (labelNum + "  " + cues[i].name, (int) i + 1);
        }
        if (selectedIndex >= 0 && selectedIndex < (int) cues.size())
            cueCombo.setSelectedId (selectedIndex + 1, juce::dontSendNotification);
        suppressComboCallback = false;
        repaint();
    }

    void SetlistBar::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.10f));
        g.fillRoundedRectangle (r, brand::radius::md);
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
    }

    void SetlistBar::resized()
    {
        // Compact, left-anchored layout:
        //   SETLIST | ◂ | combo(260px) | ▸ | + Cue | Update | Rename
        // The combo is fixed-width — the engineer doesn't want it
        // swallowing the whole row. Empty space sits to the right.
        auto r = getLocalBounds().reduced (8, 4);

        titleLabel  .setBounds (r.removeFromLeft (64));
        r.removeFromLeft (brand::space::xs);
        prevButton  .setBounds (r.removeFromLeft (28).reduced (0, 2));
        r.removeFromLeft (brand::space::xs);
        cueCombo    .setBounds (r.removeFromLeft (260).reduced (0, 2));
        r.removeFromLeft (brand::space::xs);
        nextButton  .setBounds (r.removeFromLeft (28).reduced (0, 2));
        r.removeFromLeft (brand::space::lg);
        addCueButton.setBounds (r.removeFromLeft (64).reduced (0, 2));
        r.removeFromLeft (brand::space::xs);
        updateButton.setBounds (r.removeFromLeft (74).reduced (0, 2));
    }
}
