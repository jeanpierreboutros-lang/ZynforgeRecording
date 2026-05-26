#include "EditPage.h"
#include "AutomationToolbar.h"
#include "EditToolsBar.h"
#include "EditTimeRuler.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "LedMeter.h"
#include "StripColourPicker.h"

namespace zynforge
{
    // Waveform thumbnail resolution: samples averaged per drawn min/max
    // point. Lower = finer detail (and a bigger cache). 256 stays crisp
    // even at 16x horizontal zoom (~3 points/px on a 6-min take). The
    // WaveCache.wfm on disk bakes thumbnails AT this resolution, so any
    // change here must invalidate older caches -- hence kWaveCacheVersion,
    // which is written as a header and re-checked on load. Bump the version
    // whenever the resolution (or anything else affecting the baked data)
    // changes, so stale coarse caches are discarded and re-scanned.
    static constexpr int kThumbResolution = 128;
    static constexpr int kWaveCacheMagic   = 0x5A574332; // 'ZWC2'
    // Low byte is a revision: bump it to discard previously-saved caches even
    // when the resolution is unchanged. rev 2 drops caches written by the
    // build that froze thumbnails at a fraction of a second (a partial read
    // taken mid-capture got cached as 'complete'); those files are re-scanned.
    static constexpr int kWaveCacheVersion = (kThumbResolution << 8) | 2;

    // Single home for the timeline<->pixel mapping that the EDIT lanes do
    // ~a dozen times. Build one per paint/event from the lane's inner
    // rect + the session length, then map. toX rounds (the form most
    // call sites used); toXFloor truncates (preserves the two sites that
    // historically did so); toSample is the inverse for hit-testing.
    struct TimelineMapper
    {
        int    x0, width;
        double total;

        static TimelineMapper forLane (juce::Rectangle<int> inner, juce::int64 totalSamples) noexcept
        {
            return { inner.getX(), inner.getWidth(), (double) juce::jmax<juce::int64> (1, totalSamples) };
        }
        double prop (juce::int64 s) const noexcept { return juce::jlimit (0.0, 1.0, (double) s / total); }
        int    toX      (juce::int64 s) const noexcept { return x0 + juce::roundToInt (prop (s) * (double) width); }
        int    toXFloor (juce::int64 s) const noexcept { return x0 + (int) (prop (s) * (double) width); }
        juce::int64 toSample (int x) const noexcept
        {
            const double p = juce::jlimit (0.0, 1.0, (double) (x - x0) / (double) juce::jmax (1, width));
            return (juce::int64) (p * total);
        }
    };

    // Per-track row: header on the left (colour wash + name + REC/MUTE/SOLO),
    // waveform on the right. The TrackList builds one of these per track and
    // stacks them vertically inside the EditPage's viewport.
    class EditPage::TrackRow final : public juce::Component
    {
    public:
        // Track-height presets -- Pro Tools-style 7-step scale plus a
        // dynamic "fit to window" computed from the viewport height.
        // Size::Custom is engaged whenever the user drags the row's
        // bottom edge -- the dragged pixel height is stored in customH.
        enum class Size { Micro, Mini, Small, Medium, Large, Jumbo, Extreme, FitToWindow, Custom };

        static int pixelsFor (Size s, int fitFallback = 80, int customPixels = 80)
        {
            switch (s)
            {
                case Size::Micro:       return 28;
                case Size::Mini:        return 50;
                case Size::Small:       return 80;
                case Size::Medium:      return 110;
                case Size::Large:       return 160;
                case Size::Jumbo:       return 220;
                case Size::Extreme:     return 320;
                case Size::FitToWindow: return fitFallback;
                case Size::Custom:      return customPixels;
            }
            return 80;
        }

        static constexpr int kResizeZoneH = 6;   // bottom-edge grab zone
        static constexpr int kMinRowH     = 20;
        static constexpr int kMaxRowH     = 800;

        // Callback fired by the right-click menu so the owning TrackList
        // can recompute layout (and resolve "fit to window").
        std::function<void(TrackRow&, Size)> onSizeChosen;

        // Toolbar / click-overlay context -- set by the host EditPage
        // after construction so all rows share the same global view.
        AutomationToolbar* toolbar  { nullptr };
        EditToolsBar*      toolsBar { nullptr };
        std::function<void (const juce::String& label,
                            std::function<void()> mutate)> automationEditWrapper;
        std::function<void()>                       automationDragBegin;
        std::function<void (const juce::String&)>   automationDragEnd;
        // Index of the Click track when one exists. Used to suppress
        // automation-lane edits on that row (engineers don't draw
        // gain points on the metronome). -1 = no click track.
        int    clickRowIdx  { -1 };
        // Drag tracking -- while > -1, mouseDrag moves the indexed
        // point on the active lane.
        int    draggingPointIdx { -1 };
        // Tension drag tracking. When > -1, mouseDrag bends the
        // segment starting at this point index (i.e. the segment
        // between points[i] and points[i+1]) by translating the
        // handle's current y into a tension value.
        int    draggingTensionSegIdx { -1 };

        // Clip-edit drag tracking. When the engineer grabs a clip's
        // left edge, right edge, body, or a fade handle, draggingClipIdx
        // pins which clip is moving and draggingClipModeInt picks the
        // edit mode:
        //   0 = TrimLeft   (slip-trim left edge)
        //   1 = TrimRight  (extend / shrink right edge)
        //   2 = Move       (slide on the timeline)
        //   3 = FadeIn     (drag the fade-in apex)
        //   4 = FadeOut    (drag the fade-out apex)
        int draggingClipIdx     { -1 };
        int draggingClipModeInt {  0 };
        // Crossfade-midpoint drag. When >= 0, the engineer grabbed
        // the 6 px dot that paints at the midpoint of an overlap
        // between adjacent clips. draggingXfadeAIdx is the OUTGOING
        // clip's index in the per-track list; the incoming is
        // AIdx + 1. We slide the equal-power crossfade point by
        // updating both clips' fade lengths so the midpoint follows
        // the mouse.
        int draggingXfadeAIdx { -1 };
        int dragStartX          {  0 };
        juce::int64 lastDragSamples { 0 };
        juce::int64 dragStartFadeIn  { 0 };
        juce::int64 dragStartFadeOut { 0 };

        // Strip reorder drag -- armed on swatch-column mouseDown,
        // activates once vertical movement exceeds 8 px. Each
        // additional row-height of movement swaps with the adjacent
        // strip via engine.swapTracks.
        bool reorderArmed  { false };
        bool reorderActive { false };
        int  reorderStartY { 0 };

