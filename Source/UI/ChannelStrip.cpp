#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
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
            const float maxDb =  12.0f;
            const int top    = 6;
            const int bottom = getHeight() - 6;
            const int trackH = bottom - top;

            auto yForDb = [&] (float dB) -> int
            {
                const double prop = slider.valueToProportionOfLength ((double) dB);
                return (int) (bottom - prop * (double) trackH);
            };

            g.setColour (brand::textMuted);
            g.setFont (brand::type::label());

            // dB tick values (top → bottom) — matches the reference
            // screenshot. Positive values keep the '+' sign, negatives
            // drop the minus to keep the column tight.
            const int dBValues[] = { 12, 6, 0, -5, -10, -15, -20, -30, -40, -60 };
            for (int dB : dBValues)
            {
                if ((float) dB < minDb || (float) dB > maxDb) continue;
                const int y = yForDb ((float) dB);
                g.drawHorizontalLine (y, 0.0f, 4.0f);
                const auto txt = (dB > 0) ? "+" + juce::String (dB)
                                          : juce::String (std::abs (dB));
                g.drawText (txt, 6, y - 6, getWidth() - 6, 12,
                            juce::Justification::centredLeft, false);
            }

            // Infinity glyph at the bottom of the range.
            g.drawText (juce::String::fromUTF8 ("\xe2\x88\x9e"),
                        6, bottom - 12, getWidth() - 6, 12,
                        juce::Justification::centredLeft, false);
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
        const bool stereoStrip = (pairState != nullptr);
        int current = state.inputRouting.load (std::memory_order_relaxed);
        if (current == -2) current = stripIndex;   // identity default

        // Populate enough items so a strip's identity input always has
        // a visible entry. For stereo strips we list pairs ("In 1-2",
        // "In 3-4", …); the underlying ID is still the L channel index.
        const int visible = juce::jmax (n, stripIndex + 1, stereoStrip ? 16 : 8);
        inputCombo.clear (juce::dontSendNotification);
        inputCombo.addItem ("(unrouted)", 1);
        const int stepCh = stereoStrip ? 2 : 1;
        for (int i = 0; i < visible; i += stepCh)
        {
            const bool live = stereoStrip ? (i + 1 < n) : (i < n);
            const auto label = stereoStrip
                ? juce::String ("In ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                : juce::String ("In ") + juce::String (i + 1);
            inputCombo.addItem (live ? label : (label + " (off)"), i + 2);
        }
        if (current < 0) inputCombo.setSelectedId (1,            juce::dontSendNotification);
        else             inputCombo.setSelectedId (current + 2, juce::dontSendNotification);
    }

    void ChannelStrip::setAvailableOutputs (int n)
    {
        const bool stereoStrip = (pairState != nullptr);
        int current = state.outputRouting.load (std::memory_order_relaxed);
        if (current == -2) current = stripIndex;   // identity default

        const int visible = juce::jmax (n, stripIndex + 1, stereoStrip ? 16 : 8);
        outputCombo.clear (juce::dontSendNotification);
        outputCombo.addItem ("(unrouted)", 1);
        const int stepCh = stereoStrip ? 2 : 1;
        for (int i = 0; i < visible; i += stepCh)
        {
            const bool live = stereoStrip ? (i + 1 < n) : (i < n);
            const auto label = stereoStrip
                ? juce::String ("Out ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                : juce::String ("Out ") + juce::String (i + 1);
            outputCombo.addItem (live ? label : (label + " (off)"), i + 2);
        }
        if (current < 0) outputCombo.setSelectedId (1,            juce::dontSendNotification);
        else             outputCombo.setSelectedId (current + 2, juce::dontSendNotification);
    }

    void ChannelStrip::refreshAppearance()
    {
        // Name: TrackState is authoritative; only update if it changed.
        if (nameLabel.getText() != state.name)
            nameLabel.setText (state.name, juce::dontSendNotification);

        // Colour: re-resolve and push to the swatch + sliders so the
        // fader fill / pan thumb track the live channel colour.
        const auto resolved = getResolvedColour();
        const auto knobCol  = resolved.brighter (0.30f);
        if (swatch != nullptr)
            swatch->setDisplayColour (resolved);
        gainFader.setColour (juce::Slider::thumbColourId, knobCol);
        panSlider.setColour (juce::Slider::thumbColourId, knobCol);
        repaint();
    }

    void ChannelStrip::refreshRoutingSelection()
    {
        auto pick = [] (juce::ComboBox& box, int routing) -> int
        {
            // Map routing-int back to the combo's item id system:
            // id 1 = unrouted, id 2..N+1 = device channel 0..N-1
            if (routing < 0) return 1;
            return routing + 2;
        };
        const int inR  = state.inputRouting .load (std::memory_order_relaxed);
        const int outR = state.outputRouting.load (std::memory_order_relaxed);
        const int wantedIn  = (inR  == -2) ? stripIndex + 2 : pick (inputCombo,  inR);
        const int wantedOut = (outR == -2) ? stripIndex + 2 : pick (outputCombo, outR);

        if (inputCombo .getSelectedId() != wantedIn)
            inputCombo .setSelectedId (wantedIn,  juce::dontSendNotification);
        if (outputCombo.getSelectedId() != wantedOut)
            outputCombo.setSelectedId (wantedOut, juce::dontSendNotification);
    }

    juce::Colour ChannelStrip::getResolvedColour() const
    {
        const auto argb = state.colourARGB.load (std::memory_order_relaxed);
        if (argb != 0) return juce::Colour ((juce::uint32) argb);
        return personality;
    }

    void ChannelStrip::mouseDown (const juce::MouseEvent& e)
    {
        // Only react to right-click — left-clicks go straight to whichever
        // child (button / fader / combo) received them.
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            showContextMenu();
    }

    void ChannelStrip::setMenuCallbacks (VoidCallback onDelete,
                                         VoidCallback onAdd,
                                         VoidCallback onLinkStereo,
                                         IntCallback  onLinkToOther)
    {
        deleteCb     = std::move (onDelete);
        addCb        = std::move (onAdd);
        linkStereoCb = std::move (onLinkStereo);
        linkOtherCb  = std::move (onLinkToOther);
    }

    void ChannelStrip::showContextMenu()
    {
        const bool streaming = state.streamSend.load (std::memory_order_relaxed);
        const bool isStereo  = state.isStereo .load (std::memory_order_relaxed);

        juce::PopupMenu menu;
        menu.addItem (1, "Rename…");
        menu.addItem (10, "Add channel");
        menu.addItem (11, "Delete channel");
        menu.addSeparator();
        menu.addItem (12, isStereo ? "Unlink stereo pair" : "Link to next channel (stereo)");
        menu.addSeparator();
        menu.addItem (2, "Change colour…");
        menu.addItem (3, "Reset colour");
        menu.addSeparator();
        menu.addItem (4, "Reset name");
        menu.addSeparator();
        menu.addItem (5, "Send to STREAM bus", true, streaming);

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
                case 5: state.streamSend.store (! state.streamSend.load (std::memory_order_relaxed),
                                                std::memory_order_relaxed);
                        break;
                case 10: if (addCb)        addCb();        break;
                case 11: if (deleteCb)     deleteCb();     break;
                case 12: if (linkStereoCb) linkStereoCb(); break;
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
                                IntCallback    outputCallback,
                                TrackState*    stereoPartner)
        : stripIndex (index),
          state (s),
          pairState (stereoPartner),
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
        // When this strip controls a stereo pair, point the meter at the R
        // partner so it can draw two bars side-by-side.
        if (pairState != nullptr)
            meter.setStereoPartner (pairState);
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
            if (pairState) pairState->armed.store (armButton.getToggleState(),
                                                   std::memory_order_relaxed);
        };
        armButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        armButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (armButton);

        monButton.setToggleState (s.monitor.load(), juce::dontSendNotification);
        monButton.onClick = [this]
        {
            state.monitor.store (monButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->monitor.store (monButton.getToggleState(),
                                                     std::memory_order_relaxed);
        };
        monButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        monButton.setColour (juce::ToggleButton::tickColourId, brand::accentPlay);
        addAndMakeVisible (monButton);

        muteButton.setToggleState (s.muted.load(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            state.muted.store (muteButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->muted.store (muteButton.getToggleState(),
                                                   std::memory_order_relaxed);
        };
        muteButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        muteButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (muteButton);

        soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
        soloButton.onClick = [this]
        {
            state.soloed.store (soloButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->soloed.store (soloButton.getToggleState(),
                                                    std::memory_order_relaxed);
        };
        soloButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        soloButton.setColour (juce::ToggleButton::tickColourId, brand::accentSolo);
        addAndMakeVisible (soloButton);

        dbLabel.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
        dbLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        dbLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (dbLabel);

        clipLabel.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
        clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        clipLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (clipLabel);

        // FFT spectrum removed per user request — keep the component
        // alive (it feeds off TrackState) but don't show it.

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
        panSlider.setColour (juce::Slider::thumbColourId,    getResolvedColour().brighter (0.30f));
        // Touch-friendly drag — slightly higher sensitivity so the thumb
        // tracks finger movement 1:1 instead of accelerating away.
        panSlider.setMouseDragSensitivity (160);
        panSlider.setValue (s.pan.load(), juce::dontSendNotification);
        panSlider.onValueChange = [this]
        {
            const float v = (float) panSlider.getValue();
            state.pan.store (v, std::memory_order_relaxed);
            if (pairState) pairState->pan.store (v, std::memory_order_relaxed);
            if (panCb) panCb (v);
        };
        addAndMakeVisible (panSlider);

        gainFader.setSliderStyle (juce::Slider::LinearVertical);
        gainFader.setRange (-60.0, 12.0, 0.1);
        gainFader.setSkewFactorFromMidPoint (-15.0);   // log-ish console feel
        gainFader.setDoubleClickReturnValue (true, 0.0);
        gainFader.setTextValueSuffix (" dB");
        gainFader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 14);
        gainFader.setNumDecimalPlacesToDisplay (1);
        gainFader.setColour (juce::Slider::trackColourId,        brand::edge);
        gainFader.setColour (juce::Slider::backgroundColourId,   brand::bgDeep);
        gainFader.setColour (juce::Slider::thumbColourId,        getResolvedColour().brighter (0.30f));
        gainFader.setMouseDragSensitivity (250);
        gainFader.setColour (juce::Slider::textBoxTextColourId,  brand::textPrimary);
        gainFader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        gainFader.setColour (juce::Slider::textBoxBackgroundColourId,
                             brand::bgDeep.withAlpha (0.4f));
        gainFader.setValue (s.gainDb.load(), juce::dontSendNotification);
        gainFader.onValueChange = [this]
        {
            const float v = (float) gainFader.getValue();
            state.gainDb.store (v, std::memory_order_relaxed);
            if (pairState) pairState->gainDb.store (v, std::memory_order_relaxed);
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

        // Bubble every child component's mouseDown up to this strip so a
        // right-click anywhere on the strip — even on a button or fader —
        // opens the context menu.
        addMouseListener (this, true);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto stripColour = getResolvedColour();

        // Vertical gradient wash in the personality colour — matches
        // ZynForge Live's strip finish and gives every channel a sense
        // of depth instead of a flat block.
        g.setGradientFill (brand::verticalGradient (stripColour, r, 0.18f, 0.28f));
        g.fillRoundedRectangle (r, brand::radius::xl);

        g.setColour (stripColour.brighter (0.40f).withAlpha (0.30f));
        g.drawRoundedRectangle (r, brand::radius::xl, 1.0f);
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
        spectrum.setBounds ({});  // hidden
        r.removeFromTop (2);
        dbLabel  .setBounds (r.removeFromTop (14));
        clipLabel.setBounds (r.removeFromTop (12));
        r.removeFromTop (2);
        panSlider.setBounds (r.removeFromTop (16).reduced (4, 0));
        r.removeFromTop (4);

        outLabel.setBounds (r.removeFromTop (14));

        // Fader | dB ruler | LED meter (left → right).
        const int meterW    = 30;   // wider so the meter's own dB labels fit
        const int rulerW    = 28;
        const int headroomH = 32;   // empty space above the fader so the
                                    // 0 dB mark sits where the thumb rests,
                                    // not at the absolute top of the strip.

        // Meter spans the full remaining height — it represents the live
        // signal level and should always show top peaks.
        meter.setBounds (r.removeFromRight (meterW));
        r.removeFromRight (2);

        // Fader + ruler share a column that starts headroomH below the top.
        auto faderCol = r;
        faderCol.removeFromTop (headroomH);
        if (dbRuler != nullptr)
            dbRuler->setBounds (faderCol.removeFromRight (rulerW));
        faderCol.removeFromRight (2);
        gainFader.setBounds (faderCol);
    }
}
