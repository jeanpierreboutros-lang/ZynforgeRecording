// Edit-operation methods on MainComponent (undo/redo, cut/copy/paste,
// solo, crop, split, range markers, marker drop, automation
// transactions). Extracted from MainComponent.cpp as part of the
// 2026-05-24 god-class split. Every method is a member of
// MainComponent and accesses private state via the usual
// out-of-line member-definition path.
//
// The anonymous-namespace block carries the strip-snapshot helpers
// and the two UndoableAction subclasses (AutomationSnapshotAction +
// MixerSnapshotAction). Both were only used by the edit cluster --
// moved with it so the file is self-contained.

#include "MainComponent.h"
#include "../Theme/DialogChrome.h"
#include "MarkerListDialog.h"

#include <algorithm>
#include <vector>
#include <set>

using namespace zynforge;

namespace
{
    juce::int64 currentPlayheadSamples (zynforge::AudioEngine& eng)
    {
        if (eng.isRecording()) return eng.getRecorder().getRecordTimelineSamples();
        if (eng.getPlayer().isLoaded()) return eng.getPlayer().getPositionSamples();
        return 0;
    }

    // Serialise the strip's state (name, colour, gain, pan, routing,
    // mute/solo/mon/arm) into a JSON object. Used by Cut / Copy / Paste.
    juce::var snapshotStrip (zynforge::AudioEngine& eng, int physical)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks())
            return {};
        auto& t = eng.getRecorder().getTrack (physical);
        auto obj = new juce::DynamicObject();
        obj->setProperty ("name",     t.name);
        obj->setProperty ("colour",   (int) t.colourARGB.load (std::memory_order_relaxed));
        obj->setProperty ("gainDb",   (double) t.gainDb     .load (std::memory_order_relaxed));
        obj->setProperty ("pan",      (double) t.pan        .load (std::memory_order_relaxed));
        obj->setProperty ("in",       t.inputRouting .load (std::memory_order_relaxed));
        obj->setProperty ("out",      t.outputRouting.load (std::memory_order_relaxed));
        obj->setProperty ("mute",     t.muted   .load (std::memory_order_relaxed));
        obj->setProperty ("solo",     t.soloed  .load (std::memory_order_relaxed));
        obj->setProperty ("mon",      t.monitor .load (std::memory_order_relaxed));
        obj->setProperty ("rec",      t.armed   .load (std::memory_order_relaxed));
        return juce::var (obj);
    }

    void restoreStrip (zynforge::AudioEngine& eng, int physical, const juce::var& v)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks()) return;
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;
        auto& t = eng.getRecorder().getTrack (physical);
        eng.setTrackName  (physical, obj->getProperty ("name").toString());
        eng.setTrackColour(physical, juce::Colour ((juce::uint32) (int) obj->getProperty ("colour")));
        eng.setTrackGainDb(physical, (float) (double) obj->getProperty ("gainDb"));
        eng.setTrackPan   (physical, (float) (double) obj->getProperty ("pan"));
        eng.setTrackInputRouting  (physical, (int) obj->getProperty ("in"));
        eng.setTrackOutputRouting (physical, (int) obj->getProperty ("out"));
        t.muted   .store ((bool) obj->getProperty ("mute"), std::memory_order_relaxed);
        t.soloed  .store ((bool) obj->getProperty ("solo"), std::memory_order_relaxed);
        t.monitor .store ((bool) obj->getProperty ("mon"),  std::memory_order_relaxed);
        t.armed   .store ((bool) obj->getProperty ("rec"),  std::memory_order_relaxed);
    }

    int physicalFromLogical (zynforge::AudioEngine& eng, int logical)
    {
        int phys = 0;
        auto& rec = eng.getRecorder();
        for (int k = 0; k < logical && phys < rec.getNumTracks(); ++k)
            phys += rec.getTrack (phys).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        return phys;
    }
}

// ─── Undo / redo ─────────────────────────────────────────────────────
// Wrapper UndoableAction that captures + restores a full mixer-state
// snapshot. This is coarse but always-correct for any strip-level edit.
namespace
{
    // Snapshot the engine's automation lanes before / after a user
    // edit. On undo() the engine is reverted to the 'before' var;
    // on redo() it's pushed forward to the 'after' var. Wraps every
    // EditPage automation mouseDown / drag / context-menu mutation.
    struct AutomationSnapshotAction final : public juce::UndoableAction
    {
        AutomationSnapshotAction (zynforge::AudioEngine& e,
                                  juce::var beforeIn, juce::var afterIn)
            : engine (e), before (std::move (beforeIn)), after (std::move (afterIn)) {}

        bool perform() override { engine.loadAutomationFromJson (after);  return true; }
        bool undo()    override { engine.loadAutomationFromJson (before); return true; }

        zynforge::AudioEngine& engine;
        juce::var before, after;
    };

    struct MixerSnapshotAction final : public juce::UndoableAction
    {
        MixerSnapshotAction (zynforge::AudioEngine& e, juce::var before)
            : eng (e), beforeState (std::move (before)) {}

        bool perform() override
        {
            // First invocation: capture the 'after' state so redo works.
            if (! afterCaptured)
            {
                afterState = captureAll();
                afterCaptured = true;
            }
            applyAll (afterState);
            return true;
        }

        bool undo() override
        {
            applyAll (beforeState);
            return true;
        }

        juce::var captureAll() const
        {
            juce::Array<juce::var> arr;
            for (int i = 0; i < eng.getRecorder().getNumTracks(); ++i)
                arr.add (snapshotStrip (eng, i));
            return juce::var (arr);
        }

        void applyAll (const juce::var& v) const
        {
            if (auto* arr = v.getArray())
                for (int i = 0; i < arr->size(); ++i)
                    restoreStrip (eng, i, (*arr)[i]);
        }

