#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Compact tempo widget:
    //
    //   [ TEMPO  120.0 BPM ] [ − ] [ + ] [ TAP ] [● beat LED]
    //
    // Tap-tempo tracks the last N taps in a small ring; BPM = 60 /
    // average(intervals) once at least 2 taps have arrived. Taps older
    // than 2 s are discarded so the engineer can re-start mid-set.
    //
    // The host (MainComponent) owns the canonical BPM in AudioEngine —
    // this bar fires onBpmChanged() and reads back via setBpm() when
    // the engine answers.
    class TempoBar final : public juce::Component, private juce::Timer
    {
    public:
        TempoBar();
        ~TempoBar() override;

        void setBpm (float bpm);
        float getBpm() const noexcept { return currentBpm; }

        std::function<void (float)> onBpmChanged;
        // Fired when the engineer presses the CLICK button. MainComponent
        // synthesises a click WAV at the current tempo, drops it into
        // the session's Audio Files/ folder, and grows the recorder to
        // include it as a normal track.
        std::function<void()>       onCreateClickTrack;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void doTap();
        void nudge (float delta);

        float currentBpm { 120.0f };

        juce::Label    titleLabel;
        juce::Label    valueLabel;     // editable on double-click
        juce::TextButton minusButton  { "-" };
        juce::TextButton plusButton   { "+" };
        juce::TextButton tapButton    { "TAP" };
        juce::TextButton clickButton  { "CLICK" };

        // Beat LED phase counter — runs off the timer (no audio sync;
        // it's a visual confidence cue, not a clock).
        double beatPhase { 0.0 };
        bool   beatOn    { false };

        // Tap history (timestamp in ms).
        static constexpr int kMaxTaps = 6;
        double taps[kMaxTaps] { };
        int    tapCount { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TempoBar)
    };
}
