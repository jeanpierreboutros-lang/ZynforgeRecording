#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"
#include "StripColourPicker.h"

namespace zynforge
{
    // dB scale strip: paints tick + label at the standard fader dB values,
    // mapped to its own height with the same skew as the fader slider.
    class ChannelStrip::DbRuler final : public juce::Component
    {
    public:
        explicit DbRuler (juce::Slider& s) : slider (s) {}

        void paint (juce::Graphics& g) override
        {
            const float minDb = -60.0f;
            const float maxDb = 12.0f;
            const int top    = 4;
            const int bottom = getHeight() - 4;
            const int trackH = bottom - top;

            auto yForDb = [&] (float dB) -> int
            {
                // Match juce::Slider mid-skew (0 dB at mid).
                const double prop = slider.valueToProportionOfLength ((double) dB);
                // Slider: 0 prop = bottom, 1 prop = top.
                return (int) (bottom - prop * (double) trackH);
            };

            g.setColour (brand::textMuted);
            g.setFont (juce::Font (juce::FontOptions().withHeight (9.0f)));

            const int dBValues[] = { 12, 6, 0, -6, -10, -20, -30, -40, -50, -60 };
            for (int dB : dBValues)
            {
                if ((float) dB < minDb || (float) dB > maxDb) continue;
                const int y = yForDb ((float) dB);
                g.drawHorizontalLine (y, 0.0f, 4.0f);
                g.drawText (juce::String (dB),
                            6, y - 6, getWidth() - 6, 12,
                            juce::Justification::centredLeft, false);
            }
        }

    private:
        juce::Slider& slider;
    };