        zynforge::AudioEngine& eng;
        juce::var beforeState;
        juce::var afterState;
        bool      afterCaptured { false };
    };

    // Clip / playlist undo. The edit has ALREADY happened by the time we
    // push this (the caller captured 'before' first), so perform() is a
    // no-op on first run and re-applies 'after' on redo. undo() restores
    // 'before'. State is the engine's full playlists JSON.
    struct ClipSnapshotAction final : public juce::UndoableAction
    {
        ClipSnapshotAction (zynforge::AudioEngine& e, juce::var before, juce::var after)
            : eng (e), beforeState (std::move (before)), afterState (std::move (after)) {}

        bool perform() override
        {
            if (firstRun) { firstRun = false; return true; }   // already applied
            eng.loadPlaylistsFromJson (afterState);
            return true;
        }
        bool undo() override
        {
            eng.loadPlaylistsFromJson (beforeState);
            return true;
        }

        zynforge::AudioEngine& eng;
        juce::var beforeState, afterState;
        bool      firstRun { true };
    };
}

void MainComponent::pushClipUndo (const juce::String& label, const juce::var& before)
{
    const auto after = engine.playlistsToJson();
    // Only record if the clips actually changed (callers capture 'before'
    // eagerly on mouse-down, so a plain click mustn't litter the stack).
    if (juce::JSON::toString (before) == juce::JSON::toString (after)) return;
    undoManager.beginNewTransaction (label);
    undoManager.perform (new ClipSnapshotAction (engine, before, after));
}
void MainComponent::recordUndoSnapshot (const juce::String& label)
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        arr.add (snapshotStrip (engine, i));
    undoManager.beginNewTransaction (label);
    undoManager.perform (new MixerSnapshotAction (engine, juce::var (arr)));
}

juce::var MainComponent::captureMixerSnapshot()
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        arr.add (snapshotStrip (engine, i));
    return juce::var (arr);
}

void MainComponent::rebaselineMixerUndo()
{
    committedMixerState  = captureMixerSnapshot();
    committedMixerSig    = juce::JSON::toString (committedMixerState);
    lastSeenMixerSig     = committedMixerSig;
    mixerChangeStableTicks = 0;
    mixerUndoReady = true;
}

void MainComponent::pollMixerUndo()
{
    // Never turn the act of recording into an undo step -- arm flags churn
    // while rolling, and a captured take must not be undoable. Re-baseline so
    // the post-record state becomes the next undo anchor.
    if (engine.isRecording()) { rebaselineMixerUndo(); return; }

    const auto cur    = captureMixerSnapshot();
    const auto curSig = juce::JSON::toString (cur);

    if (! mixerUndoReady) { committedMixerState = cur; committedMixerSig = curSig;
                            lastSeenMixerSig = curSig; mixerUndoReady = true; return; }

    // A strip count change is structural (add / remove channel) -- re-baseline
    // rather than snapshot (MixerSnapshotAction restores per-existing-strip and
    // can't grow/shrink the mixer). Add/remove-channel undo is a separate task.
    const auto* curArr = cur.getArray();
    const auto* comArr = committedMixerState.getArray();
    if (curArr != nullptr && comArr != nullptr && curArr->size() != comArr->size())
    { committedMixerState = cur; committedMixerSig = curSig; lastSeenMixerSig = curSig;
      mixerChangeStableTicks = 0; return; }

    if (curSig == committedMixerSig) { lastSeenMixerSig = curSig; mixerChangeStableTicks = 0; return; }

    // Changed vs the committed baseline. Wait for it to stop moving (so a fader
    // drag commits one step) by counting ticks where the state held steady.
    if (curSig != lastSeenMixerSig) { lastSeenMixerSig = curSig; mixerChangeStableTicks = 0; return; }
    if (++mixerChangeStableTicks < 3) return;   // ~300 ms settle at 10 Hz

    undoManager.beginNewTransaction ("Mixer change");
    undoManager.perform (new MixerSnapshotAction (engine, committedMixerState));
    committedMixerState = cur;
    committedMixerSig   = curSig;
    mixerChangeStableTicks = 0;
}

void MainComponent::confirmDeleteRecording (bool ripple, std::function<void()> onConfirm)
{
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Delete recording?")
            .withMessage (juce::String (ripple
                ? "Delete the selected audio and close the gap?"
                : "Delete the selected audio?")
                + "\n\nThe recorded file stays on disk -- this only removes it "
                  "from the arrangement, and Cmd+Z restores it.")
            .withButton ("Delete")
            .withButton ("Cancel"),
        // LookAndFeel_V2::createAlertWindow gives the FIRST of two buttons
        // commandID 1 and the second 0 -- so "Delete" returns 1, not 0.
        [onConfirm] (int result) { if (result == 1 && onConfirm) onConfirm(); });
}

void MainComponent::confirmDeleteChannels (std::function<void()> onConfirm)
{
    const int n = (int) selectedLogical.size();
    if (n <= 0 || engine.isRecording()) return;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle (n > 1 ? "Delete channels?" : "Delete channel?")
            .withMessage (juce::String (n > 1
                ? "Delete the " + juce::String (n) + " selected channels?"
                : "Delete the selected channel?")
                + "\n\nRecorded files stay on disk -- this removes the "
                  "channel(s) from the session.")
            .withButton ("Delete")
            .withButton ("Cancel"),
        // First of two buttons gets commandID 1 -- "Delete" returns 1.
        [onConfirm] (int result) { if (result == 1 && onConfirm) onConfirm(); });
}

