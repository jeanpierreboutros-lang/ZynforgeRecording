#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "PlaceholderView.h"
#include "TimelineMinimap.h"

#include <memory>
#include <functional>
#include <vector>
#include <set>
#include <utility>

namespace zynforge
{
    // EDIT view -- one row per channel with the strip header (colour, name,
    // REC / MUTE / SOLO) on the left and the recorded WAV's waveform drawn
    // on the right via juce::AudioThumbnail. A vertical playhead spans all
    // rows and tracks the SessionPlayer position. State is shared with the
    // mixer view (TrackState), so mute / solo / rename changes propagate
    // both ways.
    class AutomationToolbar;
    class EditToolsBar;
    class EditTimeRuler;
    class EditPage final : public juce::Component, private juce::Timer
    {
    public:
        explicit EditPage (AudioEngine& engine);
        ~EditPage() override;

        // The EDIT rows query this toolbar on every mouse event to know
        // which tool (Select / Add / Delete) and which parameter
        // (Volume / Pan / Mute) the engineer has chosen.
        void setAutomationToolbar (AutomationToolbar* t);

        // Pro Tools-style edit-mode toolbar (Smart / Selector / Trim /
        // Grabber / Fade / Scrubber). EditPage owns it and lays it out
        // along the top edge; TrackRow consults it on every left-click
        // to bias the hit-test.
        EditToolsBar* getEditToolsBar() noexcept { return toolsBar.get(); }

        // Owned automation toolbar (mirrors getEditToolsBar). EditPage
        // creates it; the host re-parents it for layout and wires its
        // callbacks. Lives here so the EDIT view owns its own chrome.
        AutomationToolbar* getAutomationToolbar() noexcept { return autoToolbar.get(); }

        // Horizontal zoom -- content widens past the viewport so the
        // engineer can navigate a 90-min show. 1.0 = fit, 16.0 = 16×.
        void  setZoom (float z);
        float getZoom() const noexcept { return zoom; }

        // Vertical (amplitude) zoom for the waveforms. 1.0 = true level
        // (peak 0 dBFS fills the lane). Lets the engineer pull up quiet
        // tracks without normalising away the dynamics. Read by each
        // TrackRow when it draws its thumbnail.
        void  setVerticalZoom (float z);
        float getVerticalZoom() const noexcept { return vZoom; }

        // Wheel-zoom helpers, called from the row list. Horizontal zooms
        // around the viewport centre (keeps the visible time stable);
        // vertical just scales amplitude. delta > 0 zooms in.
        void  wheelZoomHorizontal (float delta);
        void  wheelZoomVertical   (float delta);

        // Fires whenever zoom changes (toolbar, mouse wheel, Memory
        // Location recall, etc.). Host uses this to autosave the per-
        // session UI layout into .zfproj.
        std::function<void (float)> onZoomChanged;

        // Channel selection, shared with the MIXER. A header click on a
        // track row calls onRowSelect(physicalTrackIndex, additive) so the
        // host can fold it into the same selection set the MIXER uses
        // (drives Option+R bulk arm). isTrackSelected lets a row draw its
        // selection highlight by asking the host whether it's selected.
        std::function<void (int /*physTrack*/, bool /*additive*/)> onRowSelect;
        std::function<bool (int /*physTrack*/)>                    isTrackSelected;
        // True when NO strips are selected -- lets a row decide whether a
        // global range (set via , / .) should shade every track, vs a
        // Selector range that only shades the selected track(s).
        std::function<bool ()>                                     isStripSelectionEmpty;
        // Make the Selector range span every track (Option/Alt-drag). The host
        // clears the strip selection so the range is global (all tracks shade,
        // range edits apply to all).
        std::function<void ()>                                     onRangeSelectAllTracks;

        // Channel removal from the EDIT view. A right-click on the row
        // header (or Delete on a selected channel) calls onRowDelete with
        // the physical track index; the host selects that strip, confirms,
        // and removes it. Recorded files stay on disk.
        std::function<void (int /*physTrack*/)>                    onRowDelete;

        // Clip-edit undo bridge. A TrackRow calls beginClipEdit() when a
        // clip drag (trim / move / fade) starts and commitClipEdit() when
        // it ends; the host turns the before/after clip state into a
        // Cmd+Z step (no-op when nothing actually changed). Wired by
        // MainComponent to engine.playlistsToJson + pushClipUndo.
        std::function<juce::var ()>                                 captureClipsForUndo;
        std::function<void (const juce::var&, const juce::String&)> commitClipUndo;
        void beginClipEdit()  { if (captureClipsForUndo) clipUndoBefore = captureClipsForUndo(); }
        void commitClipEdit (const juce::String& label)
        {
            if (commitClipUndo && ! clipUndoBefore.isVoid())
                commitClipUndo (clipUndoBefore, label);
            clipUndoBefore = juce::var();
        }

