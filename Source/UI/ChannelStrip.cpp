#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "StripColourPicker.h"

// ── Heated Steel: resting-identity paint helpers (self-contained) ─────────
// Direction C drop-in. Inlined here so this single file needs no new header
// and no other edits. Colours route through brand:: tokens; the steel greys
// are raw hex for the trial -- promote them to BrandColors.h (steelHeaderHi/
// Lo + debossInk + a structuralForge() accessor) once you keep this.
// Textures/paths are built ONCE (function-local statics) so a 24-strip wall
// repaints at today's cost.
namespace
{
    namespace steel
    {
        inline juce::Colour structuralForge() { return zynforge::brand::brandOrange; }

        inline const juce::Image& hammeredTile()
        {
            static const juce::Image tile = []
            {
                const int W = 96, H = 96;
                juce::Image img (juce::Image::ARGB, W, H, true);
                juce::Graphics g (img);
                for (int x = 0; x < W; x += 4)
                {
                    g.setColour (zynforge::brand::gloss (0.045f));
                    g.fillRect (x, 0, 1, H);
                }
                juce::Graphics::ScopedSaveState ss (g);
                g.addTransform (juce::AffineTransform::rotation (0.14f, W * 0.5f, H * 0.5f));
                for (int y = -H; y < H * 2; y += 9)
                {
                    g.setColour (zynforge::brand::debossInk.withAlpha (zynforge::brand::alpha::faint));
                    g.fillRect (-W, y, W * 3, 2);
                }
                return img;
            }();
            return tile;
        }

        inline void drawHammered (juce::Graphics& g, juce::Rectangle<float> r, float a = 0.42f)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.setFillType (juce::FillType (hammeredTile(),
                           juce::AffineTransform::translation (r.getX(), r.getY())));
            g.setOpacity (a);
            g.fillRect (r);
        }

        inline void drawSteelHeader (juce::Graphics& g, juce::Rectangle<float> r)
        {
            // Pixel target: near-pure-black plate so it reads as a distinct
            // band even on an uncoloured (grey) strip; bright orange under-seam.
            g.setColour (zynforge::brand::steelHeaderHi);   // FLAT: solid plate (no gradient)
            g.fillRect (r);
            g.setColour (zynforge::brand::gloss (0.18f));                 // top bevel highlight
            g.fillRect (r.withHeight (1.0f));
            g.setColour (zynforge::brand::structuralForge().withAlpha (zynforge::brand::alpha::muted));            // subdued structural seam (2px)
            g.fillRect (r.getX(), r.getBottom() - 3.0f, r.getWidth(), 2.0f);
            g.setColour (zynforge::brand::debossInk.withAlpha (zynforge::brand::alpha::prominent));         // crisp dark divider
            g.fillRect (r.removeFromBottom (1.0f));
        }

        inline void drawSpine (juce::Graphics& g, juce::Rectangle<float> r, bool armed, float pulse = 0.0f)
        {
            const auto c = structuralForge();
            // Armed: the spine breathes -- glow alpha + width pulse with `pulse`
            // (0..1). Idle: a steady resting spine.
            // Structural identity spine: EMBER-subdued at rest, escalating to a
            // hot, glowing bar only when the channel is record-armed -- so the
            // bright orange means "this channel is live", not just "this channel
            // exists" (it no longer competes with a hot meter for the eye).
            const float glowA = armed ? (0.34f + 0.30f * pulse) : 0.18f;
            const float glowW = armed ? (9.0f + 5.0f * pulse)   : 7.0f;
            g.setColour (c.withAlpha (glowA));                            // glow
            g.fillRect (r.getX(), r.getY(), glowW, r.getHeight());
            g.setColour (c.withAlpha (armed ? 1.0f : 0.45f));             // bar: ember at rest, hot when armed
            g.fillRect (r.getX(), r.getY(), 5.0f, r.getHeight());
        }