void MainComponent::editUndo()
{
    // Commit any in-flight mixer change NOW so Cmd+Z immediately after a fader
    // / rename / colour move undoes that move -- the timer's ~300 ms settle
    // wouldn't have recorded it as an undo step yet.
    if (! engine.isRecording() && mixerUndoReady)
    {
        const auto cur    = captureMixerSnapshot();
        const auto curSig = juce::JSON::toString (cur);
        const auto* curArr = cur.getArray();
        const auto* comArr = committedMixerState.getArray();
        const bool structural = (curArr != nullptr && comArr != nullptr
                                 && curArr->size() != comArr->size());
        if (curSig != committedMixerSig && ! structural)
        {
            undoManager.beginNewTransaction ("Mixer change");
            undoManager.perform (new MixerSnapshotAction (engine, committedMixerState));
            committedMixerState = cur; committedMixerSig = curSig;
            lastSeenMixerSig = curSig; mixerChangeStableTicks = 0;
        }
    }

    if (! undoManager.canUndo()) { showStatus ("Nothing to undo"); return; }
    undoManager.undo();
    lastTrackCount = -1;
    rebaselineMixerUndo();   // the restored state is the new baseline, not a fresh change
    showStatus ("Undo: " + undoManager.getUndoDescription());
}

void MainComponent::editRedo()
{
    if (! undoManager.canRedo()) { showStatus ("Nothing to redo"); return; }
    undoManager.redo();
    lastTrackCount = -1;
    rebaselineMixerUndo();   // the restored state is the new baseline, not a fresh change
    showStatus ("Redo: " + undoManager.getRedoDescription());
}

// ─── Cut / copy / paste / delete on the strip selection ──────────────
void MainComponent::editCutSelected (bool cut)
{
    if (selectedLogical.empty()) { showStatus ("No strip selected"); return; }
    juce::Array<juce::var> arr;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        arr.add (snapshotStrip (engine, phys));
    }
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("strips", juce::var (arr));
    stripClipboard = juce::var (obj);
    showStatus (juce::String (cut ? "Cut " : "Copied ") + juce::String (arr.size())
                + " strip setting(s)");
    if (cut) deleteSelectedStrips();
}

void MainComponent::editPasteSelected()
{
    if (! stripClipboard.isObject()) { showStatus ("Clipboard is empty"); return; }
    auto* obj = stripClipboard.getDynamicObject();
    auto* arr = obj ? obj->getProperty ("strips").getArray() : nullptr;
    if (arr == nullptr || arr->isEmpty()) { showStatus ("Clipboard is empty"); return; }
    if (selectedLogical.empty()) { showStatus ("Pick a target strip first"); return; }

    recordUndoSnapshot ("Paste strip settings");

    // Round-robin: paste clipboard entries onto the selected logical
    // strips. If selection > clipboard, wraps the clipboard.
    int i = 0;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        restoreStrip (engine, phys, (*arr)[i % arr->size()]);
        ++i;
    }
    lastTrackCount = -1;
    showStatus ("Pasted strip settings onto " + juce::String ((int) selectedLogical.size())
                + " strip(s)");
}

// ─── The rest of the Edit menu ───────────────────────────────────────
void MainComponent::editSoloSelection()
{
    auto& rec = engine.getRecorder();

    // If the engineer has a selection, TOGGLE solo on exactly those logical
    // strips: press A to solo the selection, press A again to clear it. The
    // selection counts as "already soloed" only when every selected strip is
    // soloed -- so the second press releases it.
    if (! selectedLogical.empty())
    {
        bool allSelectedSoloed = true;
        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogical (engine, logical);
            if (phys >= rec.getNumTracks()
                || ! rec.getTrack (phys).soloed.load (std::memory_order_relaxed))
                { allSelectedSoloed = false; break; }
        }

        recordUndoSnapshot (allSelectedSoloed ? "Clear solo" : "Solo selection");
        for (int i = 0; i < rec.getNumTracks(); ++i)   // clean slate either way
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);

        if (allSelectedSoloed)
        {
            showStatus ("Solo cleared");
            return;
        }

        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogical (engine, logical);
            if (phys < rec.getNumTracks())
            {
                rec.getTrack (phys).soloed.store (true, std::memory_order_relaxed);
                if (rec.getTrack (phys).isStereo.load (std::memory_order_relaxed)
                    && phys + 1 < rec.getNumTracks())
                    rec.getTrack (phys + 1).soloed.store (true, std::memory_order_relaxed);
            }
        }
        showStatus ("Soloed " + juce::String ((int) selectedLogical.size()) + " strip(s)");
        return;
    }

    // No selection: toggle -- if any solo on, clear all; else solo all armed.
    bool anySolo = false;
    for (int i = 0; i < rec.getNumTracks(); ++i)
        if (rec.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

    recordUndoSnapshot (anySolo ? "Clear solos" : "Solo armed");
    if (anySolo)
    {
        for (int i = 0; i < rec.getNumTracks(); ++i)
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);
        showStatus ("Cleared all solos");
    }
    else
    {
        int n = 0;
        for (int i = 0; i < rec.getNumTracks(); ++i)
            if (rec.getTrack (i).armed.load (std::memory_order_relaxed))
            { rec.getTrack (i).soloed.store (true, std::memory_order_relaxed); ++n; }
        showStatus ("Soloed " + juce::String (n) + " armed track" + (n == 1 ? "" : "s"));
    }
}

