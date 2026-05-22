#include "MasterStrip.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    MasterStrip::MasterStrip (AudioEngine& eng)
        : engine (eng), meter (eng.getMasterState())
    {
        title.setFont (brand::type::sectionTitle());
        title.setColour (juce::Label::textColourId, brand::brandOrange);
        title.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (title);

        gainLabel.setFont (brand::type::caption());
        gainLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        gainLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (gainLabel);

        auto styleCombo = [] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
            c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
            c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
            c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
        };
        styleCombo (outLCombo);
        styleCombo (outRCombo);
        outLCombo.setTooltip ("Master output — LEFT channel.");
        outRCombo.setTooltip ("Master output — RIGHT channel.");
        outLCombo.onChange = [this]
        {
            const int id = outLCombo.getSelectedId();
            if (id > 0)
                engine.setMasterOutputs (id - 1, engine.getMasterOutputR());
        };
        outRCombo.onChange = [this]
        {
            const int id = outRCombo.getSelectedId();
            if (id > 0)
                engine.setMasterOutputs (engine.getMasterOutputL(), id - 1);
        };
        addAndMakeVisible (outLCombo);
        addAndMakeVisible (outRCombo);

        // Mono / stereo mode toggle. Reuses the toggle-button pill style;
        // text flips between 'MONO' and 'ST', tick colour goes brand-orange
        // when stereo (the default) and teal when mono so the difference
        // reads at a glance.
        modeButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        modeButton.setColour (juce::ToggleButton::tickColourId, brand::brandOrange);
        modeButton.setClickingTogglesState (true);
        modeButton.setToggleState (engine.getMasterStereo(), juce::dontSendNotification);
        modeButton.setButtonText (engine.getMasterStereo() ? "ST" : "MONO");
        modeButton.setTooltip ("Master output mode — click to toggle between MONO (one output, "
                               "one meter bar) and STEREO (two outputs, two meter bars).");
        modeButton.onClick = [this]
        {
            const bool stereo = modeButton.getToggleState();
            engine.setMasterStereo (stereo);
            modeButton.setButtonText (stereo ? "ST" : "MONO");
            meter.setStereoPartner (stereo ? &engine.getMasterStateR() : nullptr);
            outRCombo.setEnabled (stereo);
            resized();   // hide / show the R output combo
        };
        addAndMakeVisible (modeButton);

        muteButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        muteButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        muteButton.setToggleState (engine.getMasterMuted(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            engine.setMasterMuted (muteButton.getToggleState());
        };
        addAndMakeVisible (muteButton);

        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setRange (-60.0, 12.0, 0.1);
        fader.setSkewFactorFromMidPoint (-15.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextValueSuffix (" dB");
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 14);
        fader.setNumDecimalPlacesToDisplay (1);
        fader.setColour (juce::Slider::thumbColourId, brand::brandOrange.brighter (0.20f));
        fader.setColour (juce::Slider::backgroundColourId, brand::bgDeep);
        fader.setColour (juce::Slider::textBoxTextColourId, brand::textPrimary);
        fader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        fader.setMouseDragSensitivity (250);
        fader.setValue (engine.getMasterGainDb(), juce::dontSendNotification);
        fader.onValueChange = [this]
        {
            engine.setMasterGainDb ((float) fader.getValue());
        };
        addAndMakeVisible (fader);

        meter.setTooltip ("Master bus peak + RMS — click to clear clip.");
        if (engine.getMasterStereo())
            meter.setStereoPartner (&engine.getMasterStateR());
        addAndMakeVisible (meter);
        cachedStereo = engine.getMasterStereo();

        refreshOutputs();
        startTimerHz (10);
    }

    MasterStrip::~MasterStrip() { stopTimer(); }

    void MasterStrip::refreshOutputs()
    {
        int dev = 0;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            dev = d->getActiveOutputChannels().countNumberOfSetBits();
        if (dev == lastNumOutputs) return;
        lastNumOutputs = dev;

        const int visible = juce::jmax (dev, 8);
        auto fill = [&] (juce::ComboBox& box)
        {
            box.clear (juce::dontSendNotification);
            for (int i = 0; i < visible; ++i)
            {
                const bool live = (i < dev);
                box.addItem (live ? ("Out " + juce::String (i + 1))
                                  : ("Out " + juce::String (i + 1) + " (off)"),
                             i + 1);
            }
        };
        fill (outLCombo);
        fill (outRCombo);
        outLCombo.setSelectedId (engine.getMasterOutputL() + 1, juce::dontSendNotification);
        outRCombo.setSelectedId (engine.getMasterOutputR() + 1, juce::dontSendNotification);
    }

    void MasterStrip::timerCallback()
    {
        // Sync gain readout + mute / outputs / stereo mode from engine
        // atomics so other entry points (restore, future API) keep the
        // UI in lockstep.
        const float dB = engine.getMasterGainDb();
        if (std::abs ((float) fader.getValue() - dB) > 0.05f)
            fader.setValue (dB, juce::dontSendNotification);
        gainLabel.setText (juce::String (dB, 1) + " dB", juce::dontSendNotification);
        if (muteButton.getToggleState() != engine.getMasterMuted())
            muteButton.setToggleState (engine.getMasterMuted(), juce::dontSendNotification);
        const int lId = engine.getMasterOutputL() + 1;
        const int rId = engine.getMasterOutputR() + 1;
        if (outLCombo.getSelectedId() != lId) outLCombo.setSelectedId (lId, juce::dontSendNotification);
        if (outRCombo.getSelectedId() != rId) outRCombo.setSelectedId (rId, juce::dontSendNotification);

        const bool stereo = engine.getMasterStereo();
        if (stereo != cachedStereo)
        {
            cachedStereo = stereo;
            modeButton.setToggleState (stereo, juce::dontSendNotification);
            modeButton.setButtonText (stereo ? "ST" : "MONO");
            meter.setStereoPartner (stereo ? &engine.getMasterStateR() : nullptr);
            outRCombo.setEnabled (stereo);
            resized();
        }

        refreshOutputs();
    }

    void MasterStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgElevated, r, 0.05f, 0.30f));
        g.fillRoundedRectangle (r, brand::radius::xl);
        g.setColour (brand::brandOrange.withAlpha (0.40f));
        g.drawRoundedRectangle (r, brand::radius::xl, 1.5f);
    }

    void MasterStrip::resized()
    {
        auto r = getLocalBounds().reduced (8, 10);
        const bool stereo = engine.getMasterStereo();

        title    .setBounds (r.removeFromTop (20));
        r.removeFromTop (brand::space::sm);

        // Mode pill takes the full strip width so the label (ST / MONO)
        // is always readable. Sits above the output combos.
        modeButton.setBounds (r.removeFromTop (22));
        r.removeFromTop (brand::space::xs);

        outLCombo.setBounds (r.removeFromTop (22));
        r.removeFromTop (brand::space::xs);

        // R combo only shown in stereo mode; in mono we collapse the
        // slot so the rest of the layout moves up.
        if (stereo)
        {
            outRCombo.setVisible (true);
            outRCombo.setBounds (r.removeFromTop (22));
            r.removeFromTop (brand::space::sm);
        }
        else
        {
            outRCombo.setVisible (false);
            outRCombo.setBounds ({});
        }

        muteButton.setBounds (r.removeFromTop (24));
        r.removeFromTop (brand::space::sm);
        gainLabel.setBounds (r.removeFromTop (16));
        r.removeFromTop (brand::space::xs);

        // Meter on right, fader on left. Meter width shrinks slightly
        // in mono mode since only one bar is needed.
        const int meterW = stereo ? 40 : 26;
        auto meterArea = r.removeFromRight (meterW);
        meter.setBounds (meterArea);
        r.removeFromRight (4);
        fader.setBounds (r);
    }
}
