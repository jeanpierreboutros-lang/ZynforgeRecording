#include "MasterStrip.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

// The forge mark, cold: the master is chrome, not state.
namespace
{
    inline void drawForgeMark (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto xf = juce::AffineTransform::scale (r.getWidth(), r.getHeight()).translated (r.getX(), r.getY());
        juce::Path hex;
        hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
        hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
        juce::Path chv;
        chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
        chv.startNewSubPath (0.36f, 0.73f); chv.lineTo (0.50f, 0.58f); chv.lineTo (0.64f, 0.73f);
        hex.applyTransform (xf); chv.applyTransform (xf);
        g.setColour (zynforge::brand::gloss (0.10f));
        g.strokePath (hex, juce::PathStrokeType (1.0f));
        g.setColour (zynforge::brand::gloss (0.24f));
        g.strokePath (chv, juce::PathStrokeType (juce::jmax (1.0f, r.getWidth() * 0.07f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

namespace zynforge
{
    // Height of the etched "ZYNFORGE" maker's-mark band at the bottom of the
    // plate. Shared by paint() (which stamps it) and resized() (which reserves
    // it) so the fader value box never overlaps the mark.
    static constexpr int kMarkBandH = 15;

    // No interface has this many output channels.
    static constexpr int kOutputOffId = 100000;
    static const char* const kOutputTooltip = "Master output device channel(s) -- pair like 'Out 1-2' "
                                              "in stereo mode, single 'Out N' in mono. Off sends the mix "
                                              "to no output, for a rig whose outputs are all console returns.";

    static int outputComboId (int masterLeft) noexcept
    {
        return masterLeft < 0 ? kOutputOffId : masterLeft + 1;
    }

    MasterStrip::MasterStrip (AudioEngine& eng)
        : engine (eng), meter (eng.getMasterState())
    {
        title.setFont (brand::type::sectionTitle());
        title.setColour (juce::Label::textColourId, brand::textPrimary);
        title.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (title);

        gainLabel.setFont (brand::type::caption());
        gainLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        gainLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (gainLabel);

        outputCombo.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
        outputCombo.setColour (juce::ComboBox::outlineColourId,    brand::edge);
        outputCombo.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
        outputCombo.setColour (juce::ComboBox::arrowColourId,      brand::textTertiary);
        outputCombo.setTooltip (kOutputTooltip);
        outputCombo.onChange = [this]
        {
            const int id = outputCombo.getSelectedId();
            if (id < 1) return;
            if (id == kOutputOffId) { engine.setMasterOutputs (-1, -1); return; }
            const int devL = id - 1;
            if (engine.getMasterStereo())
                engine.setMasterOutputs (devL, devL + 1);
            else
                engine.setMasterOutputs (devL, devL);   // R unused in mono
        };
        addAndMakeVisible (outputCombo);

        heldLabel.setText ("MIX OFF: STRIP ON PAIR", juce::dontSendNotification);
        heldLabel.setFont (brand::type::micro());
        heldLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        heldLabel.setJustificationType (juce::Justification::centred);
        heldLabel.setMinimumHorizontalScale (0.6f);
        addChildComponent (heldLabel);

        // Mono / stereo mode toggle. Reuses the toggle-button pill style;
        // text flips between 'MONO' and 'ST', and stereo (the default) takes
        // the selection fill so the difference reads at a glance.
        modeButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        modeButton.setColour (juce::ToggleButton::tickColourId, brand::selectFill);
        modeButton.setClickingTogglesState (true);
        modeButton.setToggleState (engine.getMasterStereo(), juce::dontSendNotification);
        modeButton.setButtonText (engine.getMasterStereo() ? "ST" : "MONO");
        modeButton.setTooltip ("Master output mode -- click to toggle between MONO (one output, "
                               "one meter bar) and STEREO (two outputs, two meter bars).");
        modeButton.onClick = [this]
        {
            const bool stereo = modeButton.getToggleState();
            engine.setMasterStereo (stereo);
            modeButton.setButtonText (stereo ? "ST" : "MONO");
            meter.setStereoPartner (stereo ? &engine.getMasterStateR() : nullptr);
            // Mode flip changes the combo's item set (pairs vs singles)
            // so force refreshOutputs to rebuild it next tick.
            lastNumOutputs = -1;
            refreshOutputs();
            resized();
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
        // FineFader: wheel = deliberate 0.5 dB trim (Shift 0.1) -- precise enough
        // that a stray trackpad flick can't yank the master like the old default.
        fader.setRange (-60.0, 12.0, 0.1);
        fader.setSkewFactorFromMidPoint (-15.0);
        fader.setDoubleClickReturnValue (true, 0.0);
        fader.setTextValueSuffix (" dB");
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 14);
        fader.setNumDecimalPlacesToDisplay (1);
        fader.setColour (juce::Slider::thumbColourId, brand::faderFill);
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

        meter.setTooltip ("Master bus peak + RMS -- click to clear clip.");
        if (engine.getMasterStereo())
            meter.setStereoPartner (&engine.getMasterStateR());
        addAndMakeVisible (meter);
        cachedStereo = engine.getMasterStereo();

        // Accessibility: name the master controls for VoiceOver (the "ST"
        // toggle glyph in particular is unreadable) and group the strip.
        setTitle ("Master");
        setDescription ("Master output bus");
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
        fader      .setTitle ("Master gain");
        muteButton .setTitle ("Master mute");
        modeButton .setTitle ("Mono / stereo"); modeButton .setHelpText (modeButton .getTooltip());
        outputCombo.setTitle ("Master output");  outputCombo.setHelpText (outputCombo.getTooltip());

        refreshOutputs();
        startTimerHz (10);
    }

    MasterStrip::~MasterStrip() { stopTimer(); }

    void MasterStrip::refreshOutputs()
    {
        int dev = 0;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            dev = d->getActiveOutputChannels().countNumberOfSetBits();
        const bool stereo = engine.getMasterStereo();

        // Rebuild only on stereo-mode change OR device-count change -- this
        // is called from the 10 Hz timer, and unconditionally clearing +
        // re-adding the items rebuilt the combo (and dirtied it for repaint)
        // every tick at idle. A mode flip forces a rebuild by resetting
        // lastNumOutputs to -1; the selected-id sync stays in timerCallback.
        if (dev == lastNumOutputs && stereo == lastComboStereo) return;
        lastComboStereo = stereo;

        outputCombo.clear (juce::dontSendNotification);
        const int visible = juce::jmax (dev, stereo ? 16 : 8);
        const int step    = stereo ? 2 : 1;
        for (int i = 0; i < visible; i += step)
        {
            const bool live = stereo ? (i + 1 < dev) : (i < dev);
            const auto label = stereo
                ? juce::String ("Out ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                : juce::String ("Out ") + juce::String (i + 1);
            outputCombo.addItem (live ? label : (label + " (off)"), i + 1);
        }
        outputCombo.addItem ("Off (no output)", kOutputOffId);
        outputCombo.setSelectedId (outputComboId (engine.getMasterOutputL()), juce::dontSendNotification);
        lastNumOutputs = dev;
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
        const int outputId = outputComboId (engine.getMasterOutputL());
        if (outputCombo.getSelectedId() != outputId)
            outputCombo.setSelectedId (outputId, juce::dontSendNotification);

        const bool held = engine.isMasterHeldOff();
        if (held != cachedHeldOff)
        {
            cachedHeldOff = held;
            heldLabel.setVisible (held);
            outputCombo.setColour (juce::ComboBox::outlineColourId, held ? brand::accentRecord : brand::edge);
            outputCombo.setTooltip (held ? juce::String ("The mix is off this pair: a strip is patched to one of "
                                                         "its outputs. Pick a spare pair or Off.")
                                         : juce::String (kOutputTooltip));
        }

        const bool stereo = engine.getMasterStereo();
        if (stereo != cachedStereo)
        {
            cachedStereo = stereo;
            modeButton.setToggleState (stereo, juce::dontSendNotification);
            modeButton.setButtonText (stereo ? "ST" : "MONO");
            meter.setStereoPartner (stereo ? &engine.getMasterStateR() : nullptr);
            // Mode change means the combo's item set (pairs vs singles)
            // has to be rebuilt -- force a full refresh.
            lastNumOutputs = -1;
            resized();
        }

        refreshOutputs();
    }

    void MasterStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (brand::bgStrip);
        g.fillRoundedRectangle (r, brand::radius::lg);
        {
            juce::Path bodyClip;
            bodyClip.addRoundedRectangle (r, brand::radius::lg);
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (bodyClip);
            g.setColour (brand::steelMasterHi);
            g.fillRect (r.withHeight (30.0f));
        }
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::lg, 1.0f);
        drawForgeMark (g, { r.getX() + 8.0f, r.getY() + 6.0f, 18.0f, 18.0f });

        // Etched maker's mark -- "ZYNFORGE" pressed into the bottom of the
        // master plate like a stamp on forged steel. Deboss: a gloss lower lip
        // + a dark engraved face, so it reads as worked metal, not a label.
        const auto mark = r.removeFromBottom ((float) kMarkBandH).withTrimmedBottom (2.0f);
        g.setFont (brand::type::label());
        g.setColour (brand::gloss (0.10f));                                  // light lower lip
        g.drawText ("ZYNFORGE", mark.translated (0.0f, 1.0f).toNearestInt(),
                    juce::Justification::centred, false);
        g.setColour (brand::bgDeep);                                         // engraved (recessed) face
        g.drawText ("ZYNFORGE", mark.toNearestInt(), juce::Justification::centred, false);
    }

    void MasterStrip::resized()
    {
        auto r = getLocalBounds().reduced (brand::space::md, brand::space::lg);
        const bool stereo = engine.getMasterStereo();

        // Title sits in the near-black steel header plate, left-aligned next
        // to the forge-mark logo (painted at x+8). Inset clears the logo.
        title    .setBounds (r.removeFromTop (brand::space::ioH).withTrimmedLeft (28));
        r.removeFromTop (brand::space::sm);
        modeButton.setBounds (r.removeFromTop (brand::space::ctrlH));
        r.removeFromTop (brand::space::xs);
        outputCombo.setBounds (r.removeFromTop (brand::space::ctrlH));
        heldLabel.setBounds (r.removeFromTop (brand::space::sm + brand::space::xs));
        muteButton.setBounds (r.removeFromTop (brand::space::btnH));
        r.removeFromTop (brand::space::sm);
        gainLabel.setBounds (r.removeFromTop (brand::space::xl));
        r.removeFromTop (brand::space::xs);

        // Reserve the etched "ZYNFORGE" maker's-mark band that paint() stamps
        // into the bottom 15 px of the plate, plus a small gap, so the fader's
        // value text box doesn't collide with it. Keep in sync with paint().
        r.removeFromBottom (kMarkBandH + brand::space::xs);

        // Meter shrinks in mono since only one bar is needed.
        const int meterW = stereo ? 40 : 26;
        auto meterArea = r.removeFromRight (meterW);
        meter.setBounds (meterArea);
        r.removeFromRight (brand::space::xs);
        fader.setBounds (r);
    }
}
