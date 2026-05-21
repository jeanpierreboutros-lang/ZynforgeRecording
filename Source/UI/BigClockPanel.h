#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // A wide, high-contrast banner that shows the current state
    // (REC / PLAY / IDLE), a huge HH:MM:SS timer, marker count, and
    // a disk-health strip (free GB / last write ms / missed samples).
    //
    // All values are pushed in by MainComponent from its timer.
    class BigClockPanel final : public juce::Component
    {
    public:
        enum class Mode { Idle, Recording, Playing };

        BigClockPanel();

        void setMode      (Mode m);
        void setElapsed   (juce::int64 samples, double sampleRate);
        void setMarkers   (int count);
        void setDiskInfo  (double freeGB, int lastWriteMs, juce::int64 missedSamples,
                           double remainingSeconds);

        void paint (juce::Graphics&) override;

    private:
        Mode mode { Mode::Idle };
        juce::String elapsedText { "00:00:00" };
        int    markerCount      { 0 };
        double freeGB           { 0.0 };
        int    lastWriteMs      { 0 };
        juce::int64 missed      { 0 };
        double remainingSeconds { 0.0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BigClockPanel)
    };
}
