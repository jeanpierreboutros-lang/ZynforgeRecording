// MainComponent's keyPressed handler. Extracted from MainComponent.cpp
// as part of the 2026-05-24 god-class split so the shortcut surface
// is editable as a single unit.

#include "MainComponent.h"

using namespace zynforge;

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Escape clears the multi-strip selection so the engineer can
    // bail out of a half-built bulk action without clicking each
    // selected strip again.
    if (key == juce::KeyPress::escapeKey && ! selectedLogical.empty())
    {
        clearStripSelection();
        return true;
    }

    // Cmd+A -- select every strip. Skips when the system focus is on a
    // text editor (so name-rename Cmd+A still selects the text).
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0)
        && juce::Component::getCurrentlyFocusedComponent() != nullptr
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        selectAllStrips();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        // Universal play / stop / pause toggle. Priority:
        //   1. If recording → stop the recording
        //   2. Else if playing → pause playback
        //   3. Else if a session is loaded → start playback
        //   4. Otherwise let the engineer know nothing's loaded
        auto& player = engine.getPlayer();
        if (engine.isRecording())
        {
            onStopClicked();
        }
        else if (player.isPlaying())
        {
            engine.stopPlayback();
            playButton.setButtonText ("PLAY");
            showStatus ("Stopped");
        }
        else if (player.isLoaded())
        {
            engine.startPlayback();
            playButton.setButtonText ("PAUSE");
            showStatus ("Playing");
        }
        else
        {
            showStatus ("Load or record a session first -- nothing to play");
        }
        return true;
    }

    // Cmd+1..9 -- Pro Tools-style "Memory Location" jump. Explicit
    // shortcut for markers so the bare digit can stay reserved for
    // cues without the two competing for the same key. Skips when
    // a text editor has focus so a Cmd+1 in a name dialog still
    // reaches the editor.
    {
        const int code = key.getKeyCode();
        if (code >= '1' && code <= '9'
            && key.getModifiers().isCommandDown()
            && ! key.getModifiers().isShiftDown()
            && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
        {
            const int targetIdx = code - '1';
            const auto& list = engine.getMarkers().getAll();
            if (targetIdx < (int) list.size())
            {
                engine.getPlayer().setPositionSamples (list[(size_t) targetIdx].sampleOffset);
                showStatus ("Jumped to marker " + juce::String (targetIdx + 1)
                            + ": " + list[(size_t) targetIdx].name);
                return true;
            }
            showStatus ("No marker " + juce::String (targetIdx + 1)
                        + " -- drop one with M first");
            return true;
        }
    }

    // Number keys 1..9 jump to cue 1..9 if a setlist is loaded -- turns
    // the cue list into a real performance tool.
    {
        const int code = key.getKeyCode();
        if (code >= '1' && code <= '9' && ! key.getModifiers().isAnyModifierKeyDown())
        {
            const int target = code - '1';
            if (target < (int) cues.size())
            {
                jumpToCue (target);
                return true;
            }
        }
    }

    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

    if (c == 'm')
    {
        // Pro Tools-style marker drop: place the marker immediately at
        // the current position, then pop a naming dialog. If the
        // engineer Cancels, the marker keeps its auto-name 'Marker N'
        // (matches Pro Tools Memory Locations).
        dropMarkerAndPromptName();
        return true;
    }

    // Bare 1..9 is reserved for cue jumps (handled above). Markers
    // moved to Cmd+1..9 so the engineer always knows which list the
    // digit lands on; the old "bare digit jumps to cue OR marker
    // depending on which exists" rule was a stage trap.
    if (c == 'a') { editSoloSelection();    return true; }
    if (c == 's') { editSplitAtPlayhead();  return true; }
    if (c == ',') { editStartRange();       return true; }
    if (c == '.') { editFinishRange();      return true; }
    if (c == '4') { editToggleSnap();       return true; }
    // Cmd+Z / Cmd+R / Cmd+X / Cmd+C / Cmd+V keyboard shortcuts.
    if (key.getModifiers().isCommandDown())
    {
        if (c == 'z') { editUndo();          return true; }
        if (c == 'r') { editRedo();          return true; }
        if (c == 'x') { editCutSelected (true);  return true; }
        if (c == 'c') { editCutSelected (false); return true; }
        if (c == 'v') { editPasteSelected(); return true; }
    }
    return false;
}
