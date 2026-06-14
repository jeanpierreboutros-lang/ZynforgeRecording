#include "EditPage.h"
#include "AutomationToolbar.h"
#include "EditToolsBar.h"
#include "EditTimeRuler.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "LedMeter.h"
#include "StripColourPicker.h"
#include "RenameLabel.h"

#include <algorithm>

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

        // Apply one height to EVERY row at once (Option-drag a row's bottom
        // edge, or "Set all tracks to ▸" in the size menu). customPx is used
        // only when s == Size::Custom. Wired by TrackList.
        std::function<void (Size /*s*/, int /*customPx*/)> onApplyToAllRows;

        // Tab / Shift+Tab in the name field: ask the TrackList to commit
        // and open the next / previous row's name editor (shiftDown =
        // previous). Wired by TrackList so it can resolve sibling rows.
        std::function<void (TrackRow& /*from*/, bool /*shiftDown*/)> onTabRename;

        // Scroll this row into view (within the EDIT viewport) and open
        // its name editor -- the destination of a Tab rename hop.
        void beginRename()
        {
            if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            {
                const int rowTop = getY(), rowBot = getBottom();
                const int viewY  = vp->getViewPositionY();
                const int visH   = vp->getMaximumVisibleHeight();
                if (rowTop < viewY)            vp->setViewPosition (0, rowTop);
                else if (rowBot > viewY + visH) vp->setViewPosition (0, rowBot - visH);
            }
            nameLabel.showEditor();
        }

        int trackIndex() const noexcept { return index; }

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
        float       dragStartGainDb  { 0.0f };   // clip-gain handle drag (mode 7)
        int         dragStartGainY   { 0 };      // mouse y at gain-drag start (relative ride)

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
            nameLabel.onTabKey = [this] (bool shift)
            {
                if (onTabRename) onTabRename (*this, shift);
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

            // Live signal meter in the middle column of the header. Kept
            // deliberately narrow + label-free here: the EDIT view's job is
            // editing, not metering, so a clean slim bar reads clearer at a
            // glance than the cramped dB-tick gutter did. Full labelled
            // metering still lives on the MIXER strip.
            meter.setShowDbLabels (false);
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

        void setWaveformFiles (const juce::File& fL, const juce::File& fRin)
        {
            // Native interleaved stereo: the pair is ONE 2-channel file with
            // no separate R file. Point BOTH lanes at it (each draws its own
            // channel) so the stereo image still shows two lanes. Legacy
            // two-mono-file pairs have a real fR and keep drawing as before.
            stereoOneFile = stereo && fL.existsAsFile() && ! fRin.existsAsFile();
            const juce::File fR = stereoOneFile ? fL : fRin;

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

        // Draw one stereo lane. When the pair is a single interleaved file
        // (stereoOneFile) the L lane is file channel 0 and the R lane channel
        // 1; otherwise each thumbnail is a whole mono file -> drawChannels.
        void drawLaneL (juce::Graphics& g, juce::Rectangle<int> area,
                        double t0, double t1, float zoom)
        {
            if (stereoOneFile) thumbnailL.drawChannel  (g, area, t0, t1, 0, zoom);
            else               thumbnailL.drawChannels (g, area, t0, t1, zoom);
        }
        void drawLaneR (juce::Graphics& g, juce::Rectangle<int> area,
                        double t0, double t1, float zoom)
        {
            if (stereoOneFile) thumbnailR.drawChannel  (g, area, t0, t1, 1, zoom);
            else               thumbnailR.drawChannels (g, area, t0, t1, zoom);
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
        // Change-gated: the EditPage timer pushes this to EVERY row each
        // tick; repainting full waveform rows when the playhead hasn't
        // moved (transport idle) burned ~100% of a core in EDIT view.
        void setPlayheadX (int px)
        {
            if (px == playheadX) return;
            playheadX = px;
            repaint();
        }

        // Live capture envelope -----------------------------------------
        // EditPage flips this when a take starts / stops. Starting clears
        // the previous envelope so the new take draws from scratch.
        void setLiveRecording (bool on)
        {
            if (on && ! liveRecording)
            {
                recPeakL.clear();
                recPeakR.clear();
            }
            liveRecording = on;
        }

        // Append one column to the live envelope. Called once per UI tick
        // while recording; values are 0..1 input peaks. Caps the history
        // and max-pools down by 2 when it overflows so a long take can't
        // grow the vector without bound (the lane only has ~1 px per entry
        // to draw anyway).
        void pushRecLevel (float l, float r)
        {
            if (! liveRecording) return;
            constexpr size_t kMaxPoints = 8192;
            if (recPeakL.size() >= kMaxPoints) decimateInPlace (recPeakL);
            if (recPeakR.size() >= kMaxPoints) decimateInPlace (recPeakR);
            recPeakL.push_back (juce::jlimit (0.0f, 1.0f, l));
            if (stereo) recPeakR.push_back (juce::jlimit (0.0f, 1.0f, r));
        }

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

            // Row bottom divider (full content width). The swatch column,
            // header panel, vertical divider, selection highlight and TAKE
            // chip are NOT drawn here -- paintHeader() draws them LAST (and
            // offset by the horizontal scroll) so the header stays pinned to
            // the left edge and on top when the timeline is scrolled right.
            g.setColour (brand::edge);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

            // ─── Waveform pane
            auto wavePane = getLocalBounds().withTrimmedLeft (headerW);
            g.setColour (headerBg);
            g.fillRect (wavePane);

            // Paint the waveform in the strip's own colour so it tracks
            // any colour changes the user makes in the mixer or EDIT view.
            const auto waveColour = getStripColour().brighter (0.25f);
            const auto inner = wavePane.reduced (brand::space::xs, brand::space::sm);

            // ── Forge-heat waveform fill (brand signature) ──────────────────
            // The waveform body stays the channel's own colour in the quiet
            // centre band, but glows ember → forge-orange → white-hot toward
            // the loud peaks (the top/bottom extremes of the lane). Loud
            // sections read as heated metal at a glance -- the EDIT-tab analog
            // of the forge-heat meter. Set this as the fill before drawChannels
            // (which fills each waveform column with the current fill type).
            auto setHeatWaveFill = [this, &g, waveColour] (juce::Rectangle<int> lane)
            {
                const float cx  = (float) lane.getCentreX();
                juce::ColourGradient grad (brand::meterWhiteHot, cx, (float) lane.getY(),
                                           brand::meterWhiteHot, cx, (float) lane.getBottom(), false);
                grad.addColour (0.14, brand::meterHot);
                grad.addColour (0.32, brand::meterEmber);
                grad.addColour (0.50, waveColour);          // quiet body = channel colour
                grad.addColour (0.68, brand::meterEmber);
                grad.addColour (0.86, brand::meterHot);
                g.setGradientFill (grad);
            };

            // ─── Timeline grid ─────────────────────────────────────────
            // Faint vertical grid behind the clips, aligned to the ruler's
            // 1-2-5 second steps (minor + major), so the time scale reads
            // across every lane and snap targets have a visible context.
            {
                const auto& gp = engine.getPlayer();
                const juce::int64 gTotal = gp.isLoaded() ? gp.getTotalLengthSamples() : 0;
                const double gsr = gp.getSampleRate() > 0.0 ? gp.getSampleRate() : 48000.0;
                if (gTotal > 0 && inner.getWidth() > 16)
                {
                    const double totalSec = (double) gTotal / gsr;
                    const double pxPerSec = (double) inner.getWidth() / juce::jmax (0.001, totalSec);
                    static const double steps[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30,
                                                    60, 120, 300, 600, 900, 1800, 3600, 7200 };
                    double majorSec = 3600.0;
                    for (double s : steps) if (s * pxPerSec >= 64.0) { majorSec = s; break; }
                    double minorSec = majorSec / 5.0;
                    if (minorSec * pxPerSec < 6.0) minorSec = majorSec / 2.0;
                    if (minorSec * pxPerSec < 6.0) minorSec = majorSec;
                    const auto secToX = [&] (double s)
                    { return inner.getX() + (int) (s / totalSec * inner.getWidth()); };
                    // Minor ticks (very faint), then majors (a touch stronger).
                    g.setColour (brand::edge.withAlpha (brand::alpha::ghost));
                    for (int i = 1; i * minorSec < totalSec; ++i)
                        g.drawVerticalLine (secToX (i * minorSec),
                                            (float) inner.getY(), (float) inner.getBottom());
                    g.setColour (brand::edge.brighter (0.18f).withAlpha (brand::alpha::muted));
                    for (int i = 1; i * majorSec < totalSec; ++i)
                        g.drawVerticalLine (secToX (i * majorSec),
                                            (float) inner.getY(), (float) inner.getBottom());
                }
            }

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
            // below then overlays its content. Skip the substrate when the
            // arrangement was emptied (all clips deleted) so the deleted
            // waveform doesn't linger behind the lane.
            if (laneMode != LaneMode::Waveform && ! engine.isTrackArrangementEmpty (index))
            {
                if (stereo)
                {
                    const int laneH = inner.getHeight() / 2;
                    auto laneL = inner.withHeight (laneH);
                    auto laneR = inner.withTrimmedTop (laneH);
                    if (thumbnailL.getTotalLength() > 0.0)
                    {
                        g.setColour (waveColour.withAlpha (brand::alpha::muted));
                        drawLaneL (g, laneL, 0.0,
                                   thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                    }
                    if (thumbnailR.getTotalLength() > 0.0)
                    {
                        g.setColour (waveColour.withAlpha (brand::alpha::muted));
                        drawLaneR (g, laneR, 0.0,
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

            // (The amber Tab-to-Transient onset ticks that used to draw on
            // the active row's lane top were removed -- they read as visual
            // clutter. Tab / Shift+Tab still jump between transients; the
            // hint markers just aren't painted.)

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
                                const int beatsPerBar = juce::jmax (1, engine.getTimeSignatureNumerator());
                                const bool downbeat = (beat % beatsPerBar) == 0;
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
                            g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
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
                            g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    case LaneMode::Markers:
                    {
                        // Real session markers, drawn at their actual sample
                        // positions (mapped through the same timeline as the
                        // waveform) with the marker name. Previously this lane
                        // drew 6 fake evenly-spaced ticks + the literal word
                        // "markers" -- it ignored the real marker list.
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples()
                            : (juce::int64) (player.getSampleRate() > 0.0
                                             ? player.getSampleRate() * 300.0 : 48000.0 * 300.0);
                        const auto mapper = TimelineMapper::forLane (inner, totalSamples);
                        const auto& all = engine.getMarkers().getAll();
                        if (all.empty())
                        {
                            g.setColour (brand::textTertiary);
                            g.setFont (brand::type::caption());
                            g.drawText ("No markers -- press M to drop one",
                                        inner.reduced (4, 2), juce::Justification::topLeft, false);
                        }
                        for (const auto& mk : all)
                        {
                            const int x = mapper.toX (mk.sampleOffset);
                            if (x < inner.getX() || x > inner.getRight()) continue;
                            g.setColour (brand::accentStatus);
                            g.drawVerticalLine (x, (float) inner.getY(), (float) inner.getBottom());
                            // Flag + name at the top.
                            g.fillRect (juce::Rectangle<int> (x, inner.getY(), 6, 4));
                            if (mk.name.isNotEmpty())
                            {
                                g.setColour (brand::onSignal (brand::accentStatus));
                                g.setFont (brand::type::caption().withHeight (9.5f));
                                g.drawText (mk.name,
                                            juce::Rectangle<int> (x + 4, inner.getY() + 1, 90, 11),
                                            juce::Justification::topLeft, false);
                            }
                        }
                        // Playhead overlay still applies below.
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
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
                    g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
                    g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                      0, 2, getHeight()));
                }
                return;
            }

            // ─── Comp lanes (opt-in via the take menu) ─────────────
            // One lane per take; the active comp on top. Swipe across a
            // take lane (mouse handler) to pull that section into the comp.
            const int takeCountNow = engine.getTakeCount (index);
            if (showCompLanes && takeCountNow > 1)
            {
                const auto& player = engine.getPlayer();
                const auto totalS = player.isLoaded() ? player.getTotalLengthSamples() : 0;
                const double sr = player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0;
                if (totalS > 0 && thumbnailL.getTotalLength() > 0.0)
                {
                    const int activeTk = engine.getActiveTakeIdx (index);
                    const int laneH = juce::jmax (10, inner.getHeight() / takeCountNow);
                    const auto mapper = TimelineMapper::forLane (inner, totalS);
                    for (int tk = 0; tk < takeCountNow; ++tk)
                    {
                        auto laneR = juce::Rectangle<int> (inner.getX(), inner.getY() + tk * laneH,
                                                           inner.getWidth(), laneH - 1);
                        const bool isActive = (tk == activeTk);
                        const bool isHot    = (compDragTake == tk);
                        g.setColour (isActive ? brand::bgStrip.brighter (0.05f) : brand::bgDeep);
                        g.fillRect (laneR);

                        const auto wc = isActive ? getStripColour().brighter (0.25f)
                                                 : getStripColour().withAlpha (brand::alpha::muted);
                        g.setColour (wc);
                        for (const auto& c : engine.getTakeClips (index, tk))
                        {
                            const int xL = mapper.toX (c.timelineStartSamples);
                            const int xR = mapper.toX (c.timelineStartSamples + c.fileLengthSamples);
                            juce::Rectangle<int> cr (xL, laneR.getY() + 1,
                                                     juce::jmax (1, xR - xL), laneR.getHeight() - 2);
                            thumbnailL.drawChannels (g, cr,
                                (double) c.fileStartSamples / sr,
                                (double) (c.fileStartSamples + c.fileLengthSamples) / sr, 1.0f);
                        }
                        if (isHot)
                        {
                            const int a = juce::jmin (compDragStartX, compDragCurX);
                            const int b = juce::jmax (compDragStartX, compDragCurX);
                            g.setColour (brand::toolActive().withAlpha (brand::alpha::subtle));
                            g.fillRect (juce::Rectangle<int> (a, laneR.getY(), b - a, laneR.getHeight()));
                        }
                        g.setColour (isActive ? brand::accentSolo : brand::textMuted);
                        g.setFont (brand::type::caption().withHeight (9.5f));
                        g.drawText ((isActive ? juce::String ("COMP  ") : juce::String())
                                        + engine.getTakeName (index, tk),
                                    laneR.reduced (3, 0), juce::Justification::topLeft, false);
                        g.setColour (brand::edge.withAlpha (brand::alpha::muted));
                        g.drawHorizontalLine (laneR.getBottom(), (float) laneR.getX(), (float) laneR.getRight());
                    }
                    if (playheadX >= 0 && playheadX < wavePane.getWidth())
                    {
                        g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
                        g.fillRect (juce::Rectangle<int> (headerW + playheadX, 0, 2, getHeight()));
                    }
                    return;   // comp lanes replace the normal waveform view
                }
            }

            // When the track has a clip list, the per-clip block renderer
            // below draws each clip's waveform inside its own block (mapped to
            // the clip's file region) so a moved / slip-trimmed clip shows the
            // RIGHT audio. The continuous full-lane thumbnail here is only the
            // fallback for a track with no clips yet (fresh record / live).
            const auto* clipsForWave = engine.tryClipsFor (index);
            const bool haveClipBlocks = (clipsForWave != nullptr && ! clipsForWave->empty()
                                         && engine.getPlayer().isLoaded()
                                         && engine.getPlayer().getTotalLengthSamples() > 0);
            // When the engineer deleted every clip, the arrangement is empty
            // but the Track_NN.wav is still on disk, so the continuous-thumbnail
            // fallback would redraw the deleted audio. Suppress it in that case
            // (the live-capture envelope below still draws while recording).
            const bool arrangementEmptied = engine.isTrackArrangementEmpty (index);
            if (! haveClipBlocks)
            {
            if (stereo)
            {
                // L on top, R on bottom -- Pro-Tools-style stereo lanes.
                const int laneH = inner.getHeight() / 2;
                auto laneL = inner.withHeight (laneH);
                auto laneR = inner.withTrimmedTop (laneH);

                if (! liveRecording && thumbnailL.getTotalLength() > 0.0 && ! arrangementEmptied)
                {
                    setHeatWaveFill (laneL);
                    drawLaneL (g, laneL, 0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                }
                if (! liveRecording && thumbnailR.getTotalLength() > 0.0 && ! arrangementEmptied)
                {
                    setHeatWaveFill (laneR);
                    drawLaneR (g, laneR, 0.0, thumbnailR.getTotalLength(), waveZoom (thumbnailR));
                }
                // Live capture envelope -- grows L→R during the take, glowing
                // forge-orange so the engineer sees the take is HOT / rolling.
                if (liveRecording)
                {
                    g.setColour (brand::meterHot);
                    drawRecEnvelope (g, laneL, recPeakL, vz);
                    drawRecEnvelope (g, laneR, recPeakR, vz);
                }
                // Thin divider between lanes
                g.setColour (brand::edge);
                g.drawHorizontalLine (inner.getY() + laneH, (float) inner.getX(),
                                       (float) inner.getRight());
                // Empty lanes are left blank -- the EDIT view's PlaceholderView
                // owns the "no session" message, so no per-row hint here.
            }
            else if (liveRecording)
            {
                // While the take is rolling the live forge-orange envelope OWNS
                // the lane -- never the (possibly partial / cached) file
                // thumbnail, or the lane shows blocky garbage instead of the
                // growing capture.
                g.setColour (brand::meterHot);
                drawRecEnvelope (g, inner, recPeakL, vz);
            }
            else if (thumbnailL.getTotalLength() > 0.0 && ! arrangementEmptied)
            {
                setHeatWaveFill (inner);
                thumbnailL.drawChannels (g, inner, 0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
            }
            }   // end "! haveClipBlocks" continuous-waveform fallback

            // ─── Clip region blocks (waveform mode) ─────────────────
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
                    // The little green fade grab-squares were drawn on EVERY
                    // clip edge even with no fade, which clutters the view. Show
                    // the empty grab-target only when the Fade tool is active;
                    // a clip that already HAS a fade always shows its handle so
                    // it stays adjustable in any tool.
                    const auto fadeTool = (toolsBar != nullptr)
                                            ? toolsBar->getTool()
                                            : EditToolsBar::Tool::None;
                    const bool showFadeGrips = (fadeTool == EditToolsBar::Tool::Fade);
                    const double srW = player.getSampleRate() > 0.0
                                         ? player.getSampleRate() : 48000.0;

                    // Multi-selection: which clips on THIS track are picked.
                    EditPage* selPage = findParentComponentOfClass<EditPage>();
                    int clipIdx_ = -1;
                    for (const auto& c : *clips)
                    {
                        ++clipIdx_;
                        const int xL_ = sampleToX (c.timelineStartSamples);
                        const int xR_ = sampleToX (c.timelineStartSamples + c.fileLengthSamples);

                        // ─── DAW-style region block ───────────────────────
                        // Frame each clip as a discrete block: a name header
                        // bar across the top + a 1 px border in the strip
                        // colour. This is what makes a recorded take read as a
                        // "clip" the way Pro Tools / Logic show them, instead
                        // of one continuous waveform with cut-flags.
                        const juce::Rectangle<int> block (
                            xL_, inner2.getY(),
                            juce::jmax (1, xR_ - xL_), inner2.getHeight());
                        const auto clipTint = getStripColour();
                        const int  headH = juce::jmin (12, juce::jmax (0, block.getHeight() - 6));

                        // Per-clip waveform inside the block (below the header),
                        // mapped to the clip's file region -- so a moved / slip-
                        // trimmed clip shows the audio it actually references,
                        // not whatever a continuous full-lane thumbnail would.
                        {
                            auto waveArea = block.withTrimmedTop (headH).reduced (1, 1);
                            if (waveArea.getHeight() > 2 && thumbnailL.getTotalLength() > 0.0)
                            {
                                juce::Graphics::ScopedSaveState ss (g);
                                g.reduceClipRegion (waveArea);
                                const double t0 = (double) c.fileStartSamples / srW;
                                const double t1 = (double) (c.fileStartSamples + c.fileLengthSamples) / srW;
                                // Scale the drawn waveform by the clip's gain so
                                // the shape visibly grows when boosted and
                                // shrinks when cut -- matching what you'll hear.
                                const float gz = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
                                // Non-muted clips glow with the forge-heat fill
                                // (loud = hot); a muted clip stays a dim, cool
                                // tint so it visibly drops below the rest.
                                const auto setClipFill = [&] (juce::Rectangle<int> lane)
                                {
                                    if (c.muted) g.setColour (clipTint.brighter (0.20f).withAlpha (brand::alpha::muted));
                                    else         setHeatWaveFill (lane);
                                };
                                if (stereo && thumbnailR.getTotalLength() > 0.0)
                                {
                                    const int half = waveArea.getHeight() / 2;
                                    auto la = waveArea.withHeight (half);
                                    auto ra = waveArea.withTrimmedTop (half);
                                    setClipFill (la); drawLaneL (g, la, t0, t1, waveZoom (thumbnailL) * gz);
                                    setClipFill (ra); drawLaneR (g, ra, t0, t1, waveZoom (thumbnailR) * gz);
                                }
                                else
                                {
                                    setClipFill (waveArea);
                                    thumbnailL.drawChannels (g, waveArea, t0, t1, waveZoom (thumbnailL) * gz);
                                }
                            }
                        }

                        // ─── Clip gain: a faint level line across the clip +
                        // a Pro Tools-style fader handle in the bottom-LEFT
                        // corner (the grab target). 0 dB = centre; ±12 dB maps
                        // to the inner band.
                        {
                            auto gArea = block.withTrimmedTop (headH).reduced (3, 4);
                            const bool active = (draggingClipModeInt == 7 && draggingClipIdx == clipIdx_);
                            if (gArea.getHeight() > 6 && block.getWidth() > 8)
                            {
                                const int gy = clipGainLineY (c.gainDb, gArea);
                                g.setColour (brand::accentStatus.withAlpha (active ? 0.85f : 0.32f));
                                g.drawHorizontalLine (gy, (float) gArea.getX(), (float) gArea.getRight());
                            }
                            // Corner handle: a fader track + a triangular thumb
                            // whose height tracks the gain, then the dB readout
                            // on a dark pill so it stays legible over audio.
                            auto hr = clipGainHandleRect (block, headH);
                            if (block.getWidth() > 22 && hr.getHeight() >= 12)
                            {
                                const float g12 = juce::jlimit (-12.0f, 12.0f, c.gainDb);
                                const auto col = active ? brand::accentStatus
                                                        : brand::accentStatus.withAlpha (brand::alpha::prominent);
                                const float cx = (float) hr.getX() + 6.0f;
                                // Fader track.
                                g.setColour (brand::bgDeep.withAlpha (brand::alpha::scrim));
                                g.fillRoundedRectangle (juce::Rectangle<float> (cx - 5.0f, (float) hr.getY(), 10.0f, (float) hr.getHeight()), 2.0f);
                                g.setColour (col.withAlpha (active ? 0.95f : 0.65f));
                                g.drawVerticalLine ((int) cx, (float) hr.getY() + 1.0f, (float) hr.getBottom() - 1.0f);
                                // Thumb -- a small fader grip at the gain line
                                // (rides up for boost, down for cut). Replaces
                                // the old triangular arrow; the value is now
                                // spelled out in the GAIN readout beside it.
                                const float ty = (float) hr.getCentreY() - g12 / 12.0f * ((float) hr.getHeight() * 0.5f - 2.0f);
                                g.setColour (col);
                                g.fillRoundedRectangle (juce::Rectangle<float> (cx - 5.0f, ty - 2.5f, 10.0f, 5.0f), 2.0f);
                                // "GAIN ±X.X dB" readout on a dark pill -- always
                                // shown (when there's room) so the control reads
                                // as a labelled gain, not a mystery green arrow.
                                if (block.getWidth() > 56)
                                {
                                    const juce::String sign = c.gainDb > 0.05f ? "+" : "";
                                    const bool      touched = active || std::abs (c.gainDb) > 0.05f;
                                    const juce::Rectangle<int> pill (hr.getX() + 13, hr.getCentreY() - 9,
                                                                     juce::jmin (104, hr.getRight() - (hr.getX() + 13)), 18);
                                    g.setColour (brand::bgDeep.withAlpha (brand::alpha::bold));
                                    g.fillRoundedRectangle (pill.toFloat(), brand::radius::sm);
                                    g.setColour (touched ? col.brighter (0.5f) : brand::textSecondary);
                                    g.setFont (brand::type::caption().withHeight (11.0f).boldened());
                                    // Right-trim the full string into the pill so the "dB"
                                    // unit can never clip to a bare "d" (ellipsis on if tight).
                                    g.drawText ("GAIN " + sign + juce::String (c.gainDb, 1) + " dB",
                                                pill.reduced (4, 0), juce::Justification::centredLeft, true);
                                }
                            }
                        }

                        if (headH > 0)
                        {
                            auto headRect = block.withHeight (headH);
                            g.setColour (clipTint.withAlpha (c.muted ? 0.30f : 0.68f));
                            g.fillRect (headRect);
                            if (block.getWidth() > 26)
                            {
                                const auto clipName = c.name.isNotEmpty()
                                    ? c.name
                                    : engine.getTakeName (index, engine.getActiveTakeIdx (index));
                                g.setColour (brand::onSignal (clipTint));
                                g.setFont (brand::type::caption().withHeight (9.5f));
                                g.drawText (clipName, headRect.reduced (5, 0),
                                            juce::Justification::centredLeft, false);
                            }
                        }
                        g.setColour (clipTint.brighter (0.30f).withAlpha (brand::alpha::prominent));
                        g.drawRect (block, 1);

                        // Selected-clip highlight -- the clip you're working
                        // "runs hot": a warm ember wash + a forge-orange rim, so
                        // the target of Delete / Duplicate / Nudge is
                        // unmistakable and on-brand.
                        if (selPage != nullptr && selPage->isClipSelected (index, clipIdx_))
                        {
                            const juce::Rectangle<int> sel (xL_, inner2.getY(),
                                                            juce::jmax (1, xR_ - xL_),
                                                            inner2.getHeight());
                            g.setColour (brand::meterEmber.withAlpha (brand::alpha::subtle));
                            g.fillRect (sel);
                            g.setColour (brand::meterHot.withAlpha (brand::alpha::bold));
                            g.drawRect (sel, 2);
                        }
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
                        // (The old yellow clip-start cut-flag was removed --
                        // the region block's border now shows where each clip
                        // begins, the way a real DAW does.)

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
                            // Drag handle at the apex (top of the diagonal).
                            // Shown when a fade exists, or when the Fade tool
                            // is active (grab-target to introduce one).
                            if (c.fadeInSamples > 0 || showFadeGrips)
                            {
                                const juce::Rectangle<float> handle (
                                    (float) xF - 3.5f, (float) inner2.getY(),
                                    7.0f, 7.0f);
                                g.setColour (brand::accentStatus);
                                g.fillRect (handle);
                            }
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
                            if (c.fadeOutSamples > 0 || showFadeGrips)
                            {
                                const juce::Rectangle<float> handle (
                                    (float) xF - 3.5f, (float) inner2.getY(),
                                    7.0f, 7.0f);
                                g.setColour (brand::accentStatus);
                                g.fillRect (handle);
                            }
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
                        // Only shade the range on the track(s) it applies to:
                        // the selected strips, or EVERY track when nothing is
                        // selected (a global loop set via , / .). The Selector
                        // tool selects the track you drag on, so a single-track
                        // range only highlights that track -- not all of them.
                        bool drawHere = true;
                        if (auto* pg = findParentComponentOfClass<EditPage>())
                            drawHere = (pg->isStripSelectionEmpty && pg->isStripSelectionEmpty())
                                    || (pg->isTrackSelected && pg->isTrackSelected (index));
                        if (xB > xA && drawHere)
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

            // (The TAKE chip + swatch/header panel are drawn by paintHeader()
            // at the end, pinned to the scrolled-left edge.)

            // ─── Edit cursor overlay (Pro Tools-style insertion point)
            // Painted BEHIND the playhead so the engineer can see both
            // at once when the player is paused at a different sample.
            // Uses the same 5-min notional span as mouseDown when no
            // audio is loaded, so the cursor stays visible on a fresh
            // empty session before any recording happens.
            {
                const auto cursorSample = engine.getEditCursorSample();
                const auto& player2 = engine.getPlayer();
                const juce::int64 loadedSamples2 = player2.isLoaded()
                    ? player2.getTotalLengthSamples() : 0;
                // Pro Tools-style: when STOPPED the playhead already sits on
                // the insertion point (mouseDown merges them), so drawing a
                // second white bar would just double the green one. Only show
                // the separate white edit cursor while playing -- when the
                // green playhead has rolled away from the edit point -- or on
                // a fresh session with nothing loaded yet (no green playhead
                // exists, so the white cursor is the only insertion marker).
                const bool showEditCursor = player2.isPlaying() || loadedSamples2 <= 0;
                if (cursorSample >= 0 && showEditCursor)
                {
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
                g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (brand::alpha::prominent));
                g.fillRect (juce::Rectangle<int> (headerW + playheadX, 0, 2, getHeight()));
            }

            // ─── Pinned header, drawn LAST so it floats over the waveform at
            // the scrolled-left edge instead of scrolling off with the row.
            paintHeader (g, headerOriginX(), headerBg, fillColour);
        }

        // Horizontal scroll offset of the enclosing viewport. The header is
        // drawn + laid out + hit-tested at this x so it stays pinned to the
        // left edge of the view while the waveform scrolls underneath.
        int headerOriginX() const noexcept
        {
            if (auto* vp = findParentComponentOfClass<juce::Viewport>())
                return juce::jmax (0, vp->getViewPositionX());
            return 0;
        }

        // Hit-test helpers that follow the pinned header. While scrolled the
        // header occupies row-local x [hx, hx+headerW]; the wave pane is
        // everything to its right. NOTE: the *sample* mapping still uses the
        // un-shifted inner rect ([headerW, contentWidth]) because the waveform
        // itself is drawn there -- only the click classification shifts.
        int  waveLeftX()         const noexcept { return headerOriginX() + headerW; }
        bool inWavePane (int ex) const noexcept { return ex >= waveLeftX(); }
        bool inSwatch   (int ex) const noexcept
        { const int hx = headerOriginX(); return ex >= hx && ex < hx + swatchW; }
        bool inNameZone (int ex) const noexcept
        { const int hx = headerOriginX(); return ex >= hx + swatchW && ex < hx + headerW; }

        // Y of a clip's gain line within its inner band: 0 dB = centre,
        // +12 dB near the top, -12 dB near the bottom. Shared by paint + the
        // drag hit-test so they never disagree.
        static int clipGainLineY (float gainDb, juce::Rectangle<int> gArea) noexcept
        {
            const float g12 = juce::jlimit (-12.0f, 12.0f, gainDb);
            return gArea.getCentreY()
                 - (int) (g12 / 12.0f * ((float) gArea.getHeight() * 0.5f * 0.85f));
        }
        // Inverse: a y within gArea -> the gain dB it represents (clamped).
        static float clipGainFromY (int y, juce::Rectangle<int> gArea) noexcept
        {
            const float denom = juce::jmax (1.0f, (float) gArea.getHeight() * 0.5f * 0.85f);
            return juce::jlimit (-12.0f, 12.0f,
                                 (float) (gArea.getCentreY() - y) / denom * 12.0f);
        }
        // Pro Tools-style clip-gain handle at the bottom-LEFT corner of the
        // clip block (a small fader glyph + the dB readout). This is the grab
        // target; dragging it vertically rides the clip gain.
        static juce::Rectangle<int> clipGainHandleRect (juce::Rectangle<int> block, int headH) noexcept
        {
            auto body = block.withTrimmedTop (headH);
            const int h = juce::jmin (24, juce::jmax (14, body.getHeight() - 2));
            const int w = juce::jmin (84, juce::jmax (18, body.getWidth() - 2));
            return { body.getX() + 2, body.getBottom() - h - 2, w, h };
        }

        // Draws the swatch column, header panel, divider, selection highlight
        // and TAKE chip at x = hx (the horizontal scroll offset). Called at
        // the very end of paint() so it sits on top of the waveform; the
        // header's child components (meter / combos / buttons) paint over this
        // because children are painted after their parent.
        void paintHeader (juce::Graphics& g, int hx,
                          juce::Colour headerBg, juce::Colour fillColour)
        {
            auto header = getLocalBounds().withWidth (headerW).withX (hx);
            auto swatchArea = header.removeFromLeft (swatchW);
            g.setGradientFill (brand::verticalGradient (fillColour, swatchArea.toFloat(), 0.18f, 0.28f));
            g.fillRect (swatchArea);
            g.setColour (fillColour.darker (0.40f));
            g.drawVerticalLine (swatchArea.getRight() - 1, 0.0f, (float) getHeight());

            g.setColour (headerBg);
            g.fillRect (header);

            // ── Heated Steel: orange spine + stamped number + orange seam ──
            // Mirrors the mixer strip header identity on the EDIT track rows.
            g.setColour (brand::brandOrange.withAlpha (0.30f));               // spine glow
            g.fillRect (juce::Rectangle<float> ((float) hx, 0.0f, 7.0f, (float) getHeight()));
            g.setColour (brand::brandOrange);                                 // spine
            g.fillRect (juce::Rectangle<float> ((float) hx, 0.0f, 3.0f, (float) getHeight()));
            {
                const auto num = juce::String (index + 1).paddedLeft ('0', 2);
                auto numBox = juce::Rectangle<int> (hx + swatchW + 7, 3, 24, 18);
                g.setFont (brand::type::mono (15.0f, true));
                g.setColour (brand::debossInk.withAlpha (0.85f));            // engraved shadow
                g.drawText (num, numBox.translated (0, 1), juce::Justification::centredLeft, false);
                g.setColour (brand::debossFace);                             // raised steel face
                g.drawText (num, numBox, juce::Justification::centredLeft, false);
            }

            g.setColour (brand::edge);
            g.drawVerticalLine (hx + headerW - 1, 0.0f, (float) getHeight());
            g.setColour (brand::brandOrange.withAlpha (0.50f));               // orange seam
            g.drawVerticalLine (hx + headerW - 2, 0.0f, (float) getHeight());

            if (auto* page = findParentComponentOfClass<EditPage>())
                if (page->isTrackSelected && page->isTrackSelected (index))
                {
                    g.setColour (brand::brandOrange.withAlpha (brand::alpha::subtle));
                    g.fillRect (header);
                    g.setColour (brand::brandOrange);
                    g.fillRect (juce::Rectangle<int> (hx + swatchW, 0, 3, getHeight()));
                }

            // TAKE N / M chip at the header's right edge.
            const int takeCount  = engine.getTakeCount (index);
            const int activeTake = engine.getActiveTakeIdx (index);
            if (takeCount > 1)
            {
                const auto label = "TAKE " + juce::String (activeTake + 1)
                                 + " / " + juce::String (takeCount);
                auto chip = juce::Rectangle<int> (hx + headerW - 78, 4, 70, 13);
                g.setColour (brand::featureEngaged.darker (0.30f));
                g.fillRoundedRectangle (chip.toFloat(), brand::radius::sm);
                g.setColour (brand::featureEngaged.brighter (0.40f));
                g.drawRoundedRectangle (chip.toFloat(), brand::radius::sm, 0.75f);
                g.setColour (brand::onSignal (brand::featureEngaged.darker (0.30f)));
                g.setFont (brand::type::caption());
                g.drawText (label, chip, juce::Justification::centred, false);
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
            // Offset by the horizontal scroll (headerOriginX) so the header
            // child components stay pinned to the left edge with the painted
            // header panel. EditPage re-runs this on every scroll tick.
            auto header = getLocalBounds().withWidth (headerW).withX (headerOriginX());
            header.removeFromLeft (swatchW);
            header.removeFromLeft (4);

            const bool isClickRow =
                engine.getRecorder().getTrack (index).name == "Click";

            // -------------- LEFT (name + buttons) --------------
            constexpr int leftBlockW = 140;
            auto leftBlock = header.removeFromLeft (leftBlockW).reduced (brand::space::xs, brand::space::xs);

            // Name shifted right to clear the painted Heated Steel number stamp.
            nameLabel.setBounds (leftBlock.removeFromTop (18).withTrimmedLeft (26));
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
            // Slim LedMeter -- a clean ~22 px bar (no dB-label gutter). Small
            // and clear: enough to see signal / clip at a glance while
            // editing, without eating the header the way the old 80 px
            // labelled meter did. The freed width goes to the routing block.
            constexpr int meterBlockW = 22;
            meter.setBounds (header.removeFromLeft (meterBlockW).reduced (3, 4));
            header.removeFromLeft (6);

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
            // Comp-lanes swipe: a drag on a take lane pulls that section
            // into the active comp on mouseUp. Intercepts before the normal
            // clip-edit hit-test, but only in comp mode.
            if (showCompLanes && ! e.mods.isPopupMenu() && inWavePane (e.x)
                && engine.getTakeCount (index) > 1)
            {
                auto innerC = getLocalBounds().withTrimmedLeft (headerW)
                                              .reduced (brand::space::xs, brand::space::sm);
                if (innerC.contains (e.getPosition()))
                {
                    const int takeCountNow = engine.getTakeCount (index);
                    const int laneH = juce::jmax (10, innerC.getHeight() / takeCountNow);
                    compDragTake   = juce::jlimit (0, takeCountNow - 1, (e.y - innerC.getY()) / laneH);
                    compDragStartX = compDragCurX = e.x;
                    if (auto* page = findParentComponentOfClass<EditPage>())
                        page->setActiveRowTrackIndex (index);
                    repaint();
                    return;
                }
            }

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
                    && inNameZone (e.x)
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
                && inWavePane (e.x))
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

                // Pro Tools-style merge: while the transport is STOPPED the
                // edit insertion point and the playhead are the same thing,
                // so move the playhead to the click too -- the engineer sees
                // one line, not a separate green playhead + white cursor.
                // While playing we leave the playhead alone so it keeps
                // rolling and the white cursor marks where the next edit lands.
                if (! player.isPlaying() && loadedSamples > 0)
                    engine.getPlayer().setPositionSamples (sx);
            }

            // Right-click on a clip in the waveform lane → fade menu.
            // Anywhere else on the row → row-size menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                // Right-click on the row header (name zone, not the wave
                // pane) → channel context menu. The DAW-standard place to
                // remove a track; the host confirms before deleting.
                if (inNameZone (e.x))
                {
                    if (auto* page = findParentComponentOfClass<EditPage>())
                    {
                        if (page->onRowDelete)
                        {
                            juce::Component::SafePointer<EditPage> safePage (page);
                            const int idx = index;
                            juce::PopupMenu m;
                            m.addItem (1, "Delete Channel");
                            m.showMenuAsync (
                                juce::PopupMenu::Options().withTargetScreenArea (
                                    { e.getScreenPosition().x, e.getScreenPosition().y, 1, 1 }),
                                [safePage, idx] (int r)
                                {
                                    if (r == 1 && safePage != nullptr && safePage->onRowDelete)
                                        safePage->onRowDelete (idx);
                                });
                            return;
                        }
                    }
                }
                if (laneMode == LaneMode::Waveform && inWavePane (e.x))
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
                    && inWavePane (e.x))
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
            // Hold Option (Alt) to resize EVERY row to the same height at
            // once (Pro Tools-style "apply to all tracks").
            if (isInResizeZone (e.getPosition()))
            {
                dragStartHeight = getHeight();
                dragging        = true;
                resizeAllDrag   = e.mods.isAltDown();
                return;
            }
            // Left-click on the coloured swatch column → starts a
            // reorder-drag. If the engineer doesn't move past the
            // 8 px threshold, mouseUp falls through to the colour
            // picker (preserves the legacy 'click swatch = colour').
            if (inSwatch (e.x))
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

            if (laneMode == LaneMode::Waveform && inWavePane (e.x)
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
                        // Tie the range to THIS track: select it (Shift to add
                        // more tracks). Option/Alt-drag selects the range across
                        // ALL tracks. The range shading then highlights the
                        // selected track(s) -- or all of them -- and range edits
                        // (Delete / Crop) apply to the same set.
                        if (auto* pg = findParentComponentOfClass<EditPage>())
                        {
                            if (e.mods.isAltDown())
                            {
                                if (pg->onRangeSelectAllTracks) pg->onRangeSelectAllTracks();
                            }
                            else if (pg->onRowSelect)
                                pg->onRowSelect (index, e.mods.isShiftDown());
                        }
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
            if (laneMode == LaneMode::Waveform && inWavePane (e.x))
                if (auto* page = findParentComponentOfClass<EditPage>())
                    page->beginClipEdit();

            // Crossfade midpoint drag wins over clip-edit drag --
            // the handle sits inside the overlap band so the engineer
            // expects clicking it to grab the crossfade balance
            // rather than starting a clip move on the underlying clip.
            if (laneMode == LaneMode::Waveform && inWavePane (e.x))
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
            if (laneMode == LaneMode::Waveform && inWavePane (e.x))
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

                            // Clip-gain handle grab (Smart / Grabber): the
                            // bottom-left fader corner rides the clip's level.
                            // Takes priority over body Move; edges + fade
                            // handles still win above.
                            if (! c.locked
                                && (activeTool == EditToolsBar::Tool::Smart
                                 || activeTool == EditToolsBar::Tool::Grabber
                                 || activeTool == EditToolsBar::Tool::None))
                            {
                                const int gHeadH = juce::jmin (12, juce::jmax (0, inner.getHeight() - 6));
                                juce::Rectangle<int> gBlk (xL, inner.getY(),
                                                           juce::jmax (1, xR - xL), inner.getHeight());
                                if (clipGainHandleRect (gBlk, gHeadH).contains (e.getPosition()))
                                {
                                    // Alt/Option-click resets clip gain to 0 dB.
                                    if (e.mods.isAltDown())
                                    {
                                        auto* page = findParentComponentOfClass<EditPage>();
                                        if (page != nullptr) page->beginClipEdit();
                                        for (int peer : editGroupPeers())
                                            engine.setClipGainDb (peer, i, 0.0f);
                                        if (page != nullptr) page->commitClipEdit ("Reset clip gain");
                                        repaint();
                                        return;
                                    }
                                    draggingClipIdx     = i;
                                    draggingClipModeInt = 7;   // ClipGain
                                    dragStartGainDb     = c.gainDb;
                                    dragStartGainY      = e.y;
                                    if (auto* page = findParentComponentOfClass<EditPage>())
                                        page->beginClipEdit();
                                    return;
                                }
                            }

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
            if (toolbar != nullptr && index != clickRowIdx && inWavePane (e.x))
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
            if (compDragTake >= 0) { compDragCurX = e.x; repaint(); return; }   // comp swipe

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
                if (resizeAllDrag && onApplyToAllRows)
                {
                    onApplyToAllRows (Size::Custom, target);   // every row follows
                }
                else
                {
                    rowSize = Size::Custom;
                    customH = target;
                    if (onSizeChosen) onSizeChosen (*this, Size::Custom);
                }
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

                // Clip-gain handle drag (mode 7): relative ride from the grab
                // point -- drag up = louder. ~0.15 dB per pixel, full range.
                if (draggingClipModeInt == 7)
                {
                    const auto* clips = engine.tryClipsFor (index);
                    if (clips != nullptr && draggingClipIdx < (int) clips->size())
                    {
                        const float dB = juce::jlimit (-60.0f, 12.0f,
                            dragStartGainDb + (float) (dragStartGainY - e.y) * 0.15f);
                        for (int peer : editGroupPeers())
                            engine.setClipGainDb (peer, draggingClipIdx, dB);
                        repaint();
                    }
                    return;
                }

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
                            for (int peer : editGroupPeers())
                                engine.setClipFades (peer, draggingClipIdx, newIn, dragStartFadeOut);
                        }
                        else
                        {
                            // Drag right shrinks fade-out, drag left grows it.
                            const juce::int64 newOut = juce::jlimit<juce::int64> (
                                0, c.fileLengthSamples - dragStartFadeIn,
                                dragStartFadeOut - wantSamples);
                            for (int peer : editGroupPeers())
                                engine.setClipFades (peer, draggingClipIdx, dragStartFadeIn, newOut);
                        }
                        repaint();
                        return;
                    }

                    // Trim / Move: incremental.
                    juce::int64 stepSamples = wantSamples - lastDragSamples;
                    lastDragSamples = wantSamples;
                    if (std::abs (stepSamples) >= 1)
                    {
                        const auto mode = draggingClipModeInt == 0 ? AudioEngine::ClipEdit::TrimLeft
                                       : draggingClipModeInt == 1 ? AudioEngine::ClipEdit::TrimRight
                                                                  : AudioEngine::ClipEdit::Move;
                        // Honour the snap grid (snapSampleToGrid is an identity
                        // when snap is Off, and snaps to the nearest marker when
                        // on). We snap the *edge the engineer is dragging*: the
                        // clip start for Move / TrimLeft, the clip end for
                        // TrimRight -- so the Snap toggle affects dragging, not
                        // just split-at-playhead.
                        if (const auto* cl = engine.tryClipsFor (index))
                            if (draggingClipIdx < (int) cl->size())
                            {
                                const auto& cc = (*cl)[(size_t) draggingClipIdx];
                                const juce::int64 edge =
                                    (mode == AudioEngine::ClipEdit::TrimRight)
                                        ? cc.timelineStartSamples + cc.fileLengthSamples
                                        : cc.timelineStartSamples;
                                stepSamples = engine.snapSampleToGrid (edge + stepSamples) - edge;
                            }
                        if (stepSamples != 0)
                            for (int peer : editGroupPeers())
                                engine.editClip (peer, draggingClipIdx, mode, stepSamples);
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

        void mouseUp (const juce::MouseEvent& e) override
        {
            // Comp-lanes swipe finished: pull the swiped range from that
            // take into the active comp.
            if (compDragTake >= 0)
            {
                const int tk = compDragTake;
                compDragTake = -1;
                const auto& player = engine.getPlayer();
                const auto totalS = player.isLoaded() ? player.getTotalLengthSamples() : 0;
                if (totalS > 0 && tk != engine.getActiveTakeIdx (index))
                {
                    auto innerC = getLocalBounds().withTrimmedLeft (headerW)
                                                  .reduced (brand::space::xs, brand::space::sm);
                    const auto mapper = TimelineMapper::forLane (innerC, totalS);
                    const auto a = mapper.toSample (juce::jmin (compDragStartX, compDragCurX));
                    const auto b = mapper.toSample (juce::jmax (compDragStartX, compDragCurX));
                    if (b - a > 64)
                    {
                        auto* page = findParentComponentOfClass<EditPage>();
                        if (page != nullptr) page->beginClipEdit();
                        engine.compRangeFromTake (index, tk, a, b);
                        if (page != nullptr) page->commitClipEdit ("Comp swipe");
                    }
                }
                repaint();
                return;
            }

            // Close the automation drag transaction (if one is open).
            // mouseDown on the Select tool opened it when a point was
            // grabbed; this closes it on mouseUp regardless of whether
            // any mouseDrag happened in between (a click-without-drag
            // produces a no-op undo step, which is acceptable).
            const bool wasDraggingPoint   = (draggingPointIdx >= 0);
            const bool wasDraggingTension = (draggingTensionSegIdx >= 0);
            const bool wasDraggingClip    = (draggingClipIdx >= 0 || draggingXfadeAIdx >= 0);
            // A click that armed a clip-body grab but never dragged = a
            // selection click. Capture the clip + whether the mouse moved
            // before the resets below wipe draggingClipIdx.
            const int  clickedClipIdx = draggingClipIdx;
            const bool wasClick       = (e.getDistanceFromDragStart() < 5);
            dragging         = false;
            resizeAllDrag    = false;
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

            // Click (no drag) on a clip body selects it so Delete /
            // Duplicate / Nudge can act on it. Shift+click toggles the clip
            // in/out of a multi-selection. A plain click on empty lane area
            // clears the selection.
            if (wasClick && laneMode == LaneMode::Waveform && inWavePane (e.x))
                if (auto* page = findParentComponentOfClass<EditPage>())
                {
                    if (clickedClipIdx >= 0)
                    {
                        if (e.mods.isShiftDown())
                            page->toggleSelectedClip (index, clickedClipIdx);
                        else
                        {
                            // Plain click selects this clip + the matching
                            // clip on every EDIT-group peer.
                            std::vector<EditPage::ClipRef> refs;
                            for (int peer : editGroupPeers())
                                refs.push_back ({ peer, clickedClipIdx });
                            page->setSelectedClips (refs, { index, clickedClipIdx });
                        }
                    }
                    else if (! e.mods.isShiftDown())
                        page->clearSelectedClip();
                }
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

            // Anchor the call-out on the swatch's *pinned* on-screen position
            // (header floats at headerOriginX while the timeline is scrolled).
            const auto screenArea = localAreaToGlobal (
                juce::Rectangle<int> (headerOriginX(), 0, swatchW, getHeight()));
            juce::CallOutBox::launchAsynchronously (std::move (picker), screenArea, nullptr);
        }

        int  getHeaderWidth() const noexcept { return headerW; }
        int  getTrackIndex()  const noexcept { return index; }
        bool isStereoPair()   const noexcept { return stereo; }

        // Physical tracks this row's clip edits propagate to: itself plus
        // every member of its EDIT group (so split / trim / move / fade /
        // selection stay phase-coherent across grouped tracks). Ungrouped
        // tracks return just themselves.
        std::vector<int> editGroupPeers() const
        {
            const int g = engine.getTrackEditGroup (index);
            std::vector<int> v = g < 0 ? std::vector<int> { index }
                                       : engine.getStripsInEditGroup (g);
            if (v.empty()) v.push_back (index);

            // A collapsed stereo row edits BOTH halves: every peer that is a
            // stereo L gets its R partner appended (dedup-safe -- a grouped
            // pair may already carry both). Without this, clip ops on an
            // ungrouped stereo track hit only the LEFT channel -- the gain
            // ride the engineer hears splits the stereo image.
            std::vector<int> out;
            auto add = [&out] (int t)
            {
                if (std::find (out.begin(), out.end(), t) == out.end())
                    out.push_back (t);
            };
            const int n = engine.getRecorder().getNumTracks();
            for (int t : v)
            {
                add (t);
                if (t + 1 < n
                    && engine.getRecorder().getTrack (t).isStereo.load (std::memory_order_relaxed))
                    add (t + 1);
            }
            return out;
        }

        // True once the background thumbnail scan has finished for this row's
        // file(s) (an empty/no-source thumbnail counts as loaded). Lets the
        // page persist WaveCache.wfm the moment the whole session is scanned.
        bool waveformsLoaded() const noexcept
        {
            return thumbnailL.isFullyLoaded() && (! stereo || thumbnailR.isFullyLoaded());
        }

        Size getRowSize() const noexcept { return rowSize; }
        void setRowSize (Size s) noexcept { rowSize = s; }
        int  getCustomHeight() const noexcept { return customH; }
        void setCustomHeight (int px) noexcept { customH = juce::jlimit (kMinRowH, kMaxRowH, px); }
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
            const int  curveNow = clip.fadeCurve;

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
            menu.addItem (412, "Rename clip...");
            menu.addItem (400, muted  ? "Unmute clip" : "Mute clip");
            menu.addItem (401, locked ? "Unlock clip" : "Lock clip");
            menu.addSeparator();
            menu.addItem (410, "Duplicate clip",    ! locked);
            menu.addItem (411, "Delete clip",       ! locked);
            menu.addItem (413, "Consolidate clip",  ! locked);
            menu.addItem (414, "Normalize clip",    ! locked);
            menu.addSeparator();
            menu.addSubMenu ("Clip gain",  gain);
            menu.addSubMenu ("Fade in",    fadeIn,  ! locked);
            menu.addSubMenu ("Fade out",   fadeOut, ! locked);
            menu.addItem (320, "Equal-power fades", ! locked, curveNow == 1);
            menu.addItem (300, "Clear both fades", ! locked);

            juce::Component::SafePointer<TrackRow> self (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                [self, clipIdx, fIn, fOut, muted, locked, gainDb, curveNow, msToSamples] (int chosen)
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
                    // Structural per-clip ops mirror to every edit peer -- a
                    // collapsed stereo row carries its R partner (editGroupPeers
                    // appends it), so lock / duplicate / delete / consolidate hit
                    // BOTH channels and the pair stays in sync. Without this they
                    // edited only the LEFT half (the parallel clip lists drift).
                    case 401: withUndo (locked ? "Unlock clip" : "Lock clip",   [&]{ for (int p : self->editGroupPeers()) self->engine.setClipLocked (p, clipIdx, ! locked); }); return;
                    case 410: withUndo ("Duplicate clip", [&]{ for (int p : self->editGroupPeers()) self->engine.duplicateClip (p, clipIdx); });           return;
                    case 411: withUndo ("Delete clip",    [&]{ for (int p : self->editGroupPeers()) self->engine.deleteClip    (p, clipIdx); });           return;
                    case 414: withUndo ("Normalize clip", [&]
                              {
                                  // One shared gain for the whole (possibly stereo)
                                  // clip: take the most conservative gain across
                                  // peers (min => the loudest channel lands at
                                  // target, the stereo image stays balanced), then
                                  // apply it to every peer. For a native-stereo
                                  // pair the L's 2-ch file already spans both
                                  // channels, so its gain alone covers the pair.
                                  float g = 0.0f; bool found = false;
                                  for (int p : self->editGroupPeers())
                                  {
                                      const float pg = self->engine.clipNormalizeGainDb (p, clipIdx);
                                      if (std::isfinite (pg)) { g = found ? juce::jmin (g, pg) : pg; found = true; }
                                  }
                                  if (found)
                                      for (int p : self->editGroupPeers())
                                          self->engine.setClipGainDb (p, clipIdx, g);
                              }); return;
                    case 413: withUndo ("Consolidate clip", [&]
                              {
                                  // Flatten the SAME timeline range on every peer.
                                  // For a native-stereo pair each channel renders
                                  // to its own mono consolidated file (the R reads
                                  // file channel 1 via ArrangementSource), so the
                                  // pair stays stereo instead of collapsing to the
                                  // LEFT channel only.
                                  if (auto* cl = self->engine.tryClipsFor (self->index))
                                      if (clipIdx < (int) cl->size())
                                      {
                                          const auto& cc = (*cl)[(size_t) clipIdx];
                                          const auto s = cc.timelineStartSamples;
                                          const auto e = cc.timelineStartSamples + cc.fileLengthSamples;
                                          for (int p : self->editGroupPeers())
                                              self->engine.consolidateRange (p, s, e);
                                      }
                              }); return;
                    case 412:
                    {
                        juce::String cur;
                        if (auto* cl = self->engine.tryClipsFor (self->index))
                            if (clipIdx < (int) cl->size()) cur = (*cl)[(size_t) clipIdx].name;
                        auto* aw = new juce::AlertWindow ("Rename clip",
                            "Clip name:", juce::MessageBoxIconType::NoIcon);
                        aw->setLookAndFeel (&self->getLookAndFeel());
                        aw->addTextEditor ("name", cur);
                        dialog::primeNameEditor (*aw, "name");
                        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                        juce::Component::SafePointer<TrackRow> rowSafe (self);
                        aw->enterModalState (true, juce::ModalCallbackFunction::create (
                            [aw, rowSafe, clipIdx] (int r)
                        {
                            std::unique_ptr<juce::AlertWindow> own (aw);
                            if (r != 1 || rowSafe == nullptr) return;
                            const auto nm = own->getTextEditorContents ("name").trim();
                            if (nm.isEmpty()) return;
                            auto* page = rowSafe->findParentComponentOfClass<EditPage>();
                            if (page != nullptr) page->beginClipEdit();
                            rowSafe->engine.setClipName (rowSafe->index, clipIdx, nm);
                            if (page != nullptr) page->commitClipEdit ("Rename clip");
                            rowSafe->repaint();
                        }));
                        return;
                    }
                    // Gain presets apply to every peer (stereo R partner +
                    // edit-group members), like the gain-handle drag.
                    case 501: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx, -12.0f); }); return;
                    case 502: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx,  -6.0f); }); return;
                    case 503: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx,  -3.0f); }); return;
                    case 504: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx,   0.0f); }); return;
                    case 505: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx,   3.0f); }); return;
                    case 506: withUndo ("Clip gain", [&]{ for (int p : self->editGroupPeers()) self->engine.setClipGainDb (p, clipIdx,   6.0f); }); return;
                    case 320: withUndo ("Fade curve", [&]{ self->engine.setClipFadeCurve (self->index, clipIdx, curveNow == 1 ? 0 : 1); }); return;
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
                            for (int p : rowSafe->editGroupPeers())
                                rowSafe->engine.setClipGainDb (p, clipIdx, dB);
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

            // Apply one preset to every track at once (same as Option-dragging
            // a row edge). IDs 21..28 mirror the single-row sizes above.
            juce::PopupMenu allMenu;
            allMenu.addItem (21, "micro");
            allMenu.addItem (22, "mini");
            allMenu.addItem (23, "small");
            allMenu.addItem (24, "medium");
            allMenu.addItem (25, "large");
            allMenu.addItem (26, "jumbo");
            allMenu.addItem (27, "extreme");
            allMenu.addSeparator();
            allMenu.addItem (28, "fit to window");
            menu.addSubMenu ("Set all tracks to", allMenu);

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
            takesMenu.addItem (860, "Show take lanes (comp)", takes > 1, showCompLanes);
            // Comp: pull the selected range from another take into the
            // active comp (swipe-comp via the Range selection). IDs 870+i.
            const bool haveRange = engine.getPlayer().hasLoopRegion();
            if (takes > 1)
            {
                takesMenu.addSeparator();
                juce::PopupMenu compMenu;
                for (int i = 0; i < takes && i < 30; ++i)
                    compMenu.addItem (870 + i, engine.getTakeName (index, i), haveRange && i != active);
                takesMenu.addSubMenu (haveRange ? "Comp selection from"
                                                : "Comp selection from (select a range first)",
                                      compMenu, haveRange);
            }
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

                static const std::array<Size, 8> sizeMap { Size::Micro, Size::Mini, Size::Small,
                                                           Size::Medium, Size::Large, Size::Jumbo,
                                                           Size::Extreme, Size::FitToWindow };
                if (chosen >= 1 && chosen <= 8)
                {
                    const auto next = sizeMap[(std::size_t) (chosen - 1)];
                    row->rowSize = next;
                    if (row->onSizeChosen) row->onSizeChosen (*row, next);
                    return;
                }
                // "Set all tracks to ▸" -- 21..28 mirror the size list.
                if (chosen >= 21 && chosen <= 28)
                {
                    const auto next = sizeMap[(std::size_t) (chosen - 21)];
                    if (row->onApplyToAllRows) row->onApplyToAllRows (next, 0);
                    return;
                }

                // Take pick -- 800..829.
                if (chosen >= 800 && chosen < 830)
                {
                    row->engine.setActiveTake (row->index, chosen - 800);
                    row->repaint();
                    return;
                }
                // Comp selection from take -- 870..899.
                if (chosen >= 870 && chosen < 900)
                {
                    auto& player = row->engine.getPlayer();
                    if (! player.hasLoopRegion()) return;
                    auto* page = row->findParentComponentOfClass<EditPage>();
                    if (page != nullptr) page->beginClipEdit();
                    row->engine.compRangeFromTake (row->index, chosen - 870,
                                                   player.getLoopStart(), player.getLoopEnd());
                    if (page != nullptr) page->commitClipEdit ("Comp selection from take");
                    row->repaint();
                    return;
                }
                if (chosen == 850)   // new take from current
                {
                    row->engine.newTakeFromCurrent (row->index, {});
                    row->repaint();
                    return;
                }
                if (chosen == 860)   // toggle comp lanes
                {
                    row->showCompLanes = ! row->showCompLanes;
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

        // Halve the resolution of an envelope in place, keeping the louder
        // of each adjacent pair so the silhouette survives the downsample.
        static void decimateInPlace (std::vector<float>& v)
        {
            const size_t half = v.size() / 2;
            for (size_t i = 0; i < half; ++i)
                v[i] = juce::jmax (v[2 * i], v[2 * i + 1]);
            v.resize (half);
        }

        // Draw the live capture envelope across `area`: one mirrored
        // min/max bar per pixel column, max-pooled from the peak history,
        // in the record colour. `vz` is the same vertical zoom the file
        // thumbnails use so the live take and the post-stop waveform sit
        // at the same scale.
        static void drawRecEnvelope (juce::Graphics& g,
                                     juce::Rectangle<int> area,
                                     const std::vector<float>& peaks,
                                     float vz)
        {
            const int w = area.getWidth();
            const int n = (int) peaks.size();
            if (w <= 0 || n <= 0) return;
            const float midY = area.getCentreY();
            const float halfH = area.getHeight() * 0.5f;
            for (int x = 0; x < w; ++x)
            {
                // Map this column to a span of the history and take the
                // loudest peak in it (so the envelope never thins out when
                // there are more samples than pixels).
                const int a = (int) ((juce::int64) x       * n / w);
                const int b = juce::jmax (a + 1, (int) ((juce::int64) (x + 1) * n / w));
                float pk = 0.0f;
                for (int i = a; i < b && i < n; ++i) pk = juce::jmax (pk, peaks[(size_t) i]);
                const float h = juce::jlimit (0.0f, halfH, pk * vz * halfH);
                if (h <= 0.0f) continue;
                const float cx = (float) (area.getX() + x);
                g.drawVerticalLine ((int) cx, midY - h, midY + h);
            }
        }

        // Pro Tools-style three-column header layout:
        //  - swatch (14 px)
        //  - meter pinned to its right (12 px)
        //  - left content block: name + 4-button row + chip row (~140 px)
        //  - middle column: a/b/c/d send dots (~80 px)
        //  - right column: input/output pills + vol/pan readout + plus (~140 px)
        static constexpr int headerW = brand::space::editHeaderW;
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
        // True when the stereo pair is ONE natively-recorded interleaved
        // 2-channel file (no separate R file): the L lane draws file channel
        // 0, the R lane channel 1, both from the same thumbnail source.
        bool                      stereoOneFile { false };
        RenameLabel               nameLabel;
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
        // Live capture envelope -- one peak per UI tick while THIS track
        // records, so the lane draws a growing red waveform during the
        // take instead of staying blank until stop. Fed from the live
        // input meter (TrackState::peak), never the disk file, so it
        // costs nothing against capture integrity. recPeakR carries the
        // R partner on a stereo pair.
        std::vector<float>        recPeakL, recPeakR;
        bool                      liveRecording         { false };
        int                       lastInputDeviceCount  { -1 };
        int                       lastOutputDeviceCount { -1 };
        unsigned int              lastColourArgb        { 0 };
        Size                      rowSize               { Size::Small };
        int                       customH               { 80 };
        int                       dragStartHeight       { 0 };
        bool                      dragging              { false };
        bool                      resizeAllDrag         { false };   // Option-drag = resize every row
        bool                      menuOpen              { false };
        bool                      cursorIsResize        { false };
        // Comp-lanes view: when on (per row, opt-in via the take menu) the
        // lane splits into the active comp on top + one swipeable lane per
        // take below. Off by default -> zero change to normal editing.
        bool                      showCompLanes         { false };
        int                       compDragTake          { -1 };   // take lane being swiped
        int                       compDragStartX        { 0 };
        int                       compDragCurX          { 0 };
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

        // Apply one height preset (or a custom pixel height) to every row
        // at once -- the destination of Option-drag + "Set all tracks to".
        void applySizeToAll (TrackRow::Size s, int customPx)
        {
            for (auto& r : rows)
            {
                r->setRowSize (s);
                if (s == TrackRow::Size::Custom && customPx > 0)
                    r->setCustomHeight (customPx);
            }
            resized();
        }

        // Tab / Shift+Tab from a row's name field hops to the next /
        // previous row (wrapping at the ends) and opens its editor.
        void tabRename (TrackRow& from, bool shift)
        {
            const int n = (int) rows.size();
            if (n == 0) return;
            int pos = -1;
            for (int i = 0; i < n; ++i)
                if (rows[(size_t) i].get() == &from) { pos = i; break; }
            if (pos < 0) return;
            const int target = ((pos + (shift ? -1 : 1)) % n + n) % n;
            rows[(size_t) target]->beginRename();
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
                r->onTabRename  = [this] (TrackRow& from, bool shift) { tabRename (from, shift); };
                r->onApplyToAllRows = [this] (TrackRow::Size s, int customPx) { applySizeToAll (s, customPx); };
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

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            // DOUBLE-click in the empty area below the last row -> create a
            // new mono audio track. (Single click no longer adds a track --
            // it was too easy to spawn empty tracks by accident.) The
            // TrackList only sees the event when no TrackRow consumes it,
            // which means the engineer hit empty space. Right-click is left
            // alone for a potential future "Add multiple..." menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown()) return;
            if (engine.getRecorder().isRecording()) return;   // safety
            engine.addOneStrip();
        }

        // Dark gap left between adjacent rows so each track reads as its
        // own block (rows = light bgStrip, gaps + empty area = dark bgDeep).
        static constexpr int kRowGap = brand::space::sm;

        void resized() override
        {
            // Fit-to-window fallback distributes the viewport height across
            // the rows that opted into Size::FitToWindow -- minus the gaps
            // between rows, so a fit-to-window row still lands flush in view.
            int fixedTotal = 0;
            int fitRows    = 0;
            for (auto& r : rows)
            {
                if (r->getRowSize() == TrackRow::Size::FitToWindow) ++fitRows;
                else fixedTotal += TrackRow::pixelsFor (r->getRowSize());
            }
            const int gapTotal = (int) rows.size() > 1 ? ((int) rows.size() - 1) * kRowGap : 0;
            const int fitFallback = fitRows > 0
                ? juce::jmax (28, (viewportHeight - fixedTotal - gapTotal) / fitRows)
                : 80;

            auto bounds = getLocalBounds();
            int totalH = 0;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const int h = rows[i]->getRowPixelHeight (fitFallback);
                rows[i]->setBounds (bounds.removeFromTop (h));
                totalH += h;
                if (i + 1 < rows.size())
                {
                    bounds.removeFromTop (kRowGap);   // dark separator
                    totalH += kRowGap;
                }
            }
            // Resize ourselves so the viewport scrolls when content > view.
            setSize (getWidth(), juce::jmax (viewportHeight, totalH));
        }

        // Paint the dark substrate so the gaps between rows (and the area
        // below the last row) read as bgDeep, separating the light rows.
        void paint (juce::Graphics& g) override { g.fillAll (brand::bgDeep); }

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

        // Live capture envelope: arm / disarm every row's live draw, and
        // (each tick) feed every armed row its current input peak so the
        // lane grows a red waveform during the take. Reads only the live
        // meter atomics -- no disk I/O, so capture integrity is untouched.
        void setLiveRecording (bool on)
        {
            for (auto& r : rows) r->setLiveRecording (on);
        }
        void pushRecLevels()
        {
            auto& rec = engine.getRecorder();
            const int nTracks = rec.getNumTracks();
            for (auto& r : rows)
            {
                const int idx = r->getTrackIndex();
                if (idx < 0 || idx >= nTracks) continue;
                if (! rec.getTrack (idx).armed.load (std::memory_order_relaxed)) continue;
                const float l = rec.getTrack (idx).peak.load (std::memory_order_relaxed);
                float rr = 0.0f;
                if (r->isStereoPair() && idx + 1 < nTracks)
                    rr = rec.getTrack (idx + 1).peak.load (std::memory_order_relaxed);
                r->pushRecLevel (l, rr);
            }
        }

        void setPlayheadX (int px)
        {
            for (auto& r : rows) r->setPlayheadX (px);
        }

        void pollMixerState()
        {
            for (auto& r : rows) r->updatePollState();
        }

        // Re-pin every row's header column to the (new) horizontal scroll
        // position: re-lay the header child components and repaint so the
        // painted header panel follows. Called from the viewport's scroll
        // callback whenever the timeline scrolls horizontally.
        void relayoutHeaders()
        {
            for (auto& r : rows) { r->resized(); r->repaint(); }
        }

        int rowCount() const { return (int) rows.size(); }

        // True when every row's thumbnail has finished its background scan.
        bool allWaveformsLoaded() const
        {
            for (auto& r : rows)
                if (r != nullptr && ! r->waveformsLoaded())
                    return false;
            return true;
        }

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

        // Gig-one field report: waveform builds felt slow after record/load.
        // The AudioThumbnailCache scans on its own TimeSliceThread at default
        // priority, competing with everything; re-start it high so freshly
        // recorded takes paint their waveforms as fast as the disk allows.
        {
            auto& scanThread = thumbnailCache.getTimeSliceThread();
            scanThread.stopThread (2000);
            scanThread.startThread (juce::Thread::Priority::high);
        }

        list = std::make_unique<TrackList> (engine, formatManager, thumbnailCache);
        list->sharedToolsBar = toolsBar.get();
        viewport.setViewedComponent (list.get(), false);
        // Vertical scrollbar visible; horizontal scrollbar hidden BUT
        // horizontal scrolling still enabled (4th arg = allow horizontal
        // scrolling without a scrollbar) so the trackpad / wheel still pan
        // the timeline left-right. The redundant horizontal scrollbar (a
        // second blue bar duplicating the TimelineMinimap overview navigator)
        // is gone, but the scroll gesture is not.
        viewport.setScrollBarsShown (true, false, /*allowVertWithoutBar*/ false,
                                     /*allowHorizWithoutBar*/ true);
        // Re-pin every row's header column to the left edge on horizontal
        // scroll so the meter / routing / R-I-S-M controls never slide away.
        viewport.onScroll = [this] { if (list != nullptr) list->relayoutHeaders(); };
        addAndMakeVisible (viewport);

        // Overview navigator -- hidden until zoomed in (resized() shows it).
        addChildComponent (minimap);
        minimap.onScrollToContentX = [this] (int x)
        {
            const int maxX = juce::jmax (0, list->getWidth() - viewport.getViewWidth());
            viewport.setViewPosition (juce::jlimit (0, maxX, x), viewport.getViewPositionY());
        };

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
        zoomHOut.setTooltip ("Zoom out (1x = whole take)");
        // Accessibility: the on-screen labels are "V+/V-/H+/H-", which a
        // screen reader would read as glyphs. Give each a spoken name (and
        // surface the tooltip as help text). They are TextButtons, so the
        // button role + Return/Space activation already work.
        zoomVIn .setTitle ("Vertical zoom in");    zoomVIn .setHelpText (zoomVIn .getTooltip());
        zoomVOut.setTitle ("Vertical zoom out");   zoomVOut.setHelpText (zoomVOut.getTooltip());
        zoomHIn .setTitle ("Horizontal zoom in");  zoomHIn .setHelpText (zoomHIn .getTooltip());
        zoomHOut.setTitle ("Horizontal zoom out"); zoomHOut.setHelpText (zoomHOut.getTooltip());
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

        // While recording, keep the lanes clear (the live red capture envelope
        // owns them) and do NOT point thumbnails at the growing WAVs -- scanning
        // a half-written file paints blocky garbage and the 55-channel disk read
        // contends with the recorder's own writes. The real waveform is scanned
        // once, cleanly, the moment the take stops (recJustStopped path).
        if (engine.isRecording())
            list->setWaveformsFromSession ({});
        else
            list->setWaveformsFromSession (sessionDir);

        updatePlaceholder();
        lastLoaded = engine.getPlayer().isLoaded();
    }

    void EditPage::repaintLanes()
    {
        if (list != nullptr) list->repaint();   // rows read engine automation in paint()
        repaint();
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
        // Session-change detection + waveform attach run REGARDLESS of EDIT
        // visibility, so the AudioThumbnail background scan (and WaveCache.wfm
        // build) kick off the moment audio is imported / recorded -- not when
        // the engineer first switches to EDIT. Rows built while hidden are
        // just invisible components; the actual file read happens on the
        // thumbnail cache's background thread. Only the per-tick visual work
        // further down is gated on visibility.
        const int n = engine.getRecorder().getNumTracks();
        if (n != lastTrackCount)
            refresh();

        // Pick up session swaps -- recording starts, recording stops,
        // session loaded, session changed, ...
        const bool loaded = engine.getPlayer().isLoaded();
        const bool rec    = engine.isRecording();
        const bool recJustStopped = (! rec && lastRecording);
        const bool recJustStarted = (rec && ! lastRecording);

        // Live capture envelope: clear + arm on take start, disarm on stop
        // (before the post-stop disk re-scan swaps in the real waveform).
        if (recJustStarted && list != nullptr) list->setLiveRecording (true);
        if (recJustStopped && list != nullptr) list->setLiveRecording (false);
        if (loaded != lastLoaded || rec != lastRecording || engine.getActiveSessionDir() != lastSessionDir)
        {
            if (engine.getActiveSessionDir() != lastSessionDir)
                waveCacheSaved = false;   // new session -> its cache needs (re)writing
            lastSessionDir = engine.getActiveSessionDir();
            lastRecording  = rec;
            refresh();
        }

        // Persist WaveCache.wfm as soon as the background scan finishes for the
        // whole session -- even while EDIT is hidden -- so reopening the show
        // (or relaunching) paints waveforms instantly instead of re-scanning
        // every WAV. Without this the cache was only flushed on app quit, so a
        // freshly-imported session that wasn't quit cleanly re-scanned on the
        // next open. Done once per session; skipped while recording (files
        // still growing).
        if (! waveCacheSaved && ! rec && list != nullptr && list->rowCount() > 0
            && engine.getActiveSessionDir().isDirectory()
            && list->allWaveformsLoaded())
        {
            saveCacheToSession (engine.getActiveSessionDir());
            waveCacheSaved = true;
        }

        // Waveforms are NOT re-scanned from disk while recording: 48 channels
        // re-read at 24 Hz would contend with the recorder's own capture
        // writes and risk dropouts (capture integrity wins). The meters show
        // live signal during the take. The moment recording stops, the files
        // are final -- do one clean full re-scan (which drops any partial
        // cached mid-capture) so the waveform paints at full resolution.
        if (recJustStopped && list != nullptr)
            list->forceRefreshWaveforms();

        // Everything below is purely visual (playhead + repaints) and only
        // matters when EDIT is on screen. While recording we keep going even
        // when hidden so peaks stay current for the flip back to EDIT.
        if (! isVisible() && ! rec)
            return;

        // Append this tick's input peaks to the live capture envelope so
        // every armed lane grows a red waveform during the take. Kept
        // running even while EDIT is hidden so flipping back mid-take
        // shows the envelope already filled in.
        if (rec && list != nullptr)
            list->pushRecLevels();

        // Playhead. While RECORDING the live head is the recorder's elapsed
        // count on a grow-to-fit timeline (same timebase as the ruler + the
        // live capture envelope) -- gig-one field report: the engineer must
        // see the take rolling in EDIT, not a parked view.
        const auto& player = engine.getPlayer();
        const auto  recPos  = rec ? engine.getRecorder().getSamplesSinceStart() : 0;
        const auto  total   = rec ? juce::jmax (player.getTotalLengthSamples(), recPos)
                                  : player.getTotalLengthSamples();
        const auto  pos     = rec ? recPos : player.getPositionSamples();
        int playheadX = -1;
        if (total > 0 && list->rowCount() > 0)
        {
            constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
            // Map across the ROWS' width (list->getWidth() == contentW), not
            // the EditPage's visible width -- when zoomed in the two differ,
            // and the playhead lives inside the contentW-wide TrackRow. The
            // -8/+4 matches the lane's clip inset (brand::space::xs each side)
            // AND the ruler's mapping, so both playheads land on the same x.
            const auto wavePaneWidth = juce::jmax (1, list->getWidth() - kHeaderW);
            const double frac = (double) pos / (double) total;
            playheadX = (int) (frac * (wavePaneWidth - 8)) + 4;
        }
        list->setPlayheadX (playheadX);
        list->pollMixerState();

        // ── Auto-scroll: keep the playhead on screen while playing ──────────
        // On a long take zoomed in, the playhead would otherwise roll off the
        // right edge and you'd lose it. Page-scroll (not continuous follow, so
        // it doesn't jitter): when the playhead passes ~85% of the visible
        // width, jump so it lands just right of the pinned header, showing the
        // audio that's coming up. Only while playing and only when zoomed in
        // (content wider than the view) -- so a stopped engineer can scroll
        // freely.
        if ((player.isPlaying() || rec) && total > 0 && list != nullptr)
        {
            constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
            const int viewW = viewport.getViewWidth();
            const int contentW = list->getWidth();
            if (contentW > viewW + 1)   // zoomed in
            {
                const int contentWaveW = juce::jmax (1, contentW - kHeaderW);
                const double frac = (double) pos / (double) total;
                const int playheadContentX = kHeaderW + (int) (frac * contentWaveW);
                const int viewX = viewport.getViewPositionX();
                const int rightTrigger = viewX + (int) (viewW * 0.85);
                if (playheadContentX > rightTrigger || playheadContentX < viewX + kHeaderW)
                {
                    const int maxX = juce::jmax (0, contentW - viewW);
                    int newX = playheadContentX - kHeaderW - (int) (viewW * 0.10);
                    newX = juce::jlimit (0, maxX, newX);
                    if (newX != viewX)
                        viewport.setViewPosition (newX, viewport.getViewPositionY());
                }
            }
        }

        // Keep the overview navigator in sync with the live scroll/zoom.
        if (minimap.isVisible())
            minimap.setView (list->getWidth(), 380,
                             viewport.getViewPositionX(), viewport.getViewWidth());
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

        // Overview navigator strip along the bottom -- only when zoomed in
        // (content wider than the view), leaving room for the H/V zoom
        // clusters at the bottom-right.
        const bool showMinimap = (zoom > 1.001f);
        if (showMinimap)
        {
            auto mmRow = bounds.removeFromBottom (18);
            mmRow.removeFromRight (70);   // clear the zoom clusters
            minimap.setBounds (mmRow.reduced (4, 1));
            minimap.setVisible (true);
        }
        else
        {
            minimap.setVisible (false);
        }

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

    void EditPage::zoomToSamples (juce::int64 a, juce::int64 b)
    {
        if (list == nullptr) return;
        const auto& player = engine.getPlayer();
        const auto total = player.isLoaded() ? player.getTotalLengthSamples() : 0;
        if (total <= 0 || b <= a) return;
        // Zoom so the selection ~fills the viewport (a little headroom), then
        // centre on it. setZoom clamps to [1, 16].
        const double frac = (double) (b - a) / (double) total;
        if (frac > 0.0) setZoom ((float) juce::jlimit (1.0, 16.0, 0.9 / frac));
        scrollToSample ((a + b) / 2);
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