// Option+R -- bulk record-arm toggle over the current strip selection.
// Select the channels you want, then Option+R arms them all in one
// shot; if they're already all armed, the same shortcut disarms them.
// Mirrors across stereo pairs. UI buttons (MIXER / EDIT / PATCH) follow
// automatically off the 10 Hz refresh since they read TrackState.armed.
void MainComponent::armSelection()
{
    if (selectedLogical.empty())
    {
        showStatus ("Select channels first, then Option+R to arm / disarm");
        return;
    }

    auto& rec = engine.getRecorder();

    // Group toggle: arm unless every selected strip is already armed,
    // in which case disarm. Reads the L track of each logical strip.
    bool allArmed = true;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        if (phys < rec.getNumTracks()
            && ! rec.getTrack (phys).armed.load (std::memory_order_relaxed))
        {
            allArmed = false;
            break;
        }
    }
    const bool arm = ! allArmed;

    recordUndoSnapshot (arm ? "Arm selection" : "Disarm selection");
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        if (phys >= rec.getNumTracks()) continue;
        rec.getTrack (phys).armed.store (arm, std::memory_order_relaxed);
        if (rec.getTrack (phys).isStereo.load (std::memory_order_relaxed)
            && phys + 1 < rec.getNumTracks())
            rec.getTrack (phys + 1).armed.store (arm, std::memory_order_relaxed);
    }

    showStatus ((arm ? "Armed " : "Disarmed ")
                + juce::String ((int) selectedLogical.size()) + " selected strip(s)");
}

void MainComponent::editCropToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Set a loop region first, then Crop"); return; }
    if (engine.isRecording())     { showStatus ("Stop recording before cropping"); return; }

    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory())      { showStatus ("No active session to crop"); return; }

    // Multi-part takes (Track_NN_partNN.<ext>) span several files -- cropping
    // those correctly is out of scope, so refuse rather than mangle them.
    // ALL containers: a .wav-only glob never saw a FLAC/AIFF session's parts,
    // so Crop went ahead on exactly the multi-part take this guard exists to
    // protect. Takes are not always WAV -- see CLAUDE.md.
    const auto audioDir = dir.getChildFile ("Audio Files");
    for (auto& f : audioDir.findChildFiles (juce::File::findFiles, false,
             "Track_*.wav;Track_*.flac;Track_*.aif;Track_*.aiff"))
        if (f.getFileNameWithoutExtension().containsIgnoreCase ("_part"))
        { showStatus ("Cannot crop a multi-part take -- use Save Session As"); return; }

    const juce::int64 a = player.getLoopStart();
    const juce::int64 b = player.getLoopEnd();
    const double secs = (double) (b - a) / juce::jmax (1.0, player.getSampleRate());

    // Non-destructive: this only re-points each track's clips at the loop
    // region (the Track_NN.wav files are untouched and the change persists
    // in the .zfproj playlists), so it's reversible. A light confirmation
    // still guards against an accidental whole-session re-clip, since clip
    // edits aren't on the Cmd+Z stack yet.
    constexpr int kCrop = 1, kCancel = 2;
    auto* aw = new juce::AlertWindow ("Crop to loop range?",
        "Trim every track to the " + juce::String (secs, 2) + " s loop region?\n"
        "Non-destructive -- the recordings on disk are untouched and you can "
        "re-crop to the full range to undo.",
        juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Crop",   kCrop,   juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, a, b] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != kCrop) return;
            const auto before = engine.playlistsToJson();
            const int kept = engine.cropToRange (a, b);
            pushClipUndo ("Crop to loop range", before);   // Cmd+Z reverts the crop
            engine.getPlayer().clearLoopRegion();
            if (editPage != nullptr) editPage->repaint();
            saveUILayoutToActiveSession();   // persist the new clip layout
            showStatus ("Cropped to loop region (" + juce::String (kept)
                        + " track(s) with audio) -- non-destructive");
        }),
        false);
}

void MainComponent::editSetRangeToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion())
    {
        // Engineer hit it without a loop set -- start one at the playhead.
        const auto pos = currentPlayheadSamples (engine);
        player.setLoopRegion (pos, pos + (juce::int64) (player.getSampleRate() * 2.0));
        showStatus ("No loop region set -- defaulted to 2 s at the playhead");
        return;
    }
    auto& m = engine.getMarkers();
    m.drop (player.getLoopStart(), "Range In");
    m.drop (player.getLoopEnd(),   "Range Out");
    showStatus ("Range markers placed at loop boundaries");
}

void MainComponent::editToggleSnap()
{
    // Cycle Off -> Markers -> Off. (Bars snap used to exist but went
    // away with the Bars|Beats ruler -- live engineers navigate by
    // markers, not by bar boundaries.)
    snapMode = (SnapMode) (((int) snapMode + 1) % 2);
    snapToMarkers = (snapMode == SnapMode::Markers);
    // Mirror to the engine so splitTrackAtPlayhead + future edit
    // operations honour the same grid the engineer sees.
    engine.setSnapMode (snapMode == SnapMode::Markers ? AudioEngine::SnapMode::Markers
                                                      : AudioEngine::SnapMode::Off);
    showStatus (snapMode == SnapMode::Markers ? "Snap: Markers" : "Snap: OFF");
}

