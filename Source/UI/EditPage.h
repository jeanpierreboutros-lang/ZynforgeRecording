#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"
#include "PlaceholderView.h"

#include <memory>
#include <functional>
#include <vector>

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

        // Selected clip (clicked with the Smart / Move tool). Delete /
        // Duplicate / Nudge in the EDIT view act on it. -1 = none. The
        // index is into the track's active clip list, so structural edits
        // (split / clear / reload) clear the selection to stay valid.
        int  getSelectedClipTrack() const noexcept { return selectedClipTrack; }
        int  getSelectedClipIndex() const noexcept { return selectedClipIndex; }
        void setSelectedClip (int track, int clipIdx) noexcept
        {
            selectedClipTrack = track;
            selectedClipIndex = clipIdx;
            repaint();
        }
        void clearSelectedClip() noexcept
        {
            if (selectedClipTrack < 0 && selectedClipIndex < 0) return;
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

        AudioEngine&                       engine;
        juce::AudioFormatManager           formatManager;
        juce::AudioThumbnailCache          thumbnailCache { 256 };
        juce::Viewport                     viewport;
        std::unique_ptr<TrackList>         list;
        PlaceholderView                    placeholder;

        int        lastTrackCount  { -1 };
        bool       lastLoaded      { false };
        bool       lastRecording   { false };
        bool       waveCacheSaved  { false };   // WaveCache.wfm written for this session yet?
        juce::File lastSessionDir;
        float      zoom            { 1.0f };
        float      vZoom           { 1.0f };
        juce::var  clipUndoBefore;   // playlist snapshot taken at clip-drag start
        int        activeRowTrackIndex { -1 };
        int        selectedClipTrack   { -1 };
        int        selectedClipIndex   { -1 };
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
