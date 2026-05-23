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

            // dB tick values (top → bottom) -- matches the reference
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
        // "In 3-4", ...); the underlying ID is still the L channel index.
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
        // -2 default now resolves to -1 (master-only) so a fresh strip
        // shows '→ Master' instead of routing to a hardware output.
        if (current == -2) current = -1;

        const int visible = juce::jmax (n, stripIndex + 1, stereoStrip ? 16 : 8);
        outputCombo.clear (juce::dontSendNotification);
        outputCombo.addItem (juce::String::fromUTF8 ("\xe2\x86\x92 Master"), 1);
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
        // refreshAppearance fires 10x per second. Most ticks nothing
        // has changed -- without change detection the unconditional
        // setColour + repaint at the bottom drives 10 Hz of paint
        // churn per strip even on a silent mixer. Track whether any
        // mirror operation actually mutated state; only repaint if so.
        bool dirty = false;

        // Name
        if (nameLabel.getText() != state.name)
        {
            nameLabel.setText (state.name, juce::dontSendNotification);
            dirty = true;
        }

        // Toggle buttons (R / M / Mu / S)
        const bool armed  = state.armed .load (std::memory_order_relaxed);
        const bool mon    = state.monitor.load (std::memory_order_relaxed);
        const bool muted  = state.muted .load (std::memory_order_relaxed);
        const bool soloed = state.soloed.load (std::memory_order_relaxed);
        if (armButton .getToggleState() != armed)  { armButton .setToggleState (armed,  juce::dontSendNotification); dirty = true; }
        if (monButton .getToggleState() != mon)    { monButton .setToggleState (mon,    juce::dontSendNotification); dirty = true; }
        if (muteButton.getToggleState() != muted)  { muteButton.setToggleState (muted,  juce::dontSendNotification); dirty = true; }
        if (soloButton.getToggleState() != soloed) { soloButton.setToggleState (soloed, juce::dontSendNotification); dirty = true; }

        // Fader + pan
        const float gainDb = state.gainDb.load (std::memory_order_relaxed);
        const float panL   = state.pan   .load (std::memory_order_relaxed);
        if (std::abs ((float) gainFader.getValue() - gainDb) > 0.05f)
        {
            gainFader.setValue (gainDb, juce::dontSendNotification);
            dirty = true;
        }
        if (std::abs ((float) panSlider.getValue() - panL) > 0.01f)
        {
            panSlider.setValue (panL, juce::dontSendNotification);
            dirty = true;
        }
        if (pairState != nullptr)
        {
            const float panR = pairState->pan.load (std::memory_order_relaxed);
            if (std::abs ((float) panSliderR.getValue() - panR) > 0.01f)
            {
                panSliderR.setValue (panR, juce::dontSendNotification);
                dirty = true;
            }
        }

        // Colour mirror -- only push to LookAndFeel if the resolved
        // colour ARGB actually changed (engineer recoloured the strip,
        // VCA group was reassigned, etc.). JUCE's setColour calls
        // mark the slider as dirty whether or not the value changed,
        // so guarding here prevents a 10 Hz repaint storm.
        const auto resolved = getResolvedColour();
        if (resolved.getARGB() != lastAppliedColour)
        {
            lastAppliedColour = resolved.getARGB();
            const auto knobCol = resolved.brighter (0.30f);
            if (swatch != nullptr)
                swatch->setDisplayColour (resolved);
            gainFader.setColour (juce::Slider::thumbColourId, knobCol);
            panSlider.setColour (juce::Slider::thumbColourId, knobCol);
            panSlider.setColour (juce::Slider::rotarySliderFillColourId, knobCol);
            panSliderR.setColour (juce::Slider::thumbColourId, knobCol);
            panSliderR.setColour (juce::Slider::rotarySliderFillColourId, knobCol);
            dirty = true;
        }

        if (dirty)
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
        // Ignore clicks that originated on an actual interactive
        // child (the fader cap, a button, a combo). The bubble-up
        // through addMouseListener(this, true) means we see those
        // events on the strip too; without this guard, dragging a
        // button or fader would also select the strip. originalComponent
        // is the leaf that received the click; if it isn't 'this',
        // the leaf is handling it.
        if (e.eventComponent != this && e.originalComponent != this)
            return;

        // Right-click → context menu.
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            showContextMenu();
            return;
        }

        // Shift / Cmd click anywhere on the strip toggles its
        // multi-selection state. Additive (shift/cmd) keeps existing
        // selection; plain click clears + selects only this one.
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
        {
            if (onToggleSelection) onToggleSelection (true);
            return;
        }

        // Plain left-click: make this strip the sole selection.
        // onToggleSelection(false) means 'clear the rest, then
        // select me'. Allows simple click-to-select without
        // remembering modifiers.
        if (onToggleSelection) onToggleSelection (false);
    }

    void ChannelStrip::setSelected (bool isSelectedNow)
    {
        if (selected == isSelectedNow) return;
        selected = isSelectedNow;
        repaint();
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
        menu.addItem (1, "Rename...");
        menu.addItem (10, "Add channel");
        menu.addItem (11, "Delete channel");
        menu.addSeparator();
        menu.addItem (12, isStereo ? "Unlink stereo pair" : "Link to next channel (stereo)");
        menu.addSeparator();
        menu.addItem (2, "Change colour...");
        menu.addItem (3, "Reset colour");
        menu.addSeparator();
        menu.addItem (4, "Reset name");
        menu.addSeparator();
        menu.addItem (5, "Send to STREAM bus", true, streaming);
        const bool outMuted = state.outputMuted.load (std::memory_order_relaxed);
        menu.addItem (6, outMuted ? "Unmute physical output" : "Mute physical output (FOH only)",
                      true, outMuted);

        // VCA assignment submenu. Engineer picks a bus index 1..8 or
        // "None" to detach. Persisted on engine; mute / gain inherited
        // from the bus.
        juce::PopupMenu vcaMenu;
        const int curVca = state.vcaGroup.load (std::memory_order_relaxed);
        vcaMenu.addItem (700, "None", true, curVca < 0);
        for (int i = 0; i < 8; ++i)
            vcaMenu.addItem (701 + i, "VCA " + juce::String (i + 1),
                             true, curVca == i);
        menu.addSubMenu ("Assign to VCA", vcaMenu);

        // Aux send to a bus track (slot 0 only in the menu -- multi-send
        // gets its own dialog later). Hidden for bus tracks themselves
        // since sending a bus into another bus would feedback loop.
        if (! state.isBus.load (std::memory_order_relaxed))
        {
            juce::PopupMenu sendMenu;
            const int currentTarget = state.sends[0].targetBus.load (std::memory_order_relaxed);
            sendMenu.addItem (800, "None", true, currentTarget < 0);
            if (getBusList)
            {
                int id = 801;
                for (const auto& b : getBusList())
                {
                    sendMenu.addItem (id++, b.second, true, b.first == currentTarget);
                }
            }
            else
            {
                sendMenu.addItem (-1, "(no bus tracks - create one via +CH > Bus)", false);
            }
            menu.addSubMenu ("Send to bus", sendMenu);
        }

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
                            state.name = juce::String (stripIndex + 1);
                            nameLabel.setText (state.name, juce::dontSendNotification);
                        }
                        break;
                case 5: state.streamSend.store (! state.streamSend.load (std::memory_order_relaxed),
                                                std::memory_order_relaxed);
                        break;
                case 6: state.outputMuted.store (! state.outputMuted.load (std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
                        if (pairState)
                            pairState->outputMuted.store (state.outputMuted.load (std::memory_order_relaxed),
                                                           std::memory_order_relaxed);
                        break;
                case 10: if (addCb)        addCb();        break;
                case 11: if (deleteCb)     deleteCb();     break;
                case 12: if (linkStereoCb) linkStereoCb(); break;
                default:
                    // VCA assignment: 700 = None, 701..708 = VCA 1..8.
                    if (chosen == 700 || (chosen >= 701 && chosen <= 708))
                    {
                        const int v = (chosen == 700) ? -1 : (chosen - 701);
                        state.vcaGroup.store (v, std::memory_order_relaxed);
                        if (pairState)
                            pairState->vcaGroup.store (v, std::memory_order_relaxed);
                        if (onVcaGroupChanged) onVcaGroupChanged (v);
                    }
                    // Aux send target: 800 = None, 801..899 = bus index 0..N-1.
                    else if (chosen == 800 || (chosen >= 801 && chosen < 900))
                    {
                        int target = -1;
                        if (chosen >= 801 && getBusList)
                        {
                            const auto list = getBusList();
                            const int idx = chosen - 801;
                            if (idx < (int) list.size()) target = list[(size_t) idx].first;
                        }
                        state.sends[0].targetBus.store (target, std::memory_order_relaxed);
                        if (onSendTargetChanged) onSendTargetChanged (target);
                    }
                    break;
            }
        });
    }

    void ChannelStrip::openRenameDialog()
    {
        // Trigger the Label's inline editor -- same UX as double-click.
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
                                TrackState*    stereoPartner,
                                FloatCallback  panRCallback)
        : stripIndex (index),
          state (s),
          pairState (stereoPartner),
          personality (brand::stripColour (index)),
          colourCb (std::move (colourCallback)),
          renameCb (std::move (nameCallback)),
          gainCb   (std::move (gainCallback)),
          panCb    (std::move (panCallback)),
          panRCb   (std::move (panRCallback)),
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
        nameLabel.setFont (brand::type::channelName());
        // Double-click to rename inline; single-click does nothing.
        nameLabel.setEditable (false, true, false);
        nameLabel.onTextChange = [this]
        {
            const auto newName = nameLabel.getText().trim();
            state.name = newName.isEmpty() ? juce::String (stripIndex + 1)
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
        // R (record arm) → red gradient when armed.
        armButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (armButton);

        monButton.setToggleState (s.monitor.load(), juce::dontSendNotification);
        monButton.onClick = [this]
        {
            state.monitor.store (monButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->monitor.store (monButton.getToggleState(),
                                                     std::memory_order_relaxed);
        };
        // I (input monitor) → green gradient when on.
        monButton.setColour (juce::ToggleButton::tickColourId, brand::accentPlay);
        addAndMakeVisible (monButton);

        muteButton.setToggleState (s.muted.load(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            state.muted.store (muteButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->muted.store (muteButton.getToggleState(),
                                                   std::memory_order_relaxed);
        };
        // M (mute) → orange gradient when on.
        muteButton.setColour (juce::ToggleButton::tickColourId, brand::brandOrange);
        addAndMakeVisible (muteButton);

        soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
        soloButton.onClick = [this]
        {
            state.soloed.store (soloButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->soloed.store (soloButton.getToggleState(),
                                                    std::memory_order_relaxed);
        };
        // S (solo) → yellow gradient when on.
        soloButton.setColour (juce::ToggleButton::tickColourId, brand::accentSolo);
        addAndMakeVisible (soloButton);

        // Live-value readout -- tabular mono so peaks don't visually
        // wobble while the meter is moving.
        dbLabel.setFont (brand::type::mono (11.0f, true));
        dbLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        dbLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (dbLabel);

        clipLabel.setFont (brand::type::mono (10.0f, true));
        clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        clipLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (clipLabel);

        // FFT spectrum removed per user request -- keep the component
        // alive (it feeds off TrackState) but don't show it.

        nameLabel  .setTooltip ("Channel name -- double-click to rename, right-click for more.");
        inputCombo .setTooltip ("Hardware input this strip records from.");
        outputCombo.setTooltip ("Hardware output this strip plays VSC audio to.");
        armButton  .setTooltip ("ARM -- include this channel when RECORD is rolling.");
        monButton  .setTooltip ("Input monitor -- sum this input into the stereo monitor bus (outputs 1 + 2).");
        muteButton .setTooltip ("Mute -- silence this channel in monitor + playback. Recording still hits disk.");
        soloButton .setTooltip ("Solo -- when any track is soloed, only soloed tracks are audible.");
        spectrum   .setTooltip ("Live FFT spectrum of this channel's input signal.");
        meter      .setTooltip ("Peak + RMS LED meter -- click to clear the clip indicator.");
        if (swatch != nullptr)
            swatch->setTooltip ("Click for a colour palette. Right-click the strip for more options.");

        // Reference-screenshot pan readouts:
        //   mono   → "pan  ◂  N  ▸"
        //   stereo → "◂  100  ▸"  on each of two knobs
        auto formatPanText = [] (double v, bool compact)
        {
            const int pct = juce::jlimit (0, 100, juce::roundToInt (std::abs (v) * 100.0));
            const juce::String mid = (pct == 0)
                                       ? juce::String ("0")
                                       : (v < 0.0 ? juce::String ("L") + juce::String (pct)
                                                  : juce::String ("R") + juce::String (pct));
            const auto core = juce::String::fromUTF8 ("\xe2\x97\x82") + "  "
                            + mid + "  "
                            + juce::String::fromUTF8 ("\xe2\x96\xb8");
            return compact ? core : juce::String ("pan  ") + core;
        };

        // Helper to style a pan rotary identically for L (and optional R).
        auto stylePanKnob = [this] (juce::Slider& s)
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setRotaryParameters (juce::MathConstants<float>::pi * 1.20f,
                                   juce::MathConstants<float>::pi * 2.80f,
                                   true);
            s.setRange (-1.0, 1.0, 0.01);
            s.setDoubleClickReturnValue (true, 0.0);
            s.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s.setColour (juce::Slider::rotarySliderFillColourId,
                         getResolvedColour().brighter (0.30f));
            s.setColour (juce::Slider::rotarySliderOutlineColourId, brand::edge);
            s.setColour (juce::Slider::thumbColourId,
                         getResolvedColour().brighter (0.30f));
            s.setMouseDragSensitivity (160);
        };

        stylePanKnob (panSlider);
        // Trackpad / mouse-wheel scrolling should NOT change the pan
        // -- the engineer scrolls the mixer with the trackpad and
        // doesn't want every two-finger swipe nudging values around.
        // The slider still responds to direct click+drag and to
        // double-click for centre.
        panSlider.setScrollWheelEnabled (false);
        panSlider.setTooltip ("Pan L -- drag to set L100..0..R100. Double-click for centre.");
        panSlider.setValue (s.pan.load(), juce::dontSendNotification);
        panSlider.onValueChange = [this, formatPanText]
        {
            const float v = (float) panSlider.getValue();
            state.pan.store (v, std::memory_order_relaxed);
            // Mono strips broadcast to the pair sibling so the partner
            // mirrors. Stereo strips have independent L+R pans.
            if (pairState != nullptr)
            {
                // independent → don't sync R
            }
            if (panCb) panCb (v);
            panLabel.setText (formatPanText (v, pairState != nullptr), juce::dontSendNotification);
        };
        addAndMakeVisible (panSlider);

        // Pan label below the knob(s).
        panLabel.setFont (brand::type::mono (11.0f, true));
        panLabel.setColour (juce::Label::textColourId, brand::accentStatus);
        panLabel.setJustificationType (juce::Justification::centred);
        panLabel.setText (formatPanText (s.pan.load(), pairState != nullptr), juce::dontSendNotification);
        addAndMakeVisible (panLabel);

        // For stereo strips: a second pan knob + label for R.
        if (pairState != nullptr)
        {
            stylePanKnob (panSliderR);
            panSliderR.setScrollWheelEnabled (false);
            panSliderR.setTooltip ("Pan R -- drag to set L100..0..R100. Double-click for centre.");
            panSliderR.setValue (pairState->pan.load(), juce::dontSendNotification);
            panSliderR.onValueChange = [this, formatPanText]
            {
                const float v = (float) panSliderR.getValue();
                if (pairState != nullptr)
                    pairState->pan.store (v, std::memory_order_relaxed);
                // Persist the R-side pan via its own callback so the
                // L/R values are independent and both survive a relaunch.
                if (panRCb) panRCb (v);
                panLabelR.setText (formatPanText (v, true), juce::dontSendNotification);
            };
            addAndMakeVisible (panSliderR);

            panLabelR.setFont (brand::type::mono (11.0f, true));
            panLabelR.setColour (juce::Label::textColourId, brand::accentStatus);
            panLabelR.setJustificationType (juce::Justification::centred);
            panLabelR.setText (formatPanText (pairState->pan.load(), true), juce::dontSendNotification);
            addAndMakeVisible (panLabelR);
        }

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
        // The fader value must NEVER jump when the engineer clicks
        // somewhere on the strip body to select / right-click. Only a
        // direct drag of the thumb cap should move it. Disabling
        // "snap to mouse position" prevents click-on-track-to-jump.
        // Combined with the fader's narrow visual track, the engineer
        // has to actually grab the cap to change gain.
        gainFader.setSliderSnapsToMousePosition (false);
        gainFader.setColour (juce::Slider::textBoxTextColourId,  brand::textPrimary);
        gainFader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        gainFader.setColour (juce::Slider::textBoxBackgroundColourId,
                             brand::bgDeep.withAlpha (0.4f));
        // Same reasoning as the pan knob: scroll-wheel / trackpad
        // gestures should NEVER move the gain. The fader changes only
        // on a direct click+drag interaction.
        gainFader.setScrollWheelEnabled (false);
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
        outLabel.setFont (brand::type::ledLabel());
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
        // right-click anywhere on the strip -- even on a button or fader --
        // opens the context menu.
        addMouseListener (this, true);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        auto stripColour = getResolvedColour();

        // Hover lift -- mouse over the strip brightens the wash ~6% so
        // the engineer reads 'this is the strip my pointer is on'
        // without needing a focus ring or selection state.
        if (hovered) stripColour = stripColour.brighter (0.06f);

        // Vertical gradient wash in the personality colour -- matches
        // ZynForge Live's strip finish and gives every channel a sense
        // of depth instead of a flat block.
        g.setGradientFill (brand::verticalGradient (stripColour, r, 0.18f, 0.28f));
        g.fillRoundedRectangle (r, brand::radius::xl);

        // Hover border -- a touch brighter than the resting edge so the
        // strip lifts subtly off the canvas.
        const float edgeAlpha = hovered ? 0.55f : 0.30f;
        g.setColour (stripColour.brighter (0.40f).withAlpha (edgeAlpha));
        g.drawRoundedRectangle (r, brand::radius::xl, 1.0f);

        // Multi-select highlight -- a 2 px accent-yellow outline so a
        // group of selected strips is unambiguous from the side of the
        // room while still letting the strip's personality colour
        // dominate.
        if (selected)
        {
            g.setColour (brand::accentSolo);
            g.drawRoundedRectangle (r.reduced (1.0f), brand::radius::xl, 2.5f);
        }

        // VCA badge -- small chip in the top-right of the strip so the
        // engineer can see at a glance which group every strip is on,
        // without right-clicking each one.
        const int vca = state.vcaGroup.load (std::memory_order_relaxed);
        if (vca >= 0 && vca < 8)
        {
            const juce::String label = "V" + juce::String (vca + 1);
            auto badge = juce::Rectangle<float> (r.getRight() - 26.0f, r.getY() + 4.0f, 22.0f, 12.0f);
            g.setColour (brand::featureEngaged.darker (0.30f));
            g.fillRoundedRectangle (badge, 2.5f);
            g.setColour (brand::featureEngaged.brighter (0.40f));
            g.drawRoundedRectangle (badge, 2.5f, 0.75f);
            g.setColour (juce::Colours::white);
            g.setFont (brand::fonts::small());
            g.drawText (label, badge.toNearestInt(), juce::Justification::centred, false);
        }

        // BUS badge -- top-right, mirroring the VCA chip position.
        // Bus strips and VCA-grouped strips are mutually exclusive
        // (a bus is itself a group target, never a member of one),
        // so the two chips can share the same coordinates without
        // colliding. The previous top-left position overlapped the
        // strip's colour swatch.
        if (state.isBus.load (std::memory_order_relaxed))
        {
            auto badge = juce::Rectangle<float> (r.getRight() - 30.0f, r.getY() + 4.0f, 26.0f, 12.0f);
            g.setColour (brand::alertAmber.darker (0.30f));
            g.fillRoundedRectangle (badge, 2.5f);
            g.setColour (brand::alertAmber.brighter (0.20f));
            g.drawRoundedRectangle (badge, 2.5f, 0.75f);
            g.setColour (juce::Colours::white);
            g.setFont (brand::fonts::small());
            g.drawText ("BUS", badge.toNearestInt(), juce::Justification::centred, false);
        }
    }

    void ChannelStrip::mouseEnter (const juce::MouseEvent&)
    {
        if (! hovered) { hovered = true; repaint(); }
    }

    void ChannelStrip::mouseExit (const juce::MouseEvent& e)
    {
        // mouseListener forwarding means children fire mouseExit too --
        // only clear the hover when the cursor has actually left the
        // strip's local bounds.
        if (! getLocalBounds().contains (e.getEventRelativeTo (this).getPosition()))
        {
            if (hovered) { hovered = false; repaint(); }
        }
    }

    void ChannelStrip::resized()
    {
        auto r = getLocalBounds().reduced (6, 8);

        // ── 1. Name + colour swatch (top of strip) ─────────────────
        auto nameRow = r.removeFromTop (18);
        if (swatch != nullptr)
            swatch->setBounds (nameRow.removeFromLeft (brand::space::xl).reduced (1, 2));
        nameLabel.setBounds (nameRow);
        r.removeFromTop (brand::space::xs);

        // Routing combos: keep them in flow but small -- they live above
        // the pan section in the existing UX (engineer can also use the
        // PATCH page). 18px each.
        inputCombo .setBounds (r.removeFromTop (18).reduced (2, 1));
        outputCombo.setBounds (r.removeFromTop (18).reduced (2, 1));
        r.removeFromTop (brand::space::sm);

        // ── 2. Pan section ─────────────────────────────────────────
        // Bigger knobs -- engineers wanted them obvious at a glance.
        // Mono: one knob (72 px) centred + "pan  ◂ N ▸" readout.
        // Stereo: two knobs (56 px each) side by side + two compact
        // "◂ N ▸" readouts under each.
        const int knobH    = (pairState != nullptr) ? 56 : 72;
        const int panLabelH = 16;
        auto panKnobs = r.removeFromTop (knobH);
        auto panText  = r.removeFromTop (panLabelH);
        if (pairState != nullptr)
        {
            const int colW = panKnobs.getWidth() / 2;
            const int knobSize = juce::jmin (knobH, colW - 4);
            panSlider .setBounds (panKnobs.removeFromLeft (colW)
                                          .withSizeKeepingCentre (knobSize, knobSize));
            panSliderR.setBounds (panKnobs.withSizeKeepingCentre (knobSize, knobSize));
            panLabel  .setBounds (panText .removeFromLeft (colW));
            panLabelR .setBounds (panText);
        }
        else
        {
            const int knobSize = juce::jmin (knobH, panKnobs.getWidth() - 4);
            panSlider.setBounds (panKnobs.withSizeKeepingCentre (knobSize, knobSize));
            panLabel .setBounds (panText);
        }
        r.removeFromTop (brand::space::sm);

        // ── 3. Two rows of two buttons each: [ I | R ] / [ S | M ] ──
        //   ...except on the metronome strip ("Click"), which is
        //   playback-only -- record-arm and input-monitor make no sense
        //   so those two pills are hidden, and the SOLO + MUTE row sits
        //   alone with no gap above it.
        const bool isClickStrip = (state.name == "Click");
        const bool isBusStrip   = state.isBus.load (std::memory_order_relaxed);
        const int btnH = 24;
        const int btnGap = 3;

        // Touch-target compliance: at XS strip width (~56-70 px) the
        // 2×2 button grid gives each button ~26×24 -- below Apple HIG's
        // 44 pt minimum and below WCAG 2.5.5 (24×24 minimum, 44×44
        // enhanced). Auto-stack vertically when the strip is narrower
        // than 78 px so each toggle gets full width (~70+ px) at the
        // cost of one extra row of height. Engineers running at XS for
        // density still get a usable hit zone.
        const bool stack = getWidth() < 78;
        const int rowsForButtons = stack ? 4 : 2;

        if (isClickStrip || isBusStrip)
        {
            // Bus / click tracks have no input -- no R / I buttons.
            // Reserve the missing row(s) as empty space above the
            // remaining S / M row so the fader + meter below start at
            // the same Y as a full audio strip's. Without this, the
            // fader on a bus strip sits ~27 px higher than its
            // neighbours and meters across the mixer don't line up.
            armButton.setVisible (false); armButton.setBounds ({});
            monButton.setVisible (false); monButton.setBounds ({});
            if (stack)
            {
                // Two missing rows (mon + arm).
                r.removeFromTop (btnH);  r.removeFromTop (btnGap);
                r.removeFromTop (btnH);  r.removeFromTop (btnGap);
                soloButton.setBounds (r.removeFromTop (btnH).reduced (2, 0));
                r.removeFromTop (btnGap);
                muteButton.setBounds (r.removeFromTop (btnH).reduced (2, 0));
            }
            else
            {
                // One missing row above (the mon / arm row).
                r.removeFromTop (btnH);  r.removeFromTop (btnGap);
                auto row = r.removeFromTop (btnH);
                const int halfW = row.getWidth() / 2;
                soloButton.setBounds (row.removeFromLeft (halfW).reduced (2, 0));
                muteButton.setBounds (row.reduced (2, 0));
            }
        }
        else
        {
            armButton.setVisible (true);
            monButton.setVisible (true);
            if (stack)
            {
                monButton .setBounds (r.removeFromTop (btnH).reduced (2, 0));
                r.removeFromTop (btnGap);
                armButton .setBounds (r.removeFromTop (btnH).reduced (2, 0));
                r.removeFromTop (btnGap);
                soloButton.setBounds (r.removeFromTop (btnH).reduced (2, 0));
                r.removeFromTop (btnGap);
                muteButton.setBounds (r.removeFromTop (btnH).reduced (2, 0));
            }
            else
            {
                auto row1 = r.removeFromTop (btnH);
                r.removeFromTop (btnGap);
                auto row2 = r.removeFromTop (btnH);
                const int halfW = row1.getWidth() / 2;
                monButton.setBounds (row1.removeFromLeft (halfW).reduced (2, 0));
                armButton.setBounds (row1.reduced (2, 0));
                soloButton.setBounds (row2.removeFromLeft (halfW).reduced (2, 0));
                muteButton.setBounds (row2.reduced (2, 0));
            }
        }
        juce::ignoreUnused (rowsForButtons);
        r.removeFromTop (brand::space::sm);

        spectrum .setBounds ({});  // hidden
        clipLabel.setBounds (r.removeFromTop (12));

        // ── 4. Fader area: [ ruler | fader | meter ] ───────────────
        // dB readout sits at the bottom; reserve 16 px before laying
        // out the column.
        auto bottom = r.removeFromBottom (brand::space::xl);
        dbLabel.setBounds (bottom);

        // Ruler is now on the LEFT (matches the reference: numbers
        // 12/6/0/5/10/.../60/∞ to the left of the fader cap).
        const int meterW = (pairState != nullptr) ? 38 : 30;
        const int rulerW = 24;

        if (dbRuler != nullptr)
            dbRuler->setBounds (r.removeFromLeft (rulerW));
        r.removeFromLeft (2);
        meter.setBounds (r.removeFromRight (meterW));
        r.removeFromRight (2);
        gainFader.setBounds (r);

        outLabel.setBounds ({});  // collapsed -- output combo serves the role
    }
}
