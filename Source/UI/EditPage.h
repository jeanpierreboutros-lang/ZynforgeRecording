#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"

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

        // Horizontal zoom -- content widens past the viewport so the
        // engineer can navigate a 90-min show. 1.0 = fit, 16.0 = 16×.
        void  setZoom (float z);
        float getZoom() const noexcept { return zoom; }

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
        void setActiveRowTrackIndex (int trackIdx) noexcept { activeRowTrackIndex = trackIdx; }

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
        juce::Label                        emptyLabel;

        int        lastTrackCount  { -1 };
        bool       lastLoaded      { false };
        bool       lastRecording   { false };
        juce::File lastSessionDir;
        float      zoom            { 1.0f };
        int        activeRowTrackIndex { -1 };
        AutomationToolbar*             toolbar  { nullptr };
        std::unique_ptr<EditToolsBar>  toolsBar;
        std::unique_ptr<EditTimeRuler> ruler;
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
