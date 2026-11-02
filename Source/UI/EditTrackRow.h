#pragma once

// EditPage::TrackRow (+ the shared EDIT constants & TimelineMapper) extracted
// verbatim from EditPage.cpp, which had grown past 4,700 lines. Pure
// relocation -- no behaviour change -- to cut the god-class. Included only by
// EditPage.cpp.

#include "EditPage.h"
#include "ClipEditTargets.h"
#include "EditToolsBar.h"
#include "EditTimeRuler.h"
#include "LivePeakHistory.h"
#include "WaveCacheFile.h"
#include "WaveformOverview.h"
#include "../Audio/MultiPartReader.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "LedMeter.h"
#include "StripColourPicker.h"
#include "RenameLabel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace zynforge
{
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

        // Stop reading the (about-to-be-freed) TrackState. The EDIT rows have
        // exactly the lifetime hazard ChannelStrip::invalidate() closes on the
        // MIXER side: this row's LedMeter holds a TrackState& AND runs its own
        // timer, and the rows are only rebuilt on EditPage's next 24 Hz tick --
        // so every recorder-vector shrink (session open with fewer tracks,
        // Close Session, New Session, apply template, delete strips,
        // New-from-CSV) left the meter sampling freed memory for up to a tick.
        // condemnAllStrips() now calls through to here as well. Idempotent;
        // paint() is separately guarded by the stale-index check.
        void invalidate()
        {
            rowValid = false;
            meter.detach();
        }

        // Tools palette -- set by the host EditPage after construction so
        // all rows share the same global view.
        EditToolsBar*      toolsBar { nullptr };

        // Clip-edit drag tracking. When the engineer grabs a clip's
        // left edge, right edge, body, a fade handle or its gain line,
        // draggingClipIdx pins which clip is moving and draggingClipMode
        // picks the edit (SelectRange is a tool drag on the lane).
        enum class ClipDrag
        {
            TrimLeft,     // slip-trim the left edge
            TrimRight,    // extend / shrink the right edge
            Move,         // slide on the timeline
            FadeIn,       // drag the fade-in apex
            FadeOut,      // drag the fade-out apex
            SelectRange,  // Selector tool range
            ClipGain      // clip-gain line (relative ride)
        };
        int      draggingClipIdx  { -1 };
        ClipDrag draggingClipMode { ClipDrag::TrimLeft };
        // The clips a drag edits, resolved once at the first step and held for
        // the whole gesture: resolving again as the clip travels could land on
        // the neighbour it was dragged over.
        std::vector<std::pair<int, int>> dragPeers;
        // Crossfade-midpoint drag. When >= 0, the engineer grabbed
        // the 6 px dot that paints at the midpoint of an overlap
        // between timeline-adjacent clips. draggingXfadeAIdx is the
        // OUTGOING clip's index in the per-track list, draggingXfadeBIdx
        // the incoming one's (not necessarily AIdx + 1: a Move leaves the
        // list unsorted). We slide the equal-power crossfade point by
        // updating both clips' fade lengths so the midpoint follows
        // the mouse.
        int draggingXfadeAIdx { -1 };
        int draggingXfadeBIdx { -1 };
        int dragStartX          {  0 };
        juce::int64 lastDragSamples { 0 };
        juce::int64 dragStartFadeIn  { 0 };
        juce::int64 dragStartFadeOut { 0 };
        float       dragStartGainDb  { 0.0f };   // clip-gain handle drag (ClipDrag::ClipGain)
        int         dragStartGainY   { 0 };      // mouse y at gain-drag start (relative ride)
        juce::int64 dragTimelineSamples { 0 };   // session length when a clip drag was grabbed

        // Strip reorder drag -- armed on swatch-column mouseDown, activates
        // once vertical movement exceeds 8 px. While dragging, EditPage only
        // PREVIEWS the drop position; the move commits once, on release.
        bool reorderArmed  { false };
        bool reorderActive { false };
        int  reorderStartY { 0 };
        int  reorderInsertBefore { -1 };   // physical index to land in front of; -1 = none

        TrackRow (int trackIdx,
                  bool isStereoPair,
                  AudioEngine& eng,
                  juce::AudioFormatManager& formats,
                  juce::AudioThumbnailCache& cache)
            : index (trackIdx), stereo (isStereoPair), engine (eng),
              formats (formats),
              thumbCache (cache),
              thumbnailL (wavecache::kThumbResolution, formats, cache),
              thumbnailR (wavecache::kThumbResolution, formats, cache),
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
                const auto typed = nameLabel.getText().trim();
                const auto newName = typed.isEmpty() ? juce::String (index + 1) : typed;
                // Go through engine.setTrackName ONLY. The raw `st.name = ...`
                // this used to do bypassed setNameThreadSafe, racing
                // getNameThreadSafe() off the message thread (torn / freed
                // juce::String) -- and it was redundant, since
                // setTrackName does the locked write plus persistence.
                engine.setTrackName (index, newName);
                // A stereo pair is ONE logical strip: mirror the name onto the
                // R half like arm / mon / mute / solo already do, or the R
                // track keeps a stale name in session_mix.json and shows it
                // the moment the pair is unlinked.
                if (stereo && index + 1 < engine.getRecorder().getNumTracks())
                    engine.setTrackName (index + 1, newName + " R");
                nameLabel.setText (newName, juce::dontSendNotification);
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
            // Each toggle reaches the stereo partner and the edit group, as the
            // same button does on the MIXER strip.
            armButton .onClick = [this]
            {
                for (int t : editGroupPeers())
                    engine.setTrackArmed (t, armButton.getToggleState());
                armButton.setToggleState (engine.getRecorder().getTrack (index).armed.load(), juce::dontSendNotification);
            };
            monButton.onClick = [this]
            {
                for (int t : editGroupPeers())
                    if (t < engine.getRecorder().getNumTracks())
                        engine.getRecorder().getTrack (t).monitor.store (monButton.getToggleState());
            };
            muteButton.onClick = [this]
            {
                for (int t : editGroupPeers())
                    engine.setTrackMuted (t, muteButton.getToggleState());
            };
            soloButton.onClick = [this]
            {
                for (int t : editGroupPeers())
                    engine.setTrackSoloed (t, soloButton.getToggleState());
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
            viewButton.setTooltip ("Pick what this row's lane draws -- waveform or markers");
            viewButton.onClick = [this]
            {
                // Lane-content picker. Reserved Pro Tools-style items
                // (blocks / playlists / analysis / warp / transcript)
                // are dropped from the menu -- they were never wired and
                // the greyed entries were just clutter.
                juce::PopupMenu m;
                m.addItem (22, "waveform",    true, laneMode == LaneMode::Waveform);
                m.addItem (20, "markers",     true, laneMode == LaneMode::Markers);

                juce::Component::SafePointer<TrackRow> self (this);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton),
                                 [self] (int chosen)
                {
                    if (self == nullptr || chosen == 0) return;
                    switch (chosen)
                    {
                        case 20: self->laneMode = LaneMode::Markers;    break;
                        case 22: self->laneMode = LaneMode::Waveform;   break;
                        default: return;
                    }
                    self->repaint();
                });
            };
            addAndMakeVisible (viewButton);

            // Input + output routing combos -- same wiring as the mixer.
            auto styleCombo = [] (juce::ComboBox& c)
            {
                c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep.withAlpha (zynforge::brand::alpha::muted));
                c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
            };
            styleCombo (inputCombo);
            styleCombo (outputCombo);

            // One patch in three views: either combo sets both sides, as the
            // MIXER strip and PATCH do, and on both halves of a stereo row.
            const auto patchFrom = [this] (const juce::ComboBox& combo)
            {
                const int id = combo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackLinkedRouting (index, dev);
                if (stereo)
                    engine.setTrackLinkedRouting (index + 1, (dev < 0) ? -1 : dev + 1);
            };
            inputCombo.onChange  = [this, patchFrom] { patchFrom (inputCombo); };
            outputCombo.onChange = [this, patchFrom] { patchFrom (outputCombo); };
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
            // Condemned (TrackState being freed) or the vector already shrank
            // past us -- same guard paint() carries. Without it the 24 Hz poll
            // reads freed memory in the window before TrackList::rebuild runs.
            if (! rowValid || index >= engine.getRecorder().getNumTracks()) return;
            auto& s = engine.getRecorder().getTrack (index);
            if (armButton .getToggleState() != s.armed .load()) armButton .setToggleState (s.armed .load(), juce::dontSendNotification);
            if (muteButton.getToggleState() != s.muted .load()) muteButton.setToggleState (s.muted .load(), juce::dontSendNotification);
            if (soloButton.getToggleState() != s.soloed.load()) soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
            if (monButton .getToggleState() != s.monitor.load()) monButton .setToggleState (s.monitor.load(), juce::dontSendNotification);  // input-monitor changed in MIXER must reflect here
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
            int inR  = engine.getEffectiveInputRouting (index);   // what it hears on this device
            int outR = s.outputRouting.load();
            if (inR  == -2) inR  = index;
            if (outR == -2) outR = index;
            const int wantIn  = (inR  < 0) ? 1 : inR  + 2;
            const int wantOut = (outR < 0) ? 1 : outR + 2;
            // A STEREO row lists only even start channels (ids 2, 4, 6 ...),
            // but the routing can legitimately be odd -- a stereo L sitting at
            // an odd physical index gets inputRouting = 1 from
            // applyPersistedStripState. setSelectedId with an id that isn't in
            // the list silently cleared the box, so the engineer saw a BLANK
            // routing combo with no way to tell what the pair was patched to.
            // Add the real entry on demand so the state is always displayed.
            ensureRoutingItem (inputCombo,  wantIn,  inR,  "In ");
            ensureRoutingItem (outputCombo, wantOut, outR, "Out ");
            if (inputCombo .getSelectedId() != wantIn ) inputCombo .setSelectedId (wantIn,  juce::dontSendNotification);
            if (outputCombo.getSelectedId() != wantOut) outputCombo.setSelectedId (wantOut, juce::dontSendNotification);
        }

        // A clip keeps its file-stem name on disk and in the session ("Track_03",
        // "Track_03 continued", "Track_03 (a)"). On screen that stem reads as the
        // channel; a name the engineer gave a clip stays as it is.
        juce::String clipDisplayName (const Clip& c) const
        {
            const auto name = c.name.isNotEmpty() ? c.name
                                                  : engine.getTakeName (index, engine.getActiveTakeIdx (index));
            const auto stem = name.upToFirstOccurrenceOf (" ", false, false);
            if (stem.startsWith ("Track_") && stem.substring (6).containsOnly ("0123456789"))
                return nameLabel.getText() + name.substring (stem.length());
            return name;
        }

        // Make sure `id` exists in `box`; if not, append a correctly-labelled
        // entry for device channel `dev` so an off-grid routing still shows.
        void ensureRoutingItem (juce::ComboBox& box, int id, int dev, const char* prefix)
        {
            if (id <= 1 || dev < 0) return;                       // "(unrouted)" always exists
            if (box.indexOfItemId (id) >= 0) return;
            auto label = stereo
                ? juce::String (prefix) + juce::String (dev + 1) + "-" + juce::String (dev + 2)
                : juce::String (prefix) + juce::String (dev + 1);
            // Match rebuildRoutingCombos' decoration exactly: a channel the
            // device doesn't currently have reads "(off)". Without this the
            // on-demand entry was the ONE item in the list missing the suffix.
            const bool isOut = juce::String (prefix).startsWithIgnoreCase ("Out");
            const int  count = isOut ? engine.getCurrentDeviceOutputCount()
                                     : engine.getCurrentDeviceInputCount();
            const bool live  = stereo ? (dev + 1 < count) : (dev < count);
            if (! live) label += " (off)";
            box.addItem (label, id);
        }

        void setWaveformFiles (const juce::File& fL, const juce::File& fRin, int monoChannel = 0)
        {
            monoFileChannel = monoChannel;
            // Native interleaved stereo: the pair is ONE 2-channel file with
            // no separate R file. Point BOTH lanes at it (each draws its own
            // channel) so the stereo image still shows two lanes. Legacy
            // two-mono-file pairs have a real fR and keep drawing as before.
            stereoOneFile = stereo && fL.existsAsFile() && ! fRin.existsAsFile();
            const juce::File fR = stereoOneFile ? fL : fRin;

            if (fL != currentFileL || fR != currentFileR)
            {
                overviewCache.clear();
                ownThumbs.clear();
            }

            if (fL != currentFileL)
            {
                currentFileL = fL;
                applyThumbSource (thumbnailL, fL, false);
            }
            if (fR != currentFileR)
            {
                currentFileR = fR;
                applyThumbSource (thumbnailR, fR, false);
            }
            repaint();
        }

        // Point a thumbnail at a take. A single file -> FileInputSource (cached
        // by file hash, as before). A MULTI-PART take (Track_NN + its
        // Track_NN_partXX continuations from auto-split / continue-recording) ->
        // a ConcatReader spanning ALL parts, so the whole take draws instead of
        // only part 1. force=true drops the cached thumb first (post-stop, to
        // re-scan a take that just grew a new part).
        void applyThumbSource (juce::AudioThumbnail& thumb, const juce::File& main, bool force)
        {
            if (! main.existsAsFile()) { thumb.setSource (nullptr); return; }
            const auto parts = findTakeParts (main);
            if (parts.size() <= 1)
            {
                if (force) thumbCache.removeThumb (main.hashCode());
                thumb.setSource (new juce::FileInputSource (main));
                return;
            }
            // Stable combined hash so the multi-part thumb caches across reopen
            // without colliding with the single-file entry.
            const juce::int64 h = main.hashCode() ^ ((juce::int64) parts.size() << 48) ^ 0x5bd1e995LL;
            if (force) thumbCache.removeThumb (h);
            if (auto r = ConcatReader::create (formats, parts))
                thumb.setReader (r.release(), h);
            else
                thumb.setSource (new juce::FileInputSource (main));
        }

        // What one lane of one clip draws. Row thumbnails hold the strip's own
        // take, so a clip that plays another file (a consolidate render, a clip
        // pasted from another strip) needs that file's thumbnail: applying its
        // offsets to the take drew audio it does not play.
        struct LaneWave
        {
            juce::AudioThumbnail* thumb { nullptr };
            bool wholeFile { false };   // draw every channel of a one-channel-per-lane file
            int  channel   { 0 };
            bool hasAudio() const { return thumb != nullptr && thumb->getTotalLength() > 0.0; }
        };

        juce::AudioThumbnail* thumbnailForFile (const juce::File& file)
        {
            for (auto& own : ownThumbs)
                if (own.file == file) return own.thumb.get();
            auto thumb = std::make_unique<juce::AudioThumbnail> (wavecache::kThumbResolution, formats, thumbCache);
            applyThumbSource (*thumb, file, false);
            ownThumbs.push_back ({ file, std::move (thumb) });
            return ownThumbs.back().thumb.get();
        }

        LaneWave laneWaveFor (const Clip& c, bool rightLane)
        {
            const auto& take = rightLane ? currentFileR : currentFileL;
            if (c.audioFile != juce::File() && c.audioFile != take)
                return { thumbnailForFile (c.audioFile), false,
                         c.sourceChannel >= 0 ? juce::jlimit (0, 1, c.sourceChannel) : 0 };
            if (rightLane) return { &thumbnailR, ! stereoOneFile, stereoOneFile ? 1 : 0 };
            if (stereo)    return { &thumbnailL, ! stereoOneFile, 0 };
            return { &thumbnailL, false, monoFileChannel };
        }

        // The R lane of L's clip clipIdx: R's own clip, which can play its own file.
        LaneWave rightLaneWaveFor (int clipIdx)
        {
            if (! stereo) return {};
            const auto* list = engine.tryClipsFor (index + 1);
            const int i = clipedit::peerClipIndex (engine, index, clipIdx, index + 1);
            if (list == nullptr || i < 0 || i >= (int) list->size()) return {};
            return laneWaveFor ((*list)[(size_t) i], true);
        }

        // Every waveform the row draws through JUCE's AudioThumbnail goes
        // through here. channel < 0 draws all channels (a whole mono file).
        // JUCE refills its per-pixel cache across the WHOLE area it is given,
        // whatever the graphics clip: at deep zoom a clip block (or the lane)
        // is ~a million pixels wide, and below 128 samples per pixel every one
        // of those pixels is a read of the take on the message thread -- a
        // multi-second freeze per repaint. Narrow the area and time range to
        // the visible columns first so the work scales with the screen. A
        // fully visible area (normal zoom) is drawn exactly as before.
        static void drawThumbnailVisible (juce::Graphics& g, juce::AudioThumbnail& thumb,
                                          juce::Rectangle<int> area, double t0, double t1,
                                          int channel, float zoom)
        {
            const auto visible = area.getIntersection (g.getClipBounds());
            if (visible.isEmpty()) return;
            if (visible.getX() != area.getX() || visible.getRight() != area.getRight())
            {
                const double secondsPerPixel = (t1 - t0) / area.getWidth();
                const double v0 = t0 + (visible.getX()     - area.getX()) * secondsPerPixel;
                const double v1 = t0 + (visible.getRight() - area.getX()) * secondsPerPixel;
                area = { visible.getX(), area.getY(), visible.getWidth(), area.getHeight() };
                t0 = v0;
                t1 = v1;
            }
            if (channel < 0) thumb.drawChannels (g, area, t0, t1, zoom);
            else             thumb.drawChannel  (g, area, t0, t1, channel, zoom);
        }

        // Use a short-window median for the body of a zoomed-out take while
        // retaining the actual maximum as a thin marker in each screen column.
        // Cache the result because playhead repaints must not re-query millions
        // of thumbnail bins at 24 Hz.
        bool drawOverviewWave (juce::Graphics& g, juce::AudioThumbnail& thumb,
                               juce::Rectangle<int> area, int channel,
                               double t0, double t1, float zoom,
                               juce::Colour bodyColour, juce::Colour peakColour)
        {
            if (! thumb.isFullyLoaded() || channel < 0 || channel >= thumb.getNumChannels()
                || area.getWidth() <= 0 || area.getHeight() <= 0 || t1 <= t0
                || (t1 - t0) / area.getWidth() < 0.05)
                return false;

            // At deep zoom the lane can be millions of pixels wide.
            // Build only the portion in the viewport, then reuse it for normal
            // repaints; scanning the entire lane would stall the message thread.
            const auto visible = area.getIntersection (g.getClipBounds());
            if (visible.isEmpty()) return true;
            const double secondsPerPixel = (t1 - t0) / area.getWidth();
            const double visibleStart = t0 + (visible.getX() - area.getX()) * secondsPerPixel;
            const double visibleEnd = visibleStart + visible.getWidth() * secondsPerPixel;

            auto matches = [&] (const CachedOverview& entry)
            {
                return entry.thumbnail == &thumb && entry.channel == channel
                    && entry.width == visible.getWidth()
                    && entry.start == visibleStart && entry.end == visibleEnd;
            };
            auto it = std::find_if (overviewCache.begin(), overviewCache.end(), matches);
            if (it == overviewCache.end())
            {
                ++overviewBuilds;
                CachedOverview entry;
                entry.thumbnail = &thumb;
                entry.channel = channel;
                entry.width = visible.getWidth();
                entry.start = visibleStart;
                entry.end = visibleEnd;
                entry.levels = buildWaveformOverview (entry.width, visibleStart, visibleEnd,
                    [&] (double a, double b)
                    {
                        float lo = 0.0f, hi = 0.0f;
                        thumb.getApproximateMinMax (a, b, channel, lo, hi);
                        return juce::jmax (std::abs (lo), std::abs (hi));
                    });
                overviewCache.push_back (std::move (entry));
                it = std::prev (overviewCache.end());
            }
            it->lastPaint = overviewPaint;

            const float mid = (float) area.getCentreY();
            const float half = area.getHeight() * 0.5f;
            juce::RectangleList<float> peakRects, bodyRects;
            peakRects.ensureStorageAllocated (visible.getWidth());
            bodyRects.ensureStorageAllocated (visible.getWidth());
            for (int drawX = visible.getX(); drawX < visible.getRight(); ++drawX)
            {
                const auto x = (size_t) (drawX - visible.getX());
                const float p = juce::jlimit (0.0f, half, it->levels.peaks[x] * zoom * half);
                const float b = juce::jlimit (0.0f, half, it->levels.body[x] * zoom * half);
                if (p > 0.0f)
                    peakRects.addWithoutMerging ({ (float) drawX, mid - p, 1.0f, 2.0f * p });
                if (b > 0.0f)
                    bodyRects.addWithoutMerging ({ (float) drawX, mid - b, 1.0f, 2.0f * b });
            }
            g.setColour (peakColour);
            g.fillRectList (peakRects);
            g.setColour (bodyColour);
            g.fillRectList (bodyRects);
            return true;
        }

        // Re-issue the thumbnail's input source from the current files so
        // a file that's actively being written (recorder live-capture)
        // gets re-scanned and the waveform grows on screen.
        void reloadCurrentWaveformFiles()
        {
            ++waveformReloads;
            overviewCache.clear();
            // setSource consults the AudioThumbnailCache by file hash and, if
            // it finds an entry that claims to be fully loaded, returns it
            // WITHOUT re-reading the file. If the file has since grown (or was
            // first read mid-capture as a fraction of a second), that stale
            // partial would shadow the finished file forever. Drop the cached
            // thumb first so the now-complete file is re-scanned at full length.
            if (currentFileL.existsAsFile()) applyThumbSource (thumbnailL, currentFileL, true);
            if (currentFileR.existsAsFile()) applyThumbSource (thumbnailR, currentFileR, true);
            for (auto& own : ownThumbs) applyThumbSource (*own.thumb, own.file, true);
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
        // prefillSilentPoints seeds the envelope with that many silent columns
        // before live capture begins -- used for a CONTINUE take so the new
        // audio draws AFTER the existing take (at the base position) instead of
        // from the left edge, matching the playhead.
        void setLiveRecording (bool on, juce::int64 prefillSilentPoints = 0)
        {
            if (on && ! liveRecording)
            {
                // Seed the envelope with `prefillSilentPoints` silent columns so
                // the new audio draws AFTER the existing take (continue) or AT the
                // punch point (punch) -- the silent run encodes the timeline
                // offset; drawRecEnvelope confines the whole thing to its share.
                recPeakL.reset (prefillSilentPoints);
                recPeakR.reset (prefillSilentPoints);
                liveHold = false;           // a new take supersedes any held envelope
            }
            liveRecording = on;
        }

        // Draw the existing take dim under the live capture, across the first
        // `existingFrac` of the lane (the take's share of the timeline). For a
        // CONTINUE that's [0, takeEnd); for a PUNCH it's the WHOLE take (the
        // capture is overlaid in the middle, not appended). The caller computes
        // existingFrac from the timeline so it's right in both cases.
        void drawContinueExisting (juce::Graphics& g, juce::Rectangle<int> area,
                                   juce::AudioThumbnail& thumb, bool wholeFile, int chan, float zoom,
                                   double existingFrac)
        {
            if (existingFrac <= 0.0 || thumb.getTotalLength() <= 0.0) return;
            auto existing = area.withWidth ((int) (area.getWidth() * juce::jlimit (0.0, 1.0, existingFrac)));
            if (existing.getWidth() <= 0) return;
            g.setColour (brand::accentPlay.withAlpha (zynforge::brand::alpha::muted));   // existing take = dim context
            drawThumbnailVisible (g, thumb, existing, 0.0, thumb.getTotalLength(), wholeFile ? -1 : chan, zoom);
        }

        // Take stopped: stop appending, but KEEP the envelope drawn as a
        // provisional waveform until the file thumbnail finishes scanning.
        // No-op for rows that captured nothing (empty envelope), so existing
        // audio on un-armed rows keeps showing its real thumbnail.
        void holdLiveEnvelopeUntilScanned()
        {
            // Only a row that captured holds: another keeps its waveform, not
            // the envelope left over from an earlier take.
            const bool captured = liveRecording;
            liveRecording = false;
            // Set unconditionally on captured rows: the post-stop re-scan
            // (refreshWaveformsOf) re-issues the source right after this, so
            // waveformsLoaded() goes false and paint() only releases the hold
            // once the FRESH scan completes. Checking it here would race that.
            liveHold = captured && ! recPeakL.empty();
        }

        // Append one column to the live envelope. Called once per UI tick
        // while recording; values are 0..1 input peaks. Caps the history
        // and max-pools down by 2 when it overflows so a long take can't
        // grow the vector without bound (the lane only has ~1 px per entry
        // to draw anyway).
        // Append one fine peak column drained from the recorder's live-wave
        // overview (TrackState::liveWaveDrain). No repaint here -- EditPage
        // repaints the row once per tick after draining a batch.
        void appendLivePeakL (float p)
        {
            if (! liveRecording) return;
            recPeakL.append (p);
        }
        void appendLivePeakR (float p)
        {
            if (! liveRecording || ! stereo) return;
            recPeakR.append (p);
        }

        // Daemon snapshots carry meter peaks rather than the local fine
        // FIFO. Fill the elapsed bins with that coarse peak so the waveform
        // and playhead still advance on the daemon's capture clock.
        void appendRemotePeakL (float peak, juce::int64 absoluteBin)
        {
            if (! liveRecording) return;
            if (absoluteBin - recPeakL.spanSamples() / TrackState::kLiveBinSamples > 8192)
                recPeakL.reset (absoluteBin - 1);
            while (recPeakL.spanSamples() / TrackState::kLiveBinSamples < absoluteBin)
                recPeakL.append (peak);
        }
        void appendRemotePeakR (float peak, juce::int64 absoluteBin)
        {
            if (! liveRecording || ! stereo) return;
            if (absoluteBin - recPeakR.spanSamples() / TrackState::kLiveBinSamples > 8192)
                recPeakR.reset (absoluteBin - 1);
            while (recPeakR.spanSamples() / TrackState::kLiveBinSamples < absoluteBin)
                recPeakR.append (peak);
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
            const auto headerBg = hovered ? brand::lift (brand::bgStrip, 0.06f)
                                           : brand::bgStrip;
            if (hovered) fillColour = brand::lift (fillColour, 0.06f);

            // Row bottom divider (full content width). The swatch column,
            // header panel, vertical divider and selection highlight
            // are NOT drawn here -- paintHeader() draws them LAST (and
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
            const auto waveColour = brand::lift (getStripColour(), 0.25f);
            const auto inner = wavePane.reduced (brand::space::xs, brand::space::sm);

            // All lane-content drawing runs inside this lambda. The Markers
            // lane early-`return`s once it has drawn -- by doing so from the
            // lambda (not paint()), it does not SKIP the pinned header painted
            // LAST below, which left lane content bled over the channel name
            // + I/O combos. paintHeader runs in every lane mode.
            auto drawLane = [&]()
            {

            // ── Waveform fill (Pro Tools-style: COLOURED clip bg + DARK wave) ──
            // The "alive", easy-on-the-eyes look is the clip filled with the
            // channel's COLOUR, and the waveform drawn DARK on top of it: loud
            // sections fill dark, quiet sections let the colour show through. This
            // is the INVERSE of a bright wave on a near-black lane (which glowed
            // like a noisy barcode). The dark wave is a deep shade of the same
            // channel hue, so colour identity carries. A thin LIGHTER rim along
            // the envelope (drawn first, full height; dark body inset 1px over it)
            // gives the contour definition PT has.
            // The DARK waveform is the BOLD, dominant element (like Pro Tools);
            // the coloured clip is the lighter backdrop. Derive the wave by
            // SINKing the channel colour directly (keeps its saturation -> a deep,
            // bold navy/hue), NOT by sinking the lightened bg (that came out muddy
            // + washed, so the bg looked more alive than the wave). The bg is the
            // channel colour LIFTed light. Solid wave (no rim) so it reads bold.
            const auto waveBg   = brand::lift (getStripColour(), 0.48f);  // light coloured clip backdrop
            const auto waveDark = brand::sink (getStripColour(), 0.68f);  // BOLD deep wave = the focal point
            auto drawWaveOutlined = [this, &g, waveBg, waveDark]
                (juce::AudioThumbnail& thumb, juce::Rectangle<int> area,
                 bool wholeFile, int chan, double t0, double t1, float zoom)
            {
                g.setColour (waveBg);
                g.fillRect (area);                      // light coloured clip backdrop
                g.setColour (waveDark);                 // solid BOLD dark waveform on top
                if (! (wholeFile && thumb.getNumChannels() != 1)
                    && drawOverviewWave (g, thumb, area, chan, t0, t1, zoom,
                                         waveDark, waveDark.withAlpha (brand::alpha::muted)))
                    return;
                drawThumbnailVisible (g, thumb, area, t0, t1, wholeFile ? -1 : chan, zoom);
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
                    const auto visible = inner.getIntersection (g.getClipBounds());
                    if (! visible.isEmpty())
                    {
                        const double firstSec = (visible.getX() - inner.getX()) / pxPerSec;
                        const double lastSec = (visible.getRight() - inner.getX()) / pxPerSec;
                        const auto minor = visibleTimelineTicks (minorSec, totalSec, firstSec, lastSec);
                        const auto major = visibleTimelineTicks (majorSec, totalSec, firstSec, lastSec);
                        // Minor ticks (very faint), then majors (a touch stronger).
                        g.setColour (brand::edge.withAlpha (zynforge::brand::alpha::ghost));
                        for (juce::int64 i = minor.first; i <= minor.last; ++i)
                            g.drawVerticalLine (secToX (i * minorSec),
                                                (float) inner.getY(), (float) inner.getBottom());
                        g.setColour (brand::lift (brand::edge, 0.18f).withAlpha (zynforge::brand::alpha::muted));
                        for (juce::int64 i = major.first; i <= major.last; ++i)
                            g.drawVerticalLine (secToX (i * majorSec),
                                                (float) inner.getY(), (float) inner.getBottom());
                    }
                }
            }

            // Waveform vertical scale: nearly HONEST levels, Pro Tools-style --
            // loud takes fill the lane, quiet takes read small and clean. Only a
            // GENTLE auto-gain (cap 1.6x) lifts a genuinely quiet-but-real take so
            // it's not invisible. The old 4x cap blew a quiet recording's noise
            // floor up into a fuzzy "noisy" band (a -37 dBFS RMS take × 4 made the
            // hiss between transients prominent); honest scaling keeps it a thin,
            // clean line. Below ~-48 dBFS is treated as silent (true level, flat
            // line). vZoom (V+/V- / Shift+wheel) then scales on top for detail.
            float vz = 1.0f;
            if (auto* page = findParentComponentOfClass<EditPage>())
                vz = page->getVerticalZoom();
            auto waveZoom = [vz] (const juce::AudioThumbnail& tn) -> float
            {
                if (tn.getTotalLength() <= 0.0) return vz;
                const float peak = tn.getApproximatePeak();
                const float fit  = peak > 0.004f
                                     ? juce::jlimit (1.0f, 1.5f, 0.9f / peak)
                                     : 1.0f;
                return fit * vz;
            };

            // Pro Tools-style: the waveform is ALWAYS the base layer,
            // even when the Markers lane is selected. The lane data is
            // drawn on top with a semi-transparent backdrop. Paint
            // the waveform here as the substrate; the lane branch
            // below then overlays its content. Skip the substrate when the
            // arrangement was emptied (all clips deleted) so the deleted
            // waveform doesn't linger behind the lane.
            // The substrate sits on the SAME timeline as the markers drawn over
            // it (laneTimelineSamples): each clip's file region at its timeline
            // span, like the clip blocks. Stretching the whole file across the
            // lane drew markers against the wrong audio on any track shorter than
            // the session, or with moved / trimmed / deleted clips.
            if (laneMode != LaneMode::Waveform && ! engine.isTrackArrangementEmpty (index))
            {
                const double srB = engine.getPlayer().getSampleRate() > 0.0
                                     ? engine.getPlayer().getSampleRate() : 48000.0;
                const auto backdrop = TimelineMapper::forLane (inner, laneTimelineSamples());
                auto drawSpan = [&] (juce::int64 timelineStart, juce::int64 fileStart, juce::int64 length,
                                     const LaneWave& left, const LaneWave& right)
                {
                    const int xL = backdrop.toX (timelineStart);
                    const int xR = backdrop.toX (timelineStart + length);
                    if (xR <= xL) return;
                    const juce::Rectangle<int> span (xL, inner.getY(), xR - xL, inner.getHeight());
                    const double t0 = (double) fileStart / srB;
                    const double t1 = (double) (fileStart + length) / srB;
                    g.setColour (waveColour.withAlpha (zynforge::brand::alpha::muted));
                    auto draw = [&] (const LaneWave& w, juce::Rectangle<int> area)
                    {
                        if (w.hasAudio())
                            drawThumbnailVisible (g, *w.thumb, area, t0, t1,
                                                  w.wholeFile ? -1 : w.channel, waveZoom (*w.thumb));
                    };
                    if (stereo)
                    {
                        const int laneH = span.getHeight() / 2;
                        draw (left,  span.withHeight (laneH));
                        draw (right, span.withTrimmedTop (laneH));
                    }
                    else
                    {
                        draw (left, span);
                    }
                };
                if (const auto* backdropClips = engine.tryClipsFor (index);
                    backdropClips != nullptr && ! backdropClips->empty())
                {
                    for (int i = 0; i < (int) backdropClips->size(); ++i)
                    {
                        const auto& c = (*backdropClips)[(size_t) i];
                        drawSpan (c.timelineStartSamples, c.fileStartSamples, c.fileLengthSamples,
                                  laneWaveFor (c, false), rightLaneWaveFor (i));
                    }
                }
                else
                {
                    const double fileSec = juce::jmax (thumbnailL.getTotalLength(),
                                                       stereo ? thumbnailR.getTotalLength() : 0.0);
                    drawSpan (0, 0, (juce::int64) (fileSec * srB), laneWaveFor ({}, false),
                              stereo ? laneWaveFor ({}, true) : LaneWave {});
                }
            }

            // The Markers lane, overlaid on top of the dimmed waveform base above.
            if (laneMode == LaneMode::Markers)
            {
                g.setColour (brand::edge);
                g.drawRect (inner, 1);

                // Centre line for reference.
                g.setColour (brand::lift (brand::edge, 0.2f).withAlpha (zynforge::brand::alpha::muted));
                g.drawHorizontalLine (inner.getCentreY(),
                                      (float) inner.getX(), (float) inner.getRight());

                // Real session markers, drawn at their actual sample
                // positions (mapped through the same timeline as the
                // waveform) with the marker name. Previously this lane
                // drew 6 fake evenly-spaced ticks + the literal word
                // "markers" -- it ignored the real marker list.
                const juce::int64 totalSamples = laneTimelineSamples();
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
                        g.setFont (brand::type::micro());
                        g.drawText (mk.name,
                                    juce::Rectangle<int> (x + 4, inner.getY() + 1, 90, 11),
                                    juce::Justification::topLeft, false);
                    }
                }
                // Playhead overlay still applies below.
                if (playheadX >= 0 && playheadX < wavePane.getWidth())
                {
                    g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (zynforge::brand::alpha::prominent));
                    g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                      0, 2, getHeight()));
                }
                return;
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
            // Release the post-stop hold the moment the real thumbnail is ready
            // -- from here on the file waveform owns the lane. Until then,
            // `showLive` keeps the just-captured envelope on screen so the lane
            // is never blank while the background scan runs.
            // Not while an async STOP still finalizes: the armed rows have no
            // file yet (the settle rescan attaches it), so "loaded" is vacuous.
            if (liveHold && waveformsLoaded() && ! engine.isCaptureFinalizing()) liveHold = false;
            const bool showLive = liveRecording || liveHold;
            // While CAPTURING (or holding the just-captured envelope), the live
            // forge-orange overlay OWNS the lane -- even on a take that already
            // has clip blocks. A CONTINUE records onto a take whose existing
            // audio is a seeded clip (so haveClipBlocks is true), and we must
            // still show the NEW audio building live instead of only the static
            // clips. The clip-block renderer below is suppressed while showLive
            // for the same reason, then takes over on stop once the file scans.
            // Live-capture placement on the timeline. The envelope covers
            // [0, recPos], including a silent prefix for continue/punch.
            // the lane covers [0, total] = max(existing take length, recPos).
            //   CONTINUE: total == recPos -> liveFrac 1 (envelope fills the lane,
            //             existing take is the [0, takeEnd) prefix).
            //   PUNCH:    total == fullTake > recPos -> confine the envelope to
            //             liveFrac so the drop-in sits AT the punch point instead
            //             of stretching to the lane's right edge (which looked
            //             like it recorded at the END), existing take fills [0,W].
            // The overview retains the actual count of input bins even after
            // downsampling, so long takes never drift ahead of the recorder.
            const juce::int64 liveRecSamples = recPeakL.spanSamples();
            const juce::int64 playerTotalS   = engine.getPlayer().getTotalLengthSamples();
            const juce::int64 timelineTotalS = engine.isRecording()
                ? juce::jmax (liveRecSamples, laneTimelineSamples())
                : juce::jmax (playerTotalS, liveRecSamples);
            const double liveFrac     = timelineTotalS > 0
                ? juce::jlimit (0.0, 1.0, (double) liveRecSamples / (double) timelineTotalS) : 1.0;
            const double existingFrac = timelineTotalS > 0
                ? juce::jlimit (0.0, 1.0, (double) playerTotalS   / (double) timelineTotalS) : 1.0;
            auto confineLive = [liveFrac] (juce::Rectangle<int> a)
            { return a.withWidth (juce::jmax (1, (int) (a.getWidth() * liveFrac))); };

            if (! haveClipBlocks || showLive)
            {
            if (stereo)
            {
                // L on top, R on bottom -- Pro-Tools-style stereo lanes.
                const int laneH = inner.getHeight() / 2;
                auto laneL = inner.withHeight (laneH);
                auto laneR = inner.withTrimmedTop (laneH);

                if (! showLive && thumbnailL.getTotalLength() > 0.0 && ! arrangementEmptied)
                    drawWaveOutlined (thumbnailL, laneL, ! stereoOneFile, 0,
                                      0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
                if (! showLive && thumbnailR.getTotalLength() > 0.0 && ! arrangementEmptied)
                    drawWaveOutlined (thumbnailR, laneR, ! stereoOneFile, 1,
                                      0.0, thumbnailR.getTotalLength(), waveZoom (thumbnailR));
                // Live capture envelope -- grows L→R during the take, glowing
                // forge-orange so the engineer sees the take is HOT / rolling.
                // After stop (liveHold) it dims to ember -- a provisional
                // waveform that sits there until the real thumbnail scans in.
                if (showLive)
                {
                    // Existing take dim under the capture; capture confined to its
                    // timeline fraction so a punch lands at the punch point.
                    drawContinueExisting (g, laneL, thumbnailL, ! stereoOneFile, 0, waveZoom (thumbnailL), existingFrac);
                    drawContinueExisting (g, laneR, thumbnailR, ! stereoOneFile, 1, waveZoom (thumbnailR), existingFrac);
                    // Live capture glows hot (recording NOW); the post-stop hold dims
                // to the channel colour so it matches the clean thumbnail that
                // replaces it (no flash of orange barcode on stop).
                g.setColour (liveRecording ? brand::meterHot : waveColour);
                    drawRecEnvelope (g, confineLive (laneL), recPeakL, vz);
                    drawRecEnvelope (g, confineLive (laneR), recPeakR, vz);
                }
                // Thin divider between lanes
                g.setColour (brand::edge);
                g.drawHorizontalLine (inner.getY() + laneH, (float) inner.getX(),
                                       (float) inner.getRight());
                // Empty lanes are left blank -- the EDIT view's PlaceholderView
                // owns the "no session" message, so no per-row hint here.
            }
            else if (showLive)
            {
                // While the take is rolling the live forge-orange envelope OWNS
                // the lane -- never the (possibly partial / cached) file
                // thumbnail, or the lane shows blocky garbage instead of the
                // growing capture. After stop it dims to ember and holds until
                // the real thumbnail finishes scanning (no blank gap).
                drawContinueExisting (g, inner, thumbnailL, false, monoFileChannel, waveZoom (thumbnailL), existingFrac);
                // Live capture glows hot (recording NOW); the post-stop hold dims
                // to the channel colour so it matches the clean thumbnail that
                // replaces it (no flash of orange barcode on stop).
                g.setColour (liveRecording ? brand::meterHot : waveColour);
                drawRecEnvelope (g, confineLive (inner), recPeakL, vz);
            }
            else if (thumbnailL.getTotalLength() > 0.0 && ! arrangementEmptied)
                drawWaveOutlined (thumbnailL, inner, false, monoFileChannel,
                                  0.0, thumbnailL.getTotalLength(), waveZoom (thumbnailL));
            }   // end "! haveClipBlocks" continuous-waveform fallback

            // ─── Clip region blocks (waveform mode) ─────────────────
            // After Edit ▸ Split / Separate, the track grows a clip list.
            // Paint each clip boundary as a 1 px vertical cut + a small
            // ⌐ marker at the top of the lane so the engineer can see
            // where the split landed. Skipped while showLive -- the live
            // capture envelope above owns the lane during a take (incl. a
            // CONTINUE), so the static clips don't paint over the growing
            // forge-orange waveform.
            if (auto* clips = showLive ? nullptr : engine.tryClipsFor (index))
            {
                const auto& player = engine.getPlayer();
                // The lanes' own span: during a take it is the recording's, and
                // a block on the file's length stretches against the ruler.
                const juce::int64 totalSamples = player.isLoaded() && player.getTotalLengthSamples() > 0
                    ? laneTimelineSamples() : 0;
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
                            const auto waveL = laneWaveFor (c, false);
                            const auto waveR = rightLaneWaveFor (clipIdx_);
                            if (waveArea.getHeight() > 2 && waveL.hasAudio())
                            {
                                juce::Graphics::ScopedSaveState ss (g);
                                g.reduceClipRegion (waveArea);
                                const double t0 = (double) c.fileStartSamples / srW;
                                const double t1 = (double) (c.fileStartSamples + c.fileLengthSamples) / srW;
                                // Scale the drawn waveform by the clip's gain so
                                // the shape visibly grows when boosted and
                                // shrinks when cut -- matching what you'll hear.
                                const float gz = juce::Decibels::decibelsToGain (c.gainDb, -60.0f);
                                // A muted clip draws a single dim, cool tint so it
                                // drops below the rest; a live clip gets the same
                                // solid-body + lighter-outline look as the main
                                // waveform (drawWaveOutlined) so it reads cleanly.
                                const auto mutedCol = brand::lift (clipTint, 0.20f).withAlpha (zynforge::brand::alpha::muted);
                                auto drawClipLane = [&] (const LaneWave& w, juce::Rectangle<int> area)
                                {
                                    const float z = waveZoom (*w.thumb) * gz;
                                    if (! c.muted)
                                    {
                                        drawWaveOutlined (*w.thumb, area, w.wholeFile, w.channel, t0, t1, z);
                                        return;
                                    }
                                    g.setColour (mutedCol);
                                    if (! ((! w.wholeFile || w.thumb->getNumChannels() == 1)
                                           && drawOverviewWave (g, *w.thumb, area, w.channel, t0, t1, z, mutedCol,
                                                                mutedCol.withAlpha (brand::alpha::subtle))))
                                        drawThumbnailVisible (g, *w.thumb, area, t0, t1,
                                                              w.wholeFile ? -1 : w.channel, z);
                                };
                                if (waveR.hasAudio())
                                {
                                    const int half = waveArea.getHeight() / 2;
                                    drawClipLane (waveL, waveArea.withHeight (half));
                                    drawClipLane (waveR, waveArea.withTrimmedTop (half));
                                }
                                else
                                {
                                    drawClipLane (waveL, waveArea);
                                }
                            }
                        }

                        // ─── Clip gain: a faint level line across the clip +
                        // a Pro Tools-style fader handle in the bottom-LEFT
                        // corner (the grab target). 0 dB = centre; ±12 dB maps
                        // to the inner band.
                        {
                            const bool active = (draggingClipMode == ClipDrag::ClipGain && draggingClipIdx == clipIdx_);
                            // NO gain line across the clip. Pro Tools-style: clip
                            // gain shows ONLY as the WAVEFORM getting bigger /
                            // smaller (waveZoom * gz, above) -- never a line. The
                            // corner fader handle below stays as the drag target +
                            // dB readout.
                            // Corner handle: a fader track + a triangular thumb
                            // whose height tracks the gain, then the dB readout
                            // on a dark pill so it stays legible over audio.
                            // Clean clip at rest: the gain handle + dB pill only
                            // appear while hovering the row, dragging the gain, or
                            // when gain is actually applied -- so an untouched clip
                            // is just the coloured waveform (Pro Tools-style).
                            auto hr = clipGainHandleRect (block, headH);
                            if (block.getWidth() > 22 && hr.getHeight() >= 12
                                && (hovered || active || std::abs (c.gainDb) > 0.05f))
                            {
                                const float g12 = juce::jlimit (-12.0f, 12.0f, c.gainDb);
                                const auto col = active ? brand::accentStatus
                                                        : brand::accentStatus.withAlpha (zynforge::brand::alpha::prominent);
                                const float cx = (float) hr.getX() + 10.0f;
                                // Fader track -- wider + clearer than the old hairline.
                                g.setColour (brand::bgDeep.withAlpha (zynforge::brand::alpha::scrim));
                                g.fillRoundedRectangle (juce::Rectangle<float> (cx - 6.0f, (float) hr.getY(), 12.0f, (float) hr.getHeight()), 2.0f);
                                g.setColour (col.withAlpha (active ? 0.95f : 0.55f));
                                g.drawVerticalLine ((int) cx, (float) hr.getY() + 1.0f, (float) hr.getBottom() - 1.0f);
                                // Machined cap thumb -- a proper little fader grip
                                // (steel body + edge + a coloured centre groove),
                                // big enough to SEE and grab. Rides up = boost,
                                // down = cut; value spelled out in the GAIN pill.
                                const float ty   = (float) hr.getCentreY()
                                                 - g12 / 12.0f * ((float) hr.getHeight() * 0.5f - 6.0f);
                                const float capW = 18.0f, capH = 12.0f;
                                const auto  cap  = juce::Rectangle<float> (cx - capW * 0.5f, ty - capH * 0.5f, capW, capH);
                                g.setColour (brand::shadow::elev2());
                                g.fillRoundedRectangle (cap.translated (0.0f, 1.5f), brand::radius::sm);
                                g.setColour (brand::controlBg);   // FLAT: solid cap (no gradient)
                                g.fillRoundedRectangle (cap, brand::radius::sm);
                                g.setColour (active ? brand::lift (col, 0.30f) : brand::edge);
                                g.drawRoundedRectangle (cap, brand::radius::sm, 1.0f);
                                g.setColour (col);
                                g.fillRoundedRectangle (cap.getCentreX() - 6.0f, cap.getCentreY() - 1.0f, 12.0f, 2.0f, 1.0f);
                                // "GAIN ±X.X dB" readout on a dark pill -- always
                                // shown (when there's room) so the control reads
                                // as a labelled gain, not a mystery green arrow.
                                if (block.getWidth() > 56)
                                {
                                    const juce::String sign = c.gainDb > 0.05f ? "+" : "";
                                    const bool      touched = active || std::abs (c.gainDb) > 0.05f;
                                    const juce::Rectangle<int> pill (hr.getX() + 13, hr.getCentreY() - 9,
                                                                     juce::jmin (104, hr.getRight() - (hr.getX() + 13)), 18);
                                    g.setColour (brand::bgDeep.withAlpha (zynforge::brand::alpha::bold));
                                    g.fillRoundedRectangle (pill.toFloat(), brand::radius::sm);
                                    g.setColour (touched ? brand::lift (col, 0.5f) : brand::textSecondary);
                                    g.setFont (brand::type::caption().boldened());
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
                                g.setColour (brand::onSignal (clipTint));
                                g.setFont (brand::type::micro());
                                g.drawText (clipDisplayName (c), headRect.reduced (5, 0),
                                            juce::Justification::centredLeft, false);
                            }
                        }
                        g.setColour (brand::lift (clipTint, 0.30f).withAlpha (zynforge::brand::alpha::prominent));
                        g.drawRect (block, 1);

                        // Selected-clip highlight -- the clip you're working
                        // "runs hot": a warm ember wash + a forge-orange rim, so
                        // the target of Delete / Duplicate / Nudge is
                        // unmistakable and on-brand.
                        if (selPage != nullptr && selPage->isClipSelected (index, c))
                        {
                            const juce::Rectangle<int> sel (xL_, inner2.getY(),
                                                            juce::jmax (1, xR_ - xL_),
                                                            inner2.getHeight());
                            g.setColour (brand::meterEmber.withAlpha (zynforge::brand::alpha::subtle));
                            g.fillRect (sel);
                            g.setColour (brand::meterHot.withAlpha (zynforge::brand::alpha::bold));
                            g.drawRect (sel, 2);
                        }
                        if (c.muted)
                        {
                            // Wash the muted clip's lane span with a soft
                            // mute scrim so it visibly drops below the rest.
                            g.setColour (brand::signalMute().withAlpha (zynforge::brand::alpha::soft));
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
                            g.setColour (brand::textPrimary.withAlpha (zynforge::brand::alpha::prominent));
                            g.fillRect (juce::Rectangle<int> (lx,     ly + 2, 6, 4));
                            g.drawRect (juce::Rectangle<int> (lx + 1, ly,     4, 4), 1);
                        }
                        // (The old yellow clip-start cut-flag was removed --
                        // the region block's border now shows where each clip
                        // begins, the way a real DAW does.)

                        // Fade-in diagonal -- from the bottom-left corner
                        // of the clip up to the top of (start + fadeIn). A
                        // piece of a split fade draws only its part of the
                        // ramp (levelY: 0 = bottom, 1 = top).
                        const auto levelY = [&inner2] (float level)
                        { return (float) inner2.getBottom() - level * (float) inner2.getHeight(); };
                        {
                            const int xL = sampleToX (c.timelineStartSamples);
                            const int xF = sampleToX (c.timelineStartSamples + c.fadeInSamples);
                            if (c.fadeInSamples > 0)
                            {
                                const auto [from, to] = fadeInLevels (c);
                                g.setColour (brand::accentStatus.withAlpha (zynforge::brand::alpha::ghost));
                                juce::Path p;
                                p.startNewSubPath ((float) xL, levelY (from));
                                p.lineTo ((float) xF, levelY (to));
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
                                const auto [from, to] = fadeOutLevels (c);
                                g.setColour (brand::accentStatus.withAlpha (zynforge::brand::alpha::ghost));
                                juce::Path p;
                                p.startNewSubPath ((float) xF, levelY (from));
                                p.lineTo ((float) xR, levelY (to));
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
                    for (auto [ai, bi] : timelineCrossfades (*clips))
                    {
                        const auto& a = (*clips)[(size_t) ai];
                        const auto& b = (*clips)[(size_t) bi];
                        const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                        const auto bStart = b.timelineStartSamples;
                        const int xL = sampleToX (bStart);
                        const int xR = sampleToX (aEnd);
                        if (xR - xL < 4) continue;
                        const auto band = juce::Rectangle<int> (xL, inner2.getY(),
                                                                xR - xL,
                                                                inner2.getHeight());
                        g.setColour (brand::accentStatus.withAlpha (zynforge::brand::alpha::faint));
                        g.fillRect (band);
                        g.setColour (brand::accentStatus.withAlpha (zynforge::brand::alpha::prominent));
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
                            g.setColour (brand::accentEdit.withAlpha (zynforge::brand::alpha::subtle));
                            g.fillRect (band);
                            g.setColour (brand::accentEdit.withAlpha (zynforge::brand::alpha::strong));
                            g.drawVerticalLine (band.getX(),     0.0f, (float) getHeight());
                            g.drawVerticalLine (band.getRight(), 0.0f, (float) getHeight());
                        }
                    }
                }
            }

            // (The swatch/header panel are drawn by paintHeader()
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
                    const juce::int64 totalSamples2 = laneTimelineSamples();
                    const double prop = juce::jlimit (0.0, 1.0,
                        (double) cursorSample / (double) totalSamples2);
                    const int cx = juce::roundToInt (prop * (wavePane.getWidth() - 8)) + 4;
                    g.setColour (brand::textPrimary.withAlpha (zynforge::brand::alpha::bold));
                    g.fillRect (juce::Rectangle<int> (headerW + cx, 0, 2, getHeight()));
                }
            }

            // ─── Playhead overlay
            if (playheadX >= 0 && playheadX < wavePane.getWidth())
            {
                g.setColour ((liveRecording ? brand::meterHot : brand::accentPlay).withAlpha (zynforge::brand::alpha::prominent));
                g.fillRect (juce::Rectangle<int> (headerW + playheadX, 0, 2, getHeight()));
            }

            };   // end drawLane
            drawLane();
            ageOverviewCache();

            // ─── Pinned header, drawn LAST so it floats over the waveform at
            // the scrolled-left edge instead of scrolling off with the row.
            // ALWAYS painted -- never skipped by a lane mode's early return --
            // so no lane mode can leave its content drawn over the header
            // (name + combos).
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

        // Pro Tools-style clip-gain handle at the bottom-LEFT corner of the
        // clip block (a small fader glyph + the dB readout). This is the grab
        // target; dragging it vertically rides the clip gain.
        static juce::Rectangle<int> clipGainHandleRect (juce::Rectangle<int> block, int headH) noexcept
        {
            auto body = block.withTrimmedTop (headH);
            // Slightly taller + wider grab target than before so the (now
            // larger) cap is easy to click and ride.
            const int h = juce::jmin (30, juce::jmax (16, body.getHeight() - 2));
            const int w = juce::jmin (90, juce::jmax (22, body.getWidth() - 2));
            return { body.getX() + 2, body.getBottom() - h - 2, w, h };
        }

        // Draws the swatch column, header panel, divider and selection highlight
        // at x = hx (the horizontal scroll offset). Called at
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
            g.setColour (brand::sink (fillColour, 0.40f));
            g.drawVerticalLine (swatchArea.getRight() - 1, 0.0f, (float) getHeight());

            g.setColour (headerBg);
            g.fillRect (header);

            // ── Heated Steel: orange spine + stamped number + orange seam ──
            // Mirrors the mixer strip header identity on the EDIT track rows.
            g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::wash));               // spine glow
            g.fillRect (juce::Rectangle<float> ((float) hx, 0.0f, 7.0f, (float) getHeight()));
            g.setColour (brand::brandOrange);                                 // spine
            g.fillRect (juce::Rectangle<float> ((float) hx, 0.0f, 3.0f, (float) getHeight()));
            {
                const auto num = juce::String (index + 1).paddedLeft ('0', 2);
                auto numBox = juce::Rectangle<int> (hx + swatchW + 7, 3, 24, 18);
                g.setFont (brand::type::mono (15.0f, true));
                g.setColour (brand::debossInk.withAlpha (zynforge::brand::alpha::prominent));            // engraved shadow
                g.drawText (num, numBox.translated (0, 1), juce::Justification::centredLeft, false);
                g.setColour (brand::debossFace);                             // raised steel face
                g.drawText (num, numBox, juce::Justification::centredLeft, false);
            }

            g.setColour (brand::edge);
            g.drawVerticalLine (hx + headerW - 1, 0.0f, (float) getHeight());
            g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::half));               // orange seam
            g.drawVerticalLine (hx + headerW - 2, 0.0f, (float) getHeight());

            if (auto* page = findParentComponentOfClass<EditPage>())
                if (page->isTrackSelected && page->isTrackSelected (index))
                {
                    g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::subtle));
                    g.fillRect (header);
                    g.setColour (brand::brandOrange);
                    g.fillRect (juce::Rectangle<int> (hx + swatchW, 0, 3, getHeight()));
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

            // -------------- LEFT (name + buttons) --------------
            constexpr int leftBlockW = 140;
            auto leftBlock = header.removeFromLeft (leftBlockW).reduced (brand::space::xs, brand::space::xs);

            // Name shifted right to clear the painted Heated Steel number stamp.
            nameLabel.setBounds (leftBlock.removeFromTop (18).withTrimmedLeft (26));
            leftBlock.removeFromTop (3);

            const int btnH = 22;
            // Four buttons across one row, Pro Tools-style:
            //   [ REC ][  I  ][  S  ][  M  ]
            auto row = leftBlock.removeFromTop (btnH);
            const int quarter = row.getWidth() / 4;
            armButton .setBounds (row.removeFromLeft (quarter).reduced (1));
            monButton .setBounds (row.removeFromLeft (quarter).reduced (1));
            soloButton.setBounds (row.removeFromLeft (quarter).reduced (1));
            muteButton.setBounds (row.reduced (1));
            leftBlock.removeFromTop (4);

            // The VIEW button (lane-mode picker) takes the bottom-left.
            viewButton.setBounds (leftBlock.removeFromTop (18));

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

        // Single source of truth for the lane's sample<->x timeline span.
        // When a session is loaded it's the take length; otherwise a fixed
        // default. paint() and every hit-test MUST use this same value.
        juce::int64 laneTimelineSamples() const
        {
            const auto& player = engine.getPlayer();
            if (engine.isRecording())
            {
                const double sr = engine.getDeviceSampleRate() > 0.0
                    ? engine.getDeviceSampleRate()
                    : (player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0);
                const auto recPos = engine.isCaptureWindowActive() && player.isPlaying()
                    ? player.getPositionSamples()
                    : engine.getRecordTimelineSamples();
                return recordingTimelineSpanSamples (recPos, player.getTotalLengthSamples(), sr);
            }
            if (player.isLoaded() && player.getTotalLengthSamples() > 0)
                return player.getTotalLengthSamples();
            const double sr = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
                : 48000.0;
            // Shared notional span, matching EditTimeRuler + the edit-cursor
            // click.
            return notionalEmptyLaneSamples (sr);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Mark this row as the active row. Reset by clicking on a
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
                // ONE span for the whole lane -- see laneTimelineSamples().
                const juce::int64 totalSamples = laneTimelineSamples();
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
                reorderArmed        = true;
                reorderActive       = false;
                reorderStartY       = e.y;
                reorderInsertBefore = -1;
                return;
            }

            // Pro Tools-style edit tool -- Selector and Fade
            // intercept the left-click before the normal clip-drag
            // hit-test. Trim / Grabber bias the clip-body branch below.
            // None (nothing selected) behaves identically to Smart.
            const auto activeTool = (toolsBar != nullptr)
                                      ? toolsBar->getTool()
                                      : EditToolsBar::Tool::None;

            if (laneMode == LaneMode::Waveform && inWavePane (e.x)
                && (activeTool == EditToolsBar::Tool::Selector
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

                    // Selector: park the playhead at the click sample and
                    // seed a loop region whose end follows mouseDrag.
                    const auto sx = xToSample (e.x);
                    player.setPositionSamples (sx);
                    player.clearLoopRegion();
                    // Tie the range to THIS track: select it (Shift to add
                    // more tracks). Option/Alt-drag selects the range across
                    // ALL tracks. The range shading then highlights the
                    // selected track(s) -- or all of them -- and range edits
                    // (Delete / Crop) apply to the same set.
                    if (auto* pg = findParentComponentOfClass<EditPage>())
                    {
                        // The range is the target now. Delete and Cmd+X take a
                        // selected clip first, so an old one would answer for it.
                        pg->clearSelectedClip();
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
                    draggingClipMode = ClipDrag::SelectRange;
                    repaint();
                    return;
                }
            }

            // Snapshot clip state for Cmd+Z before any clip-edit drag
            // (crossfade / trim / move / fade) can arm below. The commit
            // in mouseUp no-ops when nothing actually moved.
            // Only snapshot when there is actually something to grab: a click
            // on empty lane area armed no drag, so mouseUp never committed and
            // the snapshot was left dangling in EditPage::clipUndoBefore. It
            // was harmless only because every commit site re-snapshots first --
            // a trap for the next edit path added.
            if (laneMode == LaneMode::Waveform && inWavePane (e.x))
                if (const auto* cl = engine.tryClipsFor (index); cl != nullptr && ! cl->empty())
                    if (auto* page = findParentComponentOfClass<EditPage>())
                        page->beginClipEdit();

            // Crossfade midpoint drag wins over clip-edit drag --
            // the handle sits inside the overlap band so the engineer
            // expects clicking it to grab the crossfade balance
            // rather than starting a clip move on the underlying clip.
            if (laneMode == LaneMode::Waveform && inWavePane (e.x))
            {
                const auto xfade = hitTestCrossfadeHandle (e.getPosition());
                if (xfade.first >= 0)
                {
                    draggingXfadeAIdx = xfade.first;
                    draggingXfadeBIdx = xfade.second;
                    return;
                }
            }

            // Clip drag-edit. Only when we're on the waveform lane and
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
                        dragTimelineSamples = totalSamples;   // drag scale, fixed at the grab
                        for (int i = 0; i < (int) clips->size(); ++i)
                        {
                            const auto& c = (*clips)[(size_t) i];
                            const int xL = sampleToX (c.timelineStartSamples);
                            const int xR = sampleToX (c.timelineStartSamples + c.fileLengthSamples);
                            const int xFadeIn  = sampleToX (c.timelineStartSamples + c.fadeInSamples);
                            const int xFadeOut = sampleToX (c.timelineStartSamples
                                                            + c.fileLengthSamples
                                                            - c.fadeOutSamples);

                            // Only grab a fade handle where it's actually
                            // DRAWN -- i.e. a fade already exists on this edge,
                            // or the Fade tool is active. This mirrors the
                            // paint condition (c.fadeXxxSamples > 0 ||
                            // showFadeGrips); otherwise an invisible handle was
                            // grabbable in every tool, so a plain click near a
                            // clip's top edge silently began a fade drag.
                            const bool fadeToolActive = (activeTool == EditToolsBar::Tool::Fade);

                            // Fade-in handle -- top-edge band, near
                            // (start + fadeIn).
                            if ((c.fadeInSamples > 0 || fadeToolActive)
                                && e.y - laneTop < fadeHandleZone
                                && std::abs (e.x - xFadeIn) <= fadeHandleZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipMode = ClipDrag::FadeIn;
                                dragStartX          = e.x;
                                lastDragSamples     = 0;
                                dragStartFadeIn     = c.fadeInSamples;
                                dragStartFadeOut    = c.fadeOutSamples;
                                return;
                            }
                            // Fade-out handle.
                            if ((c.fadeOutSamples > 0 || fadeToolActive)
                                && e.y - laneTop < fadeHandleZone
                                && std::abs (e.x - xFadeOut) <= fadeHandleZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipMode = ClipDrag::FadeOut;
                                dragStartX          = e.x;
                                lastDragSamples     = 0;
                                dragStartFadeIn     = c.fadeInSamples;
                                dragStartFadeOut    = c.fadeOutSamples;
                                return;
                            }

                            // Clip-gain handle grab (Smart / Grabber): the
                            // bottom-left fader corner rides the clip's level.
                            // Takes priority over body Move; the fade handles
                            // above and (Smart) the 6 px trim edges still win.
                            // Only where paint DRAWS the handle (a block wider
                            // than 22 px): the hit rect is at least 22 px wide,
                            // so a hidden handle on a sliver of a clip stole
                            // clicks from the next clip -- and on a Micro row
                            // it covered the left trim edge and fade-in grip.
                            if (! c.locked
                                && (activeTool == EditToolsBar::Tool::Smart
                                 || activeTool == EditToolsBar::Tool::Grabber
                                 || activeTool == EditToolsBar::Tool::None))
                            {
                                const int gHeadH = juce::jmin (12, juce::jmax (0, inner.getHeight() - 6));
                                juce::Rectangle<int> gBlk (xL, inner.getY(),
                                                           juce::jmax (1, xR - xL), inner.getHeight());
                                const auto gHandle = clipGainHandleRect (gBlk, gHeadH);
                                const bool onTrimEdge = activeTool != EditToolsBar::Tool::Grabber
                                                     && (std::abs (e.x - xL) <= hitZone
                                                      || std::abs (e.x - xR) <= hitZone);
                                if (gBlk.getWidth() > 22 && gHandle.getHeight() >= 12
                                    && ! onTrimEdge && gHandle.contains (e.getPosition()))
                                {
                                    // Alt/Option-click resets clip gain to 0 dB.
                                    if (e.mods.isAltDown())
                                    {
                                        auto* page = findParentComponentOfClass<EditPage>();
                                        if (page != nullptr) page->beginClipEdit();
                                        for (int peer : editGroupPeers())
                                            if (const int pi = peerClipIndex (peer, i); pi >= 0)
                                                engine.setClipGainDb (peer, pi, 0.0f);
                                        if (page != nullptr) page->commitClipEdit ("Reset clip gain");
                                        repaint();
                                        return;
                                    }
                                    draggingClipIdx     = i;
                                    draggingClipMode = ClipDrag::ClipGain;
                                    dragStartGainDb     = c.gainDb;
                                    dragStartGainY      = e.y;
                                    if (auto* page = findParentComponentOfClass<EditPage>())
                                        page->beginClipEdit();
                                    return;
                                }
                            }

                            const bool inClipBody = (e.x >= xL && e.x <= xR);

                            // Grabber: every click inside the clip body
                            // = Move (no edge trim).
                            if (activeTool == EditToolsBar::Tool::Grabber)
                            {
                                if (inClipBody)
                                {
                                    draggingClipIdx     = i;
                                    draggingClipMode = ClipDrag::Move;
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
                                    draggingClipMode = leftHalf ? ClipDrag::TrimLeft : ClipDrag::TrimRight;
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
                                draggingClipMode = ClipDrag::TrimLeft;
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                            if (e.x >= xR - hitZone && e.x <= xR + hitZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipMode = ClipDrag::TrimRight;
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                            if (e.x > xL + hitZone && e.x < xR - hitZone)
                            {
                                draggingClipIdx     = i;
                                draggingClipMode = ClipDrag::Move;
                                dragStartX = e.x;
                                lastDragSamples = 0;
                                return;
                            }
                        }
                    }
                }
            }
        }

        const Clip* clipOn (int track, int clip) const
        {
            const auto* list = engine.tryClipsFor (track);
            return list != nullptr && clip >= 0 && clip < (int) list->size() ? &(*list)[(size_t) clip] : nullptr;
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Capture (or its finalization) locked the timeline mid-gesture --
            // a remote / LTC-chase / R-key start. Click-through stops NEW
            // gestures, not one already in flight, so refuse the rest of it.
            if (auto* page = findParentComponentOfClass<EditPage>();
                page != nullptr && page->isTimelineReadOnly())
                return;

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
                if (clips != nullptr && draggingXfadeAIdx < (int) clips->size()
                    && draggingXfadeBIdx >= 0 && draggingXfadeBIdx < (int) clips->size())
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
                        const auto& b = (*clips)[(size_t) draggingXfadeBIdx];
                        const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                        const auto bStart = b.timelineStartSamples;
                        // Guard the degenerate overlap. With an overlap of <= 2
                        // samples the bounds invert (lo > hi): juce::jlimit
                        // asserts in debug and returns the UPPER bound in
                        // release, which produced a NEGATIVE fade length.
                        if (aEnd - bStart < 2) return;
                        const juce::int64 lo = bStart + 1, hi = aEnd - 1;
                        const juce::int64 clamped = juce::jlimit (lo, hi, midSample);
                        const juce::int64 bFadeIn  = clamped - bStart;
                        const juce::int64 aFadeOut = aEnd - clamped;
                        // Preserve existing fadeIn / fadeOut on the
                        // OPPOSITE edge of each clip; only the
                        // crossfade-side fade is being adjusted.
                        // Broadcast to EDIT-group peers like every other clip
                        // edit -- the crossfade was the one drag that stayed
                        // local, so a grouped pair silently drifted apart.
                        for (int peer : editGroupPeers())
                        {
                            const int pa = peerClipIndex (peer, draggingXfadeAIdx);
                            const int pb = peerClipIndex (peer, draggingXfadeBIdx);
                            if (pa < 0 || pb < 0) continue;
                            if (const auto* out = clipOn (peer, pa))
                                engine.setClipFades (peer, pa, out->fadeInSamples, aFadeOut);
                            if (const auto* in = clipOn (peer, pb))
                                engine.setClipFades (peer, pb, bFadeIn, in->fadeOutSamples);
                        }
                        repaint();
                    }
                }
                return;
            }

            // Strip reorder via swatch drag. Moving the pointer only PREVIEWS
            // the drop (EditPage draws a line and autoscrolls); the move
            // itself commits once, in mouseUp. Each reorderTracks() is a full
            // on-disk transaction, and running one per row crossed used to
            // stop the player and detach the audio callback on every step.
            if (reorderArmed)
            {
                if (! reorderActive && std::abs (e.y - reorderStartY) > 8)
                {
                    // Refuse while playback rolls: the move renames take
                    // files the player has open. Say so once, up front.
                    if (engine.getPlayer().isPlaying())
                    {
                        juce::AlertWindow::showAsync (
                            juce::MessageBoxOptions()
                                .withIconType (juce::MessageBoxIconType::NoIcon)
                                .withTitle ("Strip reorder paused")
                                .withMessage ("Stop playback before reordering strips -- "
                                              "moving a strip renames take files the "
                                              "player has open.")
                                .withButton ("OK"),
                            nullptr);
                        reorderArmed = false;
                        return;
                    }
                    reorderActive = true;
                    beginDragAutoRepeat (50);   // keep autoscrolling while the pointer rests at an edge
                }
                if (reorderActive)
                    if (auto* page = findParentComponentOfClass<EditPage>())
                        reorderInsertBefore = page->updateReorderPreview (
                            page->getLocalPoint (this, e.getPosition()));
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

            // Selector drag -- a loop region anchored at the mouseDown
            // position follows the mouse.
            if (draggingClipMode == ClipDrag::SelectRange)
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
                    const juce::int64 a = juce::jmin (lastDragSamples, sx);
                    const juce::int64 b = juce::jmax (lastDragSamples, sx);
                    if (b > a) player.selectRange (a, b);
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
                if (dragPeers.empty()) dragPeers = peerClipTargets (draggingClipIdx);
                const auto& player = engine.getPlayer();
                // Map pixels at the scale the clip was GRABBED at. Dragging the
                // last clip right grows the session on every step, and re-reading
                // it here made the clip outrun the mouse (about W / (W - dx) x)
                // while the lanes rescaled under it.
                const juce::int64 totalSamples = dragTimelineSamples > 0 ? dragTimelineSamples
                                               : player.isLoaded() ? player.getTotalLengthSamples() : 0;
                const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);

                // Clip-gain handle drag (ClipDrag::ClipGain): relative ride from the grab
                // point -- drag up = louder. ~0.15 dB per pixel, full range.
                if (draggingClipMode == ClipDrag::ClipGain)
                {
                    const auto* clips = engine.tryClipsFor (index);
                    if (clips != nullptr && draggingClipIdx < (int) clips->size())
                    {
                        const float dB = juce::jlimit (-60.0f, 12.0f,
                            dragStartGainDb + (float) (dragStartGainY - e.y) * 0.15f);
                        for (auto [peer, pi] : dragPeers)
                            engine.setClipGainDb (peer, pi, dB);
                        repaint();
                    }
                    return;
                }

                if (totalSamples > 0 && inner.getWidth() > 0)
                {
                    const double samplesPerPx = (double) totalSamples / (double) inner.getWidth();
                    const juce::int64 wantSamples = (juce::int64) ((e.x - dragStartX) * samplesPerPx);

                    if (draggingClipMode == ClipDrag::FadeIn || draggingClipMode == ClipDrag::FadeOut)
                    {
                        // Fade handles: absolute targets, not incremental.
                        // Drag the fade-in apex right grows fadeIn;
                        // drag the fade-out apex left grows fadeOut
                        // (so 'further from the clip end' = more fade).
                        const auto* clips = engine.tryClipsFor (index);
                        if (clips == nullptr || draggingClipIdx >= (int) clips->size())
                            return;
                        const auto& c = (*clips)[(size_t) draggingClipIdx];
                        if (draggingClipMode == ClipDrag::FadeIn)
                        {
                            const juce::int64 newIn = juce::jlimit<juce::int64> (
                                0, c.fileLengthSamples - dragStartFadeOut,
                                dragStartFadeIn + wantSamples);
                            // Each peer keeps its own fade on the other edge.
                            for (auto [peer, pi] : dragPeers)
                                if (const auto* pc = clipOn (peer, pi))
                                    engine.setClipFades (peer, pi,
                                                         juce::jmin (newIn, pc->fileLengthSamples - pc->fadeOutSamples),
                                                         pc->fadeOutSamples);
                        }
                        else
                        {
                            // Drag right shrinks fade-out, drag left grows it.
                            const juce::int64 newOut = juce::jlimit<juce::int64> (
                                0, c.fileLengthSamples - dragStartFadeIn,
                                dragStartFadeOut - wantSamples);
                            for (auto [peer, pi] : dragPeers)
                                if (const auto* pc = clipOn (peer, pi))
                                    engine.setClipFades (peer, pi, pc->fadeInSamples,
                                                         juce::jmin (newOut, pc->fileLengthSamples - pc->fadeInSamples));
                        }
                        repaint();
                        return;
                    }

                    // Trim / Move: incremental.
                    juce::int64 stepSamples = wantSamples - lastDragSamples;
                    if (std::abs (stepSamples) >= 1)
                    {
                        const auto mode = draggingClipMode == ClipDrag::TrimLeft ? AudioEngine::ClipEdit::TrimLeft
                                       : draggingClipMode == ClipDrag::TrimRight ? AudioEngine::ClipEdit::TrimRight
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
                        {
                            // One bounded delta preserves alignment when any peer
                            // reaches zero, a source edge, or a locked clip.
                            for (auto [peer, pi] : dragPeers)
                                stepSamples = engine.constrainClipEditDelta (peer, pi, mode, stepSamples);
                            for (auto [peer, pi] : dragPeers) engine.editClip (peer, pi, mode, stepSamples);
                            lastDragSamples += stepSamples;
                        }
                        repaint();
                    }
                }
                return;
            }
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            // A gesture released after the timeline locked commits nothing.
            const auto* lockPage = findParentComponentOfClass<EditPage>();
            const bool timelineLocked = lockPage != nullptr && lockPage->isTimelineReadOnly();

            const bool wasDraggingClip    = (draggingClipIdx >= 0 || draggingXfadeAIdx >= 0);
            // A click that armed a clip-body grab but never dragged = a
            // selection click. Capture the clip + whether the mouse moved
            // before the resets below wipe draggingClipIdx.
            const int  clickedClipIdx = draggingClipIdx;
            const bool wasClick       = (e.getDistanceFromDragStart() < 5);
            dragging         = false;
            resizeAllDrag    = false;
            draggingClipIdx  = -1;
            dragPeers.clear();
            draggingClipMode = ClipDrag::TrimLeft;
            draggingXfadeAIdx = -1;
            draggingXfadeBIdx = -1;
            lastDragSamples  = 0;
            dragTimelineSamples = 0;
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
                // ONE reorderTracks() for the whole drag. It renames the take
                // files and reloads the player while PRESERVING clip
                // edits -- never follow it with the wiping loadSession(). The
                // page keeps the playhead, reports failure, and the host
                // rebuilds rows and rescans peaks asynchronously.
                if (auto* page = findParentComponentOfClass<EditPage>())
                {
                    if (reorderInsertBefore >= 0 && ! timelineLocked)
                        page->commitReorder (index, reorderInsertBefore);
                    else
                        page->clearReorderPreview();
                }
            }
            reorderArmed        = false;
            reorderActive       = false;
            reorderInsertBefore = -1;

            // Click (no drag) on a clip body selects it so Delete /
            // Duplicate / Nudge can act on it. Shift+click toggles the clip
            // in/out of a multi-selection. A plain click on empty lane area
            // clears the selection.
            if (wasClick && laneMode == LaneMode::Waveform && inWavePane (e.x))
                if (auto* page = findParentComponentOfClass<EditPage>())
                {
                    if (clickedClipIdx >= 0)
                    {
                        // Either way it is this clip + the matching clip on the
                        // R half and every EDIT-group peer.
                        const auto refs = peerClipTargets (clickedClipIdx);
                        if (e.mods.isShiftDown())
                            page->toggleSelectedClips (refs, { index, clickedClipIdx });
                        else
                            page->setSelectedClips (refs, { index, clickedClipIdx });
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

            juce::Component::SafePointer<TrackRow> self (this);
            auto picker = std::make_unique<StripColourPicker> (
                current,
                [self] (juce::Colour chosen)
                {
                    if (self == nullptr) return;
                    self->engine.setTrackColour (self->index, chosen);
                    // Mirror onto the R half -- the MIXER's colour callback
                    // already does this, so recolouring the same pair from EDIT
                    // left the two views' stored colours diverged (visible in
                    // PATCH and after an unlink).
                    if (self->stereo && self->index + 1 < self->engine.getRecorder().getNumTracks())
                        self->engine.setTrackColour (self->index + 1, chosen);
                    self->repaint();
                });

            // Anchor the call-out on the swatch's *pinned* on-screen position
            // (header floats at headerOriginX while the timeline is scrolled).
            const auto screenArea = localAreaToGlobal (
                juce::Rectangle<int> (headerOriginX(), 0, swatchW, getHeight()));
            juce::CallOutBox::launchAsynchronously (std::move (picker), screenArea, nullptr);
        }

        int  getHeaderWidth() const noexcept { return headerW; }
        int  getTrackIndex()  const noexcept { return index; }
        bool isStereoPair()   const noexcept { return stereo; }

        // ── Test seams ─────────────────────────────────────────────────────
        // Narrow read-only accessors so the suite can assert on this row's
        // visible state. Same idiom as MultitrackRecorder::drainPendingForTests
        // / AudioEngine::prepareForTests. Needed because the row's header
        // widgets are private and, until this class was made publicly
        // declared, none of this was reachable from a test at all.
        juce::String getNameTextForTests()   const { return nameLabel.getText(); }
        juce::String getInputRoutingTextForTests()  const { return inputCombo.getText(); }
        juce::String getOutputRoutingTextForTests() const { return outputCombo.getText(); }
        bool         isCondemnedForTests()   const noexcept { return ! rowValid; }
        double       getWaveformSecondsForTests() const { return thumbnailL.getTotalLength(); }
        int          getOverviewBuildsForTests() const noexcept { return overviewBuilds; }
        int          getWaveformReloadsForTests() const noexcept { return waveformReloads; }

        // Physical tracks this row's clip edits propagate to: both halves of a
        // stereo strip plus every member of its EDIT group, so split / trim /
        // move / fade / selection stay phase-coherent. An ungrouped mono track
        // returns just itself.
        std::vector<int> editGroupPeers() const { return clipedit::peerTracks (engine, index); }

        // The clip on `peer` that corresponds to THIS row's clip `srcIdx`. A
        // clip index is only meaningful inside one track's list; see
        // matchingClipIndex. Returns -1 when the peer has nothing there, in
        // which case the caller skips that peer rather than guessing.
        int peerClipIndex (int peer, int srcIdx) const
        { return clipedit::peerClipIndex (engine, index, srcIdx, peer); }

        // Every edit peer's OWN clip for this row's clip `srcIdx` -- the stereo
        // R half and each EDIT-group member -- as (track, clipIndex). Resolve
        // BEFORE mutating anything: a delete / duplicate on one track shifts
        // the source clip a later peerClipIndex() lookup would read.
        std::vector<std::pair<int, int>> peerClipTargets (int srcIdx) const
        { return clipedit::peerClips (engine, index, srcIdx); }

        // True once the background thumbnail scan has finished for this row's
        // file(s) (an empty/no-source thumbnail counts as loaded). Lets the
        // page persist WaveCache.wfm the moment the whole session is scanned.
        bool waveformsLoaded() const noexcept
        {
            return thumbnailL.isFullyLoaded() && (! stereo || thumbnailR.isFullyLoaded())
                && std::all_of (ownThumbs.begin(), ownThumbs.end(),
                                [] (const OwnThumb& own) { return own.thumb->isFullyLoaded(); });
        }

        Size getRowSize() const noexcept { return rowSize; }
        void setRowSize (Size s) noexcept { rowSize = s; }
        int  getCustomHeight() const noexcept { return customH; }
        void setCustomHeight (int px) noexcept { customH = juce::jlimit (kMinRowH, kMaxRowH, px); }
        int  getRowPixelHeight (int fitFallback) const noexcept
        {
            return pixelsFor (rowSize, fitFallback, customH);
        }

        // Apply one item of a clip's right-click menu to clip `clipIdx` of this
        // row. Split out of showFadeMenu's async callback so the suite can drive
        // the menu actions without a popup (test seam).
        void applyClipMenuChoice (int clipIdx, int chosen)
        {
            if (! timelineEditAllowed()) return;
            const auto* rowClips = engine.tryClipsFor (index);
            if (rowClips == nullptr || clipIdx < 0 || clipIdx >= (int) rowClips->size()) return;
            const auto& clip = (*rowClips)[(size_t) clipIdx];
            const auto  fIn      = clip.fadeInSamples;
            const auto  fOut     = clip.fadeOutSamples;
            const bool  muted    = clip.muted;
            const bool  locked   = clip.locked;
            const float gainDb   = clip.gainDb;
            const int   curveNow = clip.fadeCurve;
            auto msToSamples = [this] (int ms) { return clipMenuMsToSamples (ms); };
            juce::Component::SafePointer<TrackRow> self (this);
            // Every item applies to each edit peer's OWN clip (the stereo R
            // half + EDIT-group members), never the raw clipIdx: a peer with a
            // different arrangement holds another region at that index.
            const auto targets = peerClipTargets (clipIdx);

            // Route every clip-mutating menu action through the EDIT
            // page's clip-undo bridge so it joins Cmd+Z, consistent
            // with the drag-edit and Split/Crop paths. The before/after
            // guard in pushClipUndo drops no-ops (e.g. selecting the
            // gain the clip already has).
            auto withUndo = [self] (const juce::String& label, auto&& op)
            { self->clipUndoStep (label, op); };

            switch (chosen)
            {
                // Per-clip ops mirror to every edit peer -- a collapsed stereo
                // row carries its R partner (editGroupPeers appends it), so
                // mute / lock / duplicate / delete / consolidate hit BOTH
                // channels and the pair stays in sync. Without this they
                // edited only the LEFT half (the parallel clip lists drift).
                case 400: withUndo (muted  ? "Unmute clip" : "Mute clip",   [&]{ for (auto [p, pi] : targets) self->engine.setClipMuted  (p, pi, ! muted); });  return;
                case 401: withUndo (locked ? "Unlock clip" : "Lock clip",   [&]{ for (auto [p, pi] : targets) self->engine.setClipLocked (p, pi, ! locked); }); return;
                case 410:
                case 411:
                {
                    if (clipedit::anyLocked (self->engine, targets))
                    {
                        if (auto* page = self->findParentComponentOfClass<EditPage>(); page != nullptr && page->onStatus)
                            page->onStatus ("A peer of this clip is locked -- unlock it first");
                        return;
                    }
                    // Each peer's own index: a duplicate or delete on one track
                    // leaves the others' lists alone.
                    if (chosen == 410) withUndo ("Duplicate clip", [&]{ for (auto [p, pi] : targets) self->engine.duplicateClip (p, pi); });
                    else               withUndo ("Delete clip",    [&]{ for (auto [p, pi] : targets) self->engine.deleteClip    (p, pi); });
                    return;
                }
                case 413:
                {
                    auto* clips = self->engine.tryClipsFor (self->index);
                    if (clips == nullptr || clipIdx >= (int) clips->size()) return;
                    const auto start = (*clips)[(size_t) clipIdx].timelineStartSamples;
                    const auto end = start + (*clips)[(size_t) clipIdx].fileLengthSamples;
                    if (auto* page = self->findParentComponentOfClass<EditPage>())
                        if (page->consolidateRange)
                            page->consolidateRange (self->editGroupPeers(), start, end);
                    return;
                }
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
                        if (r != 1 || rowSafe == nullptr || ! rowSafe->timelineEditAllowed()) return;
                        const auto nm = own->getTextEditorContents ("name").trim();
                        if (nm.isEmpty()) return;
                        auto* page = rowSafe->findParentComponentOfClass<EditPage>();
                        if (page != nullptr) page->beginClipEdit();
                        for (auto [p, pi] : rowSafe->peerClipTargets (clipIdx))
                            rowSafe->engine.setClipName (p, pi, nm);
                        if (page != nullptr) page->commitClipEdit ("Rename clip");
                        rowSafe->repaint();
                    }));
                    return;
                }
                // Gain presets apply to every peer (stereo R partner +
                // edit-group members), like the gain-handle drag.
                case 501: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi, -12.0f); }); return;
                case 502: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi,  -6.0f); }); return;
                case 503: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi,  -3.0f); }); return;
                case 504: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi,   0.0f); }); return;
                case 505: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi,   3.0f); }); return;
                case 506: withUndo ("Clip gain", [&]{ for (auto [p, pi] : targets) self->engine.setClipGainDb (p, pi,   6.0f); }); return;
                case 320: withUndo ("Fade curve", [&]{ for (auto [p, pi] : targets) self->engine.setClipFadeCurve (p, pi, curveNow == 1 ? 0 : 1); }); return;
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
                        if (r != 1 || rowSafe == nullptr || ! rowSafe->timelineEditAllowed()) return;
                        const auto txt = own->getTextEditorContents ("dB");
                        const float dB = juce::jlimit (-60.0f, 12.0f, txt.getFloatValue());
                        auto* page = rowSafe->findParentComponentOfClass<EditPage>();
                        if (page != nullptr) page->beginClipEdit();
                        for (auto [p, pi] : rowSafe->peerClipTargets (clipIdx))
                            rowSafe->engine.setClipGainDb (p, pi, dB);
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
            // Each peer keeps its own fade on the edge this item doesn't set.
            const bool setsIn  = chosen < 200 || chosen == 300;
            const bool setsOut = chosen >= 200;
            withUndo ("Clip fades", [&]
            {
                for (auto [p, pi] : targets)
                    if (const auto* pc = self->engine.tryClipsFor (p); pc != nullptr && pi < (int) pc->size())
                        self->engine.setClipFades (p, pi,
                                                   setsIn  ? newIn  : (*pc)[(size_t) pi].fadeInSamples,
                                                   setsOut ? newOut : (*pc)[(size_t) pi].fadeOutSamples);
            });
        }

    private:
        bool hovered { false };
        // False once invalidate() has run -- the backing TrackState is being
        // destroyed, so updatePollState() must stop dereferencing it too.
        bool rowValid { true };

        juce::int64 clipMenuMsToSamples (int ms) const
        {
            const double sr = engine.getPlayer().getSampleRate() > 0.0
                                 ? engine.getPlayer().getSampleRate()
                                 : 48000.0;
            return (juce::int64) ((double) ms * sr / 1000.0);
        }

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

            auto msToSamples = [this] (int ms) { return clipMenuMsToSamples (ms); };

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
            menu.addSeparator();
            menu.addSubMenu ("Clip gain",  gain);
            menu.addSubMenu ("Fade in",    fadeIn,  ! locked);
            menu.addSubMenu ("Fade out",   fadeOut, ! locked);
            menu.addItem (320, "Equal-power fades", ! locked, curveNow == 1);
            menu.addItem (300, "Clear both fades", ! locked);

            juce::Component::SafePointer<TrackRow> self (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                  .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                [self, clipIdx] (int chosen)
            {
                if (self == nullptr) return;
                self->menuOpen = false;
                if (chosen == 0) return;
                self->applyClipMenuChoice (clipIdx, chosen);
            });
        }

        // Returns the (outgoing, incoming) clip indices of the crossfade
        // whose midpoint handle is under the cursor, or { -1, -1 }. Mirrors
        // the paint code's overlap detection. 6 px tolerance around
        // the handle.
        std::pair<int, int> hitTestCrossfadeHandle (juce::Point<int> pos) const
        {
            const auto* clips = engine.tryClipsFor (index);
            if (clips == nullptr || clips->size() < 2) return { -1, -1 };
            const auto& player = engine.getPlayer();
            const juce::int64 totalSamples = player.isLoaded()
                ? player.getTotalLengthSamples() : 0;
            if (totalSamples <= 0) return { -1, -1 };
            const auto inner = getLocalBounds().withTrimmedLeft (headerW).reduced (brand::space::xs, brand::space::sm);
            auto sampleToX = [&] (juce::int64 sp) -> int
            { return TimelineMapper::forLane (inner, totalSamples).toXFloor (sp); };
            for (auto [ai, bi] : timelineCrossfades (*clips))
            {
                const auto& a = (*clips)[(size_t) ai];
                const auto& b = (*clips)[(size_t) bi];
                const auto aEnd   = a.timelineStartSamples + a.fileLengthSamples;
                const auto bStart = b.timelineStartSamples;
                const int xL = sampleToX (bStart);
                const int xR = sampleToX (aEnd);
                if (xR - xL < 4) continue;
                const int xMid = (xL + xR) / 2;
                const int yMid = inner.getY() + inner.getHeight() / 2;
                if (std::abs (pos.x - xMid) <= 6 && std::abs (pos.y - yMid) <= 6)
                    return { ai, bi };
            }
            return { -1, -1 };
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
            });
        }

        // One Cmd+Z step around an arrangement edit (no-op edits record none).
        template <typename Op>
        void clipUndoStep (const juce::String& label, Op&& op)
        {
            auto* page = findParentComponentOfClass<EditPage>();
            if (page != nullptr) page->beginClipEdit();
            op();
            if (page != nullptr) page->commitClipEdit (label);
            repaint();
        }

        // Asked again when a menu or dialog that was already open finally
        // acts: the lock stops new gestures, not one a take caught open.
        bool timelineEditAllowed() const
        {
            if (auto* page = findParentComponentOfClass<EditPage>())
                return page->canEditTimeline();
            return ! engine.isArrangementFrozen();
        }

    public:
        // What this row draws in the lane area: its own VIEW choice.
        enum class LaneMode { Waveform, Markers };
        LaneMode laneMode { LaneMode::Waveform };

    private:
        juce::Colour getStripColour() const
        {
            auto& s = engine.getRecorder().getTrack (index);
            const auto argb = s.colourARGB.load();
            if (argb != 0) return juce::Colour ((juce::uint32) argb);
            return brand::stripColour (index);
        }

        // Draw the live capture envelope across `area`: one mirrored
        // min/max bar per pixel column, max-pooled from the peak history,
        // in the record colour. `vz` is the same vertical zoom the file
        // thumbnails use so the live take and the post-stop waveform sit
        // at the same scale.
        static void drawRecEnvelope (juce::Graphics& g,
                                     juce::Rectangle<int> area,
                                     const LivePeakHistory& peaks,
                                     float vz)
        {
            const int w = area.getWidth();
            const juce::int64 n = peaks.binCount();
            if (w <= 0 || n <= 0) return;
            const auto visible = area.getIntersection (g.getClipBounds());
            if (visible.isEmpty()) return;
            const float midY = area.getCentreY();
            const float halfH = area.getHeight() * 0.5f;
            for (int drawX = visible.getX(); drawX < visible.getRight(); ++drawX)
            {
                const int x = drawX - area.getX();
                // Map by actual recorder bins, not the shrinking overview's
                // point count. This lets the recent fine-detail ring draw the
                // live edge sharply even after hours of overview compaction.
                const auto a = (juce::int64) x * n / w;
                const auto b = juce::jmax (a + 1, (juce::int64) (x + 1) * n / w);
                const float pk = peaks.maxPeakInBinRange (a, b);
                const float h = juce::jlimit (0.0f, halfH, pk * vz * halfH);
                if (h <= 0.0f) continue;
                g.drawVerticalLine (drawX, midY - h, midY + h);
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
        juce::AudioFormatManager& formats;       // for building multi-part thumbnail readers
        juce::AudioThumbnailCache& thumbCache;   // shared; used to drop stale thumbs
        juce::AudioThumbnail      thumbnailL;
        juce::AudioThumbnail      thumbnailR;
        struct CachedOverview
        {
            juce::AudioThumbnail* thumbnail { nullptr };
            int channel { 0 }, width { 0 };
            double start { 0.0 }, end { 0.0 };
            WaveformOverview levels;
            juce::uint32 lastPaint { 0 };
        };
        // Every lane of every clip on screen has an entry, so the cache holds
        // what the last two paints drew: a fixed size evicts its own entries on
        // each repaint once a row shows more clips than it has slots. Scrolling
        // and zooming age the rest out, so it stays bounded by the screen.
        juce::uint32 overviewPaint  { 0 };
        int          overviewBuilds { 0 };
        void ageOverviewCache()
        {
            const auto current = overviewPaint++;
            overviewCache.erase (std::remove_if (overviewCache.begin(), overviewCache.end(),
                                                 [current] (const CachedOverview& entry)
                                                 { return entry.lastPaint + 1 < current; }),
                                 overviewCache.end());
        }
        std::vector<CachedOverview> overviewCache;
        struct OwnThumb { juce::File file; std::unique_ptr<juce::AudioThumbnail> thumb; };
        std::vector<OwnThumb>     ownThumbs;      // files clips play instead of this strip's take
        // A mono row draws one channel of its file: the left of a stereo file
        // it kept, or the right of the file it shared before an unlink.
        int                       monoFileChannel { 0 };
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
        int                       waveformReloads       { 0 };
        // Live capture envelope fed by recorder peak bins on the UI thread.
        // It keeps a whole-take overview plus a fine recent window without
        // reading growing audio files. recPeakR carries a stereo partner.
        LivePeakHistory           recPeakL { TrackState::kLiveBinSamples };
        LivePeakHistory           recPeakR { TrackState::kLiveBinSamples };
        bool                      liveRecording         { false };
        // Provisional hand-off: when a take stops, keep the just-built live
        // envelope on screen as a coarse waveform until the real file
        // thumbnail finishes its background scan -- so the lane is never blank
        // during the post-stop scan. Released in paint() the moment the
        // thumbnail isFullyLoaded(). This is what makes the waveform appear
        // INSTANTLY on stop instead of "building" for a beat.
        bool                      liveHold              { false };
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
    };
}
