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

        paramCombo.setColour (juce::ComboBox::backgroundColourId, brand::inputBg);
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

        // WRITE-mode dropdown. Off = lanes are read-only during
        // playback. Touch / Latch / Write progressively expose more
        // aggressive write semantics (the engine today treats them
        // identically; the toolbar still surfaces them so the
        // engineer's mental model matches Pro Tools). Picking any
        // non-Off value forces TRIM off.
        writeLabel.setText ("Write:", juce::dontSendNotification);
        writeLabel.setFont (brand::type::captionBold());
        writeLabel.setColour (juce::Label::textColourId, brand::textSecondary);
        addAndMakeVisible (writeLabel);

        writeCombo.addItem ("Off",   1);
        writeCombo.addItem ("Touch", 2);
        writeCombo.addItem ("Latch", 3);
        writeCombo.addItem ("Write", 4);
        writeCombo.setSelectedId (1, juce::dontSendNotification);
        styleWriteCombo();
        writeCombo.setTooltip ("WRITE mode: fader / pan / mute moves during playback record automation. "
                                "Touch holds the value while touching, then snaps back. Latch holds and "
                                "stays. Write overwrites the whole pass. Off = read-only playback.");
        writeCombo.onChange = [this]
        {
            const auto wm = (WriteMode) (writeCombo.getSelectedId() - 1);
            if (wm != WriteMode::Off && trimButton.getToggleState())
            {
                trimButton.setToggleState (false, juce::dontSendNotification);
                if (onTrimModeChanged) onTrimModeChanged (false);
            }
            styleWriteCombo();
            if (onWriteModeChanged) onWriteModeChanged (wm);
        };
        addAndMakeVisible (writeCombo);

        // TRIM toggle. When on + playback rolling, fader / pan moves
        // add to the per-track trim offset instead of dropping new
        // points. Engineer can ride a level by ±N dB without
        // overwriting the existing automation shape.
        trimButton.setClickingTogglesState (true);
        trimButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
        trimButton.setColour (juce::TextButton::buttonOnColourId,  brand::engagedAmber);
        trimButton.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
        trimButton.setColour (juce::TextButton::textColourOnId,    brand::onSignal (brand::engagedAmber));
        trimButton.setTooltip ("TRIM mode: fader and pan moves during playback nudge the per-track "
                                "trim offset, leaving the automation curve shape untouched.");
        trimButton.onClick = [this]
        {
            const bool on = trimButton.getToggleState();
            if (on && writeCombo.getSelectedId() > 1)
            {
                writeCombo.setSelectedId (1, juce::dontSendNotification);
                styleWriteCombo();
                if (onWriteModeChanged) onWriteModeChanged (WriteMode::Off);
            }
            if (onTrimModeChanged) onTrimModeChanged (on);
        };
        addAndMakeVisible (trimButton);

        // SUSPEND -- engine ignores stored automation at read time,
        // letting the engineer audition raw fader / pan / mute moves.
        // Toggle off to restore the recorded pass.
        suspendButton.setClickingTogglesState (true);
        suspendButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
        suspendButton.setColour (juce::TextButton::buttonOnColourId,  brand::textMuted);
        suspendButton.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
        suspendButton.setColour (juce::TextButton::textColourOnId,    brand::onSignal (brand::textMuted));
        suspendButton.setTooltip ("SUSPEND: engine ignores every automation lane at read time so the "
                                  "raw fader / pan / mute values pass through. Toggle off to resume playback.");
        suspendButton.onClick = [this]
        {
            if (onSuspendChanged) onSuspendChanged (suspendButton.getToggleState());
        };
        addAndMakeVisible (suspendButton);

        // PUNCH -- automation writes only fire while the playhead is
        // inside the engine's punch range (set via the EDIT page's
        // range-select on the timeline).
        punchButton.setClickingTogglesState (true);
        punchButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
        punchButton.setColour (juce::TextButton::buttonOnColourId,  brand::accentStatus);
        punchButton.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
        punchButton.setColour (juce::TextButton::textColourOnId,    brand::onSignal (brand::accentStatus));
        punchButton.setTooltip ("PUNCH: when on, WRITE-mode writes only fire inside the punch range. "
                                "Outside the range, the engine reads existing points but doesn't record.");
        punchButton.onClick = [this]
        {
            if (onPunchChanged) onPunchChanged (punchButton.getToggleState());
        };
        addAndMakeVisible (punchButton);
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

    void AutomationToolbar::setParamSilently (Param p)
    {
        if (param == p) return;
        param = p;
        paramCombo.setSelectedId ((int) p + 1, juce::dontSendNotification);
    }

    void AutomationToolbar::setWriteModeSilently (WriteMode m)
    {
        writeCombo.setSelectedId ((int) m + 1, juce::dontSendNotification);
        styleWriteCombo();
    }

    void AutomationToolbar::setSuspendSilently (bool on)
    {
        suspendButton.setToggleState (on, juce::dontSendNotification);
    }

    void AutomationToolbar::setPunchSilently (bool on)
    {
        punchButton.setToggleState (on, juce::dontSendNotification);
    }

    void AutomationToolbar::setTrimSilently (bool on)
    {
        trimButton.setToggleState (on, juce::dontSendNotification);
    }

    void AutomationToolbar::styleWriteCombo()
    {
        // Off = neutral chrome; any write mode = brand-red so the
        // engineer's eye snags on it from across the venue.
        const bool armed = writeCombo.getSelectedId() > 1;
        const juce::Colour bg     = armed ? brand::accentRecord : brand::inputBg;
        const juce::Colour fg     = armed ? brand::onSignal (brand::accentRecord) : brand::textPrimary;
        const juce::Colour outline= armed ? brand::accentRecord : brand::edge;
        writeCombo.setColour (juce::ComboBox::backgroundColourId, bg);
        writeCombo.setColour (juce::ComboBox::textColourId,       fg);
        writeCombo.setColour (juce::ComboBox::outlineColourId,    outline);
        writeCombo.setColour (juce::ComboBox::arrowColourId,      fg);
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
        r.removeFromLeft (12);
        writeLabel  .setBounds (r.removeFromLeft (52));
        r.removeFromLeft (brand::space::sm);
        writeCombo  .setBounds (r.removeFromLeft (100).reduced (0, 3));
        r.removeFromLeft (4);
        trimButton  .setBounds (r.removeFromLeft (70).reduced (0, 3));
        r.removeFromLeft (10);
        suspendButton.setBounds (r.removeFromLeft (84).reduced (0, 3));
        r.removeFromLeft (4);
        punchButton  .setBounds (r.removeFromLeft (72).reduced (0, 3));
    }
}