        // Memory-Location recall hooks. setLogicalRowsVisible takes a
        // list of logical strip indices to show (empty = show all).
        // scrollToSample centres the horizontal viewport on a sample
        // position. Both no-op when the row list / session isn't
        // ready yet.
        void  setLogicalRowsVisible (const std::vector<int>& rows);
        void  scrollToSample (juce::int64 sample);
        // Zoom + scroll so the sample range [a, b) fills the viewport (Pro
        // Tools "Zoom to Selection"). No-op until a session is loaded.
        void  zoomToSamples (juce::int64 a, juce::int64 b);

        // Active row tracking for Tab-to-Transient. TrackRow's mouse-
        // down handler sets the row index that was last clicked; the
        // host (MainComponent) reads it to restrict Tab navigation
        // to that one row's onsets. -1 = no active row, fall back to
        // pooled (cross-track) search.
        int  getActiveRowTrackIndex() const noexcept { return activeRowTrackIndex; }
        void setActiveRowTrackIndex (int trackIdx) noexcept
        {
            if (trackIdx != activeRowTrackIndex)
            {
                activeRowTrackIndex = trackIdx;
                focusedPointIdx     = -1;
            }
        }

        // Focused automation point index (within the active row's
        // active lane). -1 = no focus. Set by arrow-key navigation.
        // Paint shows the focused point with a brighter highlight
        // ring so the engineer sees where Up/Down/Delete will act.
        int  getFocusedPointIdx() const noexcept { return focusedPointIdx; }
        void setFocusedPointIdx (int idx) noexcept { focusedPointIdx = idx; repaint(); }

        // Selected clip(s) (clicked with the Smart / Move tool). Delete /
        // Nudge act on the whole set; Duplicate / Copy / Paste act on the
        // primary (most-recently-clicked). Indices are into each track's
        // active clip list, so structural edits clear the selection.
        using ClipRef = std::pair<int, int>;   // (track, clipIndex)
        const std::set<ClipRef>& getSelectedClips() const noexcept { return selClips; }
        bool isClipSelected (int track, int clipIdx) const noexcept
        { return selClips.count ({ track, clipIdx }) > 0; }
        int  getSelectedClipTrack() const noexcept { return selectedClipTrack; }
        int  getSelectedClipIndex() const noexcept { return selectedClipIndex; }
        // Replace the selection with this one clip (plain click).
        void setSelectedClip (int track, int clipIdx) noexcept
        {
            selClips.clear();
            selClips.insert ({ track, clipIdx });
            selectedClipTrack = track;
            selectedClipIndex = clipIdx;
            repaint();
        }
        // Select several clips at once (used by edit-group expansion: one
        // click on a grouped clip selects the matching clip on every group
        // peer). `primary` is the Copy/Nudge anchor.
        void setSelectedClips (const std::vector<ClipRef>& refs, ClipRef primary) noexcept
        {
            selClips.clear();
            for (auto& r : refs) selClips.insert (r);
            selectedClipTrack = primary.first;
            selectedClipIndex = primary.second;
            repaint();
        }
        // Toggle a clip in/out of the selection (Shift+click).
        void toggleSelectedClip (int track, int clipIdx) noexcept
        {
            const ClipRef r { track, clipIdx };
            if (selClips.erase (r) == 0)
            {
                selClips.insert (r);
                selectedClipTrack = track;     // becomes the new primary
                selectedClipIndex = clipIdx;
            }
            else if (selClips.empty())
            {
                selectedClipTrack = selectedClipIndex = -1;
            }
            repaint();
        }
        void clearSelectedClip() noexcept
        {
            if (selClips.empty() && selectedClipTrack < 0) return;
            selClips.clear();
            selectedClipTrack = -1;
            selectedClipIndex = -1;
            repaint();
        }

        // Optional hook into MainComponent's UndoManager. When set,
        // every automation-point add / remove / drag goes through
        // this wrapper so Cmd+Z reverts the lane to its prior state.
        // When unset, edits go direct -- still functional, just no
        // undo. setAutomationEditWrapper also propagates the function
        // down into the TrackList -> TrackRow chain so newly-created
        // rows see it on next rebuild.
        using AutoEditWrapper = std::function<void (const juce::String&,
                                                    std::function<void()>)>;
        void setAutomationEditWrapper (AutoEditWrapper fn);
        AutoEditWrapper automationEditWrapper;

