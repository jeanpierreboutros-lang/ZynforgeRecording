// MainComponent's keyPressed handler. Extracted from MainComponent.cpp
// as part of the 2026-05-24 god-class split so the shortcut surface
// is editable as a single unit.

#include "MainComponent.h"
#include "EditToolsBar.h"

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

    // Undo / Redo. Handled EARLY and matched on the physical key code (not the
    // text character, which is unreliable under Cmd on macOS) so a focused
    // combo / label can't shadow it. Cmd+Z = undo, Cmd+Shift+Z or Cmd+R = redo.
    // Skipped while a text editor has focus -- it owns its own undo. (JUCE
    // doesn't wire native menu accelerators for non-command items, so the
    // menu's "Cmd+Z" hint is display-only; this is the real handler.)
    if (key.getModifiers().isCommandDown()
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        const int kc = key.getKeyCode();
        if (kc == 'Z' || kc == 'z')
        {
            if (key.getModifiers().isShiftDown()) editRedo();
            else                                  editUndo();
            return true;
        }
        if (kc == 'R' || kc == 'r') { editRedo(); return true; }
    }

    // Option(Alt)+R -- bulk record-arm toggle over the selected strips.
    // Select the channels, then Option+R arms them all at once; press
    // again (while all armed) to disarm. Uses the physical key code so
    // a macOS dead-key/® remap on Alt+R doesn't swallow it. Not
    // Command+R (that's Redo).
    if (key.getKeyCode() == 'R'
        && key.getModifiers().isAltDown()
        && ! key.getModifiers().isCommandDown())
    {
        armSelection();
        return true;
    }

    // Tab / Shift+Tab -- jump to next / previous transient. Pro
    // Tools-style. Uses the engine's lazy transient cache (built
    // from every Track_NN.wav on first hit, reused thereafter).
    // Skips when a text editor has focus so name dialogs still
    // tab between fields.
    if (key == juce::KeyPress::tabKey
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        const auto pos = engine.getPlayer().getPositionSamples();
        // Active-row aware. If the engineer clicked a TrackRow
        // recently, restrict Tab to that one track's onsets;
        // otherwise fall back to the pooled cross-track list.
        // Track index in the engine API is 1-based (Track_NN.wav),
        // so we add 1 to the 0-based row index.
        const int activeRow = editPage != nullptr
                                 ? editPage->getActiveRowTrackIndex() : -1;
        const int restrict  = (activeRow >= 0) ? activeRow + 1 : -1;
        const auto target = key.getModifiers().isShiftDown()
                              ? engine.prevTransientSample (pos, restrict)
                              : engine.nextTransientSample (pos, restrict);
        if (target >= 0)
        {
            engine.getPlayer().setPositionSamples (target);
            showStatus ((key.getModifiers().isShiftDown() ? "Prev transient" : "Next transient")
                        + juce::String (restrict > 0 ? " (track " + juce::String (restrict) + ")"
                                                      : " (all)")
                        + " -> "
                        + juce::String ((double) target / 48000.0, 2) + "s");
        }
        else
        {
            showStatus (key.getModifiers().isShiftDown()
                          ? "No earlier transient"
                          : "No later transient (load a session first?)");
        }
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
                bool layoutRestored = false;
                if (editPage != nullptr)
                {
                    if (m.zoom > 0.0f)
                    {
                        editPage->setZoom (m.zoom);
                        layoutRestored = true;
                    }
                    // visibleTracks is treated as authoritative when
                    // it's set on the marker; empty = no override.
                    if (! m.visibleTracks.empty())
                    {
                        editPage->setLogicalRowsVisible (m.visibleTracks);
                        layoutRestored = true;
                    }
                    // Centre the EDIT viewport on the marker's sample
                    // so the jump actually shows the marker position
                    // (rather than leaving the scroll wherever it was).
                    editPage->scrollToSample (m.sampleOffset);
                }
                showStatus ("Jumped to marker " + juce::String (targetIdx + 1)
                            + ": " + m.name
                            + (layoutRestored ? " (layout restored)" : ""));
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

    // Arrow-key automation-point navigation. Active when EDIT view
    // is showing AND an active row is set (click any row to set it)
    // AND a volume/pan/mute lane is active. Left/Right walks
    // between points; Up/Down nudges the focused point's value;
    // Delete/Backspace removes it. Skips when a text editor has
    // focus so name dialogs still receive arrow keys.
    if (editPage != nullptr
        && editPage->isVisible()
        && editPage->getActiveRowTrackIndex() >= 0
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        const int track = editPage->getActiveRowTrackIndex();
        const auto p = automationToolbar->getParam() == AutomationToolbar::Param::Pan
                          ? AudioEngine::AutomationParam::Pan
                          : automationToolbar->getParam() == AutomationToolbar::Param::Mute
                                ? AudioEngine::AutomationParam::Mute
                                : AudioEngine::AutomationParam::Volume;
        const auto& lane = engine.getAutomation (track, p);

        if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        {
            if (lane.empty()) return false;
            int next = editPage->getFocusedPointIdx();
            if (next < 0)
            {
                // Seed from playhead.
                const auto pos = engine.getPlayer().getPositionSamples();
                for (int i = 0; i < (int) lane.size(); ++i)
                    if (lane[(size_t) i].samplePos >= pos) { next = i; break; }
                if (next < 0) next = (int) lane.size() - 1;
            }
            next += (key == juce::KeyPress::rightKey ? 1 : -1);
            next = juce::jlimit (0, (int) lane.size() - 1, next);
            editPage->setFocusedPointIdx (next);
            engine.getPlayer().setPositionSamples (lane[(size_t) next].samplePos);
            return true;
        }

        const int focused = editPage->getFocusedPointIdx();
        if (focused >= 0 && focused < (int) lane.size()
            && (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey))
        {
            const auto& pt = lane[(size_t) focused];
            const float step = (key == juce::KeyPress::upKey ? +1.0f : -1.0f)
                             * (p == AudioEngine::AutomationParam::Pan ? 0.05f
                                : p == AudioEngine::AutomationParam::Mute ? 1.0f
                                : 0.5f);
            float v = pt.value + step;
            if (p == AudioEngine::AutomationParam::Pan)    v = juce::jlimit (-1.0f, 1.0f, v);
            if (p == AudioEngine::AutomationParam::Mute)   v = v >= 0.5f ? 1.0f : 0.0f;
            if (p == AudioEngine::AutomationParam::Volume) v = juce::jlimit (-60.0f, 12.0f, v);
            const auto samplePos = pt.samplePos;
            runAutomationEdit ("Nudge automation point",
                               [this, track, p, samplePos, v]
                               {
                                   engine.removeAutomationPointNear (track, p, samplePos, 1);
                                   engine.addAutomationPoint        (track, p, samplePos, v);
                               });
            editPage->repaint();
            return true;
        }

        if (focused >= 0 && focused < (int) lane.size()
            && (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey))
        {
            const auto samplePos = lane[(size_t) focused].samplePos;
            runAutomationEdit ("Delete automation point",
                               [this, track, p, samplePos]
                               {
                                   engine.removeAutomationPointNear (track, p, samplePos, 4096);
                               });
            editPage->setFocusedPointIdx (focused > 0 ? focused - 1 : -1);
            return true;
        }
    }

    // Delete / Backspace in the EDIT view: Shift = ripple (close the gap),
    // plain = leave a gap. Acts on the selected clip if one is clicked,
    // else the selected range. Reached only when no automation point was
    // focused above; skipped while a text editor has focus. Uses keycode
    // so the Shift modifier doesn't defeat the match.
    {
        const int kc = key.getKeyCode();
        if ((kc == juce::KeyPress::deleteKey || kc == juce::KeyPress::backspaceKey)
            && currentView == View::Edit
            && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
        {
            const bool haveClip  = (editPage != nullptr && editPage->getSelectedClipTrack() >= 0);
            const bool haveRange = engine.getPlayer().hasLoopRegion();
            const bool ripple    = key.getModifiers().isShiftDown();
            if (haveClip || haveRange)
            {
                // Deleting a recording is destructive enough to confirm first
                // (the engineer asked for a warning). The file stays on disk;
                // this only pulls the clip from the arrangement (undoable).
                confirmDeleteRecording (ripple, [this, ripple, haveClip, haveRange]
                {
                    if (ripple)         editRippleDelete();
                    else if (haveClip)  editDeleteSelectedClip();
                    else if (haveRange) editClearRange();
                });
                return true;
            }
        }
    }

    // Alt+Left/Right (and numpad +/-) nudge the selected clip by the
    // current nudge step (bare Left/Right stay reserved for automation-
    // point navigation). Numpad +/- with no clip selected nudges the
    // playhead instead, so it's always useful.
    if (currentView == View::Edit && key.getModifiers().isAltDown()
        && (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
        && editPage != nullptr && editPage->getSelectedClipTrack() >= 0)
    {
        editNudgeSelectedClip (key == juce::KeyPress::rightKey ? +1 : -1);
        return true;
    }
    {
        const int kc = key.getKeyCode();
        if (currentView == View::Edit
            && (kc == juce::KeyPress::numberPadAdd || kc == juce::KeyPress::numberPadSubtract))
        {
            const int dir = (kc == juce::KeyPress::numberPadAdd) ? +1 : -1;
            if (editPage != nullptr && editPage->getSelectedClipTrack() >= 0)
                editNudgeSelectedClip (dir);
            else
            {
                // No clip selected -> nudge the playhead by the nudge value.
                auto& player = engine.getPlayer();
                const double sr = juce::jmax (1.0, player.getSampleRate());
                const auto step = (juce::int64) (sr * (double) nudgeMs / 1000.0);
                player.setPositionSamples (juce::jmax ((juce::int64) 0,
                                            player.getPositionSamples() + dir * step));
                if (editPage != nullptr) editPage->repaint();
            }
            return true;
        }
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

    // ── Single-letter edit-tool selection (EDIT view only). Picks the tool
    // on the Pro Tools-style EditToolsBar so the engineer never has to reach
    // for the mouse:  S Smart · T Trim · R Range · G Grabber/Move · F Fade ·
    // B Scrub. Gated on the EDIT view so MIX-view typing isn't hijacked, and
    // on 'no modifier' so it never shadows the Cmd+ shortcuts below; the
    // text-editor focus guard earlier in this method keeps renames safe.
    if (currentView == View::Edit && editPage != nullptr
        && ! key.getModifiers().isAnyModifierKeyDown())
    {
        if (auto* tb = editPage->getEditToolsBar())
        {
            using Tool = EditToolsBar::Tool;
            switch (c)
            {
                case 's': tb->setTool (Tool::Smart);    return true;
                case 't': tb->setTool (Tool::Trim);     return true;
                case 'r': tb->setTool (Tool::Selector); return true;
                case 'g': tb->setTool (Tool::Grabber);  return true;
                case 'f': tb->setTool (Tool::Fade);     return true;
                case 'b': tb->setTool (Tool::Scrubber); return true;
                default:  break;   // fall through to the editing commands
            }
        }
    }

    if (c == 'a') { editSoloSelection();    return true; }
    if (c == 'e' && ! key.getModifiers().isCommandDown())
        { editZoomToSelection(); return true; }                 // E -- zoom to selection
    if (c == 'd' && ! key.getModifiers().isCommandDown())
        { editDuplicateSelectedClip(); return true; }          // duplicate clicked clip
    if (c == 'n' && ! key.getModifiers().isCommandDown())
        { editCycleNudgeValue(); return true; }                // cycle the nudge step
    if (c == ',') { editStartRange();       return true; }
    if (c == '.') { editFinishRange();      return true; }
    if (c == '4') { editToggleSnap();       return true; }
    // Cmd+X / Cmd+C / Cmd+V / Cmd+E keyboard shortcuts. (Cmd+Z / Cmd+R undo /
    // redo are handled early, near the top of this method.) Matched on the
    // physical key code -- getTextCharacter() is unreliable under Cmd on macOS
    // (the bug that silently broke Cmd+Z). Skipped while a text editor has
    // focus so text cut/copy/paste in a rename/dialog field still works.
    if (key.getModifiers().isCommandDown()
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        const int kc = key.getKeyCode();
        if (kc == 'X' || kc == 'x') { editClipboardCut (true);  return true; }
        if (kc == 'C' || kc == 'c') { editClipboardCut (false); return true; }
        if (kc == 'V' || kc == 'v') { editClipboardPaste();     return true; }
        if (kc == 'H' || kc == 'h') { editHealSeparation();     return true; }   // Pro Tools Heal
        // Pro Tools 'Separate Clip' (Cmd+E): separate at the selected range
        // if one is set, else split at the playhead. Relocated off the bare
        // 's' / 'b' keys, which now pick the Selector / Scrubber tools.
        if (kc == 'E' || kc == 'e')
        {
            if (engine.getPlayer().hasLoopRegion()) editSeparateAtSelection();
            else                                    editSplitAtPlayhead();
            return true;
        }
    }
    return false;
}