        inline void drawForgeMark (juce::Graphics& g, juce::Rectangle<float> r, bool hot)
        {
            const auto xf = juce::AffineTransform::scale (r.getWidth(), r.getHeight())
                                                   .translated (r.getX(), r.getY());
            juce::Path hex;
            hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
            hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
            juce::Path chv;
            chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
            chv.startNewSubPath (0.36f, 0.73f); chv.lineTo (0.50f, 0.58f); chv.lineTo (0.64f, 0.73f);
            hex.applyTransform (xf); chv.applyTransform (xf);
            g.setColour (hot ? zynforge::brand::structuralForge().withAlpha (zynforge::brand::alpha::chrome) : zynforge::brand::debossInk.withAlpha (zynforge::brand::alpha::wash));
            g.fillPath (hex);
            g.setColour (hot ? zynforge::brand::structuralForge().withAlpha (zynforge::brand::alpha::muted) : zynforge::brand::gloss (0.10f));
            g.strokePath (hex, juce::PathStrokeType (1.0f));
            g.setColour (hot ? structuralForge() : zynforge::brand::gloss (0.24f));
            g.strokePath (chv, juce::PathStrokeType (juce::jmax (1.0f, r.getWidth() * 0.07f),
                                                     juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const float s = r.getWidth() * 0.05f;
            g.setColour (hot ? structuralForge() : zynforge::brand::gloss (0.30f));
            g.fillEllipse (r.getCentreX() - s, r.getY() + r.getHeight() * 0.27f - s, s * 2.0f, s * 2.0f);
        }

        // Deboss-stamped channel number -- font sizes to the box so it fits
        // the strip's short name row. Engraved look: gloss underprint + dark face.
        inline void drawDebossNumber (juce::Graphics& g, juce::Rectangle<int> box, int channel1Based)
        {
            const auto txt = juce::String (channel1Based).paddedLeft ('0', 2);
            g.setFont (zynforge::brand::type::mono ((float) juce::jlimit (12, 26, box.getHeight()), true));
            g.setColour (zynforge::brand::debossInk.withAlpha (zynforge::brand::alpha::prominent));          // engraved shadow
            g.drawText (txt, box.translated (0, 1), juce::Justification::centredLeft, false);
            g.setColour (zynforge::brand::debossFace);                           // raised steel face
            g.drawText (txt, box, juce::Justification::centredLeft, false);
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

        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
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

            // Heated Steel: while record-armed, repaint just the left spine
            // column (12px) at 10 Hz so the orange spine breathes. Cheap --
            // the rest of the strip stays static, honouring the 24-strip
            // repaint budget; only ARMED strips animate. Skipped entirely
            // under reduced-motion (the spine then holds a static glow).
            if (! zynforge::brand::reduceMotion()
                && strip.state.armed.load (std::memory_order_relaxed))
                strip.repaint (0, 0, 12, strip.getHeight());
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
        // The strip's meter + spectrum run their OWN timers against the same
        // TrackState& -- stopping just stripTimer left them reading freed
        // memory for up to a rebuild interval after the strip was condemned.
        meter.detach();
        spectrum.detach();
    }

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

        // Fader + pan. During playback the display hooks return the
        // AUTOMATED value at the playhead so the fader / pan track the EDIT
        // curve (views stay linked); when stopped (or unhooked) they return
        // the stored manual value.
        float gainDb = state.gainDb.load (std::memory_order_relaxed);
        if (displayGainDb) gainDb = displayGainDb (gainDb);
        float panL = state.pan.load (std::memory_order_relaxed);
        if (displayPanL) panL = displayPanL (panL);
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
            float panR = pairState->pan.load (std::memory_order_relaxed);
            if (displayPanR) panR = displayPanR (panR);
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
            const auto knobCol = brand::lift (resolved, 0.30f);
            if (swatch != nullptr)
                swatch->setDisplayColour (resolved);
            gainFader.setColour (juce::Slider::thumbColourId, knobCol);
            panSlider.setColour (juce::Slider::thumbColourId, knobCol);
            panSlider.setColour (juce::Slider::rotarySliderFillColourId, knobCol);
            panSliderR.setColour (juce::Slider::thumbColourId, knobCol);
            panSliderR.setColour (juce::Slider::rotarySliderFillColourId, knobCol);
            dirty = true;
        }

        // If a VCA / edit-group / bus badge just appeared or disappeared,
        // re-lay-out so the name label reclaims or yields the badge's space.
        const int badgeNow = (state.vcaGroup .load (std::memory_order_relaxed) >= 0
                           || state.editGroup.load (std::memory_order_relaxed) >= 0
                           || state.isBus    .load (std::memory_order_relaxed)) ? 1 : 0;
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
        menu.addSeparator();
        menu.addItem (20, autoSafeOn ? "Automation Safe -- ON (writes blocked)"
                                      : "Automation Safe -- OFF",
                      true, autoSafeOn);

        // VCA assignment submenu. Engineer picks a bus index 1..8 or
        // "None" to detach. Persisted on engine; mute / gain inherited
        // from the bus.
        juce::PopupMenu vcaMenu;
        const int curVca = state.vcaGroup.load (std::memory_order_relaxed);
        vcaMenu.addItem (700, "None", true, curVca < 0);
        for (int i = 0; i < 8; ++i)
            vcaMenu.addItem (701 + i, "VCA " + juce::String (i + 1),
                             true, curVca == i);
        menu.addSubMenu (isStereo ? "Assign to VCA (L+R)" : "Assign to VCA", vcaMenu);

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
                            [this, safe = juce::Component::SafePointer<ChannelStrip> (this)] (int chosen)
        {
            if (safe == nullptr) return;   // strip rebuilt (cue recall / console session / device restart) while the menu was open
            switch (chosen)
            {
                case 1: openRenameDialog();           break;
                case 2: openColourPicker();           break;
                case 3: if (colourCb) colourCb (juce::Colour ((juce::uint32) 0)); break;
                case 4: if (renameCb)
                        {
                            renameCb ({});
                            // setNameThreadSafe, not a raw write -- same race
                            // with the companion server's getNameThreadSafe()
                            // as the inline rename editor had.
                            const auto reset = juce::String (stripIndex + 1);
                            state.setNameThreadSafe (reset);
                            nameLabel.setText (reset, juce::dontSendNotification);
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
                case 11: if (deleteCb)
                         {
                             // deleteCb() runs engine.removeStripAt, freeing
                             // this strip's TrackState synchronously. Stop the
                             // StripTimer BEFORE that so it can't dereference
                             // the dangling reference before the next rebuild.
                             invalidate();
                             deleteCb();
                         }
                         break;
                case 12: if (linkStereoCb) linkStereoCb(); break;
                case 20:
                    {
                        const bool next = ! autoSafeOn;
                        // Optimistic local update so the LED flips
                        // before the host polls back; host will re-
                        // push via setAutomationLed and that's a
                        // no-op when the state already matches.
                        autoSafeOn = next;
                        repaint();
                        if (onAutomationSafeChanged) onAutomationSafeChanged (next);
                    }
                    break;
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
                    // Edit group assignment: 900 = None, 901..908 = group 0..7.
                    else if (chosen == 900 || (chosen >= 901 && chosen <= 908))
                    {
                        const int g = (chosen == 900) ? -1 : (chosen - 901);
                        state.editGroup.store (g, std::memory_order_relaxed);
                        if (pairState)
                            pairState->editGroup.store (g, std::memory_order_relaxed);
                        if (onEditGroupChanged) onEditGroupChanged (g);
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
            const auto typed = nameLabel.getText().trim();
            const auto newName = typed.isEmpty() ? juce::String (stripIndex + 1) : typed;
            // setNameThreadSafe, not a raw `state.name = ...`. The bare
            // assignment raced the companion server's getNameThreadSafe() on
            // its worker thread -- a torn / freed juce::String. The lock only
            // helps if BOTH sides take it.
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
            if (onAfterArmedToggle) onAfterArmedToggle (stripIndex);
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

        // FFT spectrum removed per user request -- keep the component
        // alive (it feeds off TrackState) but don't show it.

        nameLabel  .setTooltip ("Channel name -- double-click to rename, right-click for more.");
        inputCombo .setTooltip ("Hardware input this strip records from.");
        outputCombo.setTooltip ("Hardware output this strip plays VSC audio to.");
        armButton  .setTooltip ("ARM -- include this channel when RECORD is rolling.");
        monButton  .setTooltip ("Input monitor -- sum this input into the stereo monitor bus (output pair set in Audio Device dialog).");
        muteButton .setTooltip ("Mute -- silence this channel in monitor + playback. Recording still hits disk.");
        soloButton .setTooltip ("Solo -- when any track is soloed, only soloed tracks are audible.");
        spectrum   .setTooltip ("Live FFT spectrum of this channel's input signal.");
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
                         brand::lift (getResolvedColour(), 0.30f));
            s.setColour (juce::Slider::rotarySliderOutlineColourId, brand::edge);
            s.setColour (juce::Slider::thumbColourId,
                         brand::lift (getResolvedColour(), 0.30f));
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
        panLabel.setFont (zynforge::brand::type::monoStamp());
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

            panLabelR.setFont (zynforge::brand::type::monoStamp());
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
        gainFader.setColour (juce::Slider::thumbColourId,        brand::lift (getResolvedColour(), 0.30f));
        // Snap to mouse position so the cap STICKS to the pointer 1:1 while
        // dragging (standard DAW fader feel). The old "relative drag"
        // (snap=false + a 250 px sensitivity longer than the fader itself)
        // made the cap drift up/down out of sync with the cursor -- the
        // gig-two complaint. The fader only receives clicks inside its own
        // narrow bounds, not the strip body, so strip-select clicks elsewhere
        // never touch it.
        gainFader.setSliderSnapsToMousePosition (true);
        gainFader.setColour (juce::Slider::textBoxTextColourId,  brand::textPrimary);
        gainFader.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        gainFader.setColour (juce::Slider::textBoxBackgroundColourId,
                             brand::bgDeep.withAlpha (zynforge::brand::alpha::ghost));
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
        // Condemned strip (invalidate() called, TrackState about to be / already
        // freed) -- paint reads state.armed / vcaGroup / colourARGB etc, so don't
        // touch it. detach() only made the meter/spectrum paint safe; this covers
        // the strip's own paint on an external repaint (hover) in the pre-rebuild
        // window. Just fill the body so it isn't a visual hole.
        if (! stripValid) { g.fillAll (brand::bgPanel); return; }

        auto r = getLocalBounds().toFloat().reduced (4.0f);
        auto stripColour = getResolvedColour();

        // Hover lift -- mouse over the strip brightens the wash ~6% so
        // the engineer reads 'this is the strip my pointer is on'
        // without needing a focus ring or selection state.
        if (hovered) stripColour = brand::lift (stripColour, 0.06f);

        // Vertical gradient wash in the personality colour -- matches
        // ZynForge Live's strip finish and gives every channel a sense
        // of depth instead of a flat block.
        g.setGradientFill (brand::verticalGradient (stripColour, r, 0.18f, 0.28f));
        g.fillRoundedRectangle (r, brand::radius::xl);

        // ── Heated Steel: forged resting identity (paint-only) ── FORGE_STEEL_V3
        {
            const bool armedNow = state.armed.load (std::memory_order_relaxed);
            juce::Path bodyClip;
            bodyClip.addRoundedRectangle (r, brand::radius::xl);
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (bodyClip);
            steel::drawHammered    (g, r, 0.38f);              // ③ hammered steel
            g.setColour (zynforge::brand::debossInk.withAlpha (zynforge::brand::alpha::edgeSoft));  // light forged scrim -- lets colour show
            g.fillRect (r);
            steel::drawSteelHeader (g, r.withHeight (30.0f));  // taller near-black stamped plate
            // Heated Steel: the spine breathes at ~1 Hz while record-armed.
            // Phase from the wall clock so it animates whenever paint() runs
            // (StripTimer repaints the spine column at 10 Hz when armed).
            float armPulse = 0.0f;
            if (armedNow)
                armPulse = zynforge::brand::reduceMotion()
                             ? 0.6f   // reduced-motion: static raised-glow end-state
                             : 0.5f + 0.5f * std::sin ((float) (juce::Time::getMillisecondCounter() % 1000)
                                                       / 1000.0f * juce::MathConstants<float>::twoPi);
            steel::drawSpine       (g, r, armedNow, armPulse); // ① structural orange spine (pulses when armed)
            if (getWidth() >= 78)                               // ② deboss channel number
                steel::drawDebossNumber (g, { 15, 7, 30, 17 }, stripIndex + 1);
            // ④ forge-mark top-right, only where no VCA/EG/BUS chip sits
            const bool badgeHere = state.vcaGroup .load (std::memory_order_relaxed) >= 0
                                || state.editGroup.load (std::memory_order_relaxed) >= 0
                                || state.isBus    .load (std::memory_order_relaxed);
            if (! badgeHere)
                steel::drawForgeMark (g, { r.getRight() - 18.0f, r.getY() + 3.0f, 13.0f, 13.0f }, armedNow);
        }

        // ── Ember armed-glow (brand signature) ──────────────────────────────
        // A forged channel runs HOT. When record-armed, the strip gets a warm
        // ember bloom weighted to the top (where REC / arm live) fading down,
        // so an armed channel reads as heated steel from across the room. The
        // meter already supplies the heat motion; this marks the *channel*.
        const bool isArmed = state.armed.load (std::memory_order_relaxed);
        if (isArmed)
        {
            // FLAT: a solid, subtle ember wash marks the armed channel (no glow gradient).
            g.setColour (brand::meterEmber.withAlpha (zynforge::brand::alpha::faint));
            g.fillRoundedRectangle (r, brand::radius::xl);
        }

        // Hover border -- a touch brighter than the resting edge so the
        // strip lifts subtly off the canvas.
        const float edgeAlpha = hovered ? 0.55f : 0.30f;
        g.setColour (brand::lift (stripColour, 0.40f).withAlpha (edgeAlpha));
        g.drawRoundedRectangle (r, brand::radius::xl, 1.0f);

        // An armed strip wins the edge: a hot forge-orange rim over the resting
        // border so "this channel is live to disk" reads instantly.
        if (isArmed)
        {
            g.setColour (brand::meterHot.withAlpha (zynforge::brand::alpha::muted));
            g.drawRoundedRectangle (r.reduced (0.5f), brand::radius::xl, 1.5f);
        }

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
            const juce::String label = "VCA " + juce::String (vca + 1);
            // Prominent VCA indicator: a filled bright-teal pill with bold
            // text PLUS a teal underline along the top edge, so a grouped
            // strip reads unmistakably at a glance. The old chip used a
            // dimmed fill + caption font and was easy to miss.
            const float topY = r.getY() + 1.0f;
            g.setColour (brand::featureEngaged.withAlpha (zynforge::brand::alpha::bold));
            g.fillRoundedRectangle (r.getX() + 4.0f, topY, r.getWidth() - 8.0f, 3.0f,
                                    brand::radius::sm);

            auto badge = juce::Rectangle<float> (r.getRight() - 42.0f, topY + 4.0f, 38.0f, 15.0f);
            g.setColour (brand::featureEngaged);
            g.fillRoundedRectangle (badge, brand::radius::sm);
            g.setColour (brand::lift (brand::featureEngaged, 0.60f));
            g.drawRoundedRectangle (badge, brand::radius::sm, 1.0f);
            g.setColour (brand::onSignal (brand::featureEngaged));
            g.setFont (brand::type::ui (10.5f, true));
            g.drawText (label, badge.toNearestInt(), juce::Justification::centred, false);
        }

        // Edit Group badge -- second chip stacked below the VCA
        // chip (or in the same slot if no VCA). Pro Tools shows
        // group names per strip so engineers can see at a glance
        // which strips will move together. "EG N" matches the
        // context-menu labels (Edit Group 1..8).
        const int eg = state.editGroup.load (std::memory_order_relaxed);
        if (eg >= 0 && eg < 8)
        {
            const juce::String label = "EG" + juce::String (eg + 1);
            // Stack underneath the (now taller) VCA pill when both are
            // present; share the top slot otherwise.
            const float yOffset = (vca >= 0 && vca < 8) ? 23.0f : 4.0f;
            auto badge = juce::Rectangle<float> (r.getRight() - 26.0f,
                                                  r.getY() + yOffset, 22.0f, 12.0f);
            g.setColour (brand::sink (brand::accentVS, 0.30f));
            g.fillRoundedRectangle (badge, brand::radius::sm);
            g.setColour (brand::lift (brand::accentVS, 0.40f));
            g.drawRoundedRectangle (badge, brand::radius::sm, 0.75f);
            g.setColour (brand::onSignal (brand::sink (brand::accentVS, 0.30f)));
            g.setFont (brand::type::caption());
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
            g.setColour (brand::sink (brand::alertAmber, 0.30f));
            g.fillRoundedRectangle (badge, brand::radius::sm);
            g.setColour (brand::lift (brand::alertAmber, 0.20f));
            g.drawRoundedRectangle (badge, brand::radius::sm, 0.75f);
            g.setColour (brand::onSignal (brand::sink (brand::alertAmber, 0.30f)));
            g.setFont (brand::type::caption());
            g.drawText ("BUS", badge.toNearestInt(), juce::Justification::centred, false);
        }

        // Automation LED -- 6 px dot just below the colour swatch in
        // the top-left. Red when WRITE-mode is armed and playback is
        // rolling, amber when the strip is locked Safe. Both at once
        // = Safe (Safe always wins because writes are blocked).
        if (autoWriteArmed || autoSafeOn)
        {
            const auto led = juce::Rectangle<float> (r.getX() + 4.0f, r.getY() + 20.0f, 6.0f, 6.0f);
            const auto c   = autoSafeOn ? brand::engagedAmber : brand::accentRecord;
            g.setColour (c.withAlpha (zynforge::brand::alpha::bold));
            g.fillEllipse (led);
            g.setColour (brand::lift (c, 0.45f));
            g.drawEllipse (led, 0.6f);
        }
    }

    void ChannelStrip::setAutomationLed (bool writeArmed, bool safeOn)
    {
        if (writeArmed == autoWriteArmed && safeOn == autoSafeOn) return;
        autoWriteArmed = writeArmed;
        autoSafeOn     = safeOn;
        repaint();
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
        auto r = getLocalBounds().reduced (brand::space::sm, brand::space::md);

        // ── 1. Name + colour swatch (top of strip) ── mock header layout
        auto nameRow = r.removeFromTop (22);
        const bool wideHdr  = getWidth() >= 78;
        const bool veryWide = getWidth() >= 150;   // L preset -- name on its OWN row
        const bool hasBadge = state.vcaGroup .load (std::memory_order_relaxed) >= 0
                           || state.editGroup.load (std::memory_order_relaxed) >= 0
                           || state.isBus    .load (std::memory_order_relaxed);
        if (veryWide)
        {
            // L view: the NAME sits INSIDE the black steel header plate,
            // centred in the space between the painted deboss number (left)
            // and the colour chip / caret / badge (right) -- no separate plate
            // underneath it.
            nameRow.removeFromLeft (44);     // painted deboss number
            nameRow.removeFromRight (20);    // collapse caret
            if (hasBadge)
                nameRow.removeFromRight (36);
            if (swatch != nullptr)
                swatch->setBounds (nameRow.removeFromRight (18).withSizeKeepingCentre (12, 12));
            nameLabel.setBounds (nameRow.reduced (4, 0));
            nameLabel.setJustificationType (juce::Justification::centred);
            nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        }
        else if (wideHdr)
        {
            nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            // Painted deboss number occupies the far left -- reserve it.
            nameRow.removeFromLeft (44);
            // Reserve the far right for the collapse caret so the name never
            // runs under it, then the colour chip -- mock header order.
            nameRow.removeFromRight (20);
            if (hasBadge && nameRow.getWidth() > 60)
                nameRow.removeFromRight (36);
            if (swatch != nullptr)
                swatch->setBounds (nameRow.removeFromRight (18).withSizeKeepingCentre (12, 12));
            nameLabel.setBounds (nameRow.reduced (2, 0));
            nameLabel.setJustificationType (juce::Justification::centredLeft);
        }
        else
        {
            // Narrow header: a small colour chip + the NAME, which gets every
            // remaining pixel -- the rename field always has room (the badge /
            // group glyph is dropped here; the spine colour still IDs the
            // strip). Left-justified so a truncated long name reads from its
            // start, and double-click still opens the rename editor.
            if (swatch != nullptr)
                swatch->setBounds (nameRow.removeFromLeft (14).withSizeKeepingCentre (10, 10));
            nameLabel.setBounds (nameRow.reduced (2, 0));
            nameLabel.setJustificationType (juce::Justification::centredLeft);
            nameLabel.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        }
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
        const int knobH    = (pairState != nullptr) ? 30 : 40;
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

        // Pro-Tools-style COMPACT fader cluster: the dB ruler hugs the fader on
        // the left and the meter hugs it on the right, with the fader just wide
        // enough for its cap -- no wide empty gutter. The cluster is centred in
        // whatever width the strip has, so on a narrow strip it fills it (lots
        // of strips per page) and on a wide one the scales still sit right
        // beside the fader instead of floating at the strip edges.
        const int meterW   = (pairState != nullptr) ? 26 : 16;
        const int rulerW    = 20;
        const int faderW    = 30;
        const int clusterW  = rulerW + 2 + faderW + 2 + meterW;

        auto cluster = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), clusterW),
                                                r.getHeight());
        if (dbRuler != nullptr)
        {
            dbRuler->setVisible (true);
            dbRuler->setBounds (cluster.removeFromLeft (rulerW));
            cluster.removeFromLeft (2);
        }
        meter.setBounds (cluster.removeFromRight (meterW));
        cluster.removeFromRight (2);
        gainFader.setBounds (cluster);

        outLabel.setBounds ({});  // collapsed -- output combo serves the role
    }
}
