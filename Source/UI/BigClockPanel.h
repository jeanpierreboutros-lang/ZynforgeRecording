#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace zynforge
{
    // A wide, high-contrast banner that shows the current state
    // (REC / PLAY / IDLE), a huge HH:MM:SS timer, and the take's health as
    // compact groups, each a small label over its value:
    //   MISSED 0   LAST WRITE 0 ms   MARKERS 0   FREE 32.7 GB   TIME LEFT 3h00m
    //
    // All values are pushed in by MainComponent from its timer.
    class BigClockPanel final : public juce::Component,
                                public juce::TooltipClient,
                                private juce::Timer
    {
    public:
        enum class Mode { Idle, Recording, Playing };

        BigClockPanel();

        void setMode      (Mode m);
        void setElapsed   (juce::int64 samples, double sampleRate);
        void setMarkers   (int count);
        // Whether the numbers are live. A figure the volume probe could not
        // refresh must read as stale, never as a confident number. With the
        // chosen sessions drive unplugged there is no figure at all: a read
        // would land on the boot disk.
        enum class DiskReading { Fresh, CopyStale, PrimaryStale, DriveMissing };
        void setDiskInfo  (double freeGB, int lastWriteMs, juce::int64 missedSamples,
                           double remainingSeconds, DiskReading reading);
        double getFreeGB() const noexcept           { return freeGB; }
        double getRemainingSeconds() const noexcept { return remainingSeconds; }
        DiskReading getDiskReading() const noexcept { return diskReading; }
        juce::int64 getMissedSamples() const noexcept { return missed; }

        // True when any strip is record-armed but the transport is
        // still idle. Lights the lamp amber and reads ARMED, so the engineer
        // sees, at a glance, that hitting RECORD will roll.
        void setArmedReady (bool ready);

        // EDIT is showing while a take rolls (or finalizes): timeline and
        // marker edits are locked, so the plate says so under its state.
        void setEditLocked (bool locked);
        bool isEditLockShown() const noexcept { return editLocked; }

        // ── The health groups ─────────────────────────────────────────────
        // Left to right. TimeLeft is the large one; before a take has an
        // estimate it holds DISK FREE instead, and the Free group is dropped.
        enum class Group { Missed, LastWrite, Markers, Free, TimeLeft };
        static constexpr int kNumGroups = 5;
        using GroupWidths = std::array<float, kNumGroups>;   // indexed by Group; 0 = not on the plate
        using GroupMask   = std::array<bool,  kNumGroups>;

        // The order the small groups give way on a narrow plate, first to
        // last. TimeLeft never does: after the timer it is what the plate is for.
        static std::array<Group, 4> giveWayOrder() noexcept
        { return { Group::Markers, Group::LastWrite, Group::Free, Group::Missed }; }

        // Which groups fit in `room` px with `gap` between neighbours: groups
        // give way in giveWayOrder() until the rest fit (TimeLeft always stays).
        static GroupMask fitGroups (const GroupWidths& widths, float room, float gap) noexcept;
        // Width of the groups `shown`, gaps included.
        static float groupsWidth (const GroupWidths& widths, const GroupMask& shown, float gap) noexcept;

        // What the groups read. A miss is marked as well as red, so it
        // doesn't rest on colour alone (white on the record plate).
        static juce::String missedValueText (juce::int64 missedSamples);
        static juce::String timeLeftLabelText (bool freeIsBig, bool copyStale);

        // The groups as laid out at the current size and values: an empty
        // rectangle is a group that gave way (test seam).
        std::array<juce::Rectangle<float>, kNumGroups> getGroupBounds() const;
        juce::String getGroupLabel (Group g) const;
        juce::String getGroupValue (Group g) const;

        void paint (juce::Graphics&) override;
        juce::String getTooltip() override;

    private:
        struct Layout
        {
            juce::Rectangle<float> inner, left, timer;
            std::array<juce::Rectangle<float>, kNumGroups> groups;
            std::array<juce::String, kNumGroups> labels, values;
            bool tall { false };
        };
        Layout computeLayout() const;
        bool   freeIsBig() const noexcept;
        juce::String freeText() const;

        Mode mode { Mode::Idle };
        juce::String elapsedText { "00:00:00" };
        int    markerCount      { 0 };
        double freeGB           { 0.0 };
        int    lastWriteMs      { 0 };
        juce::int64 missed      { 0 };
        double remainingSeconds { 0.0 };
        DiskReading diskReading { DiskReading::Fresh };
        bool   armedReady       { false };
        bool   editLocked       { false };

        // Pulse animation. timerCallback advances pulsePhase while Recording
        // is active; paint reads it to modulate the background. Otherwise the
        // timer stops and the value is reset, so an idle window doesn't burn CPU.
        float pulsePhase { 0.0f };
        float igniteLevel { 0.0f };   // ramps 0→1 over ~300ms when recording starts
        void  timerCallback() override;
        void  syncPulseTimer();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BigClockPanel)
    };
}
