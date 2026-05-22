#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // Compact three-bar performance dashboard for the header.
    //
    //  CPU   ▓▓▓▓▓░░░░░  47 %
    //  DISK  ▓▓▓▓▓▓▓▓▓░  31 MB/s
    //  BUF   ▓▓░░░░░░░░  12 %
    //
    // Each bar reads green / yellow / red based on threshold; numeric
    // value is shown to the right. Polled at 4 Hz from MainComponent.
    class PerfDashboard final : public juce::Component
    {
    public:
        PerfDashboard();

        // 0..100 audio-load %, MB/s disk throughput, 0..100 ring buffer
        // fill, juce::int64 missed samples (any non-zero glows red).
        void setMetrics (float cpuPct,
                         float diskMBPerSec,
                         float ringFillPct,
                         juce::int64 missedSamples);

        void paint (juce::Graphics&) override;

    private:
        float cpu  { 0.0f };
        float disk { 0.0f };
        float buf  { 0.0f };
        juce::int64 missed { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerfDashboard)
    };
}
