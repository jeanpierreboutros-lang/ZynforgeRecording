#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// What Help > Keyboard Shortcuts lists. MainComponent::keyPressed
// (MainComponentKeys.cpp) is what the keys do; each row names the presses it
// stands for, so a test can hold the sheet and the key handler to each other.
namespace zynforge::shortcuts
{
    enum class View { Any, Mix, Edit };

    struct Row
    {
        juce::String keys, description;
        // The presses this row stands for. Empty for a mouse gesture.
        std::vector<juce::KeyPress> presses {};
        // Where the key handler answers them.
        View view { View::Any };
        // Non-empty when the handler answers only in some state (a selection,
        // a cue, a clip), in words for the test that checks the rest.
        juce::String needs {};
    };

    struct Section { juce::String title; std::vector<Row> rows; };

    // A press as macOS delivers it: a letter's key code is upper case and its
    // character follows Shift.
    inline juce::KeyPress key (int code, int mods = 0, juce::juce_wchar text = 0)
    {
        const bool letter = code >= 'a' && code <= 'z';
        const int keyCode = letter ? code - 'a' + 'A' : code;
        if (text == 0)
            text = (juce::juce_wchar) (letter && (mods & juce::ModifierKeys::shiftModifier) != 0 ? keyCode : code);
        return juce::KeyPress (keyCode, juce::ModifierKeys (mods), text);
    }

    inline std::vector<juce::KeyPress> range (char first, char last, int mods = 0)
    {
        std::vector<juce::KeyPress> out;
        for (char c = first; c <= last; ++c) out.push_back (key (c, mods));
        return out;
    }

    inline const std::vector<Section>& sheet()
    {
        using M = juce::ModifierKeys;
        static const std::vector<Section> table {
            { "Transport", {
                { "Space", "Play / pause (stops recording if armed)", { key (juce::KeyPress::spaceKey, 0, ' ') } },
                { "M",     "Drop marker at playhead", { key ('m') } },
            }},
            { "Cues", {
                { "1 - 9", "Jump to cue 1 - 9", range ('1', '9'), View::Any, "a cue list" },
            }},
            { "Markers", {
                { "Cmd + 1 - 9", "Jump to marker 1 - 9 (recalls layout if stored)", range ('1', '9', M::commandModifier) },
            }},
            { "Edit", {
                { "A",        "Solo selected strips", { key ('a') } },
                { "S",        "Smart tool (with the EDIT tools showing)", { key ('s') }, View::Edit },
                { ",",        "Set range start at playhead", { key (',') } },
                { ".",        "Set range end at playhead", { key ('.') } },
                { "Ctrl + 4", "Toggle snap mode", { key ('4', M::ctrlModifier) } },
                { "Cmd + Z",  "Undo last clip edit", { key ('z', M::commandModifier) } },
                { "Cmd + R",  "Redo", { key ('r', M::commandModifier) } },
                { "Cmd + X",  "Cut selected strips", { key ('x', M::commandModifier) } },
                { "Cmd + C",  "Copy selected strips", { key ('c', M::commandModifier) } },
                { "Cmd + V",  "Paste strips", { key ('v', M::commandModifier) } },
            }},
            { "Mixer", {
                { "Shift + click", "Toggle multi-selection (additive)" },
                { "Cmd + A",       "Select every strip", { key ('a', M::commandModifier) }, View::Any, "keyboard focus off a text field" },
                { "Esc",           "Clear strip selection", { key (juce::KeyPress::escapeKey) }, View::Any, "a strip selection" },
                { "Right-click strip", "Rename / Colour / Stereo / Stream send / Output-mute" },
            }},
            { "Edit view", {
                { "Right-click channel name", "Lane shows waveform or markers / Row size / Delete Channel" },
                { "Right-click empty lane",   "Lane shows waveform or markers / Row size" },
                { "Right-click clip", "Mute / Lock / Duplicate / Delete / Gain / Fade in & out" },
                { "Drag swatch",      "Reorder strip (paused only)" },
            }},
        };
        return table;
    }
}
