#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // A wide, high-contrast banner that shows the current state
    // (REC / PLAY / IDLE), a huge HH:MM:SS timer, marker count, and
    // a disk-health strip (free GB / last write ms / missed samples).
    //
    // All values are pushed in by MainComponent from its timer.
    class BigClockPanel final : public juce::Component, private juce::Timer
    {
    public:
        enum class Mode { Idle, Recording, Playing };

        BigClockPanel();

        void setMode      (Mode m);
        void setElapsed   (juce::int64 samples, double sampleRate);
        void setMarkers   (int count);
        void setDiskInfo  (double freeGB, int lastWriteMs, juce::int64 missedSamples,
                           double remainingSeconds);
        // BS.1770 master loudness: integrated + momentary LUFS, true-peak dBTP.
        void setLoudness  (float integratedLufs, float momentaryLufs, float truePeakDb);

        // True when any strip is record-armed but the transport is
        // still idle. Paints a brand-orange ready border so the
        // engineer sees, at a glance, that hitting RECORD will roll.
        void setArmedReady (bool ready);

        void paint (juce::Graphics&) override;

    private:
        Mode mode { Mode::Idle };
        juce::String elapsedText { "00:00:00" };
        int    markerCount      { 0 };
        double freeGB           { 0.0 };
        int    lastWriteMs      { 0 };
        juce::int64 missed      { 0 };
        double remainingSeconds { 0.0 };
        bool   armedReady       { false };
        float  lufsIntegrated   { -120.0f };
        float  lufsMomentary    { -120.0f };
        float  truePeakDb       { -120.0f };

        // Pulse animation. timerCallback (30 Hz) advances pulsePhase
        // when either Recording is active OR armedReady is on; paint
        // reads pulsePhase to modulate background alpha + border
        // brightness. When neither state is active the timer stops and
        // the value is reset, so an idle window doesn't burn CPU.
        float pulsePhase { 0.0f };
        void  timerCallback() override;
        void  syncPulseTimer();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BigClockPanel)
    };
}
