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
                const auto& m = list[(size_t) targetIdx];
                engine.getPlayer().setPositionSamples (m.sampleOffset);
                // Pro Tools-style Memory Location recall: if the
                // marker carries layout fields, restore them too.
                // Absent fields = jump position only (backward compat
                // with markers.json files that pre-date this feature).
                if (m.zoom > 0.0f && editPage != nullptr)
                    editPage->setZoom (m.zoom);
                showStatus ("Jumped to marker " + juce::String (targetIdx + 1)
                            + ": " + m.name
                            + (m.zoom > 0.0f ? " (layout restored)" : ""));
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
    // Alt+1..4 -- recall stored EDIT-view zoom preset. Alt+Shift+1..4
    // stores the current zoom as that preset. Lets the engineer
    // toggle between "overview" and "detail" zooms with a single
    // keystroke instead of dragging the zoom slider every time.
    // Backed by appProps so presets survive relaunch.
    {
        const int code = key.getKeyCode();
        if (code >= '1' && code <= '4' && key.getModifiers().isAltDown())
        {
            const int slot = code - '0';   // 1..4
            const auto storeKey  = "editZoomPreset_" + juce::String (slot);
            if (auto* props = engine.getAppProps())
            {
                if (editPage != nullptr)
                {
                    if (key.getModifiers().isShiftDown())
                    {
                        const auto z = editPage->getZoom();
                        props->setValue (storeKey, (double) z);
                        props->saveIfNeeded();
                        showStatus ("EDIT zoom preset " + juce::String (slot)
                                    + " stored at " + juce::String (z, 2) + "x");
                    }
                    else
                    {
                        const double z = props->getDoubleValue (storeKey, -1.0);
                        if (z > 0.0)
                        {
                            editPage->setZoom ((float) z);
                            showStatus ("EDIT zoom preset " + juce::String (slot)
                                        + " -> " + juce::String (z, 2) + "x");
                        }
                        else
                        {
                            showStatus ("No EDIT zoom preset " + juce::String (slot)
                                        + " -- store one with Alt+Shift+" + juce::String (slot));
                        }
                    }
                }
            }
            return true;
        }
    }

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
