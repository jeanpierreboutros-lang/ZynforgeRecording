#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Horizontal transport strip — six custom-painted icon buttons:
    //   |◀  go to start
    //   ▶|  go to end
    //   ▶   play / pause
    //   ■   stop (hard — kills both recording and playback)
    //   ●   record toggle
    //   ⟲   loop region toggle
    class TransportBar final : public juce::Component,
                               private juce::Timer
    {
    public:
        explicit TransportBar (AudioEngine&);
        ~TransportBar() override;

        void resized() override;

    private:
        class IconButton;

        void timerCallback() override;
        void refreshStates();

        AudioEngine& engine;

        std::unique_ptr<IconButton> gotoStart;
        std::unique_ptr<IconButton> gotoEnd;
        std::unique_ptr<IconButton> play;
        std::unique_ptr<IconButton> stop;
        std::unique_ptr<IconButton> record;
        std::unique_ptr<IconButton> loop;

        bool lastRecording { false };
        bool lastPlaying   { false };
        bool lastLooping   { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
    };
}