void MainComponent::dropMarkerAndPromptName()
{
    // 1. Drop the marker at the current transport position. Auto-names
    //    it 'Marker N' where N is the new total count. Returns -1 if
    //    no session is active.
    const int newCount = engine.dropMarkerAtCurrentPosition();
    if (newCount < 0)
    {
        showStatus ("No active session -- create or open one before dropping markers");
        return;
    }

    const int rowIndex = newCount - 1;
    const auto defaultName = "Marker " + juce::String (newCount);

    // 2. Apply the default name straight away so the marker is
    //    immediately visible in the timeline + Memory Locations list
    //    with a sensible label, even if the engineer cancels the
    //    rename dialog. Also capture the current EDIT-view zoom so
    //    a later Cmd+N jump restores the engineer's layout (Pro
    //    Tools-style Memory Location recall).
    engine.getMarkers().renameMarker (rowIndex, defaultName);
    if (rowIndex >= 0 && rowIndex < (int) engine.getMarkers().getAll().size()
        && editPage != nullptr)
    {
        auto& mut = const_cast<Marker&> (engine.getMarkers().getAll()[(size_t) rowIndex]);
        mut.zoom = editPage->getZoom();
    }
    engine.getMarkers().save();
    showStatus ("Marker " + juce::String (newCount) + " dropped");

    // 3. Open the rename dialog with the default name selected, so
    //    the engineer can immediately type a meaningful name. Pro
    //    Tools' Memory Locations does exactly this on Enter.
    auto* aw = new juce::AlertWindow ("New marker",
                                      "Name this memory location:",
                                      juce::MessageBoxIconType::NoIcon,
                                      this);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    aw->addTextEditor ("markerName", defaultName, {});
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog::primeNameEditor (*aw, "markerName");   // focus + select-all + Enter = OK

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, rowIndex, defaultName] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self == nullptr) return;
            if (result != 1) return;            // Cancel keeps the auto-name

            const auto typed = dispose->getTextEditorContents ("markerName").trim();
            if (typed.isEmpty() || typed == defaultName) return;

            self->engine.getMarkers().renameMarker (rowIndex, typed);
            self->engine.getMarkers().save();
            self->showStatus ("Marker " + juce::String (rowIndex + 1)
                              + " renamed to: " + typed);
        }),
        false);
}

void MainComponent::showMarkersDialog()
{
    zynforge::MarkerListDialog::launch (engine);
}

void MainComponent::runAutomationEdit (const juce::String& label,
                                       std::function<void()> mutate)
{
    if (! mutate) return;
    const auto before = engine.automationToJson();
    mutate();
    const auto after  = engine.automationToJson();

    undoManager.beginNewTransaction (label);
    undoManager.perform (new AutomationSnapshotAction (engine, before, after));
    if (editPage != nullptr) editPage->repaint();
}

void MainComponent::beginAutomationTransaction()
{
    // Re-entry guard. If a drag fires mouseDown again before mouseUp
    // (shouldn't happen, but JUCE event ordering occasionally bunches
    // things up under stress), the earlier snapshot wins.
    if (automationTransactionOpen) return;
    pendingAutomationBefore = engine.automationToJson();
    automationTransactionOpen = true;
}

void MainComponent::endAutomationTransaction (const juce::String& label)
{
    if (! automationTransactionOpen) return;
    automationTransactionOpen = false;
    const auto after = engine.automationToJson();
    undoManager.beginNewTransaction (label);
    undoManager.perform (new AutomationSnapshotAction (engine,
                                                       pendingAutomationBefore,
                                                       after));
    pendingAutomationBefore = juce::var();
    if (editPage != nullptr) editPage->repaint();
}

// The physical tracks an edit acts on: the selected strips (incl. their
// stereo partners), or -- Pro Tools-style -- EVERY track when nothing is
// explicitly selected, so a bare edit gesture just works.
std::vector<int> MainComponent::tracksToEditPhysical()
{
    auto& rec = engine.getRecorder();
    std::set<int> picked;   // dedupe across stereo partners + edit groups
    if (! selectedLogical.empty())
    {
        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogical (engine, logical);
            if (phys < 0 || phys >= rec.getNumTracks()) continue;
            picked.insert (phys);
            if (phys + 1 < rec.getNumTracks()
                && rec.getTrack (phys).isStereo.load (std::memory_order_relaxed))
                picked.insert (phys + 1);   // stereo pair edits together
            // EDIT-group peers edit together (phase-coherent multitrack).
            const int g = engine.getTrackEditGroup (phys);
            if (g >= 0)
                for (int p : engine.getStripsInEditGroup (g))
                    if (p >= 0 && p < rec.getNumTracks()) picked.insert (p);
        }
    }
    else
    {
        for (int i = 0; i < rec.getNumTracks(); ++i) picked.insert (i);
    }
    return { picked.begin(), picked.end() };
}

void MainComponent::editSplitAtPlayhead()
{
    auto pos = engine.snapSampleToGrid (currentPlayheadSamples (engine));

    // Clip-level split across the target tracks (selected, else all).
    const auto targets = tracksToEditPhysical();

    // Zero-crossing snap (click-free cuts): nudge the cut to the nearest zero
    // crossing within ±5 ms, using the first target track as the reference so
    // every track is cut at the SAME sample (a coherent multitrack edit).
    if (zeroCrossSnap && ! targets.empty())
        pos = engine.nearestZeroCrossing (targets.front(), pos, 240);

    engine.getMarkers().drop (pos, "Split");

    const auto before = engine.playlistsToJson();   // clip-aware undo
    int splits = 0;
    for (int phys : targets)
        if (engine.splitTrackAtSample (phys, pos)) ++splits;
    if (splits > 0)
        pushClipUndo ("Split clips at playhead", before);

    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus ((splits > 0
                    ? "Split " + juce::String (splits) + " track(s) and dropped marker"
                    : juce::String ("Split marker dropped"))
                + " at "
                + juce::String ((double) pos
                                / juce::jmax (1.0, engine.getPlayer().getSampleRate()), 2) + " s");
}

// Pro Tools 'Separate' (B): isolate the selected range as its own clip on
// every target track by cutting at both range edges. With no range, falls
// back to a plain split at the playhead.
void MainComponent::editSeparateAtSelection()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion())
    {
        editSplitAtPlayhead();
        return;
    }
    const auto a = player.getLoopStart();
    const auto b = player.getLoopEnd();
    const auto before = engine.playlistsToJson();
    int n = 0;
    for (int phys : tracksToEditPhysical())
    {
        bool did  = engine.splitTrackAtSample (phys, a);
        did       = engine.splitTrackAtSample (phys, b) || did;
        if (did) ++n;
    }
    if (n > 0) pushClipUndo ("Separate selection", before);
    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus (n > 0 ? "Separated selection on " + juce::String (n) + " track(s)"
                      : juce::String ("Nothing to separate in the selection"));
}

