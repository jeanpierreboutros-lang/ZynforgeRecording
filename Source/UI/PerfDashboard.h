#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // Compact status panel beside the clock.
    //
    //  48.0 kHz / 256        LUFS M -18.2  I -23.1  TP -1.0
    //  AUDIO 12%     DISK 8.1 MB/s     BUF 3%
    //  ==== (thin disk bar)
    //
    // Each figure reads green / yellow / red on the same thresholds. It sits
    // in both views at every window size, so what has to stay visible does.
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

        // Live device format shown in the header line: sample rate (Hz) and
        // the actual buffer/block size in samples. 0 = unknown (device down).
        void setDeviceInfo (double sampleRate, int blockSizeSamples);

        // Master loudness, BS.1770: integrated + momentary LUFS, true-peak dBTP.
        void setLoudness (float integratedLufs, float momentaryLufs, float truePeakDb);

        // When recording, the dashboard draws larger type + a red-emphasis
        // border so the engineer can read load / buffer / disk at a glance.
        void setRecording (bool isRecording);

        // The loudness line as drawn; a figure below the gate reads "--".
        juce::String getLoudnessText() const;
        juce::String getTruePeakText() const;
        // True peak within 1 dB of full scale turns red.
        bool truePeakIsHot() const noexcept;
        // Whether the loudness line gives way to the take's missed samples:
        // only while a take rolls. After STOP the clock keeps the count, and
        // this row goes back to loudness for the soundcheck that follows.
        bool showsDrop() const noexcept { return recording && missed > 0; }
        // Whether the row holds the device format and the loudness line at their
        // widest, so a window size can't quietly lose the readout.
        bool loudnessFits() const;

        void paint (juce::Graphics&) override;

    private:
        float cpu  { 0.0f };
        float disk { 0.0f };
        float buf  { 0.0f };
        juce::int64 missed { 0 };
        double sampleRate { 0.0 };
        int    blockSize  { 0 };
        float  integrated { -120.0f };
        float  momentary  { -120.0f };
        float  truePeakDb { -120.0f };
        bool   recording  { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerfDashboard)
    };
}
