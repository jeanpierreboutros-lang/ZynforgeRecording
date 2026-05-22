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
    class EditPage final : public juce::Component, private juce::Timer
    {
    public:
        explicit EditPage (AudioEngine& engine);
        ~EditPage() override;

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

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EditPage)
    };
}