    // Small clickable colour chip embedded at the top-left of every strip.
    class ChannelStrip::Swatch final : public juce::Component,
                                       public juce::SettableTooltipClient
    {
    public:
        std::function<void()> onClick;
        juce::Colour displayColour;

        void setDisplayColour (juce::Colour c) { displayColour = c; repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            g.setColour (displayColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    };

    class ChannelStrip::StripTimer final : public juce::Timer
    {
    public:
        explicit StripTimer (ChannelStrip& s) : strip (s) { startTimerHz (10); }
        void timerCallback() override
        {
            const float peakLin = strip.state.peak.load (std::memory_order_relaxed);
            const float dB      = juce::Decibels::gainToDecibels (peakLin, -80.0f);
            strip.dbLabel.setText (dB <= -80.0f ? "-inf"
                                                 : juce::String (dB, 1) + " dB",
                                   juce::dontSendNotification);

            const int clips = strip.state.clipCount.load (std::memory_order_relaxed);
            if (clips > 0)
            {
                strip.clipLabel.setText ("CLIP x" + juce::String (clips),
                                         juce::dontSendNotification);
                strip.clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
            }
            else
            {
                strip.clipLabel.setText ("", juce::dontSendNotification);
            }
        }
    private:
        ChannelStrip& strip;
    };

    ChannelStrip::~ChannelStrip() = default;

    void ChannelStrip::setAvailableInputs (int n)
    {
        int current = state.inputRouting.load (std::memory_order_relaxed);
        if (current == -2) current = stripIndex;   // identity default
        inputCombo.clear (juce::dontSendNotification);
        inputCombo.addItem ("(unrouted)", 1);
        for (int i = 0; i < n; ++i)
            inputCombo.addItem ("In " + juce::String (i + 1), i + 2);
        if (current < 0 || current >= n) inputCombo.setSelectedId (1,             juce::dontSendNotification);
        else                              inputCombo.setSelectedId (current + 2, juce::dontSendNotification);
    }

    void ChannelStrip::setAvailableOutputs (int n)
    {
        int current = state.outputRouting.load (std::memory_order_relaxed);
        if (current == -2) current = stripIndex;   // identity default
        outputCombo.clear (juce::dontSendNotification);
        outputCombo.addItem ("(unrouted)", 1);
        for (int i = 0; i < n; ++i)
            outputCombo.addItem ("Out " + juce::String (i + 1), i + 2);
        if (current < 0 || current >= n) outputCombo.setSelectedId (1,             juce::dontSendNotification);
        else                              outputCombo.setSelectedId (current + 2, juce::dontSendNotification);
    }

    juce::Colour ChannelStrip::getResolvedColour() const
    {
        const auto argb = state.colourARGB.load (std::memory_order_relaxed);
        if (argb != 0) return juce::Colour ((juce::uint32) argb);
        return personality;
    }

    void ChannelStrip::mouseDown (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            showContextMenu();
    }

    void ChannelStrip::showContextMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Rename…");
        menu.addItem (2, "Change colour…");
        menu.addItem (3, "Reset colour");
        menu.addSeparator();
        menu.addItem (4, "Reset name");

        menu.showMenuAsync (juce::PopupMenu::Options(),
                            [this] (int chosen)
        {
            switch (chosen)
            {
                case 1: openRenameDialog();           break;
                case 2: openColourPicker();           break;
                case 3: if (colourCb) colourCb (juce::Colour ((juce::uint32) 0)); break;
                case 4: if (renameCb)
                        {
                            renameCb ({});
                            state.name = "In " + juce::String (stripIndex + 1);
                            nameLabel.setText (state.name, juce::dontSendNotification);
                        }
                        break;
                default: break;
            }
        });
    }

    void ChannelStrip::openRenameDialog()
    {
        // Trigger the Label's inline editor — same UX as double-click.
        nameLabel.showEditor();
    }

    void ChannelStrip::openColourPicker()
    {
        if (! colourCb) return;
        auto current = getResolvedColour();

        auto picker = std::make_unique<StripColourPicker> (
            current,
            [this] (juce::Colour chosen)
            {
                if (colourCb) colourCb (chosen);
                if (swatch != nullptr)
                {
                    swatch->setDisplayColour (getResolvedColour());
                }
                repaint();
            });

        auto screenBounds = swatch != nullptr ? swatch->getScreenBounds()
                                              : getScreenBounds();
        juce::CallOutBox::launchAsynchronously (std::move (picker), screenBounds, nullptr);
    }

    ChannelStrip::ChannelStrip (int index, TrackState& s,
                                ColourCallback colourCallback,
                                NameCallback   nameCallback,
                                FloatCallback  gainCallback,
                                FloatCallback  panCallback,
                                IntCallback    inputCallback,
                                IntCallback    outputCallback)
        : stripIndex (index),
          state (s),
          personality (brand::stripColour (index)),
          colourCb (std::move (colourCallback)),
          renameCb (std::move (nameCallback)),
          gainCb   (std::move (gainCallback)),
          panCb    (std::move (panCallback)),
          inputCb  (std::move (inputCallback)),
          outputCb (std::move (outputCallback)),
          spectrum (s),
          meter (s)
    {
        nameLabel.setText (s.name, juce::dontSendNotification);
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        nameLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold")));
        // Double-click to rename inline; single-click does nothing.
        nameLabel.setEditable (false, true, false);
        nameLabel.onTextChange = [this]
        {
            const auto newName = nameLabel.getText().trim();
            state.name = newName.isEmpty() ? juce::String ("In " + juce::String (stripIndex + 1))
                                            : newName;
            nameLabel.setText (state.name, juce::dontSendNotification);
            if (renameCb) renameCb (newName);
        };
        addAndMakeVisible (nameLabel);

        // Input + output routing combos.
        auto styleCombo = [] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
            c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
            c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
            c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
        };
        styleCombo (inputCombo);
        styleCombo (outputCombo);

        // id 1 = unrouted, id 2..N+1 = device channel index (0..N-1).
        inputCombo.onChange = [this]
        {
            const int id = inputCombo.getSelectedId();
            const int dev = (id <= 1) ? -1 : id - 2;
            if (inputCb) inputCb (dev);
        };
        outputCombo.onChange = [this]
        {
            const int id = outputCombo.getSelectedId();
            const int dev = (id <= 1) ? -1 : id - 2;
            if (outputCb) outputCb (dev);
        };
        addAndMakeVisible (inputCombo);
        addAndMakeVisible (outputCombo);

        armButton.setToggleState (s.armed.load(), juce::dontSendNotification);
        armButton.onClick = [this]
        {
            state.armed.store (armButton.getToggleState(), std::memory_order_relaxed);
        };
        armButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        armButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (armButton);

        monButton.setToggleState (s.monitor.load(), juce::dontSendNotification);
        monButton.onClick = [this]
        {
            state.monitor.store (monButton.getToggleState(), std::memory_order_relaxed);
        };
        monButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        monButton.setColour (juce::ToggleButton::tickColourId, brand::accentPlay);
        addAndMakeVisible (monButton);

        muteButton.setToggleState (s.muted.load(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            state.muted.store (muteButton.getToggleState(), std::memory_order_relaxed);
        };
        muteButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        muteButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (muteButton);

        soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
        soloButton.onClick = [this]
        {
            state.soloed.store (soloButton.getToggleState(), std::memory_order_relaxed);
        };
        soloButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        soloButton.setColour (juce::ToggleButton::tickColourId,
                              juce::Colour::fromRGB (0xff, 0xd6, 0x4d));
        addAndMakeVisible (soloButton);

        dbLabel.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
        dbLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        dbLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (dbLabel);

        clipLabel.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
        clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        clipLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (clipLabel);

        addAndMakeVisible (spectrum);

        nameLabel  .setTooltip ("Channel name — double-click to rename, right-click for more.");
        inputCombo .setTooltip ("Hardware input this strip records from.");
        outputCombo.setTooltip ("Hardware output this strip plays VSC audio to.");
        armButton  .setTooltip ("ARM — include this channel when RECORD is rolling.");
        monButton  .setTooltip ("Input monitor — sum this input into the stereo monitor bus (outputs 1 + 2).");
        muteButton .setTooltip ("Mute — silence this channel in monitor + playback. Recording still hits disk.");
        soloButton .setTooltip ("Solo — when any track is soloed, only soloed tracks are audible.");
        spectrum   .setTooltip ("Live FFT spectrum of this channel's input signal.");
        meter      .setTooltip ("Peak + RMS LED meter — click to clear the clip indicator.");
        if (swatch != nullptr)
            swatch->setTooltip ("Click for a colour palette. Right-click the strip for more options.");

        panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.setDoubleClickReturnValue (true, 0.0);
        panSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        panSlider.setColour (juce::Slider::trackColourId,    brand::edge);
        panSlider.setColour (juce::Slider::thumbColourId,    brand::accentStatus);
        panSlider.setValue (s.pan.load(), juce::dontSendNotification);
        panSlider.onValueChange = [this]
        {
            const float v = (float) panSlider.getValue();
            state.pan.store (v, std::memory_order_relaxed);
            if (panCb) panCb (v);
        };
        addAndMakeVisible (panSlider);

        gainFader.setSliderStyle (juce::Slider::LinearVertical);
        gainFader.setRange (-60.0, 12.0, 0.1);
        gainFader.setSkewFactorFromMidPoint (0.0);
        gainFader.setDoubleClickReturnValue (true, 0.0);
        gainFader.setTextValueSuffix (" dB");
        gainFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 14);
        gainFader.setNumDecimalPlacesToDisplay (1);
        gainFader.setColour (juce::Slider::trackColourId,        brand::edge);
        gainFader.setColour (juce::Slider::backgroundColourId,   brand::bgDeep);
        gainFader.setColour (juce::Slider::thumbColourId,        brand::accentStatus);
        gainFader.setColour (juce::Slider::textBoxTextColourId,  brand::textPrimary);
        gainFader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        gainFader.setColour (juce::Slider::textBoxBackgroundColourId,
                             brand::bgDeep.withAlpha (0.4f));
        gainFader.setValue (s.gainDb.load(), juce::dontSendNotification);
        gainFader.onValueChange = [this]
        {
            const float v = (float) gainFader.getValue();
            state.gainDb.store (v, std::memory_order_relaxed);
            if (gainCb) gainCb (v);
        };
        addAndMakeVisible (gainFader);

        // dB scale label strip between the fader and the LED meter.
        dbRuler = std::make_unique<DbRuler> (gainFader);
        addAndMakeVisible (*dbRuler);

        outLabel.setText ("OUT", juce::dontSendNotification);
        outLabel.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
        outLabel.setColour (juce::Label::textColourId, brand::textMuted);
        outLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (outLabel);

        addAndMakeVisible (meter);

        swatch = std::make_unique<Swatch>();
        swatch->setDisplayColour (getResolvedColour());
        swatch->onClick = [this] { openColourPicker(); };
        addAndMakeVisible (*swatch);

        stripTimer = std::make_unique<StripTimer> (*this);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto stripColour = getResolvedColour();

        g.setColour (stripColour);
        g.fillRoundedRectangle (r, 6.0f);

        g.setGradientFill (juce::ColourGradient (
            stripColour, r.getCentreX(), r.getY(),
            stripColour.darker (0.25f), r.getCentreX(), r.getBottom(),
            false));
        g.fillRoundedRectangle (r, 6.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }

    void ChannelStrip::resized()
    {
        auto r = getLocalBounds().reduced (6, 10);

        auto nameRow = r.removeFromTop (18);
        if (swatch != nullptr)
            swatch->setBounds (nameRow.removeFromLeft (16).reduced (1, 2));
        nameLabel.setBounds (nameRow);

        // Input / output routing combos.
        inputCombo .setBounds (r.removeFromTop (20).reduced (2, 1));
        outputCombo.setBounds (r.removeFromTop (20).reduced (2, 1));
        r.removeFromTop (3);

        // 2x2 button grid: ARM | MON / MUTE | SOLO.
        auto row1 = r.removeFromTop (20);
        auto row2 = r.removeFromTop (20);
        const int halfW = row1.getWidth() / 2;
        armButton .setBounds (row1.removeFromLeft (halfW).reduced (2, 1));
        monButton .setBounds (row1.reduced (2, 1));
        muteButton.setBounds (row2.removeFromLeft (halfW).reduced (2, 1));
        soloButton.setBounds (row2.reduced (2, 1));
        r.removeFromTop (4);
        spectrum .setBounds (r.removeFromTop (40));
        r.removeFromTop (2);
        dbLabel  .setBounds (r.removeFromTop (14));
        clipLabel.setBounds (r.removeFromTop (12));
        r.removeFromTop (2);
        panSlider.setBounds (r.removeFromTop (16).reduced (4, 0));
        r.removeFromTop (4);

        outLabel.setBounds (r.removeFromTop (14));

        // Fader | dB ruler | LED meter (left → right).
        const int meterW = 20;
        const int rulerW = 26;
        meter    .setBounds (r.removeFromRight (meterW));
        r.removeFromRight (2);
        if (dbRuler != nullptr)
            dbRuler->setBounds (r.removeFromRight (rulerW));
        r.removeFromRight (2);
        gainFader.setBounds (r);
    }
}
