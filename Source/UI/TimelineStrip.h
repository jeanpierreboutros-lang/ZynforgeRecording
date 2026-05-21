#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Horizontal timeline strip: playhead + marker dots + optional
    // loop region. Click on empty area seeks playback. Click on a
    // marker dot seeks to that marker. Right-click on a marker shows
    // a context menu (Rename, Delete, Loop In, Loop Out, Clear Loop).
    class TimelineStrip final : public juce::Component,
                                private juce::Timer
    {
    public:
        explicit TimelineStrip (AudioEngine& eng);
        ~TimelineStrip() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        int  findMarkerAtX (int x) const;
        int  xForSample (juce::int64 sample) const;
        juce::int64 sampleAtX (int x) const;
        void showMarkerMenu (int markerIndex, juce::Point<int> screenPos);
        void renameMarkerDialog (int markerIndex);

        AudioEngine& engine;
        juce::int64  cachedTotal { 0 };
        juce::int64  cachedPos   { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineStrip)
    };
}