        TrackRow (int trackIdx,
                  bool isStereoPair,
                  AudioEngine& eng,
                  juce::AudioFormatManager& formats,
                  juce::AudioThumbnailCache& cache)
            : index (trackIdx), stereo (isStereoPair), engine (eng),
              thumbCache (cache),
              thumbnailL (kThumbResolution, formats, cache),
              thumbnailR (kThumbResolution, formats, cache),
              meter (engine.getRecorder().getTrack (index))
        {
            if (stereo)
                meter.setStereoPartner (&engine.getRecorder().getTrack (index + 1));

            auto& s = engine.getRecorder().getTrack (index);

            nameLabel.setText (s.name, juce::dontSendNotification);
            nameLabel.setJustificationType (juce::Justification::centredLeft);
            nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
            nameLabel.setFont (brand::type::channelName());
            nameLabel.setEditable (false, true, false);
            nameLabel.setTooltip ("Double-click to rename this track. Changes mirror to the MIXER + PATCH views.");
            nameLabel.onTextChange = [this]
            {
                const auto newName = nameLabel.getText().trim();
                auto& st = engine.getRecorder().getTrack (index);
                st.name = newName.isEmpty() ? juce::String (index + 1)
                                            : newName;
                nameLabel.setText (st.name, juce::dontSendNotification);
                engine.setTrackName (index, st.name);
            };
            addAndMakeVisible (nameLabel);

            auto styleBtn = [] (juce::ToggleButton& b, juce::Colour onCol)
            {
                b.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                b.setColour (juce::ToggleButton::tickColourId, onCol);
            };
            armButton .setToggleState (s.armed  .load(), juce::dontSendNotification);
            monButton .setToggleState (s.monitor.load(), juce::dontSendNotification);
            muteButton.setToggleState (s.muted  .load(), juce::dontSendNotification);
            soloButton.setToggleState (s.soloed .load(), juce::dontSendNotification);
            // Per-button gradient colours match the mixer:
            //   I → green, R → red, M → orange, S → yellow.
            styleBtn (armButton,  brand::accentRecord);
            styleBtn (monButton,  brand::accentPlay);
            styleBtn (muteButton, brand::brandOrange);
            styleBtn (soloButton, brand::accentSolo);
            armButton .setTooltip ("R -- arm this track for recording (red when on)");
            monButton .setTooltip ("I -- input monitor (green when on); meter reflects live input even when not recording");
            muteButton.setTooltip ("M -- mute the track's playback output (orange when on)");
            soloButton.setTooltip ("S -- solo the track; other unmuted tracks drop out (yellow when on)");
            armButton .onClick = [this]
            {
                engine.getRecorder().getTrack (index).armed.store (armButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).armed.store (armButton.getToggleState());
            };
            monButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).monitor.store (monButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).monitor.store (monButton.getToggleState());
            };
            muteButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).muted.store (muteButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).muted.store (muteButton.getToggleState());
            };
            soloButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).soloed.store (soloButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).soloed.store (soloButton.getToggleState());
            };
            addAndMakeVisible (armButton);
            addAndMakeVisible (monButton);
            addAndMakeVisible (muteButton);
            addAndMakeVisible (soloButton);

            // Per-track 'VIEW' picker -- clicking pops the lane-content
            // menu matching the screenshot (blocks/playlists/analysis/
            // warp are reserved for future builds and stay disabled).
            viewButton.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
            viewButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
            viewButton.setTooltip ("Pick what this row's lane draws -- waveform / volume / pan / ...");
            viewButton.onClick = [this]
            {
                // Lane-content picker. Reserved Pro Tools-style items
                // (blocks / playlists / analysis / warp / transcript)
                // are dropped from the menu -- they were never wired and
                // the greyed entries were just clutter.
                juce::PopupMenu m;
                m.addItem (22, "waveform",    true, laneMode == LaneMode::Waveform);
                m.addItem (20, "markers",     true, laneMode == LaneMode::Markers);
                // 'volume trim' was a duplicate of 'volume' with a
                // different label and no backing trim store -- removed.
                m.addSeparator();
                m.addItem (30, "volume",      true, laneMode == LaneMode::Volume);
                m.addItem (32, "mute",        true, laneMode == LaneMode::Mute);
                m.addItem (33, "pan",         true, laneMode == LaneMode::Pan);
                m.addItem (34, "click",       true, laneMode == LaneMode::Click);

                juce::Component::SafePointer<TrackRow> self (this);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton),
                                 [self] (int chosen)
                {
                    if (self == nullptr || chosen == 0) return;
                    switch (chosen)
                    {
                        case 20: self->laneMode = LaneMode::Markers;    break;
                        case 22: self->laneMode = LaneMode::Waveform;   break;
                        case 30: self->laneMode = LaneMode::Volume;     break;
                        case 32: self->laneMode = LaneMode::Mute;       break;
                        case 33: self->laneMode = LaneMode::Pan;        break;
                        case 34: self->laneMode = LaneMode::Click;      break;
                        default: return;
                    }
                    // Keep the AUTOMATION toolbar's Lane combo in
                    // lockstep with the per-row VIEW pick, so the
                    // toolbar param the engineer adds/deletes points
                    // for matches what they're looking at. Silent
                    // setter -- doesn't bounce back through
                    // onParamChanged.
                    if (self->toolbar != nullptr)
                    {
                        using P = AutomationToolbar::Param;
                        switch (self->laneMode)
                        {
                            case LaneMode::Volume: self->toolbar->setParamSilently (P::Volume); break;
                            case LaneMode::Pan:    self->toolbar->setParamSilently (P::Pan);    break;
                            case LaneMode::Mute:   self->toolbar->setParamSilently (P::Mute);   break;
                            case LaneMode::Click:  self->toolbar->setParamSilently (P::Click);  break;
                            default: break;   // Waveform / Markers don't map to an automation param
                        }
                    }
                    self->repaint();
                });
            };
            addAndMakeVisible (viewButton);

            // Input + output routing combos -- same wiring as the mixer.
            auto styleCombo = [] (juce::ComboBox& c)
            {
                c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep.withAlpha (brand::alpha::muted));
                c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
            };
            styleCombo (inputCombo);
            styleCombo (outputCombo);

            // Input + output route independently -- the previous build
            // sent both combos through setTrackLinkedRouting which set
            // both sides to the same device channel, so changing the
            // output combo silently overwrote the input routing and
            // vice versa. Each combo now writes only its own side.
            inputCombo.onChange = [this]
            {
                const int id = inputCombo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackInputRouting (index, dev);
                if (stereo)
                    engine.setTrackInputRouting (index + 1, (dev < 0) ? -1 : dev + 1);
            };
            outputCombo.onChange = [this]
            {
                const int id = outputCombo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackOutputRouting (index, dev);
                if (stereo)
                    engine.setTrackOutputRouting (index + 1, (dev < 0) ? -1 : dev + 1);
            };
            addAndMakeVisible (inputCombo);
            addAndMakeVisible (outputCombo);

            rebuildRoutingCombos();
            refreshRoutingSelection();

            // Live signal meter in the middle column of the header.
            // 80 px wide now, so the dB-label gutter has room to draw
            // every tick -- the engineer can read absolute levels from
            // arm's length without flipping back to MIX view.
            meter.setShowDbLabels (true);
            addAndMakeVisible (meter);
            meter.setTooltip ("Live signal level -- click to clear clip.");

            updatePollState();
        }

        // Cheap poll -- called by EditPage::timerCallback so mixer-side
        // changes (mute/solo via mixer, rename, etc.) show up here.
        void updatePollState()
        {
            auto& s = engine.getRecorder().getTrack (index);
            if (armButton .getToggleState() != s.armed .load()) armButton .setToggleState (s.armed .load(), juce::dontSendNotification);
            if (muteButton.getToggleState() != s.muted .load()) muteButton.setToggleState (s.muted .load(), juce::dontSendNotification);
            if (soloButton.getToggleState() != s.soloed.load()) soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
            const auto curName = nameLabel.getText();
            if (curName != s.name) nameLabel.setText (s.name, juce::dontSendNotification);

            // Reflect device topology + patch-page changes.
            const int curIn  = engine.getCurrentDeviceInputCount();
            const int curOut = engine.getCurrentDeviceOutputCount();
            if (curIn != lastInputDeviceCount || curOut != lastOutputDeviceCount)
            {
                lastInputDeviceCount  = curIn;
                lastOutputDeviceCount = curOut;
                rebuildRoutingCombos();
            }
            refreshRoutingSelection();

            // Stripe colour might have changed (mixer right-click → colour
            // picker). Repaint the wash on the next paint cycle.
            const auto argb = s.colourARGB.load();
            if (argb != lastColourArgb)
            {
                lastColourArgb = argb;
                repaint();
            }
        }

        void rebuildRoutingCombos()
        {
            const int numIns  = engine.getCurrentDeviceInputCount();
            const int numOuts = engine.getCurrentDeviceOutputCount();
            const int visibleIn  = juce::jmax (numIns,  index + 1, stereo ? 16 : 8);
            const int visibleOut = juce::jmax (numOuts, index + 1, stereo ? 16 : 8);
            const int step = stereo ? 2 : 1;

            inputCombo.clear (juce::dontSendNotification);
            inputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleIn; i += step)
            {
                const bool live = stereo ? (i + 1 < numIns) : (i < numIns);
                const auto label = stereo
                    ? juce::String ("In ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                    : juce::String ("In ") + juce::String (i + 1);
                inputCombo.addItem (live ? label : (label + " (off)"), i + 2);
            }

            outputCombo.clear (juce::dontSendNotification);
            outputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleOut; i += step)
            {
                const bool live = stereo ? (i + 1 < numOuts) : (i < numOuts);
                const auto label = stereo
                    ? juce::String ("Out ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                    : juce::String ("Out ") + juce::String (i + 1);
                outputCombo.addItem (live ? label : (label + " (off)"), i + 2);
            }
        }

        void refreshRoutingSelection()
        {
            auto& s = engine.getRecorder().getTrack (index);
            int inR  = s.inputRouting .load();
            int outR = s.outputRouting.load();
            if (inR  == -2) inR  = index;
            if (outR == -2) outR = index;
            const int wantIn  = (inR  < 0) ? 1 : inR  + 2;
            const int wantOut = (outR < 0) ? 1 : outR + 2;
            if (inputCombo .getSelectedId() != wantIn ) inputCombo .setSelectedId (wantIn,  juce::dontSendNotification);
            if (outputCombo.getSelectedId() != wantOut) outputCombo.setSelectedId (wantOut, juce::dontSendNotification);
        }

        void setWaveformFiles (const juce::File& fL, const juce::File& fR)
        {
            if (fL != currentFileL)
            {
                currentFileL = fL;
                thumbnailL.setSource (fL.existsAsFile() ? new juce::FileInputSource (fL) : nullptr);
            }
            if (fR != currentFileR)
            {
                currentFileR = fR;
                thumbnailR.setSource (fR.existsAsFile() ? new juce::FileInputSource (fR) : nullptr);
            }
            repaint();
        }

        // Re-issue the thumbnail's input source from the current files so
        // a file that's actively being written (recorder live-capture)
        // gets re-scanned and the waveform grows on screen.
        void reloadCurrentWaveformFiles()
        {
            // setSource consults the AudioThumbnailCache by file hash and, if
            // it finds an entry that claims to be fully loaded, returns it
            // WITHOUT re-reading the file. If the file has since grown (or was
            // first read mid-capture as a fraction of a second), that stale
            // partial would shadow the finished file forever. Drop the cached
            // thumb first so the now-complete file is re-scanned at full length.
            if (currentFileL.existsAsFile())
            {
                thumbCache.removeThumb (currentFileL.hashCode());
                thumbnailL.setSource (new juce::FileInputSource (currentFileL));
            }
            if (currentFileR.existsAsFile())
            {
                thumbCache.removeThumb (currentFileR.hashCode());
                thumbnailR.setSource (new juce::FileInputSource (currentFileR));
            }
            repaint();
        }

        // Drawn position of the playhead within this row, in pixels from
        // the start of the waveform pane. -1 = playhead not visible / no
        // session loaded.
        void setPlayheadX (int px) { playheadX = px; repaint(); }

        void mouseEnter (const juce::MouseEvent&) override
        {
            if (! hovered) { hovered = true; repaint(); }
        }
        void mouseExit (const juce::MouseEvent& e) override
        {
            if (! getLocalBounds().contains (e.getEventRelativeTo (this).getPosition()))
                if (hovered) { hovered = false; repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            // Same stale-index guard as resized() -- a paint cascade
            // can fire after the engine's track vector has shrunk
            // but before TrackList::rebuild has removed this row.
            // Without this guard, getStripColour / the various
            // engine.getRecorder().getTrack (index) calls below
            // dereference out-of-bounds memory.
            if (index >= engine.getRecorder().getNumTracks())
                return;

            auto fillColour = getStripColour();
            // Every TrackRow paints itself in bgStrip (a lighter grey
            // than bgDeep). The EditPage's own background fills the
            // empty area below the last row in bgDeep, so the engineer
            // sees a clear 'rows = light, empty area = dark' contrast.
            const auto headerBg = hovered ? brand::bgStrip.brighter (0.06f)
                                           : brand::bgStrip;
            if (hovered) fillColour = fillColour.brighter (0.06f);

            // ─── Colour swatch column (click to change track colour)
            auto header = getLocalBounds().withWidth (headerW);
            auto swatchArea = header.removeFromLeft (swatchW);
            g.setGradientFill (brand::verticalGradient (fillColour, swatchArea.toFloat(), 0.18f, 0.28f));
            g.fillRect (swatchArea);
            g.setColour (fillColour.darker (0.40f));
            g.drawVerticalLine (swatchArea.getRight() - 1, 0.0f, (float) getHeight());

            // ─── Header background (panel)
            g.setColour (headerBg);
            g.fillRect (header);
            g.setColour (brand::edge);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
            g.drawVerticalLine (headerW - 1, 0.0f, (float) getHeight());

            // ─── Selection highlight (shared with the MIXER) -- a faint
            // brand-orange wash + left stripe on rows the engineer has
            // selected, so Option+R bulk arm has a visible target.
            if (auto* page = findParentComponentOfClass<EditPage>())
                if (page->isTrackSelected && page->isTrackSelected (index))
                {
                    g.setColour (brand::brandOrange.withAlpha (brand::alpha::subtle));
                    g.fillRect (header);
                    g.setColour (brand::brandOrange);
                    g.fillRect (juce::Rectangle<int> (swatchW, 0, 3, getHeight()));
                }

            // The bottom-right 'vol X.X / pan C' readout pill was
            // removed per user request -- the fader + pan values are
            // already visible on the MIXER strip and the EDIT view's
            // role is editing, not metering numbers.

            // ─── Waveform pane
            auto wavePane = getLocalBounds().withTrimmedLeft (headerW);
            g.setColour (headerBg);
            g.fillRect (wavePane);

            // Paint the waveform in the strip's own colour so it tracks
            // any colour changes the user makes in the mixer or EDIT view.
            const auto waveColour = getStripColour().brighter (0.25f);
            const auto inner = wavePane.reduced (brand::space::xs, brand::space::sm);

            // Waveform vertical scale = a GENTLE auto-gain (so a quiet but
            // real take is still readable) multiplied by the user's vertical
            // zoom. The auto-gain caps at 4x -- the old 64x cap turned every
            // idle/unused input on a big session into a solid bar of amplified
            // hiss. Anything below ~-48 dBFS is treated as silent and left at
            // true level, so unused channels read as a clean flat line, not
            // noise. vZoom (V+/V- / Shift+wheel) then scales on top, so those
            // controls always have a visible, consistent effect.
            float vz = 1.0f;
            if (auto* page = findParentComponentOfClass<EditPage>())
                vz = page->getVerticalZoom();
            auto waveZoom = [vz] (const juce::AudioThumbnail& tn) -> float
            {
                if (tn.getTotalLength() <= 0.0) return vz;
                const float peak = tn.getApproximatePeak();
                const float fit  = peak > 0.004f
                                     ? juce::jlimit (1.0f, 4.0f, 0.85f / peak)
                                     : 1.0f;
                return fit * vz;
            };

            // Pro Tools-style: the waveform is ALWAYS the base layer,
            // even when a different lane mode (Volume / Pan / Mute /
            // Click / Tempo / Markers) is selected. The lane data is
            // drawn on top with a semi-transparent backdrop. Paint
            // the waveform here as the substrate; the lane branch
            // below then overlays its content.
            if (laneMode != LaneMode::Waveform)
            {
                if (stereo)
                {
                    const int laneH = inner.getHeight() / 2;
                    auto laneL = inner.withHeight (laneH);
                    auto laneR = inner.withTrimmedTop (laneH);
                    if (thumbnailL.getTotalLength() > 0.0)
                    {
                        g.setColour (waveColour.withAlpha (brand::alpha::muted));
                        thumbnailL.drawChannels (g, laneL, 0.0,
                                                 thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                    }
                    if (thumbnailR.getTotalLength() > 0.0)
                    {
                        g.setColour (waveColour.withAlpha (brand::alpha::muted));
                        thumbnailR.drawChannels (g, laneR, 0.0,
                                                 thumbnailR.getTotalLength(), waveZoom (thumbnailR));
                    }
                }
                else if (thumbnailL.getTotalLength() > 0.0)
                {
                    g.setColour (waveColour.withAlpha (brand::alpha::muted));
                    thumbnailL.drawChannels (g, inner, 0.0,
                                             thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                }
            }

            // -------- Detected transient ticks (Tab-to-Transient hint) --------
            // Only drawn on the ACTIVE row (the one being Tab-navigated) and
            // thinned so dense material doesn't turn the lane top into a
            // solid amber bar. Drawing every onset on every lane made the
            // whole EDIT view look "noisy".
            {
                bool isActiveRow = false;
                if (auto* page = findParentComponentOfClass<EditPage>())
                    isActiveRow = (page->getActiveRowTrackIndex() == index);

                const auto& player = engine.getPlayer();
                const auto totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                const auto& onsets = engine.getTransientsForTrack (index + 1);   // disk is 1-based
                if (isActiveRow && totalSamples > 0 && ! onsets.empty())
                {
                    g.setColour (brand::engagedAmber.withAlpha (0.55f));
                    int lastX = -1000;
                    for (auto pos : onsets)
                    {
                        if (pos <= 0 || pos >= totalSamples) continue;
                        const double prop = (double) pos / (double) totalSamples;
                        const int x = inner.getX() + (int) (prop * inner.getWidth());
                        if (x - lastX < 5) continue;          // thin: min 5 px apart
                        lastX = x;
                        g.drawVerticalLine (x, (float) inner.getY(), (float) (inner.getY() + 6));
                    }
                }
            }

            // Automation lane modes -- flat horizontal line representing
            // the current value (overlaid on top of the dimmed waveform
            // base above). Time-varying automation is reserved for a
            // later build.
            if (laneMode != LaneMode::Waveform)
            {
                auto& t = engine.getRecorder().getTrack (index);
                g.setColour (brand::edge);
                g.drawRect (inner, 1);

                // Centre line for reference (zero / unity).
                g.setColour (brand::edge.brighter (0.2f).withAlpha (brand::alpha::muted));
                g.drawHorizontalLine (inner.getCentreY(),
                                      (float) inner.getX(), (float) inner.getRight());

                juce::Colour lineCol = waveColour;
                float yProp = 0.5f;  // 0 = top, 1 = bottom
                juce::String label;

                // During playback the readout + value indicator follow the
                // automation curve at the playhead (i.e. what you actually
                // hear); when stopped they show the live fader value. Without
                // this the readout sat on the static fader value and looked
                // dead even while automation was driving the gain.
                const auto& plyr = engine.getPlayer();
                const juce::int64 autoPos = plyr.isPlaying() ? plyr.getPositionSamples()
                                                             : (juce::int64) -1;
                using AP = AudioEngine::AutomationParam;

                switch (laneMode)
                {
                    case LaneMode::Volume:
                    {
                        float dB = t.gainDb.load (std::memory_order_relaxed);
                        if (autoPos >= 0)
                            dB = engine.automationValueAt (index, AP::Volume, autoPos, dB);
                        // -60..+12 dB mapped to 1..0 (loud → top)
                        yProp = 1.0f - juce::jlimit (0.0f, 1.0f,
                                                     (dB + 60.0f) / 72.0f);
                        lineCol = brand::accentStatus;
                        label   = "vol " + juce::String (dB, 1) + " dB";
                        break;
                    }
                    case LaneMode::Pan:
                    {
                        float pan = t.pan.load (std::memory_order_relaxed);
                        if (autoPos >= 0)
                            pan = engine.automationValueAt (index, AP::Pan, autoPos, pan);
                        // -1..+1 mapped to 1..0 (left = top, right = bottom)
                        yProp = (1.0f - pan) * 0.5f;
                        lineCol = brand::accentSolo;
                        const int pct = juce::roundToInt (std::abs (pan) * 100.0f);
                        label = (pct == 0) ? "pan C"
                                            : (pan < 0 ? "pan L" + juce::String (pct)
                                                       : "pan R" + juce::String (pct));
                        break;
                    }
                    case LaneMode::Mute:
                    {
                        bool muted = t.muted.load (std::memory_order_relaxed);
                        if (autoPos >= 0)
                            muted = engine.automationValueAt (index, AP::Mute, autoPos,
                                                              muted ? 1.0f : 0.0f) > 0.5f;
                        yProp = muted ? 0.05f : 0.95f;
                        lineCol = muted ? brand::brandOrange : brand::textMuted;
                        label   = muted ? "MUTED" : "open";
                        break;
                    }
                    case LaneMode::Click:
                    {
                        // Beat grid for the whole lane -- every quarter
                        // note at the current session tempo. The lane
                        // adapts its density: if beats would pack closer
                        // than ~6 px we drop to bar markers, then to
                        // 4-bar markers, etc., so the lane never moires
                        // into a solid stripe at long zoom-outs.
                        const float bpm = engine.getSessionTempoBpm();
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples() : 0;
                        const double sr = player.getSampleRate() > 0.0
                            ? player.getSampleRate()
                            : (engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                               ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
                               : 48000.0);
                        if (totalSamples > 0 && bpm > 0.0f && sr > 0.0)
                        {
                            const double samplesPerBeat = 60.0 * sr / bpm;
                            const double pxPerBeat = samplesPerBeat
                                * (double) inner.getWidth()
                                / (double) totalSamples;

                            // Pick a stride: 1 beat / 1 bar (4) / 4 bars
                            // (16) / 16 bars (64). Keeps adjacent ticks
                            // at least ~6 px apart for visual clarity.
                            int stride = 1;
                            while (pxPerBeat * stride < 6.0 && stride < 4096)
                                stride *= 4;

                            int beat = 0;
                            for (double s = 0.0; s < (double) totalSamples;
                                 s += samplesPerBeat * stride, beat += stride)
                            {
                                const double prop = s / (double) totalSamples;
                                const int x = inner.getX()
                                            + (int) (prop * inner.getWidth());
                                const bool downbeat = (beat % 4) == 0;
                                g.setColour (downbeat ? brand::brandOrange
                                                       : brand::accentStatus.withAlpha (brand::alpha::muted));
                                g.drawVerticalLine (x,
                                                    (float) inner.getY(),
                                                    (float) inner.getBottom());
                            }
                        }
                        g.setColour (brand::textTertiary);
                        g.setFont (brand::type::caption());
                        g.drawText ("click " + juce::String (bpm, 1) + " BPM",
                                    inner.reduced (4, 2),
                                    juce::Justification::topLeft, false);
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    case LaneMode::Tempo:
                    {
                        // Shared tempo curve -- one lane for the whole
                        // session, drawn identically on every row. Each
                        // point in engine.getTempoMap() is rendered as
                        // a handle on a stepped curve (40..240 BPM
                        // mapped to bottom..top of the lane).
                        const auto& tempoMap = engine.getTempoMap();
                        const auto& player   = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples()
                            : (juce::int64) (48000.0 * 60.0);

                        auto bpmToY = [&] (float bpm) -> int
                        {
                            const float clamped = juce::jlimit (40.0f, 240.0f, bpm);
                            const float yp = 1.0f - (clamped - 40.0f) / 200.0f;
                            return inner.getY() + juce::roundToInt (yp * inner.getHeight());
                        };
                        auto sampleToX = [&] (juce::int64 sp) -> int
                        { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };

                        const float sessionBpm = engine.getSessionTempoBpm();
                        g.setColour (brand::brandOrange);

                        if (tempoMap.empty())
                        {
                            const int y = bpmToY (sessionBpm);
                            g.drawHorizontalLine (y, (float) inner.getX(), (float) inner.getRight());
                        }
                        else
                        {
                            juce::Path path;
                            int prevY = bpmToY (sessionBpm);
                            path.startNewSubPath ((float) inner.getX(), (float) prevY);
                            for (const auto& tc : tempoMap)
                            {
                                const int x = sampleToX (tc.samplePos);
                                const int y = bpmToY  (tc.bpm);
                                path.lineTo ((float) x, (float) prevY);
                                path.lineTo ((float) x, (float) y);
                                prevY = y;
                            }
                            path.lineTo ((float) inner.getRight(), (float) prevY);
                            g.strokePath (path, juce::PathStrokeType (1.8f));

                            for (const auto& tc : tempoMap)
                            {
                                const int x = sampleToX (tc.samplePos);
                                const int y = bpmToY  (tc.bpm);
                                g.fillEllipse ((float) x - 4.0f, (float) y - 4.0f, 8.0f, 8.0f);
                            }
                        }

                        g.setColour (brand::textTertiary);
                        g.setFont (brand::type::caption());
                        g.drawText ("tempo " + juce::String (sessionBpm, 1) + " BPM",
                                    inner.reduced (6, 2),
                                    juce::Justification::topLeft, false);
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    case LaneMode::Markers:
                    {
                        // Markers are session-wide; draw vertical ticks
                        // (without per-marker time mapping for now -- the
                        // timeline component is authoritative on positions).
                        g.setColour (brand::accentStatus);
                        for (int i = 0; i < 6; ++i)
                        {
                            const float x = inner.getX() + (i + 1) * inner.getWidth() * 0.1f;
                            g.drawVerticalLine ((int) x,
                                                (float) inner.getY(),
                                                (float) inner.getBottom());
                        }
                        g.setColour (brand::textTertiary);
                        g.setFont (brand::type::caption());
                        g.drawText ("markers", inner.reduced (4, 2),
                                    juce::Justification::topLeft, false);
                        // Playhead overlay still applies below.
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    default: break;
                }

                // currentLaneParam() is the single source of truth for
                // which lane this row is editing -- paint, hit-test, and
                // mouseDrag all read from it so they never disagree.
                const auto chosenParam = currentLaneParam();

                const auto& points = engine.getAutomation (index, chosenParam);

                // Value-to-y mapping (mirrors laneCoordAt above).
                auto valueToY = [&] (float v) -> int
                {
                    float yp = 0.5f;
                    switch (chosenParam)
                    {
                        case AudioEngine::AutomationParam::Volume:
                            yp = 1.0f - juce::jlimit (0.0f, 1.0f, (v + 60.0f) / 72.0f);
                            break;
                        case AudioEngine::AutomationParam::Pan:
                            yp = (1.0f - juce::jlimit (-1.0f, 1.0f, v)) * 0.5f;
                            break;
                        case AudioEngine::AutomationParam::Mute:
                            yp = v > 0.5f ? 0.05f : 0.95f;
                            break;
                    }
                    return inner.getY() + juce::roundToInt (yp * inner.getHeight());
                };

                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                   : (juce::int64) (48000.0 * 60.0);
                auto sampleToX = [&] (juce::int64 sp) -> int
                { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };

                if (points.empty())
                {
                    // No automation yet -- fall back to the flat reference
                    // line for the current parameter value, so the lane
                    // still reads at a glance.
                    const int y = inner.getY()
                                + juce::roundToInt (yProp * inner.getHeight());
                    g.setColour (lineCol);
                    g.drawHorizontalLine (y, (float) inner.getX(), (float) inner.getRight());
                }
                else
                {
                    // Render the point sequence as a curve-aware
                    // polyline. Each segment between point i and i+1
                    // uses the curve type pinned on point i: Hold
                    // draws a step, Linear honours per-point tension
                    // for ease-in / ease-out, SCurve uses smoothstep,
                    // legacy ExpUp / ExpDown use their power curves.
                    // The renderer mirrors automationValueAt so what
                    // the engineer sees matches what plays back.
                    g.setColour (lineCol);
                    juce::Path path;
                    const int firstY = valueToY (points.front().value);
                    path.startNewSubPath ((float) inner.getX(), (float) firstY);

                    auto curveShape = [] (double tNorm, AudioEngine::AutomationCurve c,
                                          float tension) -> double
                    {
                        switch (c)
                        {
                            case AudioEngine::AutomationCurve::Hold:    return 0.0;
                            case AudioEngine::AutomationCurve::Linear:
                            {
                                const float tn = juce::jlimit (-1.0f, 1.0f, tension);
                                if (std::abs (tn) < 1.0e-4f) return tNorm;
                                const double exp = std::pow (2.0, (double) (-tn) * 4.0);
                                return std::pow (tNorm, exp);
                            }
                            case AudioEngine::AutomationCurve::SCurve:
                                return tNorm * tNorm * (3.0 - 2.0 * tNorm);
                            case AudioEngine::AutomationCurve::ExpUp:
                                return tNorm * tNorm;
                            case AudioEngine::AutomationCurve::ExpDown:
                                return 1.0 - (1.0 - tNorm) * (1.0 - tNorm);
                        }
                        return tNorm;
                    };

                    for (size_t i = 0; i < points.size(); ++i)
                    {
                        const auto& pt = points[i];
                        const int x  = sampleToX (pt.samplePos);
                        const int y  = valueToY  (pt.value);
                        if (i == 0)
                        {
                            path.lineTo ((float) x, (float) y);
                            continue;
                        }
                        const auto& prev = points[i - 1];
                        const int xPrev = sampleToX (prev.samplePos);
                        const int yPrev = valueToY  (prev.value);
                        if (prev.curve == AudioEngine::AutomationCurve::Hold)
                        {
                            // Step: hold prev's y across the segment.
                            path.lineTo ((float) x,    (float) yPrev);
                            path.lineTo ((float) x,    (float) y);
                            continue;
                        }
                        const int dx = juce::jmax (1, x - xPrev);
                        // Step roughly every 4 px so curved segments
                        // read smoothly without flooding the path
                        // with sub-pixel verts on long segments.
                        const int steps = juce::jlimit (4, 240, dx / 4);
                        for (int s = 1; s <= steps; ++s)
                        {
                            const double tNorm = (double) s / (double) steps;
                            const double shaped = curveShape (tNorm, prev.curve, prev.tension);
                            const double v = (double) prev.value
                                           + shaped * (double) (pt.value - prev.value);
                            const int xx = xPrev + (int) std::round (tNorm * (double) (x - xPrev));
                            const int yy = valueToY ((float) v);
                            path.lineTo ((float) xx, (float) yy);
                        }
                    }
                    path.lineTo ((float) inner.getRight(),
                                 (float) valueToY (points.back().value));
                    g.strokePath (path, juce::PathStrokeType (1.6f));

                    // Tension drag-handles on non-Hold segments.
                    // Painted as a small hollow circle at the curve's
                    // actual midpoint Y so dragging it matches what
                    // the eye sees. Skip segments that are too narrow
                    // (< 18 px) to host a handle without colliding
                    // with the endpoints.
                    const juce::Colour handleCol = lineCol.brighter (0.35f);
                    for (size_t i = 1; i < points.size(); ++i)
                    {
                        const auto& prev = points[i - 1];
                        if (prev.curve == AudioEngine::AutomationCurve::Hold) continue;
                        if (chosenParam == AudioEngine::AutomationParam::Mute) continue;
                        const auto& next = points[i];
                        if (std::abs (next.value - prev.value) < 1.0e-4f) continue;
                        const int xPrev = sampleToX (prev.samplePos);
                        const int xNext = sampleToX (next.samplePos);
                        if (xNext - xPrev < 18) continue;
                        const double shapedMid = curveShape (0.5, prev.curve, prev.tension);
                        const double vMid = (double) prev.value
                                          + shapedMid * (double) (next.value - prev.value);
                        const int xMid = (xPrev + xNext) / 2;
                        const int yMid = valueToY ((float) vMid);
                        g.setColour (handleCol.withAlpha (brand::alpha::prominent));
                        g.fillEllipse ((float) xMid - 2.8f, (float) yMid - 2.8f, 5.6f, 5.6f);
                        g.setColour (brand::bgPanel);
                        g.drawEllipse ((float) xMid - 2.8f, (float) yMid - 2.8f, 5.6f, 5.6f, 1.0f);
                    }

                    // Keyboard focus ring -- visible only when this row
                    // is the active row AND a point is focused (set by
                    // Left/Right arrow navigation in MainComponentKeys).
                    int focusedIdx = -1;
                    if (auto* parentPage = findParentComponentOfClass<EditPage>())
                        if (parentPage->getActiveRowTrackIndex() == index)
                            focusedIdx = parentPage->getFocusedPointIdx();

                    for (size_t i = 0; i < points.size(); ++i)
                    {
                        const auto& pt = points[i];
                        const int x = sampleToX (pt.samplePos);
                        const int y = valueToY  (pt.value);
                        g.setColour (lineCol);
                        g.fillEllipse ((float) x - 3.5f, (float) y - 3.5f, 7.0f, 7.0f);
                        g.setColour (brand::shadow::elev3());
                        g.drawEllipse ((float) x - 3.5f, (float) y - 3.5f, 7.0f, 7.0f, 1.0f);
                        if ((int) i == focusedIdx)
                        {
                            g.setColour (brand::accentPlay);
                            g.drawEllipse ((float) x - 6.0f, (float) y - 6.0f,
                                           12.0f, 12.0f, 1.5f);
                        }
                    }
                }

                // Value readout (e.g. "vol 0.0 dB") -- always pinned
                // to the RIGHT EDGE OF THE VISIBLE VIEWPORT, not the
                // right edge of the (possibly very wide) zoomed-in
                // lane content. Without this clamp the label sits at
                // the far right of the full content rect and scrolls
                // off-screen at any zoom > 1×, leaving the engineer
                // with no readout to dial automation against.
                int labelRight = inner.getRight();
                if (auto* vp = findParentComponentOfClass<juce::Viewport>())
                {
                    const int visRightInList = vp->getViewPositionX() + vp->getViewWidth();
                    const int visRightInRow  = visRightInList - getX();
                    if (visRightInRow < labelRight)
                        labelRight = visRightInRow;
                }
                const int labelW = 110;
                const int labelX = juce::jmax (inner.getX(),
                                               labelRight - labelW);
                juce::Rectangle<int> labelRect (labelX, inner.getY(),
                                                 labelRight - labelX, inner.getHeight());
                // Translucent backing pill so the value stays legible
                // even when it overlaps automation curve / waveform.
                g.setColour (brand::bgDeep.withAlpha (0.78f));
                g.fillRoundedRectangle (labelRect.reduced (2, 2).toFloat(),
                                        brand::radius::sm);
                g.setColour (brand::textPrimary);
                g.setFont (brand::type::uiLabel());
                g.drawText (label,
                            labelRect.reduced (6, 2),
                            juce::Justification::topRight, false);

                if (playheadX >= 0 && playheadX < wavePane.getWidth())
                {
                    g.setColour (brand::accentPlay.withAlpha (brand::alpha::prominent));
                    g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                      0, 2, getHeight()));
                }
                return;
            }

            if (stereo)
            {
                // L on top, R on bottom -- Pro-Tools-style stereo lanes.
                const int laneH = inner.getHeight() / 2;
                auto laneL = inner.withHeight (laneH);
                auto laneR = inner.withTrimmedTop (laneH);

                if (thumbnailL.getTotalLength() > 0.0)
                {
                    g.setColour (waveColour);
                    thumbnailL.drawChannels (g, laneL, 0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                }
                if (thumbnailR.getTotalLength() > 0.0)
                {
                    g.setColour (waveColour);
                    thumbnailR.drawChannels (g, laneR, 0.0, thumbnailR.getTotalLength(), waveZoom (thumbnailR));
                }
                // Thin divider between lanes
                g.setColour (brand::edge);
                g.drawHorizontalLine (inner.getY() + laneH, (float) inner.getX(),
                                       (float) inner.getRight());
                // Empty lanes are left blank -- the EDIT view's PlaceholderView
                // owns the "no session" message, so no per-row hint here.
            }
            else if (thumbnailL.getTotalLength() > 0.0)
            {
                g.setColour (waveColour);
                thumbnailL.drawChannels (g, inner, 0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
            }

            // ─── Clip boundary overlay (waveform mode only) ─────────
            // After Edit ▸ Split / Separate, the track grows a clip list.
            // Paint each clip boundary as a 1 px vertical cut + a small
            // ⌐ marker at the top of the lane so the engineer can see
            // where the split landed.
            if (auto* clips = engine.tryClipsFor (index))
            {
                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                if (totalSamples > 0)
                {
                    const auto inner2 = wavePane.reduced (brand::space::xs, brand::space::sm);
                    auto sampleToX = [&] (juce::int64 sp) -> int
                    { return TimelineMapper::forLane (inner2, totalSamples).toXFloor (sp); };
                    for (const auto& c : *clips)
                    {
                        const int xL_ = sampleToX (c.timelineStartSamples);
                        const int xR_ = sampleToX (c.timelineStartSamples + c.fileLengthSamples);
                        if (c.muted)
                        {
                            // Wash the muted clip's lane span with a soft
                            // mute scrim so it visibly drops below the rest.
                            g.setColour (brand::signalMute().withAlpha (0.22f));
                            g.fillRect (juce::Rectangle<int> (xL_, inner2.getY(),
                                                              juce::jmax (1, xR_ - xL_),
                                                              inner2.getHeight()));
                        }
                        if (c.locked)
                        {
                            // Top-right corner lock glyph (4×4 px square +
                            // shackle dot) so a held clip reads at a glance.
                            const int lx = juce::jmax (xL_, xR_ - 9);
                            const int ly = inner2.getY() + 2;
                            g.setColour (brand::textPrimary.withAlpha (brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (lx,     ly + 2, 6, 4));
                            g.drawRect (juce::Rectangle<int> (lx + 1, ly,     4, 4), 1);
                        }
                        if (c.timelineStartSamples > 0)
                        {
                            const int x = sampleToX (c.timelineStartSamples);
                            g.setColour (brand::accentSolo.withAlpha (brand::alpha::prominent));
                            g.drawVerticalLine (x, (float) inner2.getY(),
                                                (float) inner2.getBottom());
                            // Tiny corner flag at the top so the cut is
                            // visible against busy audio.
                            g.fillRect (juce::Rectangle<int> (x, inner2.getY(), 6, 3));
                        }
                        // Fade-in diagonal -- from the bottom-left corner
                        // of the clip up to the top of (start + fadeIn).
                        {
                            const int xL = sampleToX (c.timelineStartSamples);
                            const int xF = sampleToX (c.timelineStartSamples + c.fadeInSamples);
                            if (c.fadeInSamples > 0)
                            {
                                g.setColour (brand::accentStatus.withAlpha (brand::alpha::ghost));
                                juce::Path p;
                                p.startNewSubPath ((float) xL, (float) inner2.getBottom());
                                p.lineTo ((float) xF, (float) inner2.getY());
                                g.strokePath (p, juce::PathStrokeType (1.4f));
                            }
                            // Drag handle at the apex (top of the
                            // diagonal). Visible even when fadeIn == 0
                            // so the engineer has a grab-target to
                            // introduce a fade from zero.
                            const juce::Rectangle<float> handle (
                                (float) xF - 3.5f, (float) inner2.getY(),
                                7.0f, 7.0f);
                            g.setColour (brand::accentStatus);
                            g.fillRect (handle);
                        }
                        {
                            const int xR = sampleToX (c.timelineStartSamples + c.fileLengthSamples);
                            const int xF = sampleToX (c.timelineStartSamples + c.fileLengthSamples - c.fadeOutSamples);
                            if (c.fadeOutSamples > 0)
                            {
                                g.setColour (brand::accentStatus.withAlpha (brand::alpha::ghost));
                                juce::Path p;
                                p.startNewSubPath ((float) xF, (float) inner2.getY());
                                p.lineTo ((float) xR, (float) inner2.getBottom());
                                g.strokePath (p, juce::PathStrokeType (1.4f));
                            }
                            const juce::Rectangle<float> handle (
                                (float) xF - 3.5f, (float) inner2.getY(),
                                7.0f, 7.0f);
                            g.setColour (brand::accentStatus);
                            g.fillRect (handle);
                        }
                    }

                    // -------- Crossfade visualisation --------
                    // Pro Tools-style. When two adjacent clips
                    // overlap on the timeline, paint a combined
                    // X shape on the overlap span: descending line
                    // for the outgoing clip, ascending for the
                    // incoming. Translucent green fill underneath
                    // ties them together visually. Engineers can
                    // tell at a glance that "these two clips
                    // crossfade" instead of seeing two separate
                    // hard edits.
                    for (size_t ci = 0; ci + 1 < clips->size(); ++ci)
                    {
                        const auto& a = (*clips)[ci];
                        const auto& b = (*clips)[ci + 1];
                        const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                        const auto bStart = b.timelineStartSamples;
                        if (bStart >= aEnd) continue;          // no overlap
                        const auto overlap = aEnd - bStart;
                        if (overlap < 64) continue;            // too small to read
                        const int xL = sampleToX (bStart);
                        const int xR = sampleToX (aEnd);
                        if (xR - xL < 4) continue;
                        const auto band = juce::Rectangle<int> (xL, inner2.getY(),
                                                                xR - xL,
                                                                inner2.getHeight());
                        g.setColour (brand::accentStatus.withAlpha (0.10f));
                        g.fillRect (band);
                        g.setColour (brand::accentStatus.withAlpha (brand::alpha::prominent));
                        // Outgoing (a) -- top-left to bottom-right.
                        g.drawLine ((float) xL, (float) inner2.getY(),
                                    (float) xR, (float) inner2.getBottom(), 1.4f);
                        // Incoming (b) -- bottom-left to top-right.
                        g.drawLine ((float) xL, (float) inner2.getBottom(),
                                    (float) xR, (float) inner2.getY(), 1.4f);
                        // Midpoint handle for future drag-to-adjust.
                        const int xMid = (xL + xR) / 2;
                        const int yMid = inner2.getY() + inner2.getHeight() / 2;
                        g.setColour (brand::accentStatus);
                        g.fillEllipse ((float) xMid - 3.0f, (float) yMid - 3.0f,
                                       6.0f, 6.0f);
                    }
                }
            }

            // ─── Loop-region overlay (set by the Selector tool)
            {
                const auto& player = engine.getPlayer();
                if (player.hasLoopRegion() && player.isLoaded())
                {
                    const auto total = player.getTotalLengthSamples();
                    if (total > 0)
                    {
                        const auto sampleToWavX = [&] (juce::int64 sp) -> int
                        {
                            const double prop = juce::jlimit (0.0, 1.0,
                                                              (double) sp / (double) total);
                            return juce::roundToInt (prop * wavePane.getWidth());
                        };
                        const int xA = sampleToWavX (player.getLoopStart());
                        const int xB = sampleToWavX (player.getLoopEnd());
                        if (xB > xA)
                        {
                            const juce::Rectangle<int> band (
                                headerW + xA, 0, xB - xA, getHeight());
                            g.setColour (brand::accentEdit.withAlpha (brand::alpha::subtle));
                            g.fillRect (band);
                            g.setColour (brand::accentEdit.withAlpha (0.75f));
                            g.drawVerticalLine (band.getX(),     0.0f, (float) getHeight());
                            g.drawVerticalLine (band.getRight(), 0.0f, (float) getHeight());
                        }
                    }
                }
            }

            // ─── Take indicator -- small "TAKE N / M" chip in the row
            // header so the engineer sees the active comp take without
            // opening the right-click menu.
            {
                const int takeCount  = engine.getTakeCount (index);
                const int activeTake = engine.getActiveTakeIdx (index);
                if (takeCount > 1)
                {
                    const auto label = "TAKE " + juce::String (activeTake + 1)
                                     + " / " + juce::String (takeCount);
                    auto chip = juce::Rectangle<int> (headerW - 78, 4, 70, 13);
                    g.setColour (brand::featureEngaged.darker (0.30f));
                    g.fillRoundedRectangle (chip.toFloat(), brand::radius::sm);
                    g.setColour (brand::featureEngaged.brighter (0.40f));
                    g.drawRoundedRectangle (chip.toFloat(), brand::radius::sm, 0.75f);
                    g.setColour (brand::onSignal (brand::featureEngaged.darker (0.30f)));
                    g.setFont (brand::type::caption());
                    g.drawText (label, chip, juce::Justification::centred, false);
                }
            }

            // ─── Edit cursor overlay (Pro Tools-style insertion point)
            // Painted BEHIND the playhead so the engineer can see both
            // at once when the player is paused at a different sample.
            // Uses the same 5-min notional span as mouseDown when no
            // audio is loaded, so the cursor stays visible on a fresh
            // empty session before any recording happens.
            {
                const auto cursorSample = engine.getEditCursorSample();
                if (cursorSample >= 0)
                {
                    const auto& player2 = engine.getPlayer();
                    const juce::int64 loadedSamples2 = player2.isLoaded()
                        ? player2.getTotalLengthSamples() : 0;
                    const double sr2 = player2.getSampleRate() > 0.0
                                    ? player2.getSampleRate() : 48000.0;
                    const juce::int64 totalSamples2 = loadedSamples2 > 0
                        ? loadedSamples2 : (juce::int64) (sr2 * 300.0);
                    const double prop = juce::jlimit (0.0, 1.0,
                        (double) cursorSample / (double) totalSamples2);
                    const int cx = juce::roundToInt (prop * (wavePane.getWidth() - 8)) + 4;
                    g.setColour (brand::textPrimary.withAlpha (brand::alpha::bold));
                    g.fillRect (juce::Rectangle<int> (headerW + cx, 0, 2, getHeight()));
                }
            }

            // ─── Playhead overlay
            if (playheadX >= 0 && playheadX < wavePane.getWidth())
            {
                g.setColour (brand::accentPlay.withAlpha (brand::alpha::prominent));
                g.fillRect (juce::Rectangle<int> (headerW + playheadX, 0, 2, getHeight()));
            }
        }

        void resized() override
        {
            // Stale-index guard. When the engineer creates a new
            // session, MainComponent calls engine.setStripCount(0)
            // immediately -- the recorder's track vector shrinks
            // synchronously. But the TrackList's TrackRows still
            // live until EditPage's 24 Hz timer fires rebuild().
            // If a resize cascades through during that gap, this row
            // would dereference recorder.getTrack(stale_index) and
            // crash (EXC_BAD_ACCESS at ~0x2090 -- the 'name' field).
            if (index >= engine.getRecorder().getNumTracks())
                return;

            // Pro Tools-style three-column header.
            //   [swatch | LEFT block | METER block | RIGHT block]
            //              name+         wide       input/output
            //              R/I/S/M       LedMeter   + vol/pan
            //              + view btn    with dB    readout
            //                            labels
            auto header = getLocalBounds().withWidth (headerW);
            header.removeFromLeft (swatchW);
            header.removeFromLeft (4);

            const bool isClickRow =
                engine.getRecorder().getTrack (index).name == "Click";

            // -------------- LEFT (name + buttons) --------------
            constexpr int leftBlockW = 140;
            auto leftBlock = header.removeFromLeft (leftBlockW).reduced (brand::space::xs, brand::space::xs);

            nameLabel.setBounds (leftBlock.removeFromTop (18));
            leftBlock.removeFromTop (3);

            const int btnH = 22;
            if (isClickRow)
            {
                armButton .setVisible (false); armButton .setBounds ({});
                monButton .setVisible (false); monButton .setBounds ({});
                auto row = leftBlock.removeFromTop (btnH);
                const int half = row.getWidth() / 2;
                soloButton.setBounds (row.removeFromLeft (half).reduced (1));
                muteButton.setBounds (row.reduced (1));
            }
            else
            {
                armButton.setVisible (true);
                monButton.setVisible (true);
                // Four buttons across one row, Pro Tools-style:
                //   [ REC ][  I  ][  S  ][  M  ]
                auto row = leftBlock.removeFromTop (btnH);
                const int quarter = row.getWidth() / 4;
                armButton .setBounds (row.removeFromLeft (quarter).reduced (1));
                monButton .setBounds (row.removeFromLeft (quarter).reduced (1));
                soloButton.setBounds (row.removeFromLeft (quarter).reduced (1));
                muteButton.setBounds (row.reduced (1));
            }
            leftBlock.removeFromTop (4);

            // The VIEW button (lane-mode picker) takes the bottom-left.
            // On Click rows it's hidden -- metronome lane has no choice.
            viewButton.setVisible (! isClickRow);
            if (! isClickRow)
                viewButton.setBounds (leftBlock.removeFromTop (18));
            else
                viewButton.setBounds ({});

            // -------------- MIDDLE (meter) --------------
            // Wide LedMeter -- 80 px column with full dB labels so the
            // engineer can read levels at a glance from the EDIT view
            // without flipping to MIX.
            constexpr int meterBlockW = 80;
            meter.setBounds (header.removeFromLeft (meterBlockW).reduced (3, 4));
            header.removeFromLeft (4);

            // -------------- RIGHT (routing + vol/pan readout) --------------
            auto rightBlock = header.removeFromLeft (header.getWidth() - 8).reduced (2, 4);

            inputCombo .setBounds (rightBlock.removeFromTop (brand::space::ioH));
            rightBlock.removeFromTop (3);
            outputCombo.setBounds (rightBlock.removeFromTop (brand::space::ioH));
            // Vol / pan readout area is painted directly (no child
            // component) so the rest of the right block is left for
            // paint() to label.
        }

        bool isInResizeZone (juce::Point<int> p) const noexcept
        {
            return p.y >= getHeight() - kResizeZoneH;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            // Avoid spamming setMouseCursor on every pixel of mouse motion --
            // only flip when the resize-zone hit state changes.
            const bool inZone = isInResizeZone (e.getPosition());
            if (inZone != cursorIsResize)
            {
                cursorIsResize = inZone;
                setMouseCursor (inZone ? juce::MouseCursor::UpDownResizeCursor
                                       : juce::MouseCursor::NormalCursor);
            }
        }

        // Map an (x,y) inside the lane area to (samplePos, value) so
        // mouse interactions can place / move automation points.
        struct LaneCoord { juce::int64 samplePos; float value; };
        LaneCoord laneCoordAt (juce::Point<int> p) const
        {
            const auto inner = getLocalBounds().withTrimmedLeft (headerW)
                                                .reduced (brand::space::xs, brand::space::sm);
            const auto& player = engine.getPlayer();
            const juce::int64 total = player.isLoaded() ? player.getTotalLengthSamples()
                                                        : (juce::int64) (engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                                                            ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate() * 60.0
                                                            : 48000.0 * 60.0);
            const double prop = (double) (p.x - inner.getX()) / juce::jmax (1, inner.getWidth());
            const juce::int64 samplePos = (juce::int64) (juce::jlimit (0.0, 1.0, prop) * (double) total);

            // Y → value depends on the active param.
            float value = 0.0f;
            if (toolbar != nullptr)
            {
                const float yProp = juce::jlimit (0.0f, 1.0f,
                                                  (float) (p.y - inner.getY()) / juce::jmax (1, inner.getHeight()));
                switch (toolbar->getParam())
                {
                    case AutomationToolbar::Param::Volume:
                        value = (1.0f - yProp) * 72.0f - 60.0f; // 1..0 → -60..+12
                        break;
                    case AutomationToolbar::Param::Pan:
                        value = 1.0f - yProp * 2.0f;            // 1..0 → -1..+1 inverted
                        break;
                    case AutomationToolbar::Param::Mute:
                        value = yProp < 0.5f ? 1.0f : 0.0f;
                        break;
                }
            }
            return { samplePos, value };
        }

        static AudioEngine::AutomationParam toEngineParam (AutomationToolbar::Param p)
        {
            switch (p)
            {
                case AutomationToolbar::Param::Volume: return AudioEngine::AutomationParam::Volume;
                case AutomationToolbar::Param::Pan:    return AudioEngine::AutomationParam::Pan;
                case AutomationToolbar::Param::Mute:   return AudioEngine::AutomationParam::Mute;
            }
            return AudioEngine::AutomationParam::Volume;
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Mark this row as the active row for Tab-to-Transient.
            // Any subsequent Tab press will restrict the onset
            // search to this track's onsets only (instead of the
            // pooled cross-track list). Reset by clicking on a
            // different row.
            if (auto* page = findParentComponentOfClass<EditPage>())
            {
                page->setActiveRowTrackIndex (index);

                // Header click (between the swatch column and the wave
                // pane, i.e. empty header space -- the R/M/Mu/S buttons
                // and combos are child components that swallow their own
                // clicks) selects this channel into the shared MIXER/EDIT
                // selection. Shift/Cmd extends; a plain click selects only
                // this one. Drives Option+R bulk arm from the EDIT view.
                if (! (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
                    && e.x >= swatchW && e.x < headerW
                    && page->onRowSelect)
                {
                    page->onRowSelect (index, e.mods.isShiftDown()
                                              || e.mods.isCommandDown());
                }
            }

            // Pro Tools-style edit cursor. Any left-click that lands
            // in the wave pane sets the edit cursor at the corresponding
            // sample position. Works whether or not audio is loaded:
            // with no audio we use a notional 5-min span (matching
            // EditTimeRuler) so a fresh session lets the engineer place
            // the cursor anywhere and drop markers before recording.
            if (! (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
                && e.x >= headerW)
            {
                const auto& player = engine.getPlayer();
                const juce::int64 loadedSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                const double sr = player.getSampleRate() > 0.0
                                ? player.getSampleRate() : 48000.0;
                const juce::int64 totalSamples = loadedSamples > 0
                    ? loadedSamples
                    : (juce::int64) (sr * 300.0);    // 5-min notional span
                const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                const double prop = juce::jlimit (0.0, 1.0,
                    (double) (e.x - inner.getX())
                        / (double) juce::jmax (1, inner.getWidth()));
                const juce::int64 sx = (juce::int64) (prop * (double) totalSamples);
                engine.setEditCursorSample (sx);
            }

            // Right-click on a clip in the waveform lane → fade menu.
            // Anywhere else on the row → row-size menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                if (laneMode == LaneMode::Waveform && e.x >= headerW)
                {
                    if (auto* clips = engine.tryClipsFor (index))
                    {
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples() : 0;
                        if (totalSamples > 0)
                        {
                            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                            const auto sampleToX = [&] (juce::int64 sp) -> int
                            { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };
                            for (int i = 0; i < (int) clips->size(); ++i)
                            {
                                const auto& c = (*clips)[(size_t) i];
                                const int xL = sampleToX (c.timelineStartSamples);
                                const int xR = sampleToX (c.timelineStartSamples + c.fileLengthSamples);
                                if (e.x >= xL && e.x <= xR)
                                {
                                    showFadeMenu (i, e.getScreenPosition());
                                    return;
                                }
                            }
                        }
                    }
                }
                // Automation lane right-click priority:
                //   1. directly on a point handle  -> curve-type picker
                //   2. with a loop region set      -> range copy / paste / clear
                //   3. otherwise                   -> row-size menu
                if (laneMode != LaneMode::Waveform && laneMode != LaneMode::Markers
                    && e.x >= headerW)
                {
                    const int hitPoint = hitTestAutomationPoint (e.getPosition());
                    if (hitPoint >= 0)
                    {
                        showCurvePickerMenu (hitPoint, e.getScreenPosition());
                        return;
                    }
                    if (engine.getPlayer().hasLoopRegion())
                    {
                        showAutomationRangeMenu (e.getScreenPosition());
                        return;
                    }
                }
                showSizeMenu (e.getScreenPosition());
                return;
            }
            // Left-click in the bottom resize zone → start a drag-resize.
            if (isInResizeZone (e.getPosition()))
            {
                dragStartHeight = getHeight();
                dragging        = true;
                return;
            }
            // Left-click on the coloured swatch column → starts a
            // reorder-drag. If the engineer doesn't move past the
            // 8 px threshold, mouseUp falls through to the colour
            // picker (preserves the legacy 'click swatch = colour').
            if (e.x < swatchW)
            {
                reorderArmed  = true;
                reorderActive = false;
                reorderStartY = e.y;
                return;
            }

            // Pro Tools-style edit tool -- Scrubber, Selector, and Fade
            // intercept the left-click before the normal clip-drag
            // hit-test. Trim / Grabber bias the clip-body branch below.
            // None (nothing selected) behaves identically to Smart.
            const auto activeTool = (toolsBar != nullptr)
                                      ? toolsBar->getTool()
                                      : EditToolsBar::Tool::None;

            if (laneMode == LaneMode::Waveform && e.x >= headerW
                && (activeTool == EditToolsBar::Tool::Scrubber
                 || activeTool == EditToolsBar::Tool::Selector
                 || activeTool == EditToolsBar::Tool::Fade))
            {
                auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                if (totalSamples > 0)
                {
                    const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                    const auto xToSample = [&] (int x) -> juce::int64
                    { return TimelineMapper::forLane (inner, totalSamples).toSample (x); };

                    if (activeTool == EditToolsBar::Tool::Fade)
                    {
                        if (auto* clips = engine.tryClipsFor (index))
                        {
                            const auto sx = xToSample (e.x);
                            for (int i = 0; i < (int) clips->size(); ++i)
                            {
                                const auto& c = (*clips)[(size_t) i];
                                if (sx >= c.timelineStartSamples
                                    && sx <= c.timelineStartSamples + c.fileLengthSamples)
                                {
                                    showFadeMenu (i, e.getScreenPosition());
                                    return;
                                }
                            }
                        }
                        return;
                    }

                    // Scrubber + Selector: park the playhead at the
                    // click sample. Selector additionally seeds a loop
                    // region whose end follows mouseDrag.
                    const auto sx = xToSample (e.x);
                    player.setPositionSamples (sx);
                    if (activeTool == EditToolsBar::Tool::Selector)
                    {
                        player.clearLoopRegion();
                        dragStartX          = e.x;
                        lastDragSamples     = sx;     // store anchor sample
                        draggingClipIdx     = -1;
                        draggingClipModeInt = 6;      // 6 = SelectRange
                    }
                    else
                    {
                        draggingClipModeInt = 5;      // 5 = Scrub
                    }
                    repaint();
                    return;
                }
            }

            // Snapshot clip state for Cmd+Z before any clip-edit drag
            // (crossfade / trim / move / fade) can arm below. The commit
            // in mouseUp no-ops when nothing actually moved.
            if (laneMode == LaneMode::Waveform && e.x >= headerW)
                if (auto* page = findParentComponentOfClass<EditPage>())
                    page->beginClipEdit();

            // Crossfade midpoint drag wins over clip-edit drag --
            // the handle sits inside the overlap band so the engineer
            // expects clicking it to grab the crossfade balance
            // rather than starting a clip move on the underlying clip.
            if (laneMode == LaneMode::Waveform && e.x >= headerW)
            {
                const int xfadeA = hitTestCrossfadeHandle (e.getPosition());
                if (xfadeA >= 0)
                {
                    draggingXfadeAIdx = xfadeA;
                    return;
                }
            }

            // Clip drag-edit. Only when we're on the waveform lane (so
            // automation lanes still own their own drag semantics) and
            // the track actually has clips to grab.
            if (laneMode == LaneMode::Waveform && e.x >= headerW)
            {
                if (auto* clips = engine.tryClipsFor (index))
                {
                    const auto& player = engine.getPlayer();
                    const juce::int64 totalSamples = player.isLoaded()
                        ? player.getTotalLengthSamples() : 0;
                    if (totalSamples > 0)
                    {
                        const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                        const auto sampleToX = [&] (juce::int64 sp) -> int
                        { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };
                        // 6 px hit zone around each edge for trim;
                        // anything else inside a clip's body = Move.
                        // Fade handles take priority over both -- they
                        // sit in the top-edge stripe and are visually
                        // distinct dots.
                        const int hitZone = 6;
                        const int fadeHandleZone = 8;
                        const int laneTop = inner.getY();
                        for (int i = 0; i < (int) clips->size(); ++i)
                        {
                            const auto& c = (*clips)[(size_t) i];
                            const int xL = sampleToX (c.timelineStartSamples);
                            const int xR = sampleToX (c.timelineStartSamples + c.fileLengthSamples);
                            const int xFadeIn  = sampleToX (c.timelineStartSamples + c.fadeInSamples);
                            const int xFadeOut = sampleToX (c.timelineStartSamples
                                                            + c.fileLengthSamples
                                                            - c.fadeOutSamples);

                            // Fade-in handle -- top-edge band, near
                            // (start + fadeIn).
                            if (e.y - laneTop < fadeHandleZone
                                && std::abs (e.x - xFadeIn) <= fadeHandleZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipModeInt = 3;  // FadeIn
                                dragStartX          = e.x;
                                lastDragSamples     = 0;
                                dragStartFadeIn     = c.fadeInSamples;
                                dragStartFadeOut    = c.fadeOutSamples;
                                return;
                            }
                            // Fade-out handle.
                            if (e.y - laneTop < fadeHandleZone
                                && std::abs (e.x - xFadeOut) <= fadeHandleZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipModeInt = 4;  // FadeOut
                                dragStartX          = e.x;
                                lastDragSamples     = 0;
                                dragStartFadeIn     = c.fadeInSamples;
                                dragStartFadeOut    = c.fadeOutSamples;
                                return;
                            }

                            const bool inClipBody = (e.x >= xL && e.x <= xR);

                            // Grabber: every click inside the clip body
                            // = Move (no edge trim).
                            if (activeTool == EditToolsBar::Tool::Grabber)
                            {
                                if (inClipBody)
                                {
                                    draggingClipIdx     = i;
                                    draggingClipModeInt = 2;  // Move
                                    dragStartX = e.x;
                                    lastDragSamples = 0;
                                    return;
                                }
                                continue;
                            }

                            // Trim: any body click trims from the nearer
                            // edge (left half → TrimLeft, right half →
                            // TrimRight).
                            if (activeTool == EditToolsBar::Tool::Trim)
                            {
                                if (inClipBody)
                                {
                                    const bool leftHalf = (e.x - xL) < ((xR - xL) / 2);
                                    draggingClipIdx     = i;
                                    draggingClipModeInt = leftHalf ? 0 : 1;
                                    dragStartX = e.x;
                                    lastDragSamples = 0;
                                    return;
                                }
                                continue;
                            }

                            // Smart (default) -- edge zones trim, body
                            // moves.
                            if (e.x >= xL - hitZone && e.x <= xL + hitZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipModeInt = 0;  // TrimLeft
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                            if (e.x >= xR - hitZone && e.x <= xR + hitZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipModeInt = 1;  // TrimRight
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                            if (e.x > xL + hitZone && e.x < xR - hitZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipModeInt = 2;  // Move
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                        }
                    }
                }
            }

            // Lane-area interaction -- only when the toolbar is wired and
            // this row isn't the Click track itself.
            if (toolbar != nullptr && index != clickRowIdx && e.x >= headerW)
            {
                const auto coord = laneCoordAt (e.getPosition());
                const auto p     = toEngineParam (toolbar->getParam());

                // Special case: Tempo param edits the engine's shared
                // tempo map instead of the per-track point store.
                if (toolbar->getParam() == AutomationToolbar::Param::Tempo)
                {
                    const auto& player = engine.getPlayer();
                    const juce::int64 totalSamples = player.isLoaded()
                        ? player.getTotalLengthSamples()
                        : (juce::int64) (48000.0 * 60.0);
                    const juce::int64 tol = juce::jmax<juce::int64> (1,
                        totalSamples / juce::jmax (1, getWidth() - headerW) * 8);
                    // Y → BPM (40..240 range mirrors the paint mapping).
                    const auto inner2 = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                    const float yProp = juce::jlimit (0.0f, 1.0f,
                        (float) (e.y - inner2.getY()) / juce::jmax (1, inner2.getHeight()));
                    const float bpm = juce::jlimit (40.0f, 240.0f,
                                                    40.0f + (1.0f - yProp) * 200.0f);

                    switch (toolbar->getTool())
                    {
                        case AutomationToolbar::Tool::AddPoint:
                            engine.addTempoChange (coord.samplePos, bpm);
                            break;
                        case AutomationToolbar::Tool::DeletePoint:
                            engine.removeTempoChangeNear (coord.samplePos, tol);
                            break;
                        case AutomationToolbar::Tool::Select:
                            // Tempo drag isn't wired yet -- Select on
                            // the Tempo lane just records nothing.
                            break;
                    }
                    repaint();
                    return;
                }

                // Helper: route the engine call through the optional
                // undo wrapper. Plain direct call when no wrapper is
                // wired (degraded mode -- still functional, just no
                // Cmd+Z for this edit).
                auto editWrapped = [this] (const char* label, std::function<void()> fn)
                {
                    if (automationEditWrapper) automationEditWrapper (label, std::move (fn));
                    else                       fn();
                };

                switch (toolbar->getTool())
                {
                    case AutomationToolbar::Tool::AddPoint:
                        editWrapped ("Add automation point",
                                     [this, p, coord]
                                     { engine.addAutomationPoint (index, p, coord.samplePos, coord.value); });
                        repaint();
                        return;
                    case AutomationToolbar::Tool::DeletePoint:
                    {
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                           : (juce::int64) (48000.0 * 60.0);
                        const juce::int64 tol = juce::jmax<juce::int64> (1, totalSamples / juce::jmax (1, getWidth() - headerW) * 8);
                        editWrapped ("Delete automation point",
                                     [this, p, coord, tol]
                                     { engine.removeAutomationPointNear (index, p, coord.samplePos, tol); });
                        repaint();
                        return;
                    }
                    case AutomationToolbar::Tool::Select:
                    {
                        // Tension handle wins over point selection so
                        // the engineer can bend a segment without
                        // accidentally re-grabbing one of its endpoints.
                        const int segIdx = hitTestTensionHandle (e.getPosition());
                        if (segIdx >= 0)
                        {
                            draggingTensionSegIdx = segIdx;
                            if (automationDragBegin) automationDragBegin();
                            return;
                        }
                        // Try to grab the nearest point -- drag will move
                        // it if mouseDrag fires after this.
                        const auto& lane = engine.getAutomation (index, p);
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                           : (juce::int64) (48000.0 * 60.0);
                        const juce::int64 tol = juce::jmax<juce::int64> (1, totalSamples / juce::jmax (1, getWidth() - headerW) * 6);
                        draggingPointIdx = -1;
                        for (size_t i = 0; i < lane.size(); ++i)
                        {
                            if (std::abs (lane[i].samplePos - coord.samplePos) < tol)
                            {
                                draggingPointIdx = (int) i;
                                // Open an automation transaction so the
                                // entire drag becomes one undo step.
                                // mouseUp closes it via automationDragEnd.
                                if (automationDragBegin) automationDragBegin();
                                return;
                            }
                        }
                        return;
                    }
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Crossfade midpoint drag. Convert the mouse x to a
            // sample position, then set both adjacent clips' fades
            // so the equal-power crossfade point lands there.
            //   bFadeIn  = midSample - bStart
            //   aFadeOut = aEnd      - midSample
            // The midpoint is clamped inside [bStart+1, aEnd-1] so
            // a fade never goes to zero (which would visually drop
            // one side of the crossfade entirely).
            if (draggingXfadeAIdx >= 0)
            {
                auto* clips = engine.tryClipsFor (index);
                if (clips != nullptr && draggingXfadeAIdx + 1 < (int) clips->size())
                {
                    const auto& player = engine.getPlayer();
                    const juce::int64 totalSamples = player.isLoaded()
                        ? player.getTotalLengthSamples() : 0;
                    if (totalSamples > 0)
                    {
                        const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                        const double prop = juce::jlimit (0.0, 1.0,
                            (double) (e.x - inner.getX()) / (double) juce::jmax (1, inner.getWidth()));
                        const juce::int64 midSample = (juce::int64) (prop * (double) totalSamples);
                        const auto& a = (*clips)[(size_t) draggingXfadeAIdx];
                        const auto& b = (*clips)[(size_t) (draggingXfadeAIdx + 1)];
                        const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                        const auto bStart = b.timelineStartSamples;
                        const juce::int64 clamped = juce::jlimit (bStart + 1, aEnd - 1, midSample);
                        const juce::int64 bFadeIn  = clamped - bStart;
                        const juce::int64 aFadeOut = aEnd - clamped;
                        // Preserve existing fadeIn / fadeOut on the
                        // OPPOSITE edge of each clip; only the
                        // crossfade-side fade is being adjusted.
                        engine.setClipFades (index, draggingXfadeAIdx,
                                             a.fadeInSamples, aFadeOut);
                        engine.setClipFades (index, draggingXfadeAIdx + 1,
                                             bFadeIn, b.fadeOutSamples);
                        repaint();
                    }
                }
                return;
            }

            // Strip reorder via swatch drag -- once the engineer's
            // vertical movement crosses one row height, swap with the
            // adjacent strip and reset the drag origin so a continuous
            // up-up-up drag walks the strip across the list.
            if (reorderArmed)
            {
                const int delta = e.y - reorderStartY;
                const int rowH  = juce::jmax (1, getHeight());
                if (! reorderActive && std::abs (delta) > 8)
                    reorderActive = true;

                if (reorderActive && std::abs (delta) > rowH)
                {
                    // Refuse reorder while playback is actively rolling
                    // -- swapTracks renames Track_NN.wav on disk and the
                    // player's open readers would point at the wrong
                    // data mid-block. Pop a modal warning so the
                    // engineer knows the drag was ignored.
                    if (engine.getPlayer().isPlaying())
                    {
                        juce::AlertWindow::showAsync (
                            juce::MessageBoxOptions()
                                .withIconType (juce::MessageBoxIconType::NoIcon)
                                .withTitle ("Strip reorder paused")
                                .withMessage ("Stop playback before reordering strips -- "
                                              "swapping during playback would corrupt the "
                                              "player's open file readers.")
                                .withButton ("OK"),
                            nullptr);
                        reorderActive = false;
                        reorderArmed  = false;
                        return;
                    }
                    const int dir = delta > 0 ? +1 : -1;
                    // engine.swapTracks does an ADJACENT swap. For a
                    // stereo strip, swap both halves together so the
                    // logical pair stays linked.
                    const auto& t = engine.getRecorder().getTrack (index);
                    const bool s  = t.isStereo.load() && (index + 1 < engine.getRecorder().getNumTracks());
                    const int step = s ? 2 : 1;
                    const int other = index + dir * step;
                    if (other >= 0 && other + (s ? 1 : 0) < engine.getRecorder().getNumTracks())
                    {
                        if (dir > 0)
                        {
                            engine.swapTracks (index, other);
                            if (s) engine.swapTracks (index + 1, other + 1);
                        }
                        else
                        {
                            engine.swapTracks (other, index);
                            if (s) engine.swapTracks (other + 1, index + 1);
                        }
                        // Stay armed; reset origin so the next row
                        // crossing fires again.
                        reorderStartY = e.y;
                    }
                }
                return;
            }

            // Resize drag wins if it's already in flight.
            if (dragging)
            {
                const int target = juce::jlimit (kMinRowH, kMaxRowH,
                                                 dragStartHeight + e.getDistanceFromDragStartY());
                rowSize = Size::Custom;
                customH = target;
                if (onSizeChosen) onSizeChosen (*this, Size::Custom);
                return;
            }

            // Scrubber / Selector drag -- playhead chases the mouse;
            // Selector additionally seeds a loop region anchored at the
            // mouseDown position.
            if (draggingClipModeInt == 5 || draggingClipModeInt == 6)
            {
                auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                if (totalSamples > 0)
                {
                    const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                    const double prop = juce::jlimit (0.0, 1.0,
                        (double) (e.x - inner.getX()) / (double) juce::jmax (1, inner.getWidth()));
                    const juce::int64 sx = (juce::int64) (prop * (double) totalSamples);
                    if (draggingClipModeInt == 5)
                    {
                        player.setPositionSamples (sx);
                    }
                    else
                    {
                        const juce::int64 a = juce::jmin (lastDragSamples, sx);
                        const juce::int64 b = juce::jmax (lastDragSamples, sx);
                        if (b > a) player.setLoopRegion (a, b);
                    }
                    repaint();
                }
                return;
            }

            // Clip edit drag -- translate pixel delta back to sample
            // delta and feed it to engine.editClip incrementally. Fade
            // handles use the absolute drag delta from the drag start
            // (so the fade tracks the mouse position rather than
            // accumulating per-frame).
            if (draggingClipIdx >= 0)
            {
                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
                if (totalSamples > 0 && inner.getWidth() > 0)
                {
                    const double samplesPerPx = (double) totalSamples / (double) inner.getWidth();
                    const juce::int64 wantSamples = (juce::int64) ((e.x - dragStartX) * samplesPerPx);

                    if (draggingClipModeInt == 3 || draggingClipModeInt == 4)
                    {
                        // Fade handles: absolute targets, not incremental.
                        // Drag the fade-in apex right grows fadeIn;
                        // drag the fade-out apex left grows fadeOut
                        // (so 'further from the clip end' = more fade).
                        const auto* clips = engine.tryClipsFor (index);
                        if (clips == nullptr || draggingClipIdx >= (int) clips->size())
                            return;
                        const auto& c = (*clips)[(size_t) draggingClipIdx];
                        if (draggingClipModeInt == 3)
                        {
                            const juce::int64 newIn = juce::jlimit<juce::int64> (
                                0, c.fileLengthSamples - dragStartFadeOut,
                                dragStartFadeIn + wantSamples);
                            engine.setClipFades (index, draggingClipIdx, newIn, dragStartFadeOut);
                        }
                        else
                        {
                            // Drag right shrinks fade-out, drag left grows it.
                            const juce::int64 newOut = juce::jlimit<juce::int64> (
                                0, c.fileLengthSamples - dragStartFadeIn,
                                dragStartFadeOut - wantSamples);
                            engine.setClipFades (index, draggingClipIdx, dragStartFadeIn, newOut);
                        }
                        repaint();
                        return;
                    }

                    // Trim / Move: incremental.
                    const juce::int64 stepSamples = wantSamples - lastDragSamples;
                    lastDragSamples = wantSamples;
                    if (std::abs (stepSamples) >= 1)
                    {
                        const auto mode = draggingClipModeInt == 0 ? AudioEngine::ClipEdit::TrimLeft
                                       : draggingClipModeInt == 1 ? AudioEngine::ClipEdit::TrimRight
                                                                  : AudioEngine::ClipEdit::Move;
                        engine.editClip (index, draggingClipIdx, mode, stepSamples);
                        repaint();
                    }
                }
                return;
            }

            // Tension-handle drag: translate the cursor's y into a
            // tension value for the segment starting at draggingTensionSegIdx
            // and push it through the engine. The segment is forced
            // to Linear by setAutomationTensionAt (Hold / SCurve
            // would ignore tension), giving the engineer a
            // continuous-shape escape hatch from any preset.
            if (toolbar != nullptr
                && toolbar->getTool() == AutomationToolbar::Tool::Select
                && draggingTensionSegIdx >= 0
                && toolbar->getParam() != AutomationToolbar::Param::Mute)
            {
                const auto p = currentLaneParam();
                if (p == AudioEngine::AutomationParam::Mute) return;
                const auto& lane = engine.getAutomation (index, p);
                if (draggingTensionSegIdx + 1 < (int) lane.size())
                {
                    const auto pointSample = lane[(size_t) draggingTensionSegIdx].samplePos;
                    float t = tensionFromHandleY (draggingTensionSegIdx, e.y);
                    // Shift snaps to a 0.25 grid so matched curves
                    // between adjacent segments are easy to dial in.
                    if (e.mods.isShiftDown())
                        t = std::round (t * 4.0f) / 4.0f;
                    engine.setAutomationTensionAt (index, p, pointSample, 4096, t);
                    repaint();
                }
                return;
            }

            // Otherwise: drag a held automation point with the Select tool.
            if (toolbar != nullptr
                && toolbar->getTool() == AutomationToolbar::Tool::Select
                && draggingPointIdx >= 0)
            {
                const auto p     = toEngineParam (toolbar->getParam());
                const auto coord = laneCoordAt (e.getPosition());
                // Easiest path: re-add with the new (samplePos, value);
                // addAutomationPoint replaces inside its tolerance, so
                // we just delete the old + add new to allow position to
                // move freely.
                const auto& lane = engine.getAutomation (index, p);
                if (draggingPointIdx < (int) lane.size())
                {
                    const auto oldPos = lane[(size_t) draggingPointIdx].samplePos;
                    engine.removeAutomationPointNear (index, p, oldPos, 1);
                    engine.addAutomationPoint        (index, p, coord.samplePos, coord.value);
                    repaint();
                }
            }
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            // Close the automation drag transaction (if one is open).
            // mouseDown on the Select tool opened it when a point was
            // grabbed; this closes it on mouseUp regardless of whether
            // any mouseDrag happened in between (a click-without-drag
            // produces a no-op undo step, which is acceptable).
            const bool wasDraggingPoint   = (draggingPointIdx >= 0);
            const bool wasDraggingTension = (draggingTensionSegIdx >= 0);
            const bool wasDraggingClip    = (draggingClipIdx >= 0 || draggingXfadeAIdx >= 0);
            dragging         = false;
            draggingPointIdx = -1;
            draggingTensionSegIdx = -1;
            draggingClipIdx  = -1;
            draggingClipModeInt = 0;
            draggingXfadeAIdx = -1;
            lastDragSamples  = 0;
            if ((wasDraggingPoint || wasDraggingTension) && automationDragEnd)
                automationDragEnd (wasDraggingTension ? "Bend automation curve"
                                                      : "Move automation point");
            // Commit the clip-edit drag as a single Cmd+Z step (trim /
            // move / fade / crossfade). No-ops if nothing actually moved.
            if (wasDraggingClip)
                if (auto* page = findParentComponentOfClass<EditPage>())
                    page->commitClipEdit ("Edit clip");

            // Reorder finishes here. If the user clicked the swatch
            // but never crossed the 8 px threshold, fall through to
            // the colour picker (legacy 'click swatch = colour').
            if (reorderArmed && ! reorderActive)
            {
                openColourPicker();
            }
            else if (reorderActive)
            {
                // Swap renamed the on-disk Track_NN.wav files; reload
                // the session so the SessionPlayer's open readers map
                // to the new file order. Refusal-during-playback is
                // checked above, so this is always safe to call here.
                const auto dir = engine.getActiveSessionDir();
                if (dir.isDirectory())
                {
                    const auto pos = engine.getPlayer().getPositionSamples();
                    engine.loadSession (dir);
                    engine.getPlayer().setPositionSamples (pos);
                }
            }
            reorderArmed  = false;
            reorderActive = false;
        }

        void openColourPicker()
        {
            auto& s = engine.getRecorder().getTrack (index);
            auto current = (s.colourARGB.load() != 0)
                            ? juce::Colour ((juce::uint32) s.colourARGB.load())
                            : brand::stripColour (index);

            auto picker = std::make_unique<StripColourPicker> (
                current,
                [this] (juce::Colour chosen) { engine.setTrackColour (index, chosen); repaint(); });

            const auto screenArea = getScreenBounds().withWidth (swatchW);
            juce::CallOutBox::launchAsynchronously (std::move (picker), screenArea, nullptr);
        }

        int  getHeaderWidth() const noexcept { return headerW; }
        int  getTrackIndex()  const noexcept { return index; }
        bool isStereoPair()   const noexcept { return stereo; }

        Size getRowSize() const noexcept { return rowSize; }
        void setRowSize (Size s) noexcept { rowSize = s; }
        int  getCustomHeight() const noexcept { return customH; }
        int  getRowPixelHeight (int fitFallback) const noexcept
        {
            return pixelsFor (rowSize, fitFallback, customH);
        }

    private:
        bool hovered { false };

        void showFadeMenu (int clipIdx, juce::Point<int> screenPos)
        {
            if (menuOpen) return;
            menuOpen = true;

            const auto* clips = engine.tryClipsFor (index);
            if (clips == nullptr || clipIdx < 0 || clipIdx >= (int) clips->size())
                { menuOpen = false; return; }
            const auto& clip = (*clips)[(size_t) clipIdx];
            const auto fIn   = clip.fadeInSamples;
            const auto fOut  = clip.fadeOutSamples;
            const bool muted  = clip.muted;
            const bool locked = clip.locked;
            const float gainDb = clip.gainDb;

            const double sr = engine.getPlayer().getSampleRate() > 0.0
                                 ? engine.getPlayer().getSampleRate()
                                 : 48000.0;
            auto msToSamples = [sr] (int ms) -> juce::int64
            {
                return (juce::int64) ((double) ms * sr / 1000.0);
            };

            juce::PopupMenu fadeIn;
            fadeIn.addItem (101, "Off",     true, fIn == 0);
            fadeIn.addItem (102, "10 ms",   true, fIn == msToSamples (10));
            fadeIn.addItem (103, "50 ms",   true, fIn == msToSamples (50));
            fadeIn.addItem (104, "200 ms",  true, fIn == msToSamples (200));
            fadeIn.addItem (105, "500 ms",  true, fIn == msToSamples (500));
            fadeIn.addItem (106, "1 s",     true, fIn == msToSamples (1000));

            juce::PopupMenu fadeOut;
            fadeOut.addItem (201, "Off",     true, fOut == 0);
            fadeOut.addItem (202, "10 ms",   true, fOut == msToSamples (10));
            fadeOut.addItem (203, "50 ms",   true, fOut == msToSamples (50));
            fadeOut.addItem (204, "200 ms",  true, fOut == msToSamples (200));
            fadeOut.addItem (205, "500 ms",  true, fOut == msToSamples (500));
            fadeOut.addItem (206, "1 s",     true, fOut == msToSamples (1000));

            juce::PopupMenu gain;
            gain.addItem (501, "-12 dB",   true, std::abs (gainDb - (-12.0f)) < 0.05f);
            gain.addItem (502, "-6 dB",    true, std::abs (gainDb - (-6.0f))  < 0.05f);
            gain.addItem (503, "-3 dB",    true, std::abs (gainDb - (-3.0f))  < 0.05f);
            gain.addItem (504, "0 dB",     true, std::abs (gainDb - 0.0f)     < 0.05f);
            gain.addItem (505, "+3 dB",    true, std::abs (gainDb -  3.0f)    < 0.05f);
            gain.addItem (506, "+6 dB",    true, std::abs (gainDb -  6.0f)    < 0.05f);
            gain.addSeparator();
            gain.addItem (510, "Set value...");

            juce::PopupMenu menu;
            menu.addItem (400, muted  ? "Unmute clip" : "Mute clip");
            menu.addItem (401, locked ? "Unlock clip" : "Lock clip");
            menu.addSeparator();
            menu.addItem (410, "Duplicate clip", ! locked);
            menu.addItem (411, "Delete clip",    ! locked);
            menu.addSeparator();
            menu.addSubMenu ("Clip gain",  gain);
            menu.addSubMenu ("Fade in",    fadeIn,  ! locked);
            menu.addSubMenu ("Fade out",   fadeOut, ! locked);
            menu.addItem (300, "Clear both fades", ! locked);

            juce::Component::SafePointer<TrackRow> self (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                [self, clipIdx, fIn, fOut, muted, locked, gainDb, msToSamples] (int chosen)
            {
                if (self == nullptr) return;
                self->menuOpen = false;
                if (chosen == 0) return;

                // Route every clip-mutating menu action through the EDIT
                // page's clip-undo bridge so it joins Cmd+Z, consistent
                // with the drag-edit and Split/Crop paths. The before/after
                // guard in pushClipUndo drops no-ops (e.g. selecting the
                // gain the clip already has).
                auto withUndo = [self] (const juce::String& label, auto&& op)
                {
                    auto* page = self->findParentComponentOfClass<EditPage>();
                    if (page != nullptr) page->beginClipEdit();
                    op();
                    if (page != nullptr) page->commitClipEdit (label);
                    self->repaint();
                };

                switch (chosen)
                {
                    case 400: withUndo (muted  ? "Unmute clip" : "Mute clip",   [&]{ self->engine.setClipMuted  (self->index, clipIdx, ! muted); });  return;
                    case 401: withUndo (locked ? "Unlock clip" : "Lock clip",   [&]{ self->engine.setClipLocked (self->index, clipIdx, ! locked); }); return;
                    case 410: withUndo ("Duplicate clip", [&]{ self->engine.duplicateClip (self->index, clipIdx); });           return;
                    case 411: withUndo ("Delete clip",    [&]{ self->engine.deleteClip    (self->index, clipIdx); });           return;
                    case 501: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx, -12.0f); });        return;
                    case 502: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx,  -6.0f); });        return;
                    case 503: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx,  -3.0f); });        return;
                    case 504: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx,   0.0f); });        return;
                    case 505: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx,   3.0f); });        return;
                    case 506: withUndo ("Clip gain", [&]{ self->engine.setClipGainDb (self->index, clipIdx,   6.0f); });        return;
                    case 510:
                    {
                        auto* aw = new juce::AlertWindow ("Clip gain",
                            "Enter clip gain in dB (-60 .. +12).",
                            juce::MessageBoxIconType::NoIcon);
                        aw->setLookAndFeel (&self->getLookAndFeel());   // grey ZynForge chrome
                        aw->addTextEditor ("dB", juce::String (gainDb, 2));
                        dialog::primeNameEditor (*aw, "dB");
                        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                        juce::Component::SafePointer<TrackRow> rowSafe (self);
                        aw->enterModalState (true, juce::ModalCallbackFunction::create (
                            [aw, rowSafe, clipIdx] (int r)
                        {
                            std::unique_ptr<juce::AlertWindow> own (aw);
                            if (r != 1 || rowSafe == nullptr) return;
                            const auto txt = own->getTextEditorContents ("dB");
                            const float dB = juce::jlimit (-60.0f, 12.0f, txt.getFloatValue());
                            auto* page = rowSafe->findParentComponentOfClass<EditPage>();
                            if (page != nullptr) page->beginClipEdit();
                            rowSafe->engine.setClipGainDb (rowSafe->index, clipIdx, dB);
                            if (page != nullptr) page->commitClipEdit ("Clip gain");
                            rowSafe->repaint();
                        }));
                        return;
                    }
                    default: break;
                }

                juce::int64 newIn  = fIn;
                juce::int64 newOut = fOut;
                switch (chosen)
                {
                    case 101: newIn  = 0;                   break;
                    case 102: newIn  = msToSamples (10);    break;
                    case 103: newIn  = msToSamples (50);    break;
                    case 104: newIn  = msToSamples (200);   break;
                    case 105: newIn  = msToSamples (500);   break;
                    case 106: newIn  = msToSamples (1000);  break;
                    case 201: newOut = 0;                   break;
                    case 202: newOut = msToSamples (10);    break;
                    case 203: newOut = msToSamples (50);    break;
                    case 204: newOut = msToSamples (200);   break;
                    case 205: newOut = msToSamples (500);   break;
                    case 206: newOut = msToSamples (1000);  break;
                    case 300: newIn = 0; newOut = 0;        break;
                    default: return;
                }
                withUndo ("Clip fades", [&]{ self->engine.setClipFades (self->index, clipIdx, newIn, newOut); });
            });
        }

        // Per-row clipboard for the automation range copy. Static so
        // it survives across rows -- Copy on row N then Paste on row M
        // moves the points between strips, Pro Tools-style.
        static std::vector<AudioEngine::AutomationPoint>& sharedClipboard()
        {
            static std::vector<AudioEngine::AutomationPoint> clip;
            return clip;
        }

        // Returns the index of the automation point under the given
        // (row-local) position, or -1 if none. Hit tolerance is 7 px
        // around the painted handle (which is 7 px wide), so a sloppy
        // right-click still lands the menu.
        // Returns the index of the OUTGOING clip whose crossfade
        // midpoint handle is under the cursor, or -1. Mirrors the
        // paint code's overlap detection. 6 px tolerance around
        // the handle.
        int hitTestCrossfadeHandle (juce::Point<int> pos) const
        {
            const auto* clips = engine.tryClipsFor (index);
            if (clips == nullptr || clips->size() < 2) return -1;
            const auto& player = engine.getPlayer();
            const juce::int64 totalSamples = player.isLoaded()
                ? player.getTotalLengthSamples() : 0;
            if (totalSamples <= 0) return -1;
            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
            auto sampleToX = [&] (juce::int64 sp) -> int
            { return TimelineMapper::forLane (inner, totalSamples).toXFloor (sp); };
            for (size_t i = 0; i + 1 < clips->size(); ++i)
            {
                const auto& a = (*clips)[i];
                const auto& b = (*clips)[i + 1];
                const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                const auto bStart = b.timelineStartSamples;
                if (bStart >= aEnd) continue;
                if (aEnd - bStart < 64) continue;
                const int xL = sampleToX (bStart);
                const int xR = sampleToX (aEnd);
                if (xR - xL < 4) continue;
                const int xMid = (xL + xR) / 2;
                const int yMid = inner.getY() + inner.getHeight() / 2;
                if (std::abs (pos.x - xMid) <= 6 && std::abs (pos.y - yMid) <= 6)
                    return (int) i;
            }
            return -1;
        }

        int hitTestAutomationPoint (juce::Point<int> pos) const
        {
            if (laneMode == LaneMode::Waveform || laneMode == LaneMode::Markers)
                return -1;
            const auto engineParam =
                laneMode == LaneMode::Pan  ? AudioEngine::AutomationParam::Pan
              : laneMode == LaneMode::Mute ? AudioEngine::AutomationParam::Mute
                                            : AudioEngine::AutomationParam::Volume;
            const auto& lane = engine.getAutomation (index, engineParam);
            if (lane.empty()) return -1;

            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const juce::int64 loadedSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
            const juce::int64 totalSamples  = loadedSamples > 0 ? loadedSamples
                                                                : (juce::int64) (sr * 300.0);
            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
            auto sampleToX = [&] (juce::int64 sp) -> int
            { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };

            constexpr int kHitR = 7;
            for (int i = 0; i < (int) lane.size(); ++i)
            {
                const int px = sampleToX (lane[(size_t) i].samplePos);
                if (std::abs (pos.x - px) <= kHitR)
                    return i;
            }
            return -1;
        }

        // Resolves the lane parameter that's CURRENTLY painted on
        // this row. Mirrors the paint loop's chosenParam logic so
        // the hit-test and drag handlers don't read from a different
        // lane than the one the engineer sees. Falls through to
        // Volume for Click / Tempo / unknown.
        AudioEngine::AutomationParam currentLaneParam() const
        {
            if (toolbar != nullptr)
            {
                switch (toolbar->getParam())
                {
                    case AutomationToolbar::Param::Volume: return AudioEngine::AutomationParam::Volume;
                    case AutomationToolbar::Param::Pan:    return AudioEngine::AutomationParam::Pan;
                    case AutomationToolbar::Param::Mute:   return AudioEngine::AutomationParam::Mute;
                    case AutomationToolbar::Param::Click:
                    case AutomationToolbar::Param::Tempo:  break;     // not per-track lanes
                }
            }
            switch (laneMode)
            {
                case LaneMode::Pan:  return AudioEngine::AutomationParam::Pan;
                case LaneMode::Mute: return AudioEngine::AutomationParam::Mute;
                default:             return AudioEngine::AutomationParam::Volume;
            }
        }

        // Returns the index of the SEGMENT (i.e. the prev point in
        // the lane) whose tension handle is under the cursor, or -1.
        // Mirrors the paint code: skip Hold + flat segments + ones
        // narrower than 18 px. Tolerance is 6 px around the handle.
        int hitTestTensionHandle (juce::Point<int> pos) const
        {
            if (laneMode == LaneMode::Waveform || laneMode == LaneMode::Markers)
                return -1;
            const auto engineParam = currentLaneParam();
            if (engineParam == AudioEngine::AutomationParam::Mute) return -1;
            const auto& lane = engine.getAutomation (index, engineParam);
            if (lane.size() < 2) return -1;

            const auto& player = engine.getPlayer();
            const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
            const juce::int64 loadedSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
            const juce::int64 totalSamples  = loadedSamples > 0 ? loadedSamples
                                                                : (juce::int64) (sr * 300.0);
            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
            auto sampleToX = [&] (juce::int64 sp) -> int
            { return TimelineMapper::forLane (inner, totalSamples).toX (sp); };
            auto valueToY = [&] (float v) -> int
            {
                float yp = 0.5f;
                switch (engineParam)
                {
                    case AudioEngine::AutomationParam::Volume:
                        yp = 1.0f - juce::jlimit (0.0f, 1.0f, (v + 60.0f) / 72.0f);
                        break;
                    case AudioEngine::AutomationParam::Pan:
                        yp = (1.0f - juce::jlimit (-1.0f, 1.0f, v)) * 0.5f;
                        break;
                    case AudioEngine::AutomationParam::Mute:
                        yp = v > 0.5f ? 0.05f : 0.95f;
                        break;
                }
                return inner.getY() + juce::roundToInt (yp * inner.getHeight());
            };
            auto curveShape = [] (double tNorm, AudioEngine::AutomationCurve c,
                                  float tension) -> double
            {
                switch (c)
                {
                    case AudioEngine::AutomationCurve::Hold:    return 0.0;
                    case AudioEngine::AutomationCurve::Linear:
                    {
                        const float tn = juce::jlimit (-1.0f, 1.0f, tension);
                        if (std::abs (tn) < 1.0e-4f) return tNorm;
                        const double exp = std::pow (2.0, (double) (-tn) * 4.0);
                        return std::pow (tNorm, exp);
                    }
                    case AudioEngine::AutomationCurve::SCurve:
                        return tNorm * tNorm * (3.0 - 2.0 * tNorm);
                    case AudioEngine::AutomationCurve::ExpUp:
                        return tNorm * tNorm;
                    case AudioEngine::AutomationCurve::ExpDown:
                        return 1.0 - (1.0 - tNorm) * (1.0 - tNorm);
                }
                return tNorm;
            };

            constexpr int kHitR = 6;
            for (size_t i = 1; i < lane.size(); ++i)
            {
                const auto& prev = lane[i - 1];
                if (prev.curve == AudioEngine::AutomationCurve::Hold) continue;
                const auto& next = lane[i];
                if (std::abs (next.value - prev.value) < 1.0e-4f) continue;
                const int xPrev = sampleToX (prev.samplePos);
                const int xNext = sampleToX (next.samplePos);
                if (xNext - xPrev < 18) continue;
                const double shapedMid = curveShape (0.5, prev.curve, prev.tension);
                const double vMid = (double) prev.value
                                  + shapedMid * (double) (next.value - prev.value);
                const int xMid = (xPrev + xNext) / 2;
                const int yMid = valueToY ((float) vMid);
                if (std::abs (pos.x - xMid) <= kHitR
                    && std::abs (pos.y - yMid) <= kHitR)
                    return (int) (i - 1);
            }
            return -1;
        }

        // Maps a handle's y-position in the lane to the tension that
        // would put the shaped midpoint at that y. Used by mouseDrag
        // on a tension handle. Returns +1 / -1 if the handle is
        // pinned at the endpoints (avoids log(0)).
        float tensionFromHandleY (int pointIdx, int yPx) const
        {
            const auto engineParam = currentLaneParam();
            const auto& lane = engine.getAutomation (index, engineParam);
            if (pointIdx < 0 || pointIdx + 1 >= (int) lane.size()) return 0.0f;
            const auto& prev = lane[(size_t) pointIdx];
            const auto& next = lane[(size_t) pointIdx + 1];
            if (std::abs (next.value - prev.value) < 1.0e-4f) return 0.0f;

            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
            auto yToValue = [&] (int y) -> float
            {
                const float yp = juce::jlimit (0.0f, 1.0f,
                                               (float) (y - inner.getY()) / (float) juce::jmax (1, inner.getHeight()));
                if (engineParam == AudioEngine::AutomationParam::Pan)
                    return juce::jlimit (-1.0f, 1.0f, 1.0f - 2.0f * yp);
                return juce::jlimit (-60.0f, 12.0f, (1.0f - yp) * 72.0f - 60.0f);
            };
            const float vMid = yToValue (yPx);
            float shaped = (vMid - prev.value) / (next.value - prev.value);
            shaped = juce::jlimit (0.005f, 0.995f, shaped);
            const double exp = std::log ((double) shaped) / std::log (0.5);
            const double tension = -std::log2 (exp) / 4.0;
            return juce::jlimit (-1.0f, 1.0f, (float) tension);
        }

        void showCurvePickerMenu (int pointIdx, juce::Point<int> screenPos)
        {
            const auto engineParam =
                laneMode == LaneMode::Pan  ? AudioEngine::AutomationParam::Pan
              : laneMode == LaneMode::Mute ? AudioEngine::AutomationParam::Mute
                                            : AudioEngine::AutomationParam::Volume;
            const auto& lane = engine.getAutomation (index, engineParam);
            if (pointIdx < 0 || pointIdx >= (int) lane.size()) return;
            const auto current = lane[(size_t) pointIdx].curve;
            const auto pointSample = lane[(size_t) pointIdx].samplePos;

            juce::PopupMenu m;
            using C = AudioEngine::AutomationCurve;
            // Mute points are forced to Hold -- show the menu greyed
            // for Mute so the engineer sees the limitation explicitly.
            const bool isMute = (engineParam == AudioEngine::AutomationParam::Mute);
            auto add = [&] (int id, const juce::String& label, C c)
            {
                m.addItem (id, label, ! isMute, c == current);
            };
            add (701, "Hold (step)",         C::Hold);
            add (702, "Linear",              C::Linear);
            add (703, "S-Curve",             C::SCurve);
            add (704, "Exponential (ease in)",  C::ExpUp);
            add (705, "Exponential (ease out)", C::ExpDown);
            m.addSeparator();
            // Reset bend = clear the drag-handle tension back to 0
            // without changing the shape preset. Disabled when the
            // segment already has no bend (avoids a noop undo step).
            const float curTension = lane[(size_t) pointIdx].tension;
            const bool  hasBend    = std::abs (curTension) > 1.0e-3f;
            m.addItem (706, "Reset bend (handle)", hasBend && ! isMute, false);

            juce::Component::SafePointer<TrackRow> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options()
                                 .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                [safe, engineParam, pointSample] (int chosen)
                {
                    if (safe == nullptr || chosen == 0) return;
                    if (chosen == 706)
                    {
                        auto editFn = [eng = &safe->engine, idx = safe->index,
                                       engineParam, pointSample]
                                      { eng->setAutomationTensionAt (idx, engineParam,
                                                                      pointSample, 4096, 0.0f); };
                        if (safe->automationEditWrapper)
                            safe->automationEditWrapper ("Reset automation bend",
                                                          std::move (editFn));
                        else
                            editFn();
                        safe->repaint();
                        return;
                    }
                    C newCurve = C::Linear;
                    switch (chosen)
                    {
                        case 701: newCurve = C::Hold;     break;
                        case 702: newCurve = C::Linear;   break;
                        case 703: newCurve = C::SCurve;   break;
                        case 704: newCurve = C::ExpUp;    break;
                        case 705: newCurve = C::ExpDown;  break;
                        default: return;
                    }
                    auto editFn = [eng = &safe->engine, idx = safe->index,
                                   engineParam, pointSample, newCurve]
                                  { eng->setAutomationCurveAt (idx, engineParam,
                                                                pointSample, 4096, newCurve); };
                    if (safe->automationEditWrapper)
                        safe->automationEditWrapper ("Change automation curve",
                                                      std::move (editFn));
                    else
                        editFn();
                    safe->repaint();
                });
        }

        void showAutomationRangeMenu (juce::Point<int> screenPos)
        {
            const auto& player = engine.getPlayer();
            if (! player.hasLoopRegion()) return;
            const auto inSample  = player.getLoopStart();
            const auto outSample = player.getLoopEnd();

            const auto engineParam =
                laneMode == LaneMode::Pan  ? AudioEngine::AutomationParam::Pan
              : laneMode == LaneMode::Mute ? AudioEngine::AutomationParam::Mute
                                            : AudioEngine::AutomationParam::Volume;

            juce::PopupMenu m;
            const auto rangePoints = engine.copyAutomationRange (index, engineParam,
                                                                  inSample, outSample);
            m.addItem (601, "Copy automation in range ("
                            + juce::String ((int) rangePoints.size()) + " points)",
                       ! rangePoints.empty());
            m.addItem (602, "Paste automation at cursor",
                       ! sharedClipboard().empty());
            m.addSeparator();
            m.addItem (603, "Clear automation in range",
                       ! rangePoints.empty());

            juce::Component::SafePointer<TrackRow> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options()
                                 .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                [safe, engineParam, inSample, outSample] (int chosen)
                {
                    if (safe == nullptr || chosen == 0) return;
                    auto& eng = safe->engine;
                    if (chosen == 601)
                    {
                        sharedClipboard() = eng.copyAutomationRange (safe->index,
                                                                      engineParam,
                                                                      inSample, outSample);
                    }
                    else if (chosen == 602)
                    {
                        const auto anchor = eng.getEditCursorSample();
                        if (anchor < 0) return;
                        auto editFn = [&eng, idx = safe->index, engineParam, anchor]
                                      { eng.pasteAutomationRange (idx, engineParam, anchor,
                                                                  sharedClipboard()); };
                        if (safe->automationEditWrapper)
                            safe->automationEditWrapper ("Paste automation",
                                                          std::move (editFn));
                        else
                            editFn();
                        safe->repaint();
                    }
                    else if (chosen == 603)
                    {
                        auto editFn = [&eng, idx = safe->index, engineParam, inSample, outSample]
                                      { eng.clearAutomationRange (idx, engineParam,
                                                                  inSample, outSample); };
                        if (safe->automationEditWrapper)
                            safe->automationEditWrapper ("Clear automation range",
                                                          std::move (editFn));
                        else
                            editFn();
                        safe->repaint();
                    }
                });
        }

        void showSizeMenu (juce::Point<int> screenPos)
        {
            if (menuOpen) return;   // re-entrancy guard

            juce::PopupMenu menu;
            auto add = [this, &menu] (int id, const juce::String& name, Size s)
            {
                menu.addItem (id, name, true /*enabled*/, s == rowSize);
            };
            add (1, "micro",        Size::Micro);
            add (2, "mini",         Size::Mini);
            add (3, "small",        Size::Small);
            add (4, "medium",       Size::Medium);
            add (5, "large",        Size::Large);
            add (6, "jumbo",        Size::Jumbo);
            add (7, "extreme",      Size::Extreme);
            menu.addSeparator();
            add (8, "fit to window", Size::FitToWindow);

            // Takes (comp playlists) -- engineer captures the current
            // clip list as a named take, switches between takes for
            // comping. IDs 800..830 = pick a take; 850 = new take from
            // current; 851 = rename active take; 852 = delete active.
            menu.addSeparator();
            const int takes  = engine.getTakeCount (index);
            const int active = engine.getActiveTakeIdx (index);
            juce::PopupMenu takesMenu;
            for (int i = 0; i < takes && i < 30; ++i)
                takesMenu.addItem (800 + i, engine.getTakeName (index, i),
                                   true, i == active);
            takesMenu.addSeparator();
            takesMenu.addItem (850, "New take from current");
            takesMenu.addItem (851, "Rename active take...", takes > 0);
            takesMenu.addItem (852, "Delete active take",  takes > 1);
            menu.addSubMenu ("Take", takesMenu);

            // Use a screen-area target (1x1 at the click point) so the menu's
            // anchor doesn't depend on the row component still being alive
            // when the callback fires. SafePointer wraps the row body so a
            // rebuild between open and dismiss can't crash the callback.
            menuOpen = true;
            juce::Component::SafePointer<TrackRow> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                                [safe] (int chosen)
            {
                auto* row = safe.getComponent();
                if (row == nullptr) return;
                row->menuOpen = false;

                if (chosen >= 1 && chosen <= 8)
                {
                    static const std::array<Size, 8> map { Size::Micro, Size::Mini, Size::Small,
                                                           Size::Medium, Size::Large, Size::Jumbo,
                                                           Size::Extreme, Size::FitToWindow };
                    const auto next = map[(std::size_t) (chosen - 1)];
                    row->rowSize = next;
                    if (row->onSizeChosen) row->onSizeChosen (*row, next);
                    return;
                }

                // Take pick -- 800..829.
                if (chosen >= 800 && chosen < 830)
                {
                    row->engine.setActiveTake (row->index, chosen - 800);
                    row->repaint();
                    return;
                }
                if (chosen == 850)   // new take from current
                {
                    row->engine.newTakeFromCurrent (row->index, {});
                    row->repaint();
                    return;
                }
                if (chosen == 851)   // rename active take
                {
                    const int active = row->engine.getActiveTakeIdx (row->index);
                    const auto cur   = row->engine.getTakeName (row->index, active);
                    auto* aw = new juce::AlertWindow ("Rename take",
                        "Take name:", juce::MessageBoxIconType::NoIcon);
                    aw->setLookAndFeel (&row->getLookAndFeel());   // grey ZynForge chrome
                    aw->addTextEditor ("n", cur, {});
                    dialog::primeNameEditor (*aw, "n");
                    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                    juce::Component::SafePointer<TrackRow> rs (row);
                    aw->enterModalState (true, juce::ModalCallbackFunction::create (
                        [aw, rs, active] (int r)
                    {
                        std::unique_ptr<juce::AlertWindow> own (aw);
                        if (r != 1 || rs == nullptr) return;
                        rs->engine.renameTake (rs->index, active, own->getTextEditorContents ("n").trim());
                        rs->repaint();
                    }));
                    return;
                }
                if (chosen == 852)   // delete active take
                {
                    const int active = row->engine.getActiveTakeIdx (row->index);
                    row->engine.deleteTake (row->index, active);
                    row->repaint();
                    return;
                }
            });
        }

    public:
        // What this row draws in the lane area (matches the toolbar's
        // Param, or the row's own VIEW choice when no toolbar is wired).
        enum class LaneMode { Waveform, Markers, Volume, Mute, Pan, Click, Tempo };
        LaneMode laneMode { LaneMode::Waveform };

    private:
        juce::Colour getStripColour() const
        {
            auto& s = engine.getRecorder().getTrack (index);
            const auto argb = s.colourARGB.load();
            if (argb != 0) return juce::Colour ((juce::uint32) argb);
            return brand::stripColour (index);
        }

        // Pro Tools-style three-column header layout:
        //  - swatch (14 px)
        //  - meter pinned to its right (12 px)
        //  - left content block: name + 4-button row + chip row (~140 px)
        //  - middle column: a/b/c/d send dots (~80 px)
        //  - right column: input/output pills + vol/pan readout + plus (~140 px)
        static constexpr int headerW = 380;
        static constexpr int swatchW = 14;
        static constexpr int meterW  = 12;

        int                       index;
        AudioEngine&              engine;
        juce::AudioThumbnailCache& thumbCache;   // shared; used to drop stale thumbs
        juce::AudioThumbnail      thumbnailL;
        juce::AudioThumbnail      thumbnailR;
        juce::File                currentFileL;
        juce::File                currentFileR;
        bool                      stereo;
        juce::Label               nameLabel;
        // Same single-letter glyphs as the mixer strips: R / I / M / S.
        // LookAndFeel::drawToggleButton paints the pill in the colour
        // pinned via ToggleButton::tickColourId when toggled on.
        juce::ToggleButton        armButton  { "R" };
        juce::ToggleButton        monButton  { "I" };
        juce::ToggleButton        muteButton { "M" };
        juce::ToggleButton        soloButton { "S" };
        juce::ComboBox            inputCombo;
        juce::ComboBox            outputCombo;
        juce::TextButton          viewButton { "VIEW" };
        LedMeter                  meter;

        int                       playheadX             { -1 };
        int                       lastInputDeviceCount  { -1 };
        int                       lastOutputDeviceCount { -1 };
        unsigned int              lastColourArgb        { 0 };
        Size                      rowSize               { Size::Small };
        int                       customH               { 80 };
        int                       dragStartHeight       { 0 };
        bool                      dragging              { false };
        bool                      menuOpen              { false };
        bool                      cursorIsResize        { false };
    };

    // Owner of the TrackRow vertical list. EditPage drops this into the
    // viewport; this lets the rows scroll while the page header stays put.
    class EditPage::TrackList final : public juce::Component
    {
    public:
        TrackList (AudioEngine& eng,
                   juce::AudioFormatManager& fm,
                   juce::AudioThumbnailCache& cache)
            : engine (eng), formats (fm), thumbCache (cache) {}

        // Zoom gestures (DAW-standard): Cmd/Ctrl + wheel zooms the timeline
        // horizontally; add Shift for vertical (amplitude) zoom. A plain
        // wheel is forwarded to the viewport so normal scrolling is intact.
        void mouseWheelMove (const juce::MouseEvent& e,
                             const juce::MouseWheelDetails& w) override
        {
            const bool cmd = e.mods.isCommandDown() || e.mods.isCtrlDown();
            if (cmd)
            {
                if (auto* page = findParentComponentOfClass<EditPage>())
                {
                    if (e.mods.isShiftDown()) page->wheelZoomVertical   (w.deltaY);
                    else                      page->wheelZoomHorizontal (w.deltaY);
                    return;
                }
            }
            if (auto* vp = findParentComponentOfClass<juce::Viewport>())
                vp->mouseWheelMove (e.getEventRelativeTo (vp), w);
        }

        // Push-down configuration: toolbar pointer + click-overlay state.
        // Stored here so newly-created rows pick them up automatically;
        // existing rows are mutated through updateRowContext().
        AutomationToolbar* sharedToolbar       { nullptr };
        EditToolsBar*      sharedToolsBar      { nullptr };
        int                sharedClickRowIdx   { -1 };
        // Forwarded from EditPage -> MainComponent. When set, every
        // per-point automation edit a TrackRow performs goes through
        // this wrapper so Cmd+Z reverts it.
        std::function<void (const juce::String& label,
                            std::function<void()> mutate)> sharedAutomationEditWrapper;
        // Drag begin / end -- coalesces a multi-tick drag into one
        // undo step.
        std::function<void()>                       sharedAutomationDragBegin;
        std::function<void (const juce::String&)>   sharedAutomationDragEnd;

        void updateRowContext()
        {
            for (auto& r : rows)
            {
                r->toolbar                = sharedToolbar;
                r->toolsBar               = sharedToolsBar;
                r->clickRowIdx            = sharedClickRowIdx;
                r->automationEditWrapper  = sharedAutomationEditWrapper;
                r->automationDragBegin    = sharedAutomationDragBegin;
                r->automationDragEnd      = sharedAutomationDragEnd;
                r->repaint();
            }
        }

        void forceLaneMode (TrackRow::LaneMode lm)
        {
            for (auto& r : rows)
            {
                r->laneMode = lm;
                r->repaint();
            }
        }

        // The viewport's visible height -- needed to resolve "fit to window".
        // EditPage sets this on every resize.
        void setViewportHeight (int h) { viewportHeight = juce::jmax (60, h); }

        void rebuild (int numTracks)
        {
            rows.clear();
            rows.reserve ((size_t) numTracks);

            // Logical iteration -- stereo L track owns the next R partner.
            int i = 0;
            while (i < numTracks)
            {
                auto& tL = engine.getRecorder().getTrack (i);
                const bool stereo = tL.isStereo.load() && (i + 1 < numTracks);
                auto r = std::make_unique<TrackRow> (i, stereo, engine, formats, thumbCache);
                r->onSizeChosen = [this] (TrackRow&, TrackRow::Size) { resized(); };
                r->toolbar                = sharedToolbar;
                r->toolsBar               = sharedToolsBar;
                r->clickRowIdx            = sharedClickRowIdx;
                r->automationEditWrapper  = sharedAutomationEditWrapper;
                r->automationDragBegin    = sharedAutomationDragBegin;
                r->automationDragEnd      = sharedAutomationDragEnd;
                addAndMakeVisible (*r);
                rows.push_back (std::move (r));
                i += stereo ? 2 : 1;
            }
            resized();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Click in the empty area below the last row -> create a
            // new mono audio track. The TrackList itself only sees the
            // event when no TrackRow consumes it, which means the
            // engineer hit empty space. Right-click is left alone for
            // potential future "Add multiple..." menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown()) return;
            if (engine.getRecorder().isRecording()) return;   // safety
            engine.addOneStrip();
        }

        void resized() override
        {
            // Fit-to-window fallback distributes the viewport height across
            // the rows that opted into Size::FitToWindow.
            int fixedTotal = 0;
            int fitRows    = 0;
            for (auto& r : rows)
            {
                if (r->getRowSize() == TrackRow::Size::FitToWindow) ++fitRows;
                else fixedTotal += TrackRow::pixelsFor (r->getRowSize());
            }
            const int fitFallback = fitRows > 0
                ? juce::jmax (28, (viewportHeight - fixedTotal) / fitRows)
                : 80;

            auto bounds = getLocalBounds();
            int totalH = 0;
            for (auto& r : rows)
            {
                const int h = r->getRowPixelHeight (fitFallback);
                r->setBounds (bounds.removeFromTop (h));
                totalH += h;
            }
            // Resize ourselves so the viewport scrolls when content > view.
            setSize (getWidth(), juce::jmax (viewportHeight, totalH));
        }

        void setWaveformsFromSession (const juce::File& sessionDir)
        {
            if (! sessionDir.isDirectory())
            {
                for (auto& r : rows) r->setWaveformFiles ({}, {});
                return;
            }
            // Pro Tools-style: tracks live under "Audio Files/". Legacy
            // sessions kept them at the root, so fall back to the root
            // when the subfolder is absent.
            const auto audioFiles = sessionDir.getChildFile ("Audio Files");
            const auto base = audioFiles.isDirectory() ? audioFiles : sessionDir;
            // Try .wav first, then .flac/.aif so backup-format sessions
            // still draw thumbnails.
            auto findTrackFile = [&] (int trackIdx) -> juce::File
            {
                auto pad = [] (int n) { return juce::String (n).paddedLeft ('0', 2); };
                const juce::String stem = "Track_" + pad (trackIdx + 1);
                const char* exts[] = { ".wav", ".flac", ".aif", ".aiff" };
                for (auto* ext : exts)
                {
                    auto f = base.getChildFile (stem + ext);
                    if (f.existsAsFile()) return f;
                }
                return base.getChildFile (stem + ".wav");
            };
            for (auto& r : rows)
            {
                const int trackIdx = r->getTrackIndex();
                const auto fL = findTrackFile (trackIdx);
                const auto fR = r->isStereoPair() ? findTrackFile (trackIdx + 1) : juce::File();
                r->setWaveformFiles (fL, fR);
            }
        }

        // While recording, the WAV files grow on disk but the file path
        // doesn't change -- so setWaveformFiles' "if path changed"
        // shortcut skips the refresh. Call this from the EditPage timer
        // to force every row to re-scan its current file each tick.
        void forceRefreshWaveforms()
        {
            for (auto& r : rows) r->reloadCurrentWaveformFiles();
        }

        void setPlayheadX (int px)
        {
            for (auto& r : rows) r->setPlayheadX (px);
        }

        void pollMixerState()
        {
            for (auto& r : rows) r->updatePollState();
        }

        int rowCount() const { return (int) rows.size(); }

        // Used by EditPage::setLogicalRowsVisible for Memory-Location
        // recall. visibleRows is a list of logical row indices to
        // show; empty list = show all.
        void setRowsVisibility (const std::vector<int>& visibleRows)
        {
            const bool showAll = visibleRows.empty();
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i] != nullptr)
                {
                    const bool wanted = showAll
                        || std::find (visibleRows.begin(), visibleRows.end(), (int) i)
                               != visibleRows.end();
                    rows[i]->setVisible (wanted);
                }
            resized();
        }

    private:
        AudioEngine&                              engine;
        juce::AudioFormatManager&                 formats;
        juce::AudioThumbnailCache&                thumbCache;
        std::vector<std::unique_ptr<TrackRow>>    rows;
        int                                       viewportHeight { 480 };
    };

    EditPage::EditPage (AudioEngine& eng)
        : engine (eng)
    {
        formatManager.registerBasicFormats();

        // Owned by EditPage so the lifetime tracks the page, but laid
        // out by MainComponent on the same 28 px row as the automation
        // toolbar -- the host re-parents it via getEditToolsBar() +
        // addAndMakeVisible().
        toolsBar = std::make_unique<EditToolsBar>();
        // Likewise owned here, re-parented by the host via
        // getAutomationToolbar() + addAndMakeVisible().
        autoToolbar = std::make_unique<AutomationToolbar>();

        list = std::make_unique<TrackList> (engine, formatManager, thumbnailCache);
        list->sharedToolsBar = toolsBar.get();
        viewport.setViewedComponent (list.get(), false);
        // Both scrollbars -- horizontal lights up as soon as zoom > 1.
        viewport.setScrollBarsShown (true, true);
        addAndMakeVisible (viewport);

        // DAW-style edge zoom clusters, overlaid on top of the viewport.
        // V (amplitude) stacked at the right edge; H (timeline) at the
        // bottom-right. Step a fixed ratio per click.
        for (auto* b : { &zoomVIn, &zoomVOut, &zoomHIn, &zoomHOut })
        {
            b->setColour (juce::TextButton::buttonColourId, brand::controlBg);
            b->setColour (juce::TextButton::textColourOffId, brand::textPrimary);
            addAndMakeVisible (*b);
        }
        zoomVIn .setTooltip ("Taller waveforms (vertical zoom in)");
        zoomVOut.setTooltip ("Shorter waveforms (vertical zoom out)");
        zoomHIn .setTooltip ("Zoom in on the timeline");
        zoomHOut.setTooltip ("Zoom out (1× = whole take)");
        zoomVIn .onClick = [this] { setVerticalZoom (vZoom * 1.41f); };
        zoomVOut.onClick = [this] { setVerticalZoom (vZoom * 0.71f); };
        zoomHIn .onClick = [this] { setZoom (zoom * 1.41f); };
        zoomHOut.onClick = [this] { setZoom (zoom * 0.71f); };

        // Pro Tools-style Min:Secs time ruler perched above the track
        // list. Reads session length + sample rate from the engine via
        // its own 4 Hz timer.
        ruler = std::make_unique<EditTimeRuler> (engine);
        addAndMakeVisible (*ruler);

        // Reusable loading/empty/error surface, overlaid on the wave area.
        // It manages its own visibility -- shown when no session is loaded,
        // cleared once waveforms are present (see refresh()).
        addChildComponent (placeholder);

        // Now that the rows exist, point the EDIT view at its own
        // automation toolbar (wires list->sharedToolbar + row lanes).
        setAutomationToolbar (autoToolbar.get());

        refresh();
        startTimerHz (24);
    }

    EditPage::~EditPage()
    {
        stopTimer();
        // Flush the waveform cache to WaveCache.wfm in whichever
        // session is active. Best-effort: failure here only means the
        // next launch re-scans waveforms, no data loss.
        const auto sessionDir = engine.getActiveSessionDir();
        if (sessionDir.isDirectory())
            saveCacheToSession (sessionDir);
    }

    void EditPage::loadCacheFromSession (const juce::File& sessionDir)
    {
        const auto cacheFile = sessionDir.getChildFile ("WaveCache.wfm");
        if (! cacheFile.existsAsFile() || cacheFile.getSize() < 16) return;

        // Versioned header: magic + (resolution<<8|rev). A cache baked at a
        // different thumbnail resolution would re-paint as coarse stair-step
        // blocks (its min/max points are too sparse for the new draw), so a
        // mismatch -- including pre-header caches whose first int isn't our
        // magic -- means delete the stale file and let the thumbnails
        // re-scan the Track_NN.wav files at the current resolution. The
        // stream is scoped so its file handle is released before we delete.
        bool stale = false;
        {
            juce::FileInputStream in (cacheFile);
            if (! in.openedOk()) return;

            const int magic = in.readInt();
            const int ver   = in.readInt();
            if (magic != kWaveCacheMagic || ver != kWaveCacheVersion)
                stale = true;
            else
                // Best-effort: readFromStream returns false on a corrupt /
                // wrong-JUCE-version body, in which case the thumbnails just
                // re-scan -- worst case 'slow first paint', never wrong audio.
                thumbnailCache.readFromStream (in);
        }
        if (stale)
            cacheFile.deleteFile();
    }

    void EditPage::saveCacheToSession (const juce::File& sessionDir)
    {
        const auto cacheFile = sessionDir.getChildFile ("WaveCache.wfm");
        // Overwrite atomically -- write to a temp file then rename so
        // a crash mid-write leaves the previous cache intact.
        const auto tmpFile = sessionDir.getChildFile ("WaveCache.wfm.tmp");
        tmpFile.deleteFile();
        {
            juce::FileOutputStream out (tmpFile);
            if (! out.openedOk()) return;
            out.writeInt (kWaveCacheMagic);     // header: tag the resolution
            out.writeInt (kWaveCacheVersion);   // so a later res change drops it
            thumbnailCache.writeToStream (out);
            out.flush();
        }
        if (tmpFile.getSize() > 0)
        {
            cacheFile.deleteFile();
            tmpFile.moveFileTo (cacheFile);
        }
        else
        {
            tmpFile.deleteFile();
        }
    }

    void EditPage::setAutomationEditWrapper (AutoEditWrapper fn)
    {
        automationEditWrapper = std::move (fn);
        if (list != nullptr)
        {
            list->sharedAutomationEditWrapper = automationEditWrapper;
            list->sharedAutomationDragBegin   = automationDragBegin;
            list->sharedAutomationDragEnd     = automationDragEnd;
            list->updateRowContext();
        }
    }

    void EditPage::setAutomationToolbar (AutomationToolbar* t)
    {
        toolbar = t;
        if (list != nullptr)
        {
            list->sharedToolbar = t;
            list->sharedAutomationEditWrapper = automationEditWrapper;
            list->updateRowContext();
        }
        applyToolbarParamToAllRows();
        repaint();
    }

    void EditPage::applyToolbarParamToAllRows()
    {
        if (toolbar == nullptr || list == nullptr) return;

        TrackRow::LaneMode lm = TrackRow::LaneMode::Volume;
        switch (toolbar->getParam())
        {
            case AutomationToolbar::Param::Volume: lm = TrackRow::LaneMode::Volume; break;
            case AutomationToolbar::Param::Pan:    lm = TrackRow::LaneMode::Pan;    break;
            case AutomationToolbar::Param::Mute:   lm = TrackRow::LaneMode::Mute;   break;
            case AutomationToolbar::Param::Click:  lm = TrackRow::LaneMode::Click;  break;
            case AutomationToolbar::Param::Tempo:  lm = TrackRow::LaneMode::Tempo;  break;
        }
        list->forceLaneMode (lm);
    }

    void EditPage::setClickTrackPresent (bool present, int clickIdx)
    {
        clickPresent  = present;
        clickTrackIdx = clickIdx;
        if (list != nullptr)
        {
            list->sharedClickRowIdx  = present ? clickIdx : -1;
            list->updateRowContext();
        }
        repaint();
    }

    void EditPage::refresh()
    {
        const int n = engine.getRecorder().getNumTracks();

        // Compute logical (stereo-aware) row count and rebuild when it
        // differs -- physical track count alone doesn't detect isStereo
        // toggles, which collapse two rows into one.
        int logicalRows = 0;
        for (int i = 0; i < n; )
        {
            const bool s = engine.getRecorder().getTrack (i).isStereo.load() && (i + 1 < n);
            ++logicalRows;
            i += s ? 2 : 1;
        }
        if (n != lastTrackCount || logicalRows != (int) list->rowCount())
        {
            list->rebuild (n);
            lastTrackCount = n;
            resized();
        }

        // Use the engine-wide 'active' session (recorder takes priority
        // over player) so waveforms render the file being WRITTEN, not
        // just the file being read back.
        const auto sessionDir = engine.getActiveSessionDir();

        // When the session changes, pull the on-disk WaveCache.wfm
        // into the thumbnail cache BEFORE the new TrackRows ask
        // their thumbnails for sources -- otherwise the thumbnails
        // re-scan the WAV files even though cached peaks exist.
        if (sessionDir != lastSessionDir && sessionDir.isDirectory())
            loadCacheFromSession (sessionDir);

        list->setWaveformsFromSession (sessionDir);

        updatePlaceholder();
        lastLoaded = engine.getPlayer().isLoaded();
    }

    void EditPage::updatePlaceholder()
    {
        // Show the empty-state placeholder only when there are NO channels.
        // Once channels exist (recorded or not) the rows own the view, so the
        // overlay never sits on top of them. getState() guard = no re-announce.
        const bool hasChannels = engine.getRecorder().getNumTracks() > 0;
        const auto want = hasChannels ? PlaceholderView::State::Hidden
                                      : PlaceholderView::State::Empty;
        if (placeholder.getState() == want) return;
        if (hasChannels) placeholder.clear();
        else             placeholder.showEmpty ("No session loaded",
                             "Add channels (+CH), or open / record a session.");
    }

    void EditPage::timerCallback()
    {
        // EDIT view runs at 24 Hz to drive the playhead + waveform
        // re-scan. When the engineer is in MIX view, EditPage is
        // hidden and none of that work needs to happen. Bail early.
        // Exception: a recording in progress -- the playhead doesn't
        // matter when hidden, but we DO want to keep waveform thumbs
        // refreshing so flipping back to EDIT shows current peaks.
        if (! isVisible() && ! engine.isRecording()) return;

        const int n = engine.getRecorder().getNumTracks();
        if (n != lastTrackCount)
            refresh();

        // Pick up session swaps -- recording starts, recording stops,
        // session loaded, session changed, ...
        const bool loaded = engine.getPlayer().isLoaded();
        const bool rec    = engine.isRecording();
        const bool recJustStopped = (! rec && lastRecording);
        if (loaded != lastLoaded || rec != lastRecording || engine.getActiveSessionDir() != lastSessionDir)
        {
            lastSessionDir = engine.getActiveSessionDir();
            lastRecording  = rec;
            refresh();
        }

        // Waveforms are NOT re-scanned from disk while recording: 48 channels
        // re-read at 24 Hz would contend with the recorder's own capture
        // writes and risk dropouts (capture integrity wins). The meters show
        // live signal during the take. The moment recording stops, the files
        // are final -- do one clean full re-scan (which drops any partial
        // cached mid-capture) so the waveform paints at full resolution.
        if (recJustStopped && list != nullptr)
            list->forceRefreshWaveforms();

        // Playhead
        const auto& player = engine.getPlayer();
        const auto total = player.getTotalLengthSamples();
        const auto pos   = player.getPositionSamples();
        int playheadX = -1;
        if (total > 0 && list->rowCount() > 0)
        {
            // Mirror TrackRow::headerW (private, but the value is
            // pinned in the design system). Keep these in sync.
            constexpr int kHeaderW = 380;
            const auto wavePaneWidth = juce::jmax (1, getWidth() - kHeaderW);
            // -8 px to account for waveform reduction in paint.
            const double frac = (double) pos / (double) total;
            playheadX = (int) (frac * (wavePaneWidth - 8)) + 4;
        }
        list->setPlayheadX (playheadX);
        list->pollMixerState();
    }

    void EditPage::paint (juce::Graphics& g)
    {
        g.fillAll (brand::bgDeep);
    }

    void EditPage::resized()
    {
        auto bounds = getLocalBounds();

        // Time ruler perches across the top, 46 px tall:
        //   20 px marker strip + 26 px Min:Secs scale.
        // Spans the full width so the header label column aligns with
        // each TrackRow's header column.
        const int rulerH = 46;
        if (ruler != nullptr)
            ruler->setBounds (bounds.removeFromTop (rulerH));

        viewport.setBounds (bounds);
        placeholder.setBounds (bounds);   // overlays the wave area when shown
        list->setViewportHeight (viewport.getHeight());
        // Apply the zoom factor -- content widens past the viewport when
        // zoom > 1; the horizontal scrollbar lights up to navigate.
        const int contentW = juce::jmax (viewport.getWidth(),
                                         (int) (viewport.getWidth() * zoom));
        list->setSize (contentW, list->getHeight());
        list->resized();

        // Push the same content width into the ruler so its
        // pixels-per-second matches the wave pane below it.
        if (ruler != nullptr)
            ruler->setContentWidth (contentW);

        // Edge zoom clusters, overlaid in the bottom-right corner:
        //   V+        (vertical / amplitude, stacked)
        //   V-
        //   H- H+     (horizontal / timeline, side by side)
        const int zb = 26, pad = 8;
        const int rightX = bounds.getRight()  - zb - pad;
        const int botY   = bounds.getBottom() - zb - pad;
        zoomVIn .setBounds (rightX,            botY - 2 * (zb + 4), zb, zb);
        zoomVOut.setBounds (rightX,            botY -     (zb + 4), zb, zb);
        zoomHOut.setBounds (rightX - zb - 4,   botY,                zb, zb);
        zoomHIn .setBounds (rightX,            botY,                zb, zb);
        for (auto* b : { &zoomVIn, &zoomVOut, &zoomHIn, &zoomHOut })
            b->toFront (false);
    }

    void EditPage::setLogicalRowsVisible (const std::vector<int>& visibleRows)
    {
        if (list == nullptr) return;
        list->setRowsVisibility (visibleRows);
    }

    void EditPage::scrollToSample (juce::int64 sample)
    {
        if (list == nullptr || sample < 0) return;
        const auto& player = engine.getPlayer();
        const auto totalSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
        if (totalSamples <= 0) return;
        const int contentW = list->getWidth();
        if (contentW <= 0) return;
        const double prop = (double) sample / (double) totalSamples;
        const int    targetX = (int) (prop * (double) contentW);
        const int    halfV   = viewport.getViewWidth() / 2;
        viewport.setViewPosition (juce::jmax (0, targetX - halfV),
                                  viewport.getViewPositionY());
    }

    void EditPage::setZoom (float z)
    {
        z = juce::jlimit (1.0f, 16.0f, z);
        if (std::abs (z - zoom) < 0.01f) return;
        zoom = z;
        resized();
        if (onZoomChanged) onZoomChanged (zoom);
    }

    void EditPage::setVerticalZoom (float z)
    {
        z = juce::jlimit (0.25f, 32.0f, z);
        if (std::abs (z - vZoom) < 0.001f) return;
        vZoom = z;
        if (list != nullptr) list->repaint();
    }

    void EditPage::wheelZoomHorizontal (float delta)
    {
        // Keep the time under the viewport centre stable across the zoom.
        const double centreFrac = (viewport.getViewPositionX()
                                   + viewport.getWidth() * 0.5)
                                  / (double) juce::jmax (1, list->getWidth());
        setZoom (zoom * (delta > 0.0f ? 1.18f : 1.0f / 1.18f));
        const int newW = list->getWidth();
        viewport.setViewPosition (
            juce::jmax (0, (int) (centreFrac * newW - viewport.getWidth() * 0.5)),
            viewport.getViewPositionY());
    }

    void EditPage::wheelZoomVertical (float delta)
    {
        setVerticalZoom (vZoom * (delta > 0.0f ? 1.18f : 1.0f / 1.18f));
    }
}
