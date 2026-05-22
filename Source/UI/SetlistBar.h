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
        struct Cue
        {
            juce::String name;
            juce::int64  samplePos { 0 };
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
