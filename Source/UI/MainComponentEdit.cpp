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

using namespace zynforge;

namespace
{
    juce::int64 currentPlayheadSamples (zynforge::AudioEngine& eng)
    {
        if (eng.isRecording()) return eng.getRecorder().getSamplesSinceStart();
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
}
void MainComponent::recordUndoSnapshot (const juce::String& label)
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        arr.add (snapshotStrip (engine, i));
    undoManager.beginNewTransaction (label);
    undoManager.perform (new MixerSnapshotAction (engine, juce::var (arr)));
}

void MainComponent::editUndo()
{
    if (! undoManager.canUndo()) { showStatus ("Nothing to undo"); return; }
    undoManager.undo();
    lastTrackCount = -1;
    showStatus ("Undo: " + undoManager.getUndoDescription());
}

void MainComponent::editRedo()
{
    if (! undoManager.canRedo()) { showStatus ("Nothing to redo"); return; }
    undoManager.redo();
    lastTrackCount = -1;
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

    // If the engineer has a selection, solo exactly those logical strips.
    if (! selectedLogical.empty())
    {
        recordUndoSnapshot ("Solo selection");
        for (int i = 0; i < rec.getNumTracks(); ++i)
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);
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

    // Multi-part takes (Track_NN_partNN.wav) span several files -- cropping
    // those correctly is out of scope, so refuse rather than mangle them.
    const auto audioDir = dir.getChildFile ("Audio Files");
    for (auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav"))
        if (f.getFileNameWithoutExtension().containsIgnoreCase ("_part"))
        { showStatus ("Cannot crop a multi-part take -- use Save Session As"); return; }

    const juce::int64 a = player.getLoopStart();
    const juce::int64 b = player.getLoopEnd();
    const double secs = (double) (b - a) / juce::jmax (1.0, player.getSampleRate());

    constexpr int kCrop = 1, kCancel = 2;
    auto* aw = new juce::AlertWindow ("Crop to loop range?",
        "Trim every track in \"" + dir.getFileName() + "\" to the "
        + juce::String (secs, 2) + " s loop region?\n"
        "The originals are backed up to \"Session File Backups\" first.",
        juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Crop",   kCrop,   juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, dir, a, b] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != kCrop) return;
            cropSessionToRange (dir, a, b);
        }),
        false);
}

void MainComponent::cropSessionToRange (const juce::File& dir,
                                        juce::int64 startSample, juce::int64 endSample)
{
    engine.stopPlayback();

    const auto audioDir = dir.getChildFile ("Audio Files");
    const auto backupDir = dir.getChildFile ("Session File Backups")
                              .getChildFile ("Pre-Crop "
                                  + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"));
    backupDir.createDirectory();

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    int cropped = 0;
    for (auto& f : audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav"))
    {
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        if (rd == nullptr) continue;

        const juce::int64 start = juce::jlimit ((juce::int64) 0, (juce::int64) rd->lengthInSamples, startSample);
        const juce::int64 end   = juce::jlimit (start, (juce::int64) rd->lengthInSamples, endSample);
        const juce::int64 len   = end - start;
        if (len <= 0) continue;

        const int    bits = (int) juce::jmax ((unsigned int) 16, rd->bitsPerSample);
        const double sr   = rd->sampleRate;
        const int    chans = (int) rd->numChannels;
        const auto   tmp  = f.getSiblingFile (f.getFileNameWithoutExtension() + ".cropping.wav");
        tmp.deleteFile();

        bool ok = false;
        if (auto os = tmp.createOutputStream())
        {
            juce::WavAudioFormat wav;
            if (auto* wr = wav.createWriterFor (os.get(), sr, (unsigned int) chans, bits, {}, 0))
            {
                os.release();   // writer owns the stream now
                std::unique_ptr<juce::AudioFormatWriter> writer (wr);
                juce::AudioBuffer<float> buf (chans, (int) juce::jmin (len, (juce::int64) 65536));
                juce::int64 done = 0;
                ok = true;
                while (done < len)
                {
                    const int n = (int) juce::jmin ((juce::int64) buf.getNumSamples(), len - done);
                    rd->read (&buf, 0, n, start + done, true, true);
                    if (! writer->writeFromAudioSampleBuffer (buf, 0, n)) { ok = false; break; }
                    done += n;
                }
            }
        }
        rd.reset();   // close the source before moving it

        if (ok && f.moveFileTo (backupDir.getChildFile (f.getFileName()))
               && tmp.moveFileTo (f))
            ++cropped;
        else
            tmp.deleteFile();
    }

    engine.getPlayer().clearLoopRegion();
    engine.loadSession (dir);
    lastTrackCount = -1;
    showStatus ("Cropped " + juce::String (cropped) + " track(s); originals backed up");
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
    aw->addTextEditor ("markerName", defaultName, {});
    if (auto* ed = aw->getTextEditor ("markerName"))
    {
        ed->setSelectAllWhenFocused (true);
        // Defer focus grab until after AlertWindow finishes its own
        // layout pass -- otherwise the focus call is swallowed and
        // the engineer still has to click the field before typing.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (ed)]
        {
            if (safe != nullptr) safe->grabKeyboardFocus();
        });
    }
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

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

void MainComponent::editSplitAtPlayhead()
{
    const auto pos = currentPlayheadSamples (engine);
    engine.getMarkers().drop (pos, "Split");

    // Real clip-level split -- only act on the selected strips. If
    // nothing is selected, the action stays as 'just drop a marker' so
    // it's safe to bind to the S hotkey by default.
    int splits = 0;
    if (! selectedLogical.empty())
    {
        recordUndoSnapshot ("Split clips at playhead");
        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogicalIdx (logical);
            if (engine.splitTrackAtPlayhead (phys)) ++splits;
        }
    }
    if (editPage != nullptr) editPage->repaint();
    showStatus ((splits > 0
                    ? "Split " + juce::String (splits) + " clip(s) and dropped marker"
                    : juce::String ("Split marker dropped"))
                + " at "
                + juce::String ((double) pos
                                / juce::jmax (1.0, engine.getPlayer().getSampleRate()), 2) + " s");
}

void MainComponent::editStartRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto end = player.hasLoopRegion() ? player.getLoopEnd()
                                            : pos + (juce::int64) (player.getSampleRate() * 2.0);
    player.setLoopRegion (pos, end);
    engine.getMarkers().drop (pos, "Range In");
    showStatus ("Range In set at playhead");
}

void MainComponent::editFinishRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto start = player.hasLoopRegion() ? player.getLoopStart() : juce::int64 (0);
    player.setLoopRegion (start, pos);
    engine.getMarkers().drop (pos, "Range Out");
    showStatus ("Range Out set at playhead");
}

// ─── Setlist plumbing ────────────────────────────────────────────────
//
// Cues live as a JSON array under the session's .zfproj document:
//   "setlist": [ {"name": "Intro", "samplePos": 0}, ... ]
// Loaded on session swap, persisted on every add / update so a crash
// mid-show doesn't lose what the engineer just dialled in.



