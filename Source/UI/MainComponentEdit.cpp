// Edit-operation methods on MainComponent (undo/redo, cut/copy/paste,
// solo, crop, split, range markers, marker drop). Extracted from
// MainComponent.cpp as part of the
// 2026-05-24 god-class split. Every method is a member of
// MainComponent and accesses private state via the usual
// out-of-line member-definition path.
//
// The anonymous-namespace block carries the strip-snapshot helpers
// and the UndoableAction subclasses. They are only used by the edit
// cluster -- moved with it so the file is self-contained.

#include "MainComponent.h"
#include "../Theme/DialogChrome.h"
#include "ClipEditTargets.h"
#include "MarkerListDialog.h"
#include "MixerSnapshot.h"
#include "RecordPosition.h"

#include <algorithm>
#include <map>
#include <vector>
#include <set>

using namespace zynforge;

namespace
{
    // Strip snapshot/restore for Cut / Copy / Paste and mixer undo share one
    // field table with the undo change fingerprint (MixerSnapshot.h).
    using zynforge::mixersnapshot::snapshotStrip;
    using zynforge::mixersnapshot::restoreStrip;

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
                // The gesture is already applied. Recording its undo state
                // must not replay every strip's synchronous settings writes.
                return true;
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
            eng.restorePlaylistSnapshot (afterState);
            return true;
        }
        bool undo() override
        {
            eng.restorePlaylistSnapshot (beforeState);
            return true;
        }

        zynforge::AudioEngine& eng;
        juce::var beforeState, afterState;
        bool      firstRun { true };
    };

    // A marker dropped as part of an edit. It is already in the list when this
    // is pushed.
    struct MarkerDropAction final : public juce::UndoableAction
    {
        MarkerDropAction (MarkersManager& m, Marker dropped) : markers (m), marker (std::move (dropped)) {}

        bool perform() override
        {
            if (firstRun) { firstRun = false; return true; }
            markers.insert (marker);
            return true;
        }
        bool undo() override { markers.removeById (marker.runtimeId); return true; }

        MarkersManager& markers;
        Marker marker;
        bool firstRun { true };
    };

    // Crop / Ripple Delete: the edit has already happened when this is pushed.
    struct TimelineCutAction final : public juce::UndoableAction
    {
        TimelineCutAction (std::function<void()> redoFn, std::function<void()> undoFn)
            : redo (std::move (redoFn)), revert (std::move (undoFn)) {}

        bool perform() override
        {
            if (firstRun) { firstRun = false; return true; }
            redo();
            return true;
        }
        bool undo() override { revert(); return true; }

        std::function<void()> redo, revert;
        bool firstRun { true };
    };

    // Markers and cues are not on the undo stack, so one added since the step
    // (a take's scene marker, say) must not vanish with it.
    template <class Item, class Same>
    void keepAddedSince (std::vector<Item>& target, const std::vector<Item>& stepState,
                         const std::vector<Item>& current, Same same)
    {
        for (const auto& item : current)
            if (std::none_of (stepState.begin(), stepState.end(),
                              [&] (const Item& known) { return same (known, item); }))
                target.push_back (item);
    }
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
MainComponent::TimelineSnapshot MainComponent::captureTimeline() const
{
    return { engine.captureTimelineState(), cues };
}

void MainComponent::restoreTimeline (TimelineSnapshot target, const TimelineSnapshot& replaced)
{
    keepAddedSince (target.engineState.markers, replaced.engineState.markers,
                    engine.getMarkers().getAll(),
                    [] (const Marker& a, const Marker& b) { return a.runtimeId == b.runtimeId; });
    keepAddedSince (target.cues, replaced.cues, cues,
                    [] (const SetlistBar::Cue& a, const SetlistBar::Cue& b)
                    { return a.name == b.name && a.samplePos == b.samplePos; });

    engine.restoreTimelineState (target.engineState);
    cues = std::move (target.cues);
    if (currentCueIndex >= (int) cues.size()) currentCueIndex = -1;
    setlistBar.setCues (cues, currentCueIndex);
    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    saveCueChanges ({});
}

// Cues move with the audio like markers do.
std::vector<SetlistBar::Cue> MainComponent::cuesAfterCut (const std::vector<SetlistBar::Cue>& cues,
                                                          const timelinecut::Cut& cut,
                                                          bool wholeTimeline)
{
    if (! wholeTimeline) return cues;
    std::vector<SetlistBar::Cue> kept;
    for (auto cue : cues)
        if (const auto pos = cut.map (cue.samplePos))
        {
            cue.samplePos = *pos;
            kept.push_back (std::move (cue));
        }
    return kept;
}

// The engine has moved the clips and markers; this finishes the
// edit. The save is immediate because markers.json is already written: a crash
// must not leave the markers moved and the clips not.
void MainComponent::commitTimelineCut (const juce::String& label, const TimelineSnapshot& before,
                                       const timelinecut::Cut& cut, const std::vector<int>& tracks)
{
    cues = cuesAfterCut (cues, cut, engine.namesEveryStrip (tracks));
    finishTimelineEdit (label, before);
}

