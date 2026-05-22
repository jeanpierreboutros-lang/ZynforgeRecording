#include "SetlistBar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    SetlistBar::SetlistBar()
    {
        titleLabel.setText ("SETLIST", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
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
        addCueButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
        addCueButton.setTooltip ("Drop a new cue at the current transport position");
        addCueButton.onClick = [this] { if (onAddCue) onAddCue(); };
        addAndMakeVisible (addCueButton);

        updateButton.setButtonText ("Update");
        updateButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
        updateButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        updateButton.setTooltip ("Overwrite the current cue's position with the live transport");
        updateButton.onClick = [this] { if (onUpdateCue) onUpdateCue(); };
        addAndMakeVisible (updateButton);
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
        auto r = getLocalBounds().reduced (8, 4);

        titleLabel  .setBounds (r.removeFromLeft (64));
        r.removeFromLeft (4);
        prevButton  .setBounds (r.removeFromLeft (28).reduced (0, 2));
        r.removeFromLeft (4);

        // Action buttons pinned right; combo fills what's left in the
        // middle so long cue names breathe.
        updateButton.setBounds (r.removeFromRight (74).reduced (0, 2));
        r.removeFromRight (4);
        addCueButton.setBounds (r.removeFromRight (64).reduced (0, 2));
        r.removeFromRight (8);
        nextButton  .setBounds (r.removeFromRight (28).reduced (0, 2));
        r.removeFromRight (4);

        cueCombo    .setBounds (r.reduced (0, 2));
    }
}
