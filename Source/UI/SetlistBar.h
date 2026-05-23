#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Setlist + cue bar: [SETLIST | ◂ | <dropdown> | ▸ | + Cue | Update].
    //
    // A cue is just a named transport-position bookmark. The engineer
    // builds an ordered list of cues (one per song, intro, scene…) and
    // then nudges with ◂/▸ during the show to jump between them.
    //
    // The bar is purely UI. Cue storage + transport jumps live in
    // MainComponent, which wires the callbacks below.
    class SetlistBar final : public juce::Component
    {
    public:
        // Per-strip snapshot captured at +Cue / Update time. Restored
        // when the cue is recalled (◂ / ▸ or dropdown pick).
        struct StripSnapshot
        {
            // Stable strip UUID — set on capture, looked up on recall.
            // Empty string means 'pre-v2 snapshot' — recall falls back
            // to array index (the position in the cue.strips vector).
            juce::String stripId;

            float gainDb       { 0.0f };
            float pan          { 0.0f };
            int   inputRouting { -1 };
            int   outputRouting{ -1 };
            bool  muted        { false };
            bool  soloed       { false };
            bool  monitor      { false };
            bool  armed        { false };
        };

        // How the cue transitions in — Snap is instant (the prior
        // behaviour); Fade ramps every strip's gain + pan from the
        // current value to the captured value across N beats at the
        // cue's tempo.
        enum class Transition : int { Snap = 0, Fade };

        // Tempo automation point — sample offset relative to the cue's
        // samplePos. Audio thread interpolates linearly between points.
        struct TempoPoint
        {
            juce::int64 offsetSamples { 0 };
            float       bpm           { 120.0f };
        };

        struct Cue
        {
            juce::String name;
            juce::int64  samplePos { 0 };
            std::vector<StripSnapshot> strips;   // length = recorder.getNumTracks() at capture time
            float        tempoBpm  { 0.0f };     // 0 = 'use session default' (older cues)
            Transition   transition { Transition::Snap };
            float        fadeBeats { 0.0f };     // > 0 only meaningful for Fade
            // Multi-point tempo curve for accel / rit within the song.
            // Empty → constant tempoBpm. When non-empty, the engine
            // interpolates between points; the first point's bpm
            // overrides tempoBpm.
            std::vector<TempoPoint> tempoCurve;
        };

        SetlistBar();

        // Repopulate the dropdown from the current cue list. selectedIndex
        // is the cue currently active (highlighted); pass -1 if none.
        void setCues (const std::vector<Cue>& cues, int selectedIndex);

        // Triggered by the engineer:
        //   onPick(int)          — chose a cue from the dropdown
        //   onPrev() / onNext()  — clicked ◂ or ▸
        //   onAddCue()           — clicked '+ Cue' (drop at current pos)
        //   onUpdateCue()        — clicked 'Update' (overwrite current pos)
        std::function<void (int)> onPick;
        std::function<void()>     onPrev;
        std::function<void()>     onNext;
        std::function<void()>     onAddCue;
        std::function<void()>     onUpdateCue;
        // Triggered by the right-click menu on the bar or combo. The
        // engineer picks the cue first (combo selection or step), then
        // right-clicks for Rename / Delete.
        std::function<void()>     onRenameCue;
        std::function<void()>     onDeleteCue;
        // Reorder the active cue: -1 = move up (earlier in list),
        // +1 = move down (later). Host applies the swap + persists.
        std::function<void (int /*direction*/)> onMoveCue;

        void mouseDown (const juce::MouseEvent&) override;

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        juce::Label        titleLabel;
        juce::TextButton   prevButton;
        juce::ComboBox     cueCombo;
        juce::TextButton   nextButton;
        juce::TextButton   addCueButton;
        juce::TextButton   updateButton;

        bool suppressComboCallback { false };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetlistBar)
    };
}