void MainComponent::finishTimelineEdit (const juce::String& label, const TimelineSnapshot& before)
{
    if (currentCueIndex >= (int) cues.size()) currentCueIndex = -1;
    setlistBar.setCues (cues, currentCueIndex);

    auto after = captureTimeline();
    undoManager.beginNewTransaction (label);
    undoManager.perform (new TimelineCutAction (
        [this, before, after] { restoreTimeline (after, before); },
        [this, before, after] { restoreTimeline (before, after); }));
    saveCueChanges ({});
}

int MainComponent::rippleTimeline (const juce::String& label, const std::vector<int>& tracks,
                                   juce::int64 start, juce::int64 end)
{
    const timelinecut::Cut cut { start, end, false };
    if (const auto refusal = engine.timelineCutRefusal (cut, tracks); refusal.isNotEmpty())
    {
        showStatus (refusal);
        return -1;
    }
    const auto before = captureTimeline();
    const int changed = engine.rippleDeleteTracks (tracks, start, end);
    if (changed > 0) commitTimelineCut (label, before, cut, tracks);
    return changed;
}

// Every gap in one edit: one undo step and one save. The save starts a metadata
// job, so an edit per gap would have the first one refuse the rest.
int MainComponent::rippleSpans (const juce::String& label,
                                const std::vector<zynforge::clipedit::RippleSpan>& spans)
{
    for (const auto& span : spans)
        if (const auto refusal = engine.timelineCutRefusal ({ span.start, span.end, false }, span.tracks);
            refusal.isNotEmpty())
        {
            showStatus (refusal);
            return -1;
        }
    const auto before = captureTimeline();
    int changed = 0;
    for (const auto& span : spans)
    {
        const int moved = engine.rippleDeleteTracks (span.tracks, span.start, span.end);
        if (moved <= 0) continue;
        changed += moved;
        cues = cuesAfterCut (cues, { span.start, span.end, false },
                             engine.namesEveryStrip (span.tracks));
    }
    if (changed > 0) finishTimelineEdit (label, before);
    return changed;
}

// Commit a mixer change still settling, so a bulk action starts from a clean
// baseline and Cmd+Z undoes the latest move.
void MainComponent::flushMixerUndo()
{
    if (! mixerUndoReady) return;
    const auto fp = zynforge::mixersnapshot::fingerprint (engine);
    const bool structural = engine.getRecorder().getNumTracks() != committedMixerCount;
    if (fp == committedMixerFp || structural) return;
    undoManager.beginNewTransaction ("Mixer change");
    undoManager.perform (new MixerSnapshotAction (engine, committedMixerState));
    committedMixerState = captureMixerSnapshot();
    committedMixerFp = fp;
    lastSeenMixerFp = fp;
    mixerChangeStableTicks = 0;
}

juce::var MainComponent::beginMixerUndo()
{
    flushMixerUndo();
    return captureMixerSnapshot();
}

// Runs after the change: MixerSnapshotAction reads its "after" state when
// performed, so pushing it first recorded nothing and left the real change for
// the settle poll to add as a second step.
void MainComponent::commitMixerUndo (const juce::String& label, const juce::var& before)
{
    undoManager.beginNewTransaction (label);
    undoManager.perform (new MixerSnapshotAction (engine, before));
    rebaselineMixerUndo();
}

juce::var MainComponent::captureMixerSnapshot()
{
    return zynforge::mixersnapshot::snapshotAll (engine);
}

void MainComponent::rebaselineMixerUndo()
{
    committedMixerState  = captureMixerSnapshot();
    committedMixerFp     = zynforge::mixersnapshot::fingerprint (engine);
    committedMixerCount  = engine.getRecorder().getNumTracks();
    lastSeenMixerFp      = committedMixerFp;
    mixerChangeStableTicks = 0;
    mixerUndoReady = true;
}

void MainComponent::pollMixerUndo()
{
    // Never turn the act of recording into an undo step -- arm flags churn
    // while rolling, and a captured take must not be undoable. Mark the
    // baseline stale instead of rebuilding it every tick of the take; the
    // first idle tick after the take re-baselines once.
    if (engine.isRecording()) { mixerUndoReady = false; return; }

    // Change detection is the allocation-free fingerprint; the JSON snapshot
    // is built only when a baseline is (re)taken or a step is committed.
    const auto fp = zynforge::mixersnapshot::fingerprint (engine);
    if (! mixerUndoReady) { rebaselineMixerUndo(); return; }

    // A strip count change is structural (add / remove channel) -- re-baseline
    // rather than snapshot (MixerSnapshotAction restores per-existing-strip and
    // can't grow/shrink the mixer). Add/remove-channel undo is a separate task.
    if (engine.getRecorder().getNumTracks() != committedMixerCount) { rebaselineMixerUndo(); return; }

    if (fp == committedMixerFp) { lastSeenMixerFp = fp; mixerChangeStableTicks = 0; return; }

    // Changed vs the committed baseline. Wait for it to stop moving (so a fader
    // drag commits one step) by counting ticks where the state held steady.
    if (fp != lastSeenMixerFp) { lastSeenMixerFp = fp; mixerChangeStableTicks = 0; return; }
    if (++mixerChangeStableTicks < 3) return;   // ~300 ms settle at 10 Hz

    undoManager.beginNewTransaction ("Mixer change");
    undoManager.perform (new MixerSnapshotAction (engine, committedMixerState));
    committedMixerState = captureMixerSnapshot();
    committedMixerFp    = fp;
    mixerChangeStableTicks = 0;
}