// Heal Separation (Cmd+H): rejoin a clean split at the playhead, or every
// healable split inside the selected range, on the target tracks.
void MainComponent::editHealSeparation()
{
    auto& player = engine.getPlayer();
    const auto before = engine.playlistsToJson();
    int n = 0;
    if (player.hasLoopRegion())
    {
        const auto a = player.getLoopStart(), b = player.getLoopEnd();
        for (int phys : tracksToEditPhysical()) n += engine.healSeparationRange (phys, a, b);
    }
    else
    {
        const auto pos = currentPlayheadSamples (engine);
        for (int phys : tracksToEditPhysical()) if (engine.healSeparationAt (phys, pos)) ++n;
    }
    if (n > 0) pushClipUndo ("Heal separation", before);
    if (editPage != nullptr) editPage->repaint();
    showStatus (n > 0 ? "Healed " + juce::String (n) + " separation(s)"
                      : juce::String ("No healable split here (clips were moved or trimmed apart)"));
}

// Zoom to Selection (E): fit the selected range to the EDIT viewport.
void MainComponent::editZoomToSelection()
{
    auto& player = engine.getPlayer();
    if (editPage == nullptr) return;
    if (! player.hasLoopRegion())
    { showStatus ("Select a range first (Range tool, or , and .), then E to zoom"); return; }
    editPage->zoomToSamples (player.getLoopStart(), player.getLoopEnd());
}

// Strip Silence: open a Pro Tools-style settings box with draggable SLIDERS
// (threshold / min strip / min clip / pad), then separate each target track's
// take around the silence.
void MainComponent::editStripSilence()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded()) { showStatus ("Load or record a session first"); return; }

    // Four labelled horizontal sliders, themed with the app LookAndFeel.
    struct SilencePanel final : public juce::Component
    {
        juce::Slider thr, sil, clip, pad;
        juce::Label  thrL, silL, clipL, padL;
        SilencePanel (juce::LookAndFeel& lf)
        {
            setLookAndFeel (&lf);
            auto add = [this, &lf] (juce::Slider& s, juce::Label& l, const juce::String& name,
                                    double lo, double hi, double step, double def, const juce::String& suffix)
            {
                l.setText (name, juce::dontSendNotification);
                l.setColour (juce::Label::textColourId, brand::textPrimary);
                l.setFont (brand::type::uiBody());
                addAndMakeVisible (l);
                s.setLookAndFeel (&lf);
                s.setSliderStyle (juce::Slider::LinearHorizontal);
                s.setRange (lo, hi, step);
                s.setValue (def, juce::dontSendNotification);
                s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
                s.setTextValueSuffix (suffix);
                addAndMakeVisible (s);
            };
            add (thr,  thrL,  "Strip threshold",     -90.0, 0.0,    1.0, -42.0, " dB");
            add (sil,  silL,  "Min strip / silence",   0.0, 2000.0, 10.0, 300.0, " ms");
            add (clip, clipL, "Min clip length",       0.0, 2000.0, 10.0, 150.0, " ms");
            add (pad,  padL,  "Clip start/end pad",    0.0,  500.0,  5.0,  20.0, " ms");
            setSize (460, 232);
        }
        ~SilencePanel() override
        {
            for (auto* s : { &thr, &sil, &clip, &pad }) s->setLookAndFeel (nullptr);
            setLookAndFeel (nullptr);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced (2);
            auto row = [&r] (juce::Label& l, juce::Slider& s)
            {
                auto rr = r.removeFromTop (56);
                l.setBounds (rr.removeFromTop (20));
                s.setBounds (rr.reduced (0, 2));
            };
            row (thrL, thr); row (silL, sil); row (clipL, clip); row (padL, pad);
        }
    };

    auto panel = std::make_shared<SilencePanel> (getLookAndFeel());
    auto* aw = new juce::AlertWindow ("Strip Silence",
        "Separate clips around silence on the selected track(s), then drop the gaps.",
        juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&getLookAndFeel());
    aw->addCustomComponent (panel.get());
    aw->addButton ("Strip",  1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([self, aw, panel] (int r)
    {
        std::unique_ptr<juce::AlertWindow> own (aw);
        if (r != 1 || self == nullptr) return;
        const float  thr = (float) panel->thr.getValue();
        const double sr  = juce::jmax (1.0, self->engine.getPlayer().getSampleRate());
        auto ms = [sr] (juce::Slider& s) { return (juce::int64) (sr * s.getValue() / 1000.0); };
        const auto before = self->engine.playlistsToJson();
        int tracks = 0, clips = 0;
        for (int phys : self->tracksToEditPhysical())
        {
            const int n = self->engine.stripSilence (phys, thr, ms (panel->sil), ms (panel->clip), ms (panel->pad));
            if (n > 0) { ++tracks; clips += n; }
        }
        if (tracks > 0) self->pushClipUndo ("Strip silence", before);
        if (self->editPage != nullptr) self->editPage->repaint();
        self->showStatus (tracks > 0 ? "Strip silence: " + juce::String (clips) + " clip(s) on "
                                        + juce::String (tracks) + " track(s)"
                                     : juce::String ("Strip silence found nothing to separate"));
    }));
}

