#include "AutomationToolbar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    AutomationToolbar::AutomationToolbar()
    {
        title.setText ("AUTOMATION", juce::dontSendNotification);
        title.setFont (brand::type::sectionTitle());
        title.setColour (juce::Label::textColourId, brand::accentStatus);
        addAndMakeVisible (title);

        styleToolButton (selectButton, brand::accentPlay);
        styleToolButton (addButton,    brand::accentStatus);
        styleToolButton (deleteButton, brand::accentRecord);
        selectButton .onClick = [this] { selectTool (Tool::Select);      };
        addButton    .onClick = [this] { selectTool (Tool::AddPoint);    };
        deleteButton .onClick = [this] { selectTool (Tool::DeletePoint); };
        selectButton .setTooltip ("Select tool -- drag an existing point to move it");
        addButton    .setTooltip ("Add point -- click in a row's automation lane to drop a point");
        deleteButton .setTooltip ("Delete tool -- click a point to remove it");
        addAndMakeVisible (selectButton);
        addAndMakeVisible (addButton);
        addAndMakeVisible (deleteButton);
        selectButton.setToggleState (true, juce::dontSendNotification);

        paramLabel.setText ("Lane:", juce::dontSendNotification);
        paramLabel.setFont (brand::type::captionBold());
        paramLabel.setColour (juce::Label::textColourId, brand::textSecondary);
        addAndMakeVisible (paramLabel);

        paramCombo.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff000000));
        paramCombo.setColour (juce::ComboBox::outlineColourId,    brand::edge);
        paramCombo.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
        paramCombo.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
        paramCombo.addItem ("Volume", 1);
        paramCombo.addItem ("Pan",    2);
        paramCombo.addItem ("Mute",   3);
        paramCombo.addItem ("Click",  4);
        paramCombo.addItem ("Tempo",  5);
        paramCombo.setSelectedId (1, juce::dontSendNotification);
        paramCombo.onChange = [this]
        {
            param = (Param) (paramCombo.getSelectedId() - 1);
            if (onParamChanged) onParamChanged (param);
        };
        addAndMakeVisible (paramCombo);

        clearButton.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
        clearButton.setColour (juce::TextButton::textColourOffId, brand::textMuted);
        clearButton.setTooltip ("Clear every automation point on every row for the current parameter");
        clearButton.onClick = [this] { if (onClearAll) onClearAll(); };
        addAndMakeVisible (clearButton);
    }

    void AutomationToolbar::styleToolButton (juce::TextButton& b, juce::Colour activeColour)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (4242);
        b.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
        b.setColour (juce::TextButton::buttonOnColourId,  activeColour);
        b.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
        b.setColour (juce::TextButton::textColourOnId,    brand::onSignal (activeColour));
    }

    void AutomationToolbar::selectTool (Tool t)
    {
        tool = t;
        if (onToolChanged) onToolChanged (tool);
    }

    void AutomationToolbar::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.10f));
        g.fillRoundedRectangle (r, brand::radius::md);
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
    }

    void AutomationToolbar::resized()
    {
        // All widths bumped ~50% for the larger toolbar height -- the
        // engineer can read the controls from arm's length and hit
        // the buttons under stage lighting.
        auto r = getLocalBounds().reduced (10, 6);

        title       .setBounds (r.removeFromLeft (130));
        r.removeFromLeft (brand::space::md);
        selectButton.setBounds (r.removeFromLeft (88).reduced (0, 3));
        r.removeFromLeft (4);
        addButton   .setBounds (r.removeFromLeft (96).reduced (0, 3));
        r.removeFromLeft (4);
        deleteButton.setBounds (r.removeFromLeft (88).reduced (0, 3));
        r.removeFromLeft (16);

        paramLabel  .setBounds (r.removeFromLeft (50));
        r.removeFromLeft (brand::space::sm);
        paramCombo  .setBounds (r.removeFromLeft (180).reduced (0, 3));
        r.removeFromLeft (16);

        clearButton .setBounds (r.removeFromLeft (110).reduced (0, 3));
    }
}