void MainComponent::confirmDeleteRecording (bool ripple, std::function<void()> onConfirm)
{
    // Asked over a rolling take, this would stand in front of STOP for an edit
    // the take refuses anyway.
    if (! canMutateTimeline ("deleting audio")) return;
    if (confirmForTests) { confirmForTests (std::move (onConfirm)); return; }
    juce::Component::SafePointer<MainComponent> self (this);
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
        [self, onConfirm] (int result)
        { if (result == 1 && self != nullptr && onConfirm) onConfirm(); });
}

void MainComponent::confirmDeleteChannels (std::function<void()> onConfirm)
{
    confirmDeleteChannels ((int) selectedLogical.size(), std::move (onConfirm));
}

void MainComponent::confirmDeleteChannels (int n, std::function<void()> onConfirm)
{
    if (n <= 0 || ! canMutateTimeline ("deleting a channel")) return;
    if (confirmForTests) { confirmForTests (std::move (onConfirm)); return; }

    juce::Component::SafePointer<MainComponent> self (this);
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle (n > 1 ? "Delete channels?" : "Delete channel?")
            .withMessage (juce::String (n > 1
                ? "Delete the " + juce::String (n) + " selected channels?"
                : "Delete this channel?")
                + "\n\nRecorded files stay on disk -- this removes the "
                  "channel(s) from the session. It clears the undo history, "
                  "so Cmd+Z cannot bring them back.")
            .withButton ("Delete")
            .withButton ("Cancel"),
        // First of two buttons gets commandID 1 -- "Delete" returns 1.
        [self, onConfirm] (int result)
        { if (result == 1 && self != nullptr && onConfirm) onConfirm(); });
}

void MainComponent::editUndo()
{
    if (! canMutateTimeline()) return;
    // Commit any in-flight mixer change NOW so Cmd+Z immediately after a fader
    // / rename / colour move undoes that move -- the timer's ~300 ms settle
    // wouldn't have recorded it as an undo step yet.
    flushMixerUndo();

    if (! undoManager.canUndo()) { showStatus ("Nothing to undo"); return; }
    const auto undone = undoManager.getUndoDescription();   // read before undo() moves the stack
    undoManager.undo();
    lastTrackCount = -1;
    rebaselineMixerUndo();   // the restored state is the new baseline, not a fresh change
    showStatus ("Undo: " + undone);
}

void MainComponent::editRedo()
{
    if (! canMutateTimeline()) return;
    if (! undoManager.canRedo()) { showStatus ("Nothing to redo"); return; }
    const auto redone = undoManager.getRedoDescription();
    undoManager.redo();
    lastTrackCount = -1;
    rebaselineMixerUndo();   // the restored state is the new baseline, not a fresh change
    showStatus ("Redo: " + redone);
}

// ─── Cut / copy / paste / delete on the strip selection ──────────────
void MainComponent::editCutSelected (bool cut)
{
    if (cut && ! canMutateTimeline()) return;
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
    if (! cut)
    {
        showStatus ("Copied " + zynforge::counted (arr.size(), "strip setting"));
        return;
    }
    // Removing a channel takes its audio out of the session and clears undo,
    // so a cut asks first like every other way of deleting one.
    confirmDeleteChannels ([this] { deleteSelectedStrips(); });
}

void MainComponent::editPasteSelected()
{
    if (! canMutateTimeline()) return;
    if (! stripClipboard.isObject()) { showStatus ("Clipboard is empty"); return; }
    auto* obj = stripClipboard.getDynamicObject();
    auto* arr = obj ? obj->getProperty ("strips").getArray() : nullptr;
    if (arr == nullptr || arr->isEmpty()) { showStatus ("Clipboard is empty"); return; }
    if (selectedLogical.empty()) { showStatus ("Pick a target strip first"); return; }

    const auto before = beginMixerUndo();

    // Round-robin: paste clipboard entries onto the selected logical
    // strips. If selection > clipboard, wraps the clipboard.
    int i = 0;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        restoreStrip (engine, phys, (*arr)[i % arr->size()]);
        ++i;
    }
    commitMixerUndo ("Paste strip settings", before);
    lastTrackCount = -1;
    showStatus ("Pasted strip settings onto " + zynforge::counted ((int) selectedLogical.size(), "strip"));
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

        const auto before = beginMixerUndo();
        for (int i = 0; i < rec.getNumTracks(); ++i)   // clean slate either way
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);

        if (allSelectedSoloed)
        {
            commitMixerUndo ("Clear solo", before);
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
        commitMixerUndo ("Solo selection", before);
        showStatus ("Soloed " + zynforge::counted ((int) selectedLogical.size(), "strip"));
        return;
    }

    // No selection: toggle -- if any solo on, clear all; else solo all armed.
    bool anySolo = false;
    for (int i = 0; i < rec.getNumTracks(); ++i)
        if (rec.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

    const auto before = beginMixerUndo();
    if (anySolo)
    {
        for (int i = 0; i < rec.getNumTracks(); ++i)
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);
        commitMixerUndo ("Clear solos", before);
        showStatus ("Cleared all solos");
    }
    else
    {
        int n = 0;
        for (int i = 0; i < rec.getNumTracks(); ++i)
            if (rec.getTrack (i).armed.load (std::memory_order_relaxed))
            { rec.getTrack (i).soloed.store (true, std::memory_order_relaxed); ++n; }
        if (n > 0) commitMixerUndo ("Solo armed", before);
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
    if (engine.isRecording()) { showStatus ("Stop recording before changing record arms"); return; }
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

    const auto before = beginMixerUndo();
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        if (phys >= rec.getNumTracks()) continue;
        rec.getTrack (phys).armed.store (arm, std::memory_order_relaxed);
        if (rec.getTrack (phys).isStereo.load (std::memory_order_relaxed)
            && phys + 1 < rec.getNumTracks())
            rec.getTrack (phys + 1).armed.store (arm, std::memory_order_relaxed);
    }
    commitMixerUndo (arm ? "Arm selection" : "Disarm selection", before);

    showStatus ((arm ? "Armed " : "Disarmed ")
                + zynforge::counted ((int) selectedLogical.size(), "selected strip"));
}