// Consolidate: flatten the selected range on each target track into one new
// file (bakes in gains / fades / clip order).
void MainComponent::editConsolidateSelection()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Select a range first, then Consolidate"); return; }
    const auto a = player.getLoopStart(), b = player.getLoopEnd();
    const auto before = engine.playlistsToJson();
    int n = 0;
    for (int phys : tracksToEditPhysical()) if (engine.consolidateRange (phys, a, b)) ++n;
    if (n > 0) pushClipUndo ("Consolidate", before);
    if (editPage != nullptr) editPage->repaint();
    showStatus (n > 0 ? "Consolidated " + juce::String (n) + " track(s)"
                      : juce::String ("Nothing to consolidate in the selection"));
}

// Clear (Delete): remove the audio inside the selected range on every
// target track, leaving a gap. Non-destructive -- the files are untouched
// and Cmd+Z restores the clips.
void MainComponent::editClearRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion())
    {
        showStatus ("Select a range first (Selector tool, or , and .) then Delete to clear");
        return;
    }
    const auto a = player.getLoopStart();
    const auto b = player.getLoopEnd();
    const auto before = engine.playlistsToJson();
    int n = 0;
    for (int phys : tracksToEditPhysical())
        if (engine.clearTrackRange (phys, a, b)) ++n;
    if (n > 0) pushClipUndo ("Clear selection", before);
    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus (n > 0 ? "Cleared selection on " + juce::String (n) + " track(s)"
                      : juce::String ("Nothing inside the selection to clear"));
}

// ─── Operations on the clicked (selected) clip(s) ────────────────────
void MainComponent::editDeleteSelectedClip()
{
    if (editPage == nullptr) return;
    std::vector<std::pair<int, int>> sel (editPage->getSelectedClips().begin(),
                                          editPage->getSelectedClips().end());
    if (sel.empty()) { showStatus ("No clip selected -- click a clip first"); return; }
    // Delete higher clip indices first so lower ones stay valid (per track,
    // and harmless across tracks since their lists are independent).
    std::sort (sel.begin(), sel.end(),
               [] (auto& a, auto& b) { return a.second > b.second; });
    const auto before = engine.playlistsToJson();
    int deleted = 0;
    for (auto& [track, idx] : sel)
        if (engine.deleteClip (track, idx)) ++deleted;
    if (deleted > 0) pushClipUndo ("Delete clip(s)", before);
    editPage->clearSelectedClip();
    editPage->repaint();
    showStatus (deleted > 0 ? "Deleted " + juce::String (deleted) + " clip(s)"
                            : juce::String ("Clip is locked -- unlock before deleting"));
}

void MainComponent::editDuplicateSelectedClip()
{
    if (editPage == nullptr) return;
    const int track = editPage->getSelectedClipTrack();
    const int idx   = editPage->getSelectedClipIndex();
    if (track < 0 || idx < 0) { showStatus ("No clip selected -- click a clip first"); return; }
    const auto before = engine.playlistsToJson();
    if (engine.duplicateClip (track, idx))
    {
        pushClipUndo ("Duplicate clip", before);
        editPage->setSelectedClip (track, idx + 1);   // follow the new copy
        editPage->repaint();
        showStatus ("Duplicated clip");
    }
    else showStatus ("Clip is locked -- unlock it before duplicating");
}

void MainComponent::editNudgeSelectedClip (int dir)
{
    if (editPage == nullptr) return;
    const auto& sel = editPage->getSelectedClips();
    if (sel.empty()) { showStatus ("No clip selected -- click a clip first"); return; }
    const double sr   = juce::jmax (1.0, engine.getPlayer().getSampleRate());
    const juce::int64 step = (juce::int64) (sr * (double) nudgeMs / 1000.0) * (dir > 0 ? 1 : -1);
    const auto before = engine.playlistsToJson();
    int n = 0;
    // Move doesn't reindex clips, so iterating the selection set is safe.
    for (auto& [track, idx] : sel)
        if (engine.editClip (track, idx, AudioEngine::ClipEdit::Move, step)) ++n;
    if (n > 0)
    {
        pushClipUndo ("Nudge clip(s)", before);
        editPage->repaint();
        showStatus ("Nudged " + juce::String (n) + " clip(s) " + (dir > 0 ? "right" : "left")
                    + " " + juce::String (nudgeMs) + " ms");
    }
}

void MainComponent::editCycleNudgeValue()
{
    static const int steps[] = { 10, 25, 50, 100, 250, 500, 1000 };
    int idx = 0;
    for (int i = 0; i < (int) (sizeof (steps) / sizeof (int)); ++i)
        if (steps[i] == nudgeMs) { idx = i; break; }
    nudgeMs = steps[(idx + 1) % (int) (sizeof (steps) / sizeof (int))];
    if (auto* props = engine.getAppProps()) props->setValue ("editNudgeMs", nudgeMs);
    showStatus ("Nudge value: " + juce::String (nudgeMs) + " ms"
                + "  (Alt+Left/Right or numpad +/- to nudge the selected clip)");
}

// Cmd+X / Cmd+C -- cut/copy the selected AUDIO clip when one is picked in
// the EDIT view, else the strip-settings clipboard (legacy behaviour).
void MainComponent::editClipboardCut (bool cut)
{
    if (currentView == View::Edit && editPage != nullptr
        && editPage->getSelectedClipTrack() >= 0)
    {
        const int track   = editPage->getSelectedClipTrack();
        const int idx     = editPage->getSelectedClipIndex();
        const auto* clips = engine.tryClipsFor (track);
        if (clips != nullptr && idx >= 0 && idx < (int) clips->size())
        {
            const auto& c = (*clips)[(size_t) idx];
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("track",      track);
            obj->setProperty ("fileStart",  (juce::int64) c.fileStartSamples);
            obj->setProperty ("fileLength", (juce::int64) c.fileLengthSamples);
            obj->setProperty ("fadeIn",     (juce::int64) c.fadeInSamples);
            obj->setProperty ("fadeOut",    (juce::int64) c.fadeOutSamples);
            obj->setProperty ("gainDb",     (double) c.gainDb);
            obj->setProperty ("name",       c.name);
            clipClipboard = juce::var (obj);
            showStatus (juce::String (cut ? "Cut" : "Copied") + " clip -- Cmd+V pastes it at the playhead");
            if (cut) editDeleteSelectedClip();
            return;
        }
    }
    editCutSelected (cut);   // strip-settings fallback
}

