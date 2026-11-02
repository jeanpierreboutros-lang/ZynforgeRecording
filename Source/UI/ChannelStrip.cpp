#include "ChannelStrip.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "StripColourPicker.h"

// Strip paint helpers. Flat: the channel colour lives in the header and the
// spine, so the fader and meter sit on one calm body on every strip.
namespace
{
    namespace steel
    {
        // A take rolling on this strip is the one thing that lights the spine.
        inline void drawSpine (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour,
                               bool rolling, float pulse)
        {
            if (rolling)
            {
                const auto hot = zynforge::brand::structuralForge();
                g.setColour (hot.withAlpha ((0.34f + 0.30f * pulse)));
                g.fillRect (r.getX(), r.getY(), 9.0f + 5.0f * pulse, r.getHeight());
                colour = hot;
            }
            g.setColour (colour);
            g.fillRect (r.getX(), r.getY(), 4.0f, r.getHeight());
        }

        // Channel number: a small line above the name, which is what gets read.
        inline void drawNumber (juce::Graphics& g, juce::Rectangle<int> box, int channel1Based)
        {
            g.setFont (zynforge::brand::type::mono (10.0f, true));
            g.setColour (zynforge::brand::textPrimary);
            g.drawText (juce::String (channel1Based).paddedLeft ('0', 2), box,
                        juce::Justification::centredLeft, false);
        }
    }
}

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

            // Crisp tabular (mono, bold) numerals -- a dB scale must be
            // glanceable in a dark venue, so legibility wins over subtlety.
            g.setFont (brand::type::mono (10.0f, true));

            // Draw a label with a 1 px dark drop-shadow then a bright face, so
            // it stays readable both over the dark track AND over the coloured
            // fader fill below the thumb.
            auto drawScaleLabel = [&] (const juce::String& txt, int y)
            {
                const auto box = juce::Rectangle<int> (6, y - 6, getWidth() - 6, 12);
                g.setColour (brand::shadow::elev3());
                g.drawText (txt, box.translated (0, 1), juce::Justification::centredLeft, false);
                g.setColour (brand::textSecondary);
                g.drawText (txt, box, juce::Justification::centredLeft, false);
            };

            // dB tick values (top → bottom). Positive values keep the '+' sign,
            // negatives drop the minus to keep the column tight.
            const int dBValues[] = { 12, 6, 0, -5, -10, -15, -20, -30, -40, -60 };
            for (int dB : dBValues)
            {
                if ((float) dB < minDb || (float) dB > maxDb) continue;
                const int y = yForDb ((float) dB);
                g.setColour (brand::textTertiary);
                g.drawHorizontalLine (y, 0.0f, 4.0f);
                drawScaleLabel ((dB > 0) ? "+" + juce::String (dB)
                                         : juce::String (std::abs (dB)), y);
            }

            // Infinity glyph at the bottom of the range.
            drawScaleLabel (juce::String::fromUTF8 ("\xe2\x88\x9e"), bottom - 6);
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
            g.fillRoundedRectangle (r, brand::radius::sm);
            g.setColour (brand::gloss (zynforge::brand::alpha::dimmed));
            g.drawRoundedRectangle (r, brand::radius::sm, 1.0f);
        }

        void mouseDown (const juce::MouseEvent&) override { if (onClick && isEnabled()) onClick(); }
    };

    class ChannelStrip::StripTimer final : public juce::Timer
    {
    public:
        explicit StripTimer (ChannelStrip& s) : strip (s) { startTimerHz (10); }
        void timerCallback() override
        {
            // The strip's backing TrackState is destroyed when the strip is
            // deleted in-place (invalidate() stops this timer first, but bail
            // defensively so a queued callback can't touch freed memory).
            if (! strip.stripValid) { stopTimer(); return; }

            const float peakLin = strip.state.peak.load (std::memory_order_relaxed);
            const float dB      = juce::Decibels::gainToDecibels (peakLin, -80.0f);
            // Only push text into the label when it actually changed -- an
            // idle/silent strip otherwise re-lays-out its labels 10x/s for
            // nothing (this runs on 32 strips at once).
            const juce::String dbText = dB <= -80.0f ? juce::String ("-inf")
                                                      : juce::String (dB, 1) + " dB";
            if (dbText != lastDbText)
            {
                strip.dbLabel.setText (dbText, juce::dontSendNotification);
                lastDbText = dbText;
            }

            const int clips = strip.state.clipCount.load (std::memory_order_relaxed);
            if (clips != lastClips)
            {
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
                lastClips = clips;
            }

            // Let the audio thread publish its next FFT snapshot, so the
            // soundcheck report reads the strip as it is now.
            strip.state.fftBlockReady.store (false, std::memory_order_release);

            // While a take rolls on this strip, repaint just the left spine
            // column at 10 Hz so it breathes. Cheap: the rest of the strip stays
            // static, honouring the 24-strip repaint budget. Skipped under
            // reduced-motion (the spine then holds a static glow).
            if (! zynforge::brand::reduceMotion() && strip.takeRolling
                && strip.state.armed.load (std::memory_order_relaxed))
                strip.repaint (0, 0, 16, strip.getHeight());
        }
    private:
        ChannelStrip& strip;
        juce::String  lastDbText;
        int           lastClips { -1 };
    };

    ChannelStrip::~ChannelStrip() = default;

    void ChannelStrip::invalidate()
    {
        stripValid = false;
        // stopTimer() guarantees no further StripTimer::timerCallback after it
        // returns (message-thread timer), so the freed TrackState& can't be
        // read once the caller destroys it.
        if (stripTimer != nullptr)
            stripTimer->stopTimer();
        // The strip's meter runs its OWN timer against the same TrackState& --
        // stopping just stripTimer left it reading freed memory for up to a
        // rebuild interval after the strip was condemned.
        meter.detach();
    }

    void ChannelStrip::setAvailableInputs (int n)
    {
        const bool stereoStrip = (pairState != nullptr);
        availableInputs = n;
        // Show what the strip hears on THIS device (a saved patch past a
        // smaller device's inputs wraps live; the saved patch is unchanged).
        int current = effectiveInputRouting (stripIndex, state.inputRouting.load (std::memory_order_relaxed), n);
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
        ensureRoutingItem (inputCombo, current, n, "In ");
        if (current < 0) inputCombo.setSelectedId (1,            juce::dontSendNotification);
        else             inputCombo.setSelectedId (current + 2, juce::dontSendNotification);
    }

    // A stereo strip lists the odd pairs (In 1-2, In 3-4), but a pair can sit on
    // an even channel (In 6-7). Selecting an id the box doesn't hold blanks it,
    // so the real entry is added on demand, as the EDIT row does.
    void ChannelStrip::ensureRoutingItem (juce::ComboBox& box, int dev, int channels, const char* prefix)
    {
        if (dev < 0 || box.indexOfItemId (dev + 2) >= 0) return;
        const bool stereoStrip = (pairState != nullptr);
        auto label = juce::String (prefix) + juce::String (dev + 1);
        if (stereoStrip) label << "-" << juce::String (dev + 2);
        const bool live = stereoStrip ? (dev + 1 < channels) : (dev < channels);
        box.addItem (live ? label : (label + " (off)"), dev + 2);
    }

    void ChannelStrip::setAvailableOutputs (int n)
    {
        availableOutputs = n;
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
        ensureRoutingItem (outputCombo, current, n, "Out ");
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
            setTitle (state.name);   // keep the strip's accessible name in sync
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

        // Fader + pan.
        const float gainDb = state.gainDb.load (std::memory_order_relaxed);
        const float panL = state.pan.load (std::memory_order_relaxed);
        if (std::abs ((float) gainFader.getValue() - gainDb) > 0.05f)
        {
            gainFader.setValue (gainDb, juce::dontSendNotification);
            showFaderValue();
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
        // etc.). JUCE's setColour calls
        // mark the slider as dirty whether or not the value changed,
        // so guarding here prevents a 10 Hz repaint storm.
        const auto resolved = getResolvedColour();
        if (resolved.getARGB() != lastAppliedColour)
        {
            lastAppliedColour = resolved.getARGB();
            if (swatch != nullptr)
                swatch->setDisplayColour (resolved);
            dirty = true;
        }

        // If an edit-group badge just appeared or disappeared,
        // re-lay-out so the name label reclaims or yields the badge's space.
        const int badgeNow = state.editGroup.load (std::memory_order_relaxed) >= 0 ? 1 : 0;
        if (badgeNow != lastBadgeShown)
        {
            lastBadgeShown = badgeNow;
            resized();
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
        const int inR  = effectiveInputRouting (stripIndex, state.inputRouting.load (std::memory_order_relaxed),
                                                availableInputs);
        const int outR = state.outputRouting.load (std::memory_order_relaxed);
        const int wantedIn  = (inR  == -2) ? stripIndex + 2 : pick (inputCombo,  inR);
        const int wantedOut = (outR == -2) ? stripIndex + 2 : pick (outputCombo, outR);

        ensureRoutingItem (inputCombo,  wantedIn  - 2, availableInputs,  "In ");
        ensureRoutingItem (outputCombo, wantedOut - 2, availableOutputs, "Out ");
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
        // JUCE delivers mouseDown to a disabled component, so LOCK has to be
        // checked here: only stock widgets refuse by themselves.
        if (! isEnabled()) return;

        // Right-click → context menu, from ANYWHERE on the strip including
        // over its child controls (name, combos, knobs, meter, buttons).
        // Checked BEFORE the child-origin guard below: the children cover
        // almost the whole strip, so requiring a bare-background hit made
        // the menu feel broken -- most right-clicks landed on a child and
        // did nothing. The controls don't use the right button themselves,
        // so there's no conflict.
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            showContextMenu();
            return;
        }

        // For LEFT clicks, ignore those that originated on an actual
        // interactive child (the fader cap, a button, a combo). The
        // bubble-up through addMouseListener(this, true) means we see those
        // events on the strip too; without this guard, dragging a button or
        // fader would also select the strip. originalComponent is the leaf
        // that received the click; if it isn't 'this', the leaf handles it.
        if (e.eventComponent != this && e.originalComponent != this)
            return;

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
                                         VoidCallback onLinkStereo)
    {
        deleteCb     = std::move (onDelete);
        addCb        = std::move (onAdd);
        linkStereoCb = std::move (onLinkStereo);
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

        // Edit Group submenu. Group ids 1..8 (1-based for menu
        // readability; engine stores raw int). -1 = unlinked.
        // Gain / pan / mute / solo / arm broadcast to every other
        // strip sharing the same id when the engineer fires a
        // gesture on this strip.
        juce::PopupMenu egMenu;
        const int curEg = state.editGroup.load (std::memory_order_relaxed);
        egMenu.addItem (900, "None", true, curEg < 0);
        for (int i = 0; i < 8; ++i)
            egMenu.addItem (901 + i, "Edit Group " + juce::String (i + 1),
                            true, curEg == i);
        menu.addSubMenu (isStereo ? "Assign to Edit Group (L+R)" : "Assign to Edit Group", egMenu);

        menu.showMenuAsync (juce::PopupMenu::Options(),
                            [this, safe = juce::Component::SafePointer<ChannelStrip> (this)] (int chosen)
        {
            if (safe == nullptr) return;   // strip rebuilt (cue recall / console session / device restart) while the menu was open
            if (! isEnabled()) return;     // LOCK engaged while the menu was open
            switch (chosen)
            {
                case 1: openRenameDialog();           break;
                case 2: openColourPicker();           break;
                case 3: if (colourCb) colourCb (juce::Colour ((juce::uint32) 0)); break;
                case 4: if (renameCb)
                        {
                            renameCb ({});
                            // setNameThreadSafe, not a raw write -- same race
                            // with getNameThreadSafe() off the message thread
                            // as the inline rename editor had.
                            const auto reset = juce::String (stripIndex + 1);
                            state.setNameThreadSafe (reset);
                            nameLabel.setText (reset, juce::dontSendNotification);
                        }
                        break;
                case 5: if (onStreamSendChanged)
                            onStreamSendChanged (! state.streamSend.load (std::memory_order_relaxed));
                        break;
                case 6: state.outputMuted.store (! state.outputMuted.load (std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
                        if (pairState)
                            pairState->outputMuted.store (state.outputMuted.load (std::memory_order_relaxed),
                                                           std::memory_order_relaxed);
                        break;
                case 10: if (addCb)        addCb();        break;
                case 11: requestDelete(); break;
                case 12: if (linkStereoCb) linkStereoCb(); break;
                default:
                    // Edit group assignment: 900 = None, 901..908 = group 0..7.
                    if (chosen == 900 || (chosen >= 901 && chosen <= 908))
                    {
                        const int g = (chosen == 900) ? -1 : (chosen - 901);
                        state.editGroup.store (g, std::memory_order_relaxed);
                        if (pairState)
                            pairState->editGroup.store (g, std::memory_order_relaxed);
                        if (onEditGroupChanged) onEditGroupChanged (g);
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
            [this, safe = juce::Component::SafePointer<ChannelStrip> (this)] (juce::Colour chosen)
            {
                if (safe == nullptr) return;   // strip rebuilt while the colour picker was open
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
          keepAlive (s.weak_from_this().lock()),
          pairState (stereoPartner),
          personality (brand::stripColour (index)),
          colourCb (std::move (colourCallback)),
          renameCb (std::move (nameCallback)),
          gainCb   (std::move (gainCallback)),
          panCb    (std::move (panCallback)),
          panRCb   (std::move (panRCallback)),
          inputCb  (std::move (inputCallback)),
          outputCb (std::move (outputCallback)),
          meter (s)
    {
        // When this strip controls a stereo pair, point the meter at the R
        // partner so it can draw two bars side-by-side.
        if (pairState != nullptr)
            meter.setStereoPartner (pairState);
        nameLabel.setText (s.name, juce::dontSendNotification);
        nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        nameLabel.setFont (brand::type::channelName());
        nameLabel.setJustificationType (juce::Justification::centredLeft);
        nameLabel.setBorderSize ({});
        nameLabel.setMinimumHorizontalScale (0.6f);   // squeeze before it ever truncates
        nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        // Double-click to rename inline; single-click does nothing.
        nameLabel.setEditable (false, true, false);
        nameLabel.onTextChange = [this]
        {
            const auto typed = nameLabel.getText().trim();
            const auto newName = typed.isEmpty() ? juce::String (stripIndex + 1) : typed;
            // setNameThreadSafe, not a raw `state.name = ...`. The bare
            // assignment raced getNameThreadSafe() off the message thread --
            // a torn / freed juce::String. The lock only helps if BOTH sides
            // take it.
            state.setNameThreadSafe (newName);
            nameLabel.setText (newName, juce::dontSendNotification);
            setTitle (newName);   // keep the strip's accessible name in sync
            if (renameCb) renameCb (newName);
        };
        nameLabel.onTabKey = [this] (bool shift)
        {
            if (onTabRename) onTabRename (stripIndex, shift);
        };
        addAndMakeVisible (nameLabel);

        // Input + output routing combos.
        auto styleCombo = [] (juce::ComboBox& c)
        {
            c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
            c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
            c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
            c.setColour (juce::ComboBox::arrowColourId,      brand::textTertiary);
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
            if (canChangeArm && ! canChangeArm())
            { armButton.setToggleState (state.armed.load(), juce::dontSendNotification); return; }
            state.armed.store (armButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->armed.store (armButton.getToggleState(),
                                                   std::memory_order_relaxed);
            if (onAfterArmedToggle) onAfterArmedToggle (stripIndex);
        };
        // R (record arm) → red gradient when armed.
        armButton.setColour (juce::ToggleButton::tickColourId, brand::armFill);
        addAndMakeVisible (armButton);

        monButton.setToggleState (s.monitor.load(), juce::dontSendNotification);
        monButton.onClick = [this]
        {
            state.monitor.store (monButton.getToggleState(), std::memory_order_relaxed);
            if (pairState) pairState->monitor.store (monButton.getToggleState(),
                                                     std::memory_order_relaxed);
            if (onAfterMonitorToggle) onAfterMonitorToggle (stripIndex);
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
            if (onAfterMuteToggle) onAfterMuteToggle (stripIndex);
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
            if (onAfterSoloToggle) onAfterSoloToggle (stripIndex);
        };
        // S (solo) → yellow gradient when on.
        soloButton.setColour (juce::ToggleButton::tickColourId, brand::accentSolo);
        addAndMakeVisible (soloButton);

        // Live-value readout -- tabular mono so peaks don't visually
        // wobble while the meter is moving.
        dbLabel.setFont (zynforge::brand::type::monoStamp());
        dbLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        dbLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (dbLabel);

        clipLabel.setFont (brand::type::mono (10.0f, true));
        clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        clipLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (clipLabel);

        nameLabel  .setTooltip ("Channel name -- double-click to rename, right-click for more.");
        inputCombo .setTooltip ("Hardware input this strip records from.");
        outputCombo.setTooltip ("Hardware output this strip plays VSC audio to.");
        armButton  .setTooltip ("ARM -- include this channel when RECORD is rolling.");
        monButton  .setTooltip ("Input monitor -- sum this input into the stereo monitor bus (output pair set in Audio Device dialog).");
        muteButton .setTooltip ("Mute -- silence this channel in monitor + playback. Recording still hits disk.");
        soloButton .setTooltip ("Solo -- when any track is soloed, only soloed tracks are audible.");
        meter      .setTooltip ("Peak + RMS LED meter -- click to clear the clip indicator.");
        if (swatch != nullptr)
            swatch->setTooltip ("Click for a colour palette. Right-click the strip for more options.");

        // ── Accessibility ──────────────────────────────────────────────
        // The single-letter glyphs (R / I / M / S) are meaningless to a
        // screen reader, so give every control a spoken name + help text,
        // and make the strip a labelled focus-container group named after
        // the channel. Help text reuses the tooltips set just above.
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
        setTitle (state.name);
        setDescription ("Channel strip");

        armButton .setTitle ("Record arm");    armButton .setHelpText (armButton .getTooltip());
        monButton .setTitle ("Input monitor"); monButton .setHelpText (monButton .getTooltip());
        muteButton.setTitle ("Mute");          muteButton.setHelpText (muteButton.getTooltip());
        soloButton.setTitle ("Solo");          soloButton.setHelpText (soloButton.getTooltip());
        inputCombo .setTitle ("Input routing");  inputCombo .setHelpText (inputCombo .getTooltip());
        outputCombo.setTitle ("Output routing"); outputCombo.setHelpText (outputCombo.getTooltip());
        nameLabel  .setTitle ("Channel name");
        gainFader  .setTitle ("Gain");
        panSlider  .setTitle (pairState != nullptr ? "Pan left" : "Pan");
        panSliderR .setTitle ("Pan right");

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
            s.setColour (juce::Slider::rotarySliderFillColourId, brand::textSecondary);
            s.setColour (juce::Slider::rotarySliderOutlineColourId, brand::edge);
            s.setColour (juce::Slider::thumbColourId, brand::textSecondary);
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
        panSlider.onValueChange = [this]
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
            showPanText();
        };
        addAndMakeVisible (panSlider);

        // Pan label below the knob(s).
        panLabel.setFont (zynforge::brand::type::monoStamp());
        panLabel.setColour (juce::Label::textColourId, brand::textSecondary);
        panLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (panLabel);

        // For stereo strips: a second pan knob + label for R.
        if (pairState != nullptr)
        {
            stylePanKnob (panSliderR);
            panSliderR.setScrollWheelEnabled (false);
            panSliderR.setTooltip ("Pan R -- drag to set L100..0..R100. Double-click for centre.");
            panSliderR.setValue (pairState->pan.load(), juce::dontSendNotification);
            panSliderR.onValueChange = [this]
            {
                const float v = (float) panSliderR.getValue();
                if (pairState != nullptr)
                    pairState->pan.store (v, std::memory_order_relaxed);
                // Persist the R-side pan via its own callback so the
                // L/R values are independent and both survive a relaunch.
                if (panRCb) panRCb (v);
                showPanText();
            };
            addAndMakeVisible (panSliderR);

            panLabelR.setFont (zynforge::brand::type::monoStamp());
            panLabelR.setColour (juce::Label::textColourId, brand::textSecondary);
            panLabelR.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (panLabelR);
        }
        showPanText();

        gainFader.setSliderStyle (juce::Slider::LinearVertical);
        gainFader.setRange (-60.0, 12.0, 0.1);
        gainFader.setSkewFactorFromMidPoint (-15.0);   // log-ish console feel
        gainFader.setDoubleClickReturnValue (true, 0.0);
        gainFader.setTextValueSuffix (" dB");
        gainFader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        gainFader.setNumDecimalPlacesToDisplay (1);
        gainFader.setColour (juce::Slider::trackColourId,        brand::edge);
        gainFader.setColour (juce::Slider::backgroundColourId,   brand::bgDeep);
        gainFader.setColour (juce::Slider::thumbColourId,        brand::faderFill);
        // Snap to mouse position so the cap STICKS to the pointer 1:1 while
        // dragging (standard DAW fader feel). The old "relative drag"
        // (snap=false + a 250 px sensitivity longer than the fader itself)
        // made the cap drift up/down out of sync with the cursor -- the
        // gig-two complaint. The fader only receives clicks inside its own
        // narrow bounds, not the strip body, so strip-select clicks elsewhere
        // never touch it.
        gainFader.setSliderSnapsToMousePosition (true);
        // Same reasoning as the pan knob: scroll-wheel / trackpad
        // gestures should NEVER move the gain. The fader changes only
        // on a direct click+drag interaction.
        // Scroll = fine trim now (FineFader: 0.5 dB/notch, Shift 0.1).
        gainFader.setValue (s.gainDb.load(), juce::dontSendNotification);
        gainFader.onValueChange = [this]
        {
            const float v = (float) gainFader.getValue();
            state.gainDb.store (v, std::memory_order_relaxed);
            if (pairState) pairState->gainDb.store (v, std::memory_order_relaxed);
            if (gainCb) gainCb (v);
            showFaderValue();
        };
        addAndMakeVisible (gainFader);

        // The fader's own text box is only as wide as the cap, which cut the
        // value to "0....", so the value gets the strip's width. It stays
        // click-to-type, as the box was.
        faderValue.setFont (zynforge::brand::type::monoStamp());
        faderValue.setColour (juce::Label::textColourId, brand::textPrimary);
        faderValue.setColour (juce::Label::backgroundColourId,
                              brand::bgDeep.withAlpha (zynforge::brand::alpha::ghost));
        faderValue.setJustificationType (juce::Justification::centred);
        faderValue.setBorderSize ({});
        faderValue.setMinimumHorizontalScale (0.6f);
        faderValue.setEditable (true, false, false);
        faderValue.setTitle ("Gain value");
        faderValue.setTooltip ("Fader level -- click to type a value.");
        faderValue.onTextChange = [this]
        {
            const auto typed = gainFader.getValueFromText (faderValue.getText());
            gainFader.setValue (gainFader.snapValue (typed, juce::Slider::notDragging),
                                juce::sendNotificationSync);
            showFaderValue();   // a refused or clamped entry shows what was applied
        };
        showFaderValue();
        addAndMakeVisible (faderValue);

        // dB scale label strip between the fader and the LED meter.
        dbRuler = std::make_unique<DbRuler> (gainFader);
        addAndMakeVisible (*dbRuler);

        outLabel.setText ("OUT", juce::dontSendNotification);
        outLabel.setFont (brand::type::ledLabel());
        outLabel.setColour (juce::Label::textColourId, brand::textTertiary);
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

    void ChannelStrip::setTakeRolling (bool rolling)
    {
        if (takeRolling == rolling) return;
        takeRolling = rolling;
        armButton.setColour (juce::ToggleButton::tickColourId, rolling ? brand::armRollFill : brand::armFill);
        repaint();
    }

    // Pan readout for the room there is: "pan < L40 >", "< L40 >" or just "L40",
    // so a narrow strip shows the value rather than a clipped word.
    void ChannelStrip::showPanText()
    {
        auto format = [] (double v, int detail)
        {
            const int pct = juce::jlimit (0, 100, juce::roundToInt (std::abs (v) * 100.0));
            const juce::String mid = (pct == 0)
                                       ? juce::String ("0")
                                       : (v < 0.0 ? juce::String ("L") + juce::String (pct)
                                                  : juce::String ("R") + juce::String (pct));
            if (detail == 0) return mid;
            const auto core = juce::String::fromUTF8 ("\xe2\x97\x82") + "  "
                            + mid + "  "
                            + juce::String::fromUTF8 ("\xe2\x96\xb8");
            return detail == 1 ? core : juce::String ("pan  ") + core;
        };
        const int room = pairState != nullptr ? getWidth() / 2 : getWidth();
        const int detail = room < 60 ? 0 : (room < 96 ? 1 : 2);
        panLabel.setText (format (panSlider.getValue(), detail), juce::dontSendNotification);
        if (pairState != nullptr)
            panLabelR.setText (format (panSliderR.getValue(), detail), juce::dontSendNotification);
    }

    void ChannelStrip::showFaderValue()
    {
        faderValue.setText (gainFader.getTextFromValue (gainFader.getValue()), juce::dontSendNotification);
    }

    // Clear of the spine on the left.
    juce::Rectangle<int> ChannelStrip::contentArea() const
    {
        return getLocalBounds().withTrimmedLeft (brand::space::md + 1)
                               .withTrimmedRight (brand::space::sm - 1)
                               .reduced (0, brand::space::sm);
    }

    juce::Rectangle<int> ChannelStrip::numberBounds() const
    {
        return contentArea().removeFromTop (kIdRowH).withWidth (20);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        // Condemned strip (invalidate() called, TrackState about to be / already
        // freed) -- paint reads state.armed / colourARGB etc, so don't
        // touch it. detach() only made the meter paint safe; this covers
        // the strip's own paint on an external repaint (hover) in the pre-rebuild
        // window. Just fill the body so it isn't a visual hole.
        if (! stripValid) { g.fillAll (brand::bgPanel); return; }

        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto stripColour = getResolvedColour();
        const auto body = hovered ? brand::lift (brand::bgStrip, brand::tint::hover) : brand::bgStrip;

        {
            juce::Path bodyClip;
            bodyClip.addRoundedRectangle (r, brand::radius::lg);
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (bodyClip);
            g.setColour (body);
            g.fillRect (r);
            g.setColour (brand::stripHeaderFill (stripColour));
            g.fillRect (r.withHeight ((float) kHeaderH));

            // The spine breathes at ~1 Hz while a take rolls here. Phase from
            // the wall clock so it animates whenever paint() runs (StripTimer
            // repaints the spine column at 10 Hz).
            const bool rollingHere = takeRolling && state.armed.load (std::memory_order_relaxed);
            float armPulse = 0.0f;
            if (rollingHere)
                armPulse = zynforge::brand::reduceMotion()
                             ? 0.6f   // reduced-motion: static raised-glow end-state
                             : 0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 1000)
                                                       / 1000.0f * juce::MathConstants<float>::twoPi);
            steel::drawSpine (g, r, stripColour, rollingHere, armPulse);
            steel::drawNumber (g, numberBounds(), stripIndex + 1);
        }

        g.setColour (hovered ? brand::borderBright : brand::edge);
        g.drawRoundedRectangle (r, brand::radius::lg, 1.0f);

        // Multi-select highlight -- a 2 px outline in the selection fill so
        // a group of selected strips is unambiguous from the side of the room.
        if (selected)
        {
            g.setColour (brand::selectFill);
            g.drawRoundedRectangle (r.reduced (1.0f), brand::radius::lg, 2.5f);
        }

        // Edit Group badge. Pro Tools shows group names per strip
        // so engineers can see at a glance
        // which strips will move together. "EG N" matches the
        // context-menu labels (Edit Group 1..8).
        const int eg = state.editGroup.load (std::memory_order_relaxed);
        if (eg >= 0 && eg < 8)
        {
            const juce::String label = "EG" + juce::String (eg + 1);
            auto badge = juce::Rectangle<float> (r.getRight() - 26.0f,
                                                  r.getY() + 4.0f, 22.0f, 12.0f);
            g.setColour (brand::sink (brand::accentVS, 0.30f));
            g.fillRoundedRectangle (badge, brand::radius::sm);
            g.setColour (brand::lift (brand::accentVS, 0.40f));
            g.drawRoundedRectangle (badge, brand::radius::sm, 0.75f);
            g.setColour (brand::onSignal (brand::sink (brand::accentVS, 0.30f)));
            g.setFont (brand::type::caption());
            g.drawText (label, badge.toNearestInt(), juce::Justification::centred, false);
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
        auto r = contentArea();

        // ── 1. Header ── a thin line for the number + colour chip, then the
        // NAME across the strip's whole width at every size: it is what gets
        // read from the mix position, so nothing else shares its row.
        auto idRow = r.removeFromTop (kIdRowH);
        if (state.editGroup.load (std::memory_order_relaxed) >= 0 && getWidth() >= 78)
            idRow.removeFromRight (26);      // painted EG badge
        if (swatch != nullptr)
            swatch->setBounds (idRow.removeFromRight (kIdRowH).withSizeKeepingCentre (10, 10));
        // Largest type on the strip at every size, chosen for the room there is.
        nameLabel.setFont (getWidth() < 64 ? brand::type::channelName()
                         : getWidth() < 96 ? brand::type::sectionTitle()
                                           : brand::type::headline());
        nameLabel.setBounds (r.removeFromTop (kNameRowH));
        r.removeFromTop (brand::space::md);

        // Routing combos: keep them in flow but small -- they live above
        // the pan section in the existing UX (engineer can also use the
        // PATCH page). 18px each.
        inputCombo .setBounds (r.removeFromTop (18).reduced (2, 1));
        outputCombo.setBounds (r.removeFromTop (18).reduced (2, 1));
        r.removeFromTop (brand::space::sm);

        // ── 2. Pan section ─────────────────────────────────────────
        // Bigger knobs -- engineers wanted them obvious at a glance.
        // Mono: one knob centred + "pan  ◂ N ▸" readout. Stereo: two smaller
        // knobs + two compact "◂ N ▸" readouts under each. The row is the same
        // height either way, so every fader on the page lines up and 0 dB sits
        // at one height across the mixer.
        const int knobH    = 40;
        const int stereoKnob = 30;
        const int panLabelH = 16;
        auto panKnobs = r.removeFromTop (knobH);
        auto panText  = r.removeFromTop (panLabelH);
        if (pairState != nullptr)
        {
            const int colW = panKnobs.getWidth() / 2;
            // Too narrow for two knobs side by side: one above the other.
            if (colW - 4 < knobH / 2 + 4)
            {
                panSlider .setBounds (panKnobs.removeFromTop (knobH / 2).withSizeKeepingCentre (knobH / 2, knobH / 2));
                panSliderR.setBounds (panKnobs.withSizeKeepingCentre (knobH / 2, knobH / 2));
            }
            else
            {
                const int knobSize = juce::jmin (stereoKnob, colW - 4);
                panSlider .setBounds (panKnobs.removeFromLeft (colW)
                                              .withSizeKeepingCentre (knobSize, knobSize));
                panSliderR.setBounds (panKnobs.withSizeKeepingCentre (knobSize, knobSize));
            }
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
        const int btnH = 24;
        const int btnGap = 3;

        // Touch-target compliance: at XS strip width (~56-70 px) the
        // 2×2 button grid gives each button ~26×24 -- below Apple HIG's
        // 44 pt minimum and below WCAG 2.5.5 (24×24 minimum, 44×44
        // enhanced). Auto-stack vertically when the strip is narrower
        // than 78 px so each toggle gets full width (~70+ px) at the
        // cost of one extra row of height. Engineers running at XS for
        // density still get a usable hit zone.
        const bool stack = getWidth() < kStackBelowW;

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
        r.removeFromTop (brand::space::sm);

        clipLabel.setBounds (r.removeFromTop (12));

        // ── 4. Fader area: [ ruler | fader | meter ] ───────────────
        // dB readout sits at the bottom; reserve 16 px before laying
        // out the column.
        auto bottom = r.removeFromBottom (brand::space::xl);
        dbLabel.setBounds (bottom);
        faderValue.setBounds (r.removeFromBottom (kFaderValueH));

        // Pro-Tools-style COMPACT fader cluster: the dB ruler hugs the fader on
        // the left and the meter hugs it on the right, with the fader just wide
        // enough for its cap -- no wide empty gutter. The cluster is centred in
        // whatever width the strip has, so on a narrow strip it fills it (lots
        // of strips per page) and on a wide one the scales still sit right
        // beside the fader instead of floating at the strip edges.
        // The numbers beside the fader are the fader's, so the meter carries its
        // own dBFS scale wherever it fits: full scale is the top of the meter,
        // level with +12 on the ruler, and without this -12 dBFS looks like 0.
        const int barsW     = (pairState != nullptr) ? 26 : 16;
        const int scaleW    = 16;
        const int rulerW    = 20;
        const int faderW    = 30;
        const bool roomForScale = r.getWidth() >= rulerW + 2 + kMinFaderW + 2 + barsW + scaleW;
        const int meterW    = barsW + (roomForScale ? scaleW : 0);
        const int clusterW  = rulerW + 2 + faderW + 2 + meterW;
        meter.setShowDbLabels (roomForScale);

        auto cluster = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), clusterW),
                                                r.getHeight());
        // On a narrow strip the meter's scale has to do: the ruler would take
        // the whole fader's width, and a fader nobody can see can't be moved.
        const bool roomForRuler = cluster.getWidth() >= rulerW + 2 + kMinFaderW + 2 + meterW;
        if (dbRuler != nullptr)
        {
            dbRuler->setVisible (roomForRuler);
            if (roomForRuler)
            {
                dbRuler->setBounds (cluster.removeFromLeft (rulerW));
                cluster.removeFromLeft (2);
            }
        }
        meter.setBounds (cluster.removeFromRight (meterW));
        cluster.removeFromRight (2);
        gainFader.setBounds (cluster);

        outLabel.setBounds ({});  // collapsed -- output combo serves the role
        showPanText();
    }
}