void MainComponent::editCropToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Set a loop region first, then Crop"); return; }
    if (! canMutateTimeline ("cropping")) return;

    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory())      { showStatus ("No active session to crop"); return; }

    const juce::int64 a = player.getLoopStart();
    const juce::int64 b = player.getLoopEnd();
    const timelinecut::Cut cut { a, b, true };
    if (const auto refusal = engine.timelineCutRefusal (cut, engine.allTimelineTracks());
        refusal.isNotEmpty())
    { showStatus (refusal); return; }
    const double secs = (double) (b - a) / juce::jmax (1.0, player.getSampleRate());

    constexpr int kCrop = 1, kCancel = 2;
    auto* aw = new juce::AlertWindow ("Crop to loop range?",
        "Trim every track to the " + juce::String (secs, 2) + " s loop region?\n"
        "Markers, cues and other takes move with the "
        "audio, and whatever lies outside the region goes.\n"
        "Non-destructive -- the recordings on disk are untouched and Cmd+Z restores everything.",
        juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Crop",   kCrop,   juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, a, b, cut, dir] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != kCrop || self == nullptr) return;
            // The dialog can sit open while a take starts from a desk macro or
            // a remote, or the session changes: decide again now.
            if (! self->canMutateTimeline ("cropping")) return;
            if (self->engine.getActiveSessionDir() != dir)
            { self->showStatus ("The session changed -- nothing was cropped"); return; }
            const auto tracks = self->engine.allTimelineTracks();
            if (const auto refusal = self->engine.timelineCutRefusal (cut, tracks); refusal.isNotEmpty())
            { self->showStatus (refusal); return; }

            const auto before = self->captureTimeline();
            const int kept = self->engine.cropToRange (a, b);
            self->commitTimelineCut ("Crop to loop range", before, cut, tracks);
            self->engine.getPlayer().clearLoopRegion();
            if (self->editPage != nullptr) self->editPage->repaint();
            self->showStatus ("Cropped to loop region (" + zynforge::counted (kept, "track") + " with audio) -- non-destructive");
        }),
        false);
}

void MainComponent::editSetRangeToLoopRange()
{
    if (! canMutateTimeline ("editing range markers")) return;
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion())
    {
        // Engineer hit it without a loop set -- start one at the playhead.
        // Parked at the take end (where STOP leaves it) there is nothing after
        // the playhead to loop, so take the last 2 s of the session instead.
        const auto pos   = currentPlayheadSamples (engine);
        const auto len   = (juce::int64) (player.getSampleRate() * 2.0);
        const auto total = player.getTotalLengthSamples();
        const bool atEnd = total > 0 && pos >= total;
        const auto start = atEnd ? juce::jmax ((juce::int64) 0, total - len) : pos;
        player.setLoopRegion (start, start + len);
        showStatus (atEnd ? "No loop region set -- defaulted to the last 2 s of the session"
                          : "No loop region set -- defaulted to 2 s at the playhead");
        return;
    }
    auto& m = engine.getMarkers();
    m.drop (player.getLoopStart(), "Range In", "range");
    m.drop (player.getLoopEnd(),   "Range Out", "range");
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

    const auto markerId = engine.getMarkers().lastDroppedId;
    int rowIndex = -1;
    const auto& markers = engine.getMarkers().getAll();
    for (int i = 0; i < (int) markers.size(); ++i)
        if (markers[(size_t) i].runtimeId == markerId) { rowIndex = i; break; }
    if (rowIndex < 0) return;
    const auto defaultName = markers[(size_t) rowIndex].name;

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
    // A modal stands between the engineer and STOP, so during a take the marker
    // keeps its name until there is time to change it.
    if (engine.isRecording())
    {
        showStatus ("Marker " + juce::String (newCount) + " dropped -- name it after the take");
        return;
    }
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
        juce::ModalCallbackFunction::create ([self, aw, markerId, defaultName] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self == nullptr) return;
            if (result != 1) return;            // Cancel keeps the auto-name

            const auto typed = dispose->getTextEditorContents ("markerName").trim();
            if (typed.isEmpty() || typed == defaultName) return;

            int rowIndex = -1;
            const auto& list = self->engine.getMarkers().getAll();
            for (int i = 0; i < (int) list.size(); ++i)
                if (list[(size_t) i].runtimeId == markerId) { rowIndex = i; break; }
            if (rowIndex < 0) return; // marker deleted or another session opened
            self->engine.getMarkers().renameMarker (rowIndex, typed);
            self->engine.getMarkers().save();
            self->showStatus ("Marker " + juce::String (rowIndex + 1)
                              + " renamed to: " + typed);
        }),
        false);
}