// Cmd+V -- paste the copied audio clip at the playhead on its source
// track, else paste strip settings.
void MainComponent::editClipboardPaste()
{
    if (currentView == View::Edit && clipClipboard.isObject())
    {
        if (auto* obj = clipClipboard.getDynamicObject())
            if (obj->hasProperty ("fileLength"))
            {
                const int sourceTrack = (int) obj->getProperty ("track");
                // Paste onto the active EDIT row when one is set; else the
                // clip's own track. Cross-track paste references the source
                // track's file so the destination plays the copied audio.
                int target = sourceTrack;
                if (editPage != nullptr && editPage->getActiveRowTrackIndex() >= 0)
                    target = editPage->getActiveRowTrackIndex();

                juce::File audioFile;   // empty = same-track
                if (target != sourceTrack)
                {
                    auto dir = engine.getActiveSessionDir().getChildFile ("Audio Files");
                    audioFile = dir.getChildFile (
                        juce::String::formatted ("Track_%02d.wav", sourceTrack + 1));
                }

                const auto pos    = currentPlayheadSamples (engine);
                const auto before = engine.playlistsToJson();
                const int newIdx  = engine.pasteClip (
                    target, pos,
                    (juce::int64) obj->getProperty ("fileStart"),
                    (juce::int64) obj->getProperty ("fileLength"),
                    (juce::int64) obj->getProperty ("fadeIn"),
                    (juce::int64) obj->getProperty ("fadeOut"),
                    (float) (double) obj->getProperty ("gainDb"),
                    obj->getProperty ("name").toString(), audioFile);
                if (newIdx >= 0)
                {
                    pushClipUndo ("Paste clip", before);
                    if (editPage != nullptr) editPage->setSelectedClip (target, newIdx);
                    if (editPage != nullptr) editPage->repaint();
                    showStatus (target == sourceTrack ? juce::String ("Pasted clip at the playhead")
                                                      : "Pasted clip onto track " + juce::String (target + 1));
                }
                else showStatus ("Paste failed -- no audio on the source track");
                return;
            }
    }
    editPasteSelected();   // strip-settings fallback
}

// Shift+Delete: ripple-delete the selected clip (or range), closing the
// gap so later audio slides earlier.
void MainComponent::editRippleDelete()
{
    if (editPage != nullptr && editPage->getSelectedClipTrack() >= 0)
    {
        const int track   = editPage->getSelectedClipTrack();
        const int idx     = editPage->getSelectedClipIndex();
        const auto* clips = engine.tryClipsFor (track);
        if (clips == nullptr || idx < 0 || idx >= (int) clips->size())
        { showStatus ("No clip selected"); return; }
        const auto a = (*clips)[(size_t) idx].timelineStartSamples;
        const auto b = a + (*clips)[(size_t) idx].fileLengthSamples;
        const auto before = engine.playlistsToJson();
        if (engine.rippleDeleteRange (track, a, b))
        {
            pushClipUndo ("Ripple delete clip", before);
            editPage->clearSelectedClip();
            editPage->repaint();
            showStatus ("Ripple-deleted clip -- gap closed");
        }
        return;
    }
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Select a clip or a range first"); return; }
    const auto a = player.getLoopStart();
    const auto b = player.getLoopEnd();
    const auto before = engine.playlistsToJson();
    int n = 0;
    for (int phys : tracksToEditPhysical())
        if (engine.rippleDeleteRange (phys, a, b)) ++n;
    if (n > 0) pushClipUndo ("Ripple delete selection", before);
    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus (n > 0 ? "Ripple-deleted selection on " + juce::String (n) + " track(s) -- gap closed"
                      : juce::String ("Nothing to ripple-delete"));
}

void MainComponent::editStartRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto end = player.hasLoopRegion() ? player.getLoopEnd()
                                            : pos + (juce::int64) (player.getSampleRate() * 2.0);
    // Ordering guard (same as TimelineStrip's "Set as Loop In"): if the
    // playhead is at/past the existing loop end, keep end > start instead of
    // passing an inverted range that setLoopRegion treats as "clear".
    player.setLoopRegion (pos, juce::jmax (end, pos + 1));
    engine.getMarkers().drop (pos, "Range In");
    showStatus ("Range In set at playhead");
}

void MainComponent::editFinishRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto start = player.hasLoopRegion() ? player.getLoopStart() : juce::int64 (0);
    // Ordering guard (same as TimelineStrip's "Set as Loop Out"): if the
    // playhead is at/before the existing loop start, keep start < end instead
    // of passing an inverted range that setLoopRegion treats as "clear".
    player.setLoopRegion (juce::jmin (start, pos - 1), pos);
    engine.getMarkers().drop (pos, "Range Out");
    showStatus ("Range Out set at playhead");
}

// ─── Setlist plumbing ────────────────────────────────────────────────
//
// Cues live as a JSON array under the session's .zfproj document:
//   "setlist": [ {"name": "Intro", "samplePos": 0}, ... ]
// Loaded on session swap, persisted on every add / update so a crash
// mid-show doesn't lose what the engineer just dialled in.



