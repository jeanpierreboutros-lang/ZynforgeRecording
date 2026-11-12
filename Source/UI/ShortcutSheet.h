#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

// What Help > Keyboard Shortcuts lists. MainComponent::keyPressed
// (MainComponentKeys.cpp) is what the keys do; each row names the presses it
// stands for, and a test holds the two to each other: every key the window
// answers is on the sheet, and every key on the sheet is answered. A new
// shortcut gets its row here.
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
        constexpr int cmd = M::commandModifier, shift = M::shiftModifier, alt = M::altModifier, ctrl = M::ctrlModifier;
        static const std::vector<Section> table {
            { "Transport", {
                { "Space", "Play / pause from the cursor. In a take: press twice within 2 s to stop (a punch ends on one press)",
                  { key (juce::KeyPress::spaceKey, 0, ' ') } },
                { "M", "Drop a marker at the playhead, then name it", { key ('m') } },
                { "1 - 9", "Jump to cue 1 - 9", range ('1', '9'), View::Any, "a cue list" },
                { "Cmd + 1 - 9", "Jump to marker 1 - 9 (recalls its layout if stored)", range ('1', '9', cmd) },
            }},
            { "Mixer and selection", {
                { "Shift + click", "Add a strip to the selection, or take it out" },
                { "Cmd + A", "Select every strip", { key ('a', cmd) }, View::Any, "keyboard focus off a text field" },
                { "Esc", "Clear the strip selection", { key (juce::KeyPress::escapeKey) }, View::Any, "a strip selection" },
                { "A", "Solo the selected strips; again to unsolo", { key ('a') } },
                { "Option + R", "Arm the selected strips; again to disarm", { key ('r', alt, 0x00AE) } },
                { "Cmd + =", "Switch between MIXER and EDIT",
                  { key ('=', cmd), key ('+', cmd), key (juce::KeyPress::numberPadAdd, cmd) } },
                { "Right-click strip", "Rename / Colour / Stereo / Stream send / Output-mute" },
            }},
            { "Session and clipboard", {
                { "Cmd + S", "Save the session", { key ('s', cmd) } },
                { "Cmd + Z", "Undo", { key ('z', cmd) } },
                { "Cmd + Shift + Z, Cmd + R", "Redo", { key ('z', cmd | shift), key ('r', cmd) } },
                { "Cmd + X", "Cut the selected clip or range (EDIT), else the selected strips (asks first)", { key ('x', cmd) } },
                { "Cmd + C", "Copy the selected clip (EDIT), else the selected strips' settings", { key ('c', cmd) } },
                { "Cmd + V", "Paste clips at the playhead (EDIT), else strip settings", { key ('v', cmd) } },
            }},
            { "Edit tools (EDIT view)", {
                { "S  T  R  G  F", "Smart / Trim / Range / Grabber / Fade tool",
                  { key ('s'), key ('t'), key ('r'), key ('g'), key ('f') }, View::Edit },
                { ",", "Range start at the playhead", { key (',') } },
                { ".", "Range end at the playhead", { key ('.') } },
                { "E", "Zoom to the selected range", { key ('e') } },
                { "D", "Duplicate the selected clip", { key ('d') } },
                { "N", "Next nudge step (10 ms to 1 s)", { key ('n') } },
                { "Option + Left / Right", "Nudge the selected clip",
                  { key (juce::KeyPress::leftKey, alt), key (juce::KeyPress::rightKey, alt) }, View::Edit, "a selected clip" },
                { "Numpad + / -", "Nudge the selected clip, or the playhead",
                  { key (juce::KeyPress::numberPadAdd), key (juce::KeyPress::numberPadSubtract) }, View::Edit },
                { "Delete", "Delete the selected clip or range, leaving a gap (asks first); with only strips selected, delete those channels",
                  { key (juce::KeyPress::deleteKey), key (juce::KeyPress::backspaceKey) }, View::Edit, "a clip, range or strip selection" },
                { "Shift + Delete", "Delete the selected clip or range and close the gap (asks first)",
                  { key (juce::KeyPress::deleteKey, shift), key (juce::KeyPress::backspaceKey, shift) }, View::Edit, "a clip or range" },
                { "Cmd + E", "Separate at the range, else split at the playhead", { key ('e', cmd) } },
                { "Cmd + H", "Heal a split", { key ('h', cmd) } },
                { "Ctrl + Cmd + C", "Crop to the selected range", { key ('c', ctrl | cmd) } },
                { "Ctrl + 4", "Snap to markers on / off", { key ('4', ctrl) } },
                { "Cmd + ]  /  Cmd + [", "Zoom the timeline in / out", { key (']', cmd), key ('[', cmd) }, View::Edit },
                { "Option + 1 - 4", "Recall EDIT zoom preset 1 - 4", range ('1', '4', alt) },
                { "Option + Shift + 1 - 4", "Store the EDIT zoom as preset 1 - 4", range ('1', '4', alt | shift) },
            }},
            { "Edit view mouse", {
                { "Right-click channel name", "Lane shows waveform or markers / Row size / Delete Channel" },
                { "Right-click empty lane",   "Lane shows waveform or markers / Row size" },
                { "Right-click clip", "Mute / Lock / Duplicate / Delete / Gain / Fade in & out" },
                { "Drag swatch",      "Reorder strip (paused only)" },
            }},
        };
        return table;
    }
}