bool MainComponent::canMutateTimeline (const juce::String& action)
{
    if (engine.isRecording())
    { showStatus ("Stop recording before " + action); return false; }
    if (sessionIoBusy.load())
    { showStatus ("Wait for the session operation to finish"); return false; }
    if (sessionLocked)
    { showStatus ("LOCKED -- click UNLOCK to resume control"); return false; }
    return true;
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
            for (int peer : zynforge::clipedit::peerTracks (engine, phys))
                if (peer >= 0 && peer < rec.getNumTracks()) picked.insert (peer);
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
    if (! canMutateTimeline()) return;
    auto pos = engine.snapSampleToGrid (currentPlayheadSamples (engine));

    // Clip-level split across the target tracks (selected, else all).
    const auto targets = tracksToEditPhysical();

    // Zero-crossing snap (click-free cuts): nudge the cut to the nearest zero
    // crossing within ±5 ms, using the first target track as the reference so
    // every track is cut at the SAME sample (a coherent multitrack edit).
    if (zeroCrossSnap && ! targets.empty())
        pos = engine.nearestZeroCrossing (targets.front(), pos, 240);

    engine.getMarkers().drop (pos, "Split", "split");
    const auto splitMarkerId = engine.getMarkers().lastDroppedId;

    const auto before = engine.playlistsToJson();   // clip-aware undo
    int splits = 0;
    for (int phys : targets)
        if (engine.splitTrackAtSample (phys, pos)) ++splits;
    if (splits > 0)
    {
        pushClipUndo ("Split clips at playhead", before);
        // In the split's transaction: its marker goes when the split does.
        for (const auto& marker : engine.getMarkers().getAll())
            if (marker.runtimeId == splitMarkerId)
            {
                undoManager.perform (new MarkerDropAction (engine.getMarkers(), marker));
                break;
            }
    }

    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus ((splits > 0
                    ? "Split " + zynforge::counted (splits, "track") + " and dropped marker"
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
    if (! canMutateTimeline()) return;
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
    showStatus (n > 0 ? "Separated selection on " + zynforge::counted (n, "track")
                      : juce::String ("Nothing to separate in the selection"));
}

// Heal Separation (Cmd+H): rejoin a clean split at the playhead, or every
// healable split inside the selected range, on the target tracks.
void MainComponent::editHealSeparation()
{
    if (! canMutateTimeline()) return;
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
    showStatus (n > 0 ? "Healed " + zynforge::counted (n, "separation")
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

// Consolidate: flatten the selected range on each target track into one new
// file (bakes in gains / fades / clip order).
void MainComponent::editConsolidateSelection()
{
    if (! canMutateTimeline ("consolidating audio")) return;
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Select a range first, then Consolidate"); return; }
    const auto targets = tracksToEditPhysical();
    if (targets.empty()) { showStatus ("Select at least one track"); return; }
    consolidateTracks (targets, player.getLoopStart(), player.getLoopEnd());
}

// A session job whichever menu started it: it writes a WAV into the session, so
// RECORD and other jobs must wait, and its undo step must not depend on the row
// that asked still existing.
void MainComponent::consolidateTracks (const std::vector<int>& targets, juce::int64 start, juce::int64 end)
{
    if (! canMutateTimeline ("consolidating audio")) return;
    if (! sessionJobs.reserve())
    { showStatus ("Wait for the session operation to finish"); return; }
    const auto before = engine.playlistsToJson();
    if (editPage != nullptr) editPage->setEnabled (false);
    showStatus ("Consolidating audio in the background...");
    juce::Component::SafePointer<MainComponent> self (this);
    engine.consolidateRangeAsync (targets, start, end, [self, before] (int n, bool complete)
    {
        if (self == nullptr) return;
        self->sessionJobs.release();
        if (self->editPage != nullptr)
        {
            self->editPage->setReadOnlyWhileRecording (self->engine.isRecording());
            self->editPage->setEnabled (! self->sessionLocked);
            self->editPage->repaint();
        }
        if (n > 0)
        {
            self->pushClipUndo ("Consolidate", before);
            if (self->editPage != nullptr) self->editPage->sessionMediaAdded();
        }
        self->showStatus (! complete
            ? (n > 0 ? "Consolidation completed only part of the selection"
                     : "Consolidation failed; the arrangement was not changed")
            : "Consolidated " + zynforge::counted (n, "track"));
    });
}

// Clear (Delete): remove the audio inside the selected range on every
// target track, leaving a gap. Non-destructive -- the files are untouched
// and Cmd+Z restores the clips.
void MainComponent::editClearRange()
{
    if (! canMutateTimeline()) return;
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
    showStatus (n > 0 ? "Cleared selection on " + zynforge::counted (n, "track")
                      : juce::String ("Nothing inside the selection to clear"));
}

// ─── Operations on the clicked (selected) clip(s) ────────────────────
std::vector<EditPage::ClipRef> MainComponent::selectedClipTargets()
{
    return zynforge::clipedit::closure (engine, editPage->getSelectedClips(),
                                        { editPage->getSelectedClipTrack(),
                                          editPage->getSelectedClipIndex() });
}

void MainComponent::editDeleteSelectedClip()
{
    if (! canMutateTimeline()) return;
    if (editPage == nullptr) return;
    auto sel = selectedClipTargets();
    if (sel.empty()) { showStatus ("No clip selected -- click a clip first"); return; }
    if (zynforge::clipedit::anyLocked (engine, sel))
    { showStatus ("Clip is locked -- unlock before deleting"); return; }
    // Delete higher clip indices first so lower ones stay valid (per track,
    // and harmless across tracks since their lists are independent).
    std::sort (sel.begin(), sel.end(),
               [] (auto& a, auto& b) { return a.second > b.second; });
    const auto before = engine.playlistsToJson();
    int deleted = 0;
    for (auto& [track, idx] : sel)
        if (engine.deleteClip (track, idx)) ++deleted;
    if (deleted > 0) pushClipUndo ("Delete clips", before);
    editPage->clearSelectedClip();
    editPage->repaint();
    showStatus ("Deleted " + zynforge::counted (deleted, "clip"));
}

void MainComponent::editDuplicateSelectedClip()
{
    if (! canMutateTimeline()) return;
    if (editPage == nullptr) return;
    const auto sel = selectedClipTargets();
    if (sel.empty()) { showStatus ("No clip selected -- click a clip first"); return; }
    if (zynforge::clipedit::anyLocked (engine, sel))
    { showStatus ("Clip is locked -- unlock it before duplicating"); return; }

    const EditPage::ClipRef source { editPage->getSelectedClipTrack(), editPage->getSelectedClipIndex() };
    const auto before = engine.playlistsToJson();
    // Highest index first per track, so the lower ones stay valid. A copy lands
    // right after its source, behind one slot for each copy made earlier in
    // that track's list.
    bool allCopied = true;
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
        allCopied = engine.duplicateClip (it->first, it->second) && allCopied;
    pushClipUndo ("Duplicate clip", before);
    if (! allCopied)
    {
        editPage->clearSelectedClip();
        showStatus ("Could not duplicate every selected clip -- check them, Cmd+Z reverts");
        return;
    }

    // Follow the new copies, the primary included.
    std::map<int, int> copiesBelow;
    std::vector<EditPage::ClipRef> copies;
    auto primary = source;
    for (const auto& clip : sel)
    {
        copies.emplace_back (clip.first, clip.second + 1 + copiesBelow[clip.first]++);
        if (clip == source) primary = copies.back();
    }
    editPage->setSelectedClips (copies, primary);
    editPage->repaint();
    showStatus ("Duplicated " + zynforge::counted ((int) copies.size(), "clip"));
}

void MainComponent::editNudgeSelectedClip (int dir)
{
    if (! canMutateTimeline()) return;
    if (editPage == nullptr) return;
    const auto sel = selectedClipTargets();
    if (sel.empty()) { showStatus ("No clip selected -- click a clip first"); return; }
    const double sr   = juce::jmax (1.0, engine.getPlayer().getSampleRate());
    juce::int64 step = (juce::int64) (sr * (double) nudgeMs / 1000.0) * (dir > 0 ? 1 : -1);
    // One delta for the whole selection: a clip stopped at zero or a lock
    // must hold its peers, or the pair slides apart.
    for (auto& [track, idx] : sel)
        step = engine.constrainClipEditDelta (track, idx, AudioEngine::ClipEdit::Move, step);
    if (step == 0) { showStatus ("Clip cannot move any further (or is locked)"); return; }
    const auto before = engine.playlistsToJson();
    int n = 0;
    // Move doesn't reindex clips, so iterating the selection set is safe.
    for (auto& [track, idx] : sel)
        if (engine.editClip (track, idx, AudioEngine::ClipEdit::Move, step)) ++n;
    if (n > 0)
    {
        pushClipUndo ("Nudge clips", before);
        editPage->repaint();
        showStatus ("Nudged " + zynforge::counted (n, "clip") + " " + (dir > 0 ? "right" : "left")
                    + " " + juce::String ((double) std::abs (step) * 1000.0 / sr, 0) + " ms");
    }
}

void MainComponent::editCycleNudgeValue()
{
    static const int steps[] = { 10, 25, 50, 100, 250, 500, 1000 };
    int idx = 0;
    for (int i = 0; i < (int) (sizeof (steps) / sizeof (int)); ++i)
        if (steps[i] == nudgeMs) { idx = i; break; }
    nudgeMs = steps[(idx + 1) % (int) (sizeof (steps) / sizeof (int))];
    zynforge::updateSettings (engine.getAppProps(), [this] (juce::PropertiesFile& props)
                              { props.setValue ("editNudgeMs", nudgeMs); });
    showStatus ("Nudge value: " + juce::String (nudgeMs) + " ms"
                + "  (Alt+Left/Right or numpad +/- to nudge the selected clip)");
}

// Cmd+X / Cmd+C -- cut/copy the selected AUDIO clip when one is picked in
// the EDIT view, else the strip-settings clipboard (legacy behaviour).
void MainComponent::editClipboardCut (bool cut)
{
    if (cut && ! canMutateTimeline()) return;
    if (currentView == View::Edit && editPage != nullptr
        && editPage->getSelectedClipTrack() >= 0)
    {
        const auto sel = selectedClipTargets();
        if (cut && zynforge::clipedit::anyLocked (engine, sel))
        { showStatus ("Clip is locked -- unlock before cutting"); return; }
        juce::Array<juce::var> entries;
        juce::int64 anchor = std::numeric_limits<juce::int64>::max();
        for (auto [track, idx] : sel)
        {
            const auto* clips = engine.tryClipsFor (track);
            if (clips == nullptr || idx < 0 || idx >= (int) clips->size()) continue;
            const auto& c = (*clips)[(size_t) idx];
            int channel = c.sourceChannel;
            auto source = c.audioFile;
            if (source == juce::File()) source = engine.getTrackAudioFile (track, &channel);
            if (! source.existsAsFile())
            { showStatus ("Copy failed: selected clip media is missing"); return; }
            auto* o = new juce::DynamicObject();
            o->setProperty ("track", track);
            o->setProperty ("stereoHalf", zynforge::clipedit::stereoHalf (engine, track));
            o->setProperty ("position", c.timelineStartSamples);
            o->setProperty ("audioFile", source.getFullPathName());
            o->setProperty ("sourceChannel", channel);
            o->setProperty ("fadeCurve", c.fadeCurve);
            o->setProperty ("fileStart", c.fileStartSamples);
            o->setProperty ("fileLength", c.fileLengthSamples);
            o->setProperty ("fadeIn", c.fadeInSamples);
            o->setProperty ("fadeOut", c.fadeOutSamples);
            o->setProperty ("fadeInRamp", c.fadeInRampSamples);
            o->setProperty ("fadeInRampOffset", c.fadeInRampOffsetSamples);
            o->setProperty ("fadeOutRamp", c.fadeOutRampSamples);
            o->setProperty ("fadeOutRampOffset", c.fadeOutRampOffsetSamples);
            o->setProperty ("gainDb", c.gainDb);
            o->setProperty ("muted", c.muted);
            o->setProperty ("name", c.name);
            entries.add (juce::var (o));
            anchor = juce::jmin (anchor, c.timelineStartSamples);
        }
        if (entries.isEmpty()) { showStatus ("No editable clips selected"); return; }
        for (auto& entry : entries)
            entry.getDynamicObject()->setProperty ("position",
                (juce::int64) entry["position"] - anchor);
        auto* root = new juce::DynamicObject();
        root->setProperty ("track", editPage->getSelectedClipTrack());
        root->setProperty ("clips", entries);
        clipClipboard = juce::var (root);
        clipClipboardSession = engine.getActiveSessionDir();
        if (cut) editDeleteSelectedClip();
        showStatus (juce::String (cut ? "Cut " : "Copied ") + juce::String (entries.size()) + " clips");
        return;
    }
    // A Range drag selects the strip it ran along, so the range outranks that
    // selection here, as it does for Delete: Cmd+X must not take the channel.
    if (cut && currentView == View::Edit && engine.getPlayer().hasLoopRegion())
    {
        confirmDeleteRecording (false, [this] { editClearRange(); });
        return;
    }
    editCutSelected (cut);   // strip-settings fallback
}

// Cmd+V -- paste the copied audio clip at the playhead on its source
// track, else paste strip settings.
void MainComponent::editClipboardPaste()
{
    if (! canMutateTimeline()) return;
    if (clipClipboard.isObject() && clipClipboardSession != engine.getActiveSessionDir())
        clipClipboard = juce::var();   // those clips read another session's files
    if (currentView == View::Edit && clipClipboard.isObject())
    {
        auto* entries = clipClipboard["clips"].getArray();
        if (entries == nullptr || entries->isEmpty()) return;
        const int sourceTrack = (int) clipClipboard["track"];
        const int target = editPage != nullptr && editPage->getActiveRowTrackIndex() >= 0
                         ? editPage->getActiveRowTrackIndex() : sourceTrack;
        const int offset = target - sourceTrack;
        const int tracks = juce::jmax (engine.getRecorder().getNumTracks(), engine.getPlayer().getNumTracks());
        for (const auto& entry : *entries)
        {
            const int dest = (int) entry["track"] + offset;
            if (dest < 0 || dest >= tracks || (juce::int64) entry["fileLength"] <= 0
                || ! juce::File (entry["audioFile"].toString()).existsAsFile())
            { showStatus ("Paste failed: missing media or selection extends beyond available tracks"); return; }
            // A stereo clip needs a stereo strip and a mono clip a mono one: an
            // R half landing on the next strip plays in an unrelated channel.
            if ((int) entry["stereoHalf"] != zynforge::clipedit::stereoHalf (engine, dest))
            { showStatus ("Paste refused: the clips and this strip differ in stereo layout"); return; }
        }
        const auto pos = currentPlayheadSamples (engine);
        const auto before = engine.playlistsToJson();
        std::vector<EditPage::ClipRef> pasted;
        // Timeline order keeps earlier inserted indices stable on each track.
        auto ordered = *entries;
        std::sort (ordered.begin(), ordered.end(), [] (const auto& a, const auto& b)
            { return (juce::int64) a["position"] < (juce::int64) b["position"]; });
        for (const auto& entry : ordered)
        {
            const int dest = (int) entry["track"] + offset;
            const int idx = engine.pasteClip (dest, pos + (juce::int64) entry["position"],
                (juce::int64) entry["fileStart"], (juce::int64) entry["fileLength"],
                (juce::int64) entry["fadeIn"], (juce::int64) entry["fadeOut"],
                (float) (double) entry["gainDb"], entry["name"].toString(),
                juce::File (entry["audioFile"].toString()), (int) entry["sourceChannel"], (int) entry["fadeCurve"]);
            if (idx >= 0)
            {
                engine.setClipFadeRamps (dest, idx, (juce::int64) entry["fadeInRamp"],
                                         (juce::int64) entry["fadeInRampOffset"],
                                         (juce::int64) entry["fadeOutRamp"],
                                         (juce::int64) entry["fadeOutRampOffset"]);
                engine.setClipMuted (dest, idx, (bool) entry["muted"]);
                pasted.emplace_back (dest, idx);
            }
        }
        pushClipUndo ("Paste clips", before);
        if (editPage != nullptr && ! pasted.empty())
            editPage->setSelectedClips (pasted, pasted.front());
        showStatus ("Pasted " + juce::String ((int) pasted.size()) + " clips at the playhead");
        return;
    }
    editPasteSelected();   // strip-settings fallback
}

// Shift+Delete: ripple-delete the selected clip (or range), closing the
// gap so later audio slides earlier.
void MainComponent::editRippleDelete()
{
    if (! canMutateTimeline()) return;
    if (editPage != nullptr && editPage->getSelectedClipTrack() >= 0)
    {
        const auto sel = selectedClipTargets();
        if (sel.empty()) { showStatus ("No clip selected"); return; }
        if (zynforge::clipedit::anyLocked (engine, sel))
        { showStatus ("Clip is locked -- unlock before ripple deleting"); return; }
        const auto plan = zynforge::clipedit::ripplePlan (engine, sel, editPage->getSelectedClipTrack());
        if (rippleSpans ("Ripple delete clip", plan) > 0)
        {
            editPage->clearSelectedClip();
            editPage->repaint();
            showStatus ("Ripple-deleted clip -- gap closed");
        }
        return;
    }
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("Select a clip or a range first"); return; }
    const int n = rippleTimeline ("Ripple delete selection", tracksToEditPhysical(),
                                  player.getLoopStart(), player.getLoopEnd());
    if (n < 0) return;   // refused, and rippleTimeline said why
    if (editPage != nullptr) { editPage->clearSelectedClip(); editPage->repaint(); }
    showStatus (n > 0 ? "Ripple-deleted selection on " + zynforge::counted (n, "track") + " -- gap closed"
                      : juce::String ("Nothing to ripple-delete"));
}

void MainComponent::editStartRange()
{
    if (! canMutateTimeline()) return;
    if (editPage != nullptr) editPage->clearSelectedClip();   // the range is the target now
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const bool otherEdgeSet = player.hasLoopRegion();
    const auto end = otherEdgeSet ? player.getLoopEnd()
                                  : pos + (juce::int64) (player.getSampleRate() * 2.0);
    // Ordering guard: if the playhead is at/past the existing loop end, keep
    // end > start instead of passing an inverted range that setLoopRegion
    // treats as "clear".
    // Half a range is a default, not a selection, so it can't arm a punch.
    if (otherEdgeSet) player.selectRange (pos, juce::jmax (end, pos + 1));
    else              player.setLoopRegion (pos, juce::jmax (end, pos + 1));
    engine.getMarkers().drop (pos, "Range In", "range");
    showStatus ("Range In set at playhead");
}

void MainComponent::editFinishRange()
{
    if (! canMutateTimeline()) return;
    if (editPage != nullptr) editPage->clearSelectedClip();   // the range is the target now
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const bool otherEdgeSet = player.hasLoopRegion();
    const auto start = otherEdgeSet ? player.getLoopStart() : juce::int64 (0);
    // Ordering guard: if the playhead is at/before the existing loop start,
    // keep start < end instead of passing an inverted range that setLoopRegion
    // treats as "clear".
    // Half a range is a default, not a selection, so it can't arm a punch.
    if (otherEdgeSet) player.selectRange (juce::jmin (start, pos - 1), pos);
    else              player.setLoopRegion (juce::jmin (start, pos - 1), pos);
    engine.getMarkers().drop (pos, "Range Out", "range");
    showStatus ("Range Out set at playhead");
}

// ─── Setlist plumbing ────────────────────────────────────────────────
//
// Cues live as a JSON array under the session's .zfproj document:
//   "setlist": [ {"name": "Intro", "samplePos": 0}, ... ]
// Loaded on session swap, persisted on every add / update so a crash
// mid-show doesn't lose what the engineer just dialled in.
