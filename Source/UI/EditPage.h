#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../Audio/AudioEngine.h"

#include <memory>
#include <vector>

namespace zynforge
{
    // EDIT view — one row per channel with the strip header (colour, name,
    // REC / MUTE / SOLO) on the left and the recorded WAV's waveform drawn
    // on the right via juce::AudioThumbnail. A vertical playhead spans all
    // rows and tracks the SessionPlayer position. State is shared with the
    // mixer view (TrackState), so mute / solo / rename changes propagate
    // both ways.
    class AutomationToolbar;
    class EditToolsBar;
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

        int  lastTrackCount  { -1 };
        bool lastLoaded      { false };
        AutomationToolbar*             toolbar  { nullptr };
        std::unique_ptr<EditToolsBar>  toolsBar;
        bool clickPresent { false };
        int  clickTrackIdx { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditPage)
    };
}