        // Begin / end an automation drag transaction. The host
        // (MainComponent) snapshots automation lanes at begin and
        // again at end, pushing a single AutomationSnapshotAction
        // so the whole drag is one undo step instead of N.
        std::function<void()>                       automationDragBegin;
        std::function<void (const juce::String&)>   automationDragEnd;

        // Force every row's lane content to follow the toolbar's
        // Param choice. Called whenever the toolbar's onParamChanged
        // fires.
        void applyToolbarParamToAllRows();

        // True when MainComponent has dropped a metronome track; the
        // EDIT rows draw a beat-overlay on every other row when on.
        void setClickTrackPresent (bool present, int clickTrackIdx);

        // Re-scan the session dir, rebuild row list, and re-issue
        // thumbnail-load requests for each track. Safe to call repeatedly.
        void refresh();
        void repaintLanes();        // force every track row to repaint (e.g. after a cue recall changes automation)
        void updatePlaceholder();   // show/hide the empty-state overlay by channel count

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        class TrackRow;
        class TrackList;

        void timerCallback() override;

        // Viewport that reports scroll changes so the EDIT rows can re-pin
        // their header column to the left edge when the timeline scrolls
        // horizontally (otherwise the header + meter scroll off-screen).
        struct ScrollViewport final : public juce::Viewport
        {
            std::function<void()> onScroll;
            void visibleAreaChanged (const juce::Rectangle<int>&) override
            {
                if (onScroll) onScroll();
            }
        };

        AudioEngine&                       engine;
        juce::AudioFormatManager           formatManager;
        // Waveform scanning is sharded across N AudioThumbnailCaches, each with
        // its OWN TimeSliceThread, so an un-cached session's WAVs scan in
        // PARALLEL (N cores) instead of serially on one thread -- the dominant
        // cost of first-opening a freshly-imported multitrack session. A track
        // is assigned to a shard by its physical index (Track_NN), so the
        // assignment is stable across reopen and WaveCache hits land. Fixed N
        // (not CPU-derived) keeps the on-disk cache layout portable between
        // machines. WaveCache.wfm stores one length-prefixed section per shard.
        static constexpr int               kNumScanShards = 4;
        std::vector<std::unique_ptr<juce::AudioThumbnailCache>> thumbnailCaches;
        ScrollViewport                     viewport;
        std::unique_ptr<TrackList>         list;
        PlaceholderView                    placeholder;
        TimelineMinimap                    minimap;   // overview navigator (zoomed)

        int        lastTrackCount  { -1 };
        int        lastTrackGen    { -1 };   // rebuild on a stereo link/unlink (count unchanged, generation bumps)
        bool       lastLoaded      { false };
        bool       lastRecording   { false };
        bool       waveCacheSaved  { false };   // WaveCache.wfm written for this session yet?
        juce::File lastSessionDir;
        float      zoom            { 1.0f };
        float      vZoom           { 1.0f };
        juce::var  clipUndoBefore;   // playlist snapshot taken at clip-drag start
        int        activeRowTrackIndex { -1 };
        int        selectedClipTrack   { -1 };   // primary clip (Copy / Nudge anchor)
        int        selectedClipIndex   { -1 };
        std::set<std::pair<int, int>> selClips;  // multi-selection (track, clipIdx)
        int        focusedPointIdx     { -1 };
        AutomationToolbar*             toolbar  { nullptr };
        std::unique_ptr<EditToolsBar>  toolsBar;
        std::unique_ptr<AutomationToolbar> autoToolbar;   // owned; host re-parents for layout
        std::unique_ptr<EditTimeRuler> ruler;

        // DAW-style edge zoom clusters overlaid on the view: a vertical
        // (amplitude) +/- pair stacked at the right edge, and a horizontal
        // (timeline) +/- pair at the bottom-right. Wired to setVerticalZoom
        // / setZoom.
        // Labelled by axis so it's obvious which is amplitude (V) vs
        // timeline (H): the engineer asked for the zoom to "define what it
        // does". V↑/V↓ change waveform height; H+/H− change time scale.
        juce::TextButton zoomVIn  { "V+" }, zoomVOut { "V-" };
        juce::TextButton zoomHIn  { "H+" }, zoomHOut { "H-" };
        bool clickPresent { false };
        int  clickTrackIdx { -1 };

        // WaveCache.wfm persistence: writes the AudioThumbnailCache
        // contents to disk so the EDIT view re-renders waveforms
        // instantly on session reopen instead of re-scanning every
        // Track_NN.wav. Pro Tools-style.
        void loadCacheFromSession (const juce::File& sessionDir);
        void saveCacheToSession   (const juce::File& sessionDir);
        juce::Time lastCacheSaveTime;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditPage)
    };
}
