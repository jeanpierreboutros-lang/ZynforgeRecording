// Session lifecycle + persistence + export + template methods on
// MainComponent. Extracted from MainComponent.cpp as part of the
// 2026-05-24 god-class split.
//
// Includes: saveSessionStateTo, exportTracksTo, onSaveSessionState,
// onImportAudioFiles, onSaveSessionAs, onExportAllTracks,
// onExportIndividualTrack, warnIfSampleRateMismatch,
// onLoadSessionClicked, getSessionsRoot, templatesDir,
// promptSaveSessionTemplate, applySessionTemplate,
// loadUILayoutFromActiveSession, promptDeleteSessionTemplate,
// makeNewSessionDir, createSessionFolderStructure.
//
// showPreflightChecklist + onDeviceClicked stay in MainComponent.cpp
// because they're not really session IO (preflight is a status
// report; onDeviceClicked launches the audio-device dialog).

#include "MainComponent.h"
#include "../Audio/AudioImport.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/TrackFileTransaction.h"
#include "../Audio/PathSafety.h"
#include "../Theme/DialogChrome.h"
#include "../Audio/TimelineExport.h"
#include "NewSessionDialog.h"
#include "ExportDialog.h"
#include "TrackSelectDialog.h"
#include "SessionProjPath.h"

#include <array>

using namespace zynforge;

namespace
{
bool copyDirectoryCancellable (const juce::File& source, const juce::File& dest,
                               const std::atomic<bool>& cancel)
{
    // Never follow links while cloning a session. Apart from leaking files
    // outside the session, a link back into the source makes this recursion
    // unbounded. Canonical containment also catches a destination alias that
    // File::isAChildOf's lexical comparison misses.
    if (! source.isDirectory() || source.isSymbolicLink() || dest.isSymbolicLink()
        || pathsafety::isSameOrDescendant (source, dest))
        return false;
    if ((! dest.isDirectory() && dest.createDirectory().failed()) || cancel.load())
        return false;

    for (const auto& child : source.findChildFiles (juce::File::findFilesAndDirectories, false))
    {
        if (cancel.load (std::memory_order_relaxed)) return false;
        if (child.isSymbolicLink()) return false;
        const auto target = dest.getChildFile (child.getFileName());
        if (child.isDirectory())
        {
            if (! copyDirectoryCancellable (child, target, cancel)) return false;
        }
        else
        {
            auto in = child.createInputStream();
            target.deleteFile();
            auto out = target.createOutputStream();
            if (in == nullptr || out == nullptr) return false;
            std::array<char, 1024 * 1024> buffer {};
            while (! in->isExhausted())
            {
                if (cancel.load (std::memory_order_relaxed)) return false;
                const int got = in->read (buffer.data(), (int) buffer.size());
                if (got < 0 || (got > 0 && ! out->write (buffer.data(), (size_t) got)))
                    return false;
                if (got == 0) break;
            }
            out->flush();
            if (out->getStatus().failed()) return false;
        }
    }
    return ! cancel.load (std::memory_order_relaxed);
}

void removePartialCopy (const juce::File& dest, bool removeFolder)
{
    if (removeFolder) { dest.deleteRecursively(); return; }
    for (const auto& child : dest.findChildFiles (juce::File::findFilesAndDirectories, false))
        child.deleteRecursively();
}

bool ensureSessionScaffold (const juce::File& dest)
{
    const auto ensureDir = [] (const juce::File& dir)
    {
        return dir.isDirectory() || dir.createDirectory().wasOk();
    };
    if (! ensureDir (dest.getChildFile ("Audio Files"))
        || ! ensureDir (dest.getChildFile ("Export Files"))
        || ! ensureDir (dest.getChildFile ("Session File Backups")))
        return false;

    if (findSessionProj (dest).existsAsFile()) return true;
    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("name", dest.getFileName());
    root->setProperty ("createdAt", juce::Time::getCurrentTime().toISO8601 (true));
    return zynforge::atomicfile::writeText (
        dest.getChildFile (dest.getFileName() + ".zfproj"),
        juce::JSON::toString (juce::var (root.get())));
}
}

int MainComponent::openSessionFolder (const juce::File& dir)
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before switching sessions"); return -1; }
    if (! dir.isDirectory()) return 0;
    if (! zynforge::TrackFileTransaction::recover (dir))
    { showStatus ("Session has an incomplete file move; recovery files were retained in Session File Backups"); return -1; }

    // Condemn the current strips before loading -- a smaller session shrinks
    // the recorder vector and frees TrackStates the live strips still point at.
    condemnAllStrips();
    engine.stopPlayback();
    engine.clearSessionState();
    engine.setActiveSessionDir (dir);        // pin so Save / Save As / Export stay lit
    const int n = engine.loadSession (dir);  // audio -> player + clips
    const bool loadedMix = engine.loadSessionMixFrom (dir);
    // Recovery fallback: a session recorded but never explicitly Saved has no
    // session_mix.json, so loadSessionMixFrom can't size the mixer and the app
    // would show "No channels yet" with every Edit / Track / Export menu grey.
    // Size one strip per loaded audio track so the channels (and the menus)
    // come back.
    if (! loadedMix)
        engine.setStripCount (n);
    // Playlists/automation must be restored after loadSession seeds its default
    // clips; loading them first caused the seed to erase the restored edits.
    loadSetlistFromActiveSession();
    loadUILayoutFromActiveSession();
    const auto projectSettings = juce::JSON::parse (findSessionProj (dir));
    if ((double) projectSettings["sampleRate"] > 0.0)
        pendingSampleRate = (double) projectSettings["sampleRate"];
    const auto settings = juce::JSON::parse (dir.getChildFile ("session_settings.json"));
    if (auto* s = settings.getDynamicObject())
    {
        if (s->hasProperty ("captureFormat"))
            engine.getRecorder().setCaptureFormat ((CaptureFormat) juce::jlimit (0, (int) CaptureFormat::Flac24,
                                                                                 (int) s->getProperty ("captureFormat")));
        if (s->hasProperty ("preRollSeconds"))
            engine.getRecorder().setPreRollSeconds (juce::jlimit (0, 30, (int) s->getProperty ("preRollSeconds")));
        if (s->hasProperty ("loopStart") && s->hasProperty ("loopEnd"))
            engine.getPlayer().setLoopRegion ((juce::int64) s->getProperty ("loopStart"),
                                             (juce::int64) s->getProperty ("loopEnd"));
        if (s->hasProperty ("sampleRate")) pendingSampleRate = (double) s->getProperty ("sampleRate");
    }
    if (engine.getPlayer().isLoaded()) pendingSampleRate = engine.getPlayer().getSampleRate();
    engine.setSessionSampleRate (pendingSampleRate);
    // The click strip's index is SESSION-scoped. Reset it and re-detect from
    // the just-loaded session, so a stale index carried over from a previous
    // session can never deleteFile()/overwrite THIS session's Track_NN.wav on
    // a tempo change -- and so tempo refresh keeps working when reopening a
    // session that already contains a "Click" strip.
    clickTrackIndex = -1;
    {
        // Identify the generated click strip by name AND its playback-only
        // routing (inputRouting < 0). A recorded console "Click"/metronome-
        // return channel has a real input, so it is never adopted -- otherwise
        // a tempo change would deleteFile()/overwrite that recorded take.
        auto& rec = engine.getRecorder();
        for (int i = 0; i < rec.getNumTracks(); ++i)
        {
            auto& t = rec.getTrack (i);
            if (t.getNameThreadSafe() == "Click"
                && t.inputRouting.load (std::memory_order_relaxed) < 0)
            { clickTrackIndex = i; break; }
        }
    }
    lastTrackCount = -1;                      // force a strip rebuild on the next tick
    undoManager.clearUndoHistory();           // a fresh session starts with a clean undo stack
    rebaselineMixerUndo();                    // don't record the load itself as a mixer undo step
    if (n > 0)
    {
        statusLabel.setText ("Loaded " + juce::String (n) + " tracks", juce::dontSendNotification);
        warnIfSampleRateMismatch();
    }
    return n;
}

bool MainComponent::openSessionDocument (const juce::File& document, bool confirmBeforeReplacing)
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before opening another session"); return false; }
    if (sessionIoBusy.load())
    {
        showStatus ("Wait for the session file operation to finish");
        return false;
    }
    const auto dir = document.isDirectory() ? document : document.getParentDirectory();
    if (! dir.isDirectory() || (! document.isDirectory() && ! document.hasFileExtension ("zfproj")))
        return false;
    const auto current = engine.getActiveSessionDir();
    if (confirmBeforeReplacing && current.isDirectory() && current != dir)
    {
        juce::Component::SafePointer<MainComponent> self (this);
        confirmSessionReplacement ([self, document]
        {
            if (self != nullptr) self->openSessionDocument (document, false);
        });
        return true;
    }
    explicitDocumentOpened = true;
    const int n = openSessionFolder (dir);
    if (n < 0) return false;
    showStatus (n > 0 ? "Loaded: " + dir.getFileName()
                      : "Opened empty session: " + dir.getFileName());
    return true;
}

void MainComponent::confirmSessionReplacement (std::function<void()> continuation)
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before replacing the session"); return; }
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) { continuation(); return; }

    auto* aw = new juce::AlertWindow (
        "Replace current session?",
        "Save the current session before opening or creating another one?",
        juce::MessageBoxIconType::QuestionIcon, this);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Save & Continue", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Continue Without Saving", 2);
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [self, aw, dir, continuation = std::move (continuation)] (int result) mutable
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self == nullptr || result == 0) return;
            if (self->engine.isRecording() || self->sessionIoBusy.load())
            { self->showStatus ("Session switch cancelled: recording or file operation in progress"); return; }
            if (result == 1 && ! self->saveSessionStateTo (dir))
            {
                self->showStatus ("Session switch cancelled -- current session could not be saved");
                return;
            }
            continuation();
        }), false);
}

void MainComponent::closeSession()
{
    if (engine.isRecording())
    {
        showStatus ("Stop recording before closing the session");
        return;
    }

    // Closing loses any UNSAVED mixer / edit / cue / automation changes
    // (recorded audio is always already on disk). Offer Save & Close /
    // Close Without Saving / Cancel -- the same choice as quitting, but it
    // only unloads the session and returns to Welcome (the app stays open).
    juce::Component::SafePointer<MainComponent> self (this);
    const auto sessionDir = engine.getActiveSessionDir();

    auto* aw = new juce::AlertWindow (
        "Close session?",
        "Return to the Welcome screen without quitting the app.\n\n"
        "Recorded audio is always on disk. Unsaved mixer / edit / cue / "
        "automation changes since the last Save will be lost unless you save.",
        juce::MessageBoxIconType::QuestionIcon);
    aw->setLookAndFeel (&getLookAndFeel());
    aw->addButton ("Save & Close",         1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Close Without Saving", 2);
    aw->addButton ("Cancel",               0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [self, aw, sessionDir] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self == nullptr || result == 0) return;          // Cancel

            if (result == 1 && sessionDir.isDirectory()
                && ! self->saveSessionStateTo (sessionDir))
            {
                self->showStatus ("Close cancelled -- session state could not be saved");
                return;
            }

            // Forget the session so showStartupWelcome won't auto-reopen it,
            // and reset every session-scoped bit of state to the fresh-empty
            // slate (mirrors a launch with no session).
            auto& engine = self->engine;
            engine.clearSessionState();
            engine.clearAllStripOverrides();
            self->condemnAllStrips();   // stop strip timers before setStripCount(0) frees the TrackStates
            engine.setStripCount (0);

            self->cues.clear();
            self->currentCueIndex = -1;
            self->clickTrackIndex = -1;           // session-scoped; must not survive a close
            self->undoManager.clearUndoHistory();
            self->lastTrackCount = -1;            // force a strip rebuild (now empty)
            self->updateTransportLabels();
            self->showStatus (result == 1 ? "Saved + closed session" : "Session closed");

            // activeSessionDir is empty now -> this shows the Welcome dialog
            // (New / Open) instead of auto-reopening the last session.
            self->showStartupWelcome();
        }), false);
}

bool MainComponent::saveSessionStateTo (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;

    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("captureFormat",  (int) recorder.getCaptureFormat());
    root->setProperty ("preRollSeconds", recorder.getPreRollSeconds());
    root->setProperty ("sampleRate", player.isLoaded() ? player.getSampleRate() : pendingSampleRate);

    if (player.hasLoopRegion())
    {
        root->setProperty ("loopStart", (juce::int64) player.getLoopStart());
        root->setProperty ("loopEnd",   (juce::int64) player.getLoopEnd());
    }

    juce::Array<juce::var> trackArr;
    for (int i = 0; i < recorder.getNumTracks(); ++i)
    {
        juce::DynamicObject::Ptr t (new juce::DynamicObject());
        t->setProperty ("index", i);
        t->setProperty ("name",  recorder.getTrack (i).name);
        t->setProperty ("colourARGB",
                        (int) recorder.getTrack (i).colourARGB.load (std::memory_order_relaxed));
        trackArr.add (juce::var (t.get()));
    }
    root->setProperty ("tracks", trackArr);

    const auto json = juce::JSON::toString (juce::var (root.get()), true);
    const bool wroteSettings = zynforge::atomicfile::writeText (
        dir.getChildFile ("session_settings.json"), json);

    // Persist the FULL per-strip mixer state WITH the session (name, colour,
    // gain, pan, mute, solo, monitor, arm, routing, stereo, VCA + edit group)
    // so it travels per-show instead of leaking between sessions via global
    // appProps.
    const bool wroteMix = engine.saveSessionMixTo (dir);

    // Persist cues + comp playlists (Takes) + automation lanes into the
    // .zfproj. These were only auto-saved on cue edits before, so drawing
    // automation and hitting Save (without touching a cue) used to lose it.
    const bool wroteSetlist = saveSetlistToActiveSession (false);

    // Also persist the UI layout into the session's .zfproj so reopening
    // the show brings back the engineer's view choice, strip width,
    // VCA-panel visibility, and EDIT zoom.
    const bool wroteLayout = saveUILayoutToActiveSession();

    // Take the recoverable snapshot only after every live metadata file has
    // been updated, and include snapshot copy failures in the save result.
    const bool wroteBackup = wroteSettings && wroteMix && wroteSetlist && wroteLayout
                          && writeSessionBackupSnapshot();

    const bool ok = wroteSettings && wroteMix && wroteSetlist && wroteLayout && wroteBackup;
    // Re-baseline only after every mandatory artifact landed. Marking a failed
    // save as clean hides unsaved edits from later dirty checks.
    if (ok)
        lastSavedUndoUnits = undoManager.getNumberOfUnitsTakenUpByStoredCommands();
    return ok;
}

void MainComponent::serviceAutosave()
{
    if (sessionIoBusy.load()) return;
    auto* props = engine.getAppProps();
    if (props == nullptr) return;

    const int mins = props->getIntValue ("autosaveMinutes", 5);   // default: every 5 min
    if (mins <= 0) return;                                          // Off

    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) return;                               // nothing to save into

    const auto now = juce::Time::getMillisecondCounter();
    if (lastAutosaveMs == 0)                                        // first tick -> start the clock
    {
        lastAutosaveMs = now;
        return;
    }
    if (now - lastAutosaveMs < (juce::uint32) mins * 60000) return;
    // A number of session mutations are deliberately not represented by the
    // undo manager (recording, routing, cue recall, imports).  Saving on each
    // configured interval is the only reliable way not to miss those changes.
    if (saveSessionStateTo (dir))   // writes mix + .zfproj + a timestamped backup snapshot (10 kept)
    {
        lastAutosaveMs = now;
        statusLabel.setText ("Auto-saved " + juce::Time::getCurrentTime().formatted ("%H:%M:%S"),
                             juce::dontSendNotification);
    }
    else
    {
        // Retry soon, but not on every 24 Hz timer tick. Most failures are a
        // full/unmounted/read-only volume and require an explicit operator
        // warning rather than silently waiting another full interval.
        const auto intervalMs = (juce::uint32) mins * 60000u;
        const auto retryMs = juce::jmin ((juce::uint32) 15000u, intervalMs);
        lastAutosaveMs = now - intervalMs + retryMs;
        showStatus ("AUTO-SAVE FAILED -- check session volume permissions and free space; retrying shortly");
    }
}

void MainComponent::showAutosaveSettings()
{
    auto* aw = new juce::AlertWindow ("Auto-Save & Backup",
        "Automatically save the session (mix, cues, automation, layout) on a timer and keep a "
        "timestamped backup snapshot (10 newest) in 'Session File Backups'. Recordings are always "
        "written to disk live, independent of this -- a take is never at risk.",
        juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default

    const juce::StringArray labels { "Off", "Every 1 minute", "Every 2 minutes",
                                     "Every 5 minutes", "Every 10 minutes", "Every 15 minutes" };
    static const int mapMins[] = { 0, 1, 2, 5, 10, 15 };
    aw->addComboBox ("interval", labels, "Auto-save & back up:");

    const int cur = engine.getAppProps() ? engine.getAppProps()->getIntValue ("autosaveMinutes", 5) : 5;
    int sel = 3;   // default -> "Every 5 minutes"
    for (int i = 0; i < 6; ++i) if (mapMins[i] == cur) sel = i;
    if (auto* cb = aw->getComboBoxComponent ("interval"))
        cb->setSelectedItemIndex (sel, juce::dontSendNotification);

    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw, self] (int r)
    {
        std::unique_ptr<juce::AlertWindow> dispose (aw);
        if (r != 1 || self == nullptr) return;

        int idx = 3;
        if (auto* cb = aw->getComboBoxComponent ("interval")) idx = cb->getSelectedItemIndex();
        const int mins = mapMins[juce::jlimit (0, 5, idx)];

        if (auto* p = self->engine.getAppProps())
        { p->setValue ("autosaveMinutes", mins); p->saveIfNeeded(); }
        self->lastAutosaveMs = 0;   // restart the clock against the new interval
        self->showStatus (mins > 0 ? ("Auto-save every " + juce::String (mins)
                                      + (mins == 1 ? " minute" : " minutes"))
                                   : juce::String ("Auto-save turned off"));
    }), true);
}

bool MainComponent::saveUILayoutToActiveSession()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) return false;

    const auto proj = findSessionProj (dir);
    if (proj == juce::File{}) return false;

    juce::DynamicObject::Ptr obj;
    const auto parsed = juce::JSON::parse (proj);
    if (parsed.isObject()) obj = parsed.getDynamicObject();
    if (obj == nullptr)    obj = new juce::DynamicObject();

    juce::DynamicObject::Ptr ui (new juce::DynamicObject());
    ui->setProperty ("view",         currentView == View::Mix ? "Mix" : "Edit");
    ui->setProperty ("stripWidth",
                      stripWidthPreset == StripWidth::XS ? "XS"
                    : stripWidthPreset == StripWidth::S  ? "S"
                    : stripWidthPreset == StripWidth::L  ? "L"
                                                         : "M");
    ui->setProperty ("vcaPanel",     showVcaPanel);
    ui->setProperty ("editZoom",     editPage != nullptr ? (double) editPage->getZoom() : 1.0);
    obj->setProperty ("ui", juce::var (ui.get()));
    obj->setProperty ("updatedAt", juce::Time::getCurrentTime().toISO8601 (true));
    return zynforge::atomicfile::writeText (
        proj, juce::JSON::toString (juce::var (obj.get())));
}

void MainComponent::startExportTracksTo (const juce::File& destDir,
                                         const std::vector<int>& channelIndices,
                                         const zynforge::ExportOptions& opts)
{
    auto sourceDir = engine.getActiveSessionDir();
    if (! sourceDir.isDirectory() || ! destDir.isDirectory()) return;

    destDir.createDirectory();

    // Recordings live under "Audio Files/" (legacy sessions kept them at the
    // session root). Search the subfolder first, fall back to the root --
    // otherwise the export found no Track_*.wav and reported "Export failed".
    const auto audioDir = sourceDir.getChildFile ("Audio Files");
    const auto srcBase  = audioDir.isDirectory() ? audioDir : sourceDir;
    // Bare Track_* also matches Track_NN.punchbase.<ext> (a crash-orphaned punch
    // sidecar). matchesIndex below demands an EXACT "Track_NN" stem so one can't
    // slip through today, but exclude it here too rather than depend on that.
    auto allFiles = srcBase.findChildFiles (juce::File::findFiles, false, "Track_*");
    allFiles.removeIf ([] (const juce::File& f)
                       { return f.getFileName().containsIgnoreCase (".punchbase"); });

    auto matchesIndex = [] (const juce::File& f, int index1Based) -> bool
    {
        const auto base = f.getFileNameWithoutExtension();
        const auto suffix = juce::String::formatted ("Track_%02d", index1Based);
        return base == suffix;
    };

    auto findTrackFile = [&] (int index1Based) -> juce::File
    {
        for (auto& src : allFiles)
            if (matchesIndex (src, index1Based)) return src;
        return {};
    };

    // ── Phase 1 (MESSAGE THREAD): resolve every job ────────────────────────
    // Reading TrackStates (name / isStereo) and globbing the session folder
    // must happen here; the worker below then touches nothing but plain values.
    struct ExportJob
    {
        juce::File srcL, srcR;     // srcR set only for the LEGACY two-mono-file pair
        juce::File destStem;
        bool       stereoPair { false };   // true => interleave srcL + srcR
    };
    std::vector<ExportJob> jobs;
    jobs.reserve (channelIndices.size());

    auto& rec = engine.getRecorder();
    for (int i : channelIndices)
    {
        // A stereo pair exports as ONE interleaved stereo file from its L
        // half. Skip the R half when its L is also in the export set
        // (it's already written). A stereo track is the L; the next
        // physical track is its R.
        if (i > 0 && rec.getTrack (i - 1).isStereo.load (std::memory_order_relaxed)
            && std::find (channelIndices.begin(), channelIndices.end(), i - 1) != channelIndices.end())
            continue;

        auto& trackState = rec.getTrack (i);
        const bool stereo = trackState.isStereo.load (std::memory_order_relaxed);
        // Replace '.' too: the exporter appends the extension via
        // withFileExtension, which treats a name like "Out 2.1" as if ".1"
        // were the extension -- so "Out 2.1" and "Out 2.2" both collapsed to
        // "...Out 2.wav" and the second export silently overwrote the first.
        const auto safeName = trackState.name.replaceCharacter ('/', '_')
                                              .replaceCharacter ('\\', '_')
                                              .replaceCharacter ('.', '_');
        const auto baseName = juce::String::formatted ("Track_%02d - ", i + 1) + safeName;

        ExportJob job;
        job.destStem = destDir.getChildFile (baseName);
        if (stereo)
        {
            const auto srcL = findTrackFile (i + 1);
            const auto srcR = findTrackFile (i + 2);   // legacy: separate R mono file
            if (srcL.existsAsFile() && srcR.existsAsFile())
            {
                // LEGACY layout -- two mono files -> interleave into stereo.
                job.srcL = srcL; job.srcR = srcR; job.stereoPair = true;
            }
            else if (srcL.existsAsFile())
            {
                // NATIVE layout (2026-06-13): the pair is ONE interleaved
                // 2-channel file at the L slot with no Track_(N+2). exportTrack
                // faithfully preserves the source's channel count, so exporting
                // the 2-channel file directly yields a correct STEREO file.
                // Do NOT route this to exportStereoPair -- that helper expects
                // two MONO sources and reads only channel 0 of each, so it
                // would collapse a native pair to dual-mono of the left channel.
                job.srcL = srcL;
            }
        }
        else
        {
            job.srcL = findTrackFile (i + 1);
        }
        if (job.srcL.existsAsFile()) jobs.push_back (std::move (job));
    }

    if (jobs.empty()) { showStatus ("Export failed: no matching take files"); return; }

    // ── Phase 2 (BACKGROUND): decode / resample / encode ───────────────────
    // Owned + joinable thread (same pattern as the bounce) so a quit mid-export
    // can't leave a detached worker running against freed state.
    if (sessionIoBusy.exchange (true))
    {
        showStatus ("Another session file operation is already running");
        return;
    }
    joinExportThread();   // one export at a time
    showStatus ("Exporting " + juce::String ((int) jobs.size()) + " track(s)...");

    juce::Component::SafePointer<MainComponent> self (this);
    const auto destName = destDir.getFileName();
    exportThread = std::thread ([this, self, jobs, opts, destName]
    {
        zynforge::TrackExporter exporter;
        int succeeded = 0, attempted = 0;
        juce::String firstError;

        for (const auto& j : jobs)
        {
            if (exportCancel.load (std::memory_order_relaxed)) return;
            juce::String err;
            const bool ok = j.stereoPair
                ? exporter.exportStereoPair (j.srcL, j.srcR, j.destStem, opts, err)
                : exporter.exportTrack      (j.srcL,         j.destStem, opts, err);
            ++attempted;
            if (ok) ++succeeded;
            else if (firstError.isEmpty() && err.isNotEmpty()) firstError = err;
        }

        const bool cancelled = exportCancel.load (std::memory_order_relaxed);
        juce::MessageManager::callAsync (
            [self, succeeded, attempted, firstError, destName, cancelled]
        {
            if (self == nullptr) return;
            self->sessionIoBusy.store (false);
            if (cancelled) return;
            // Surface PARTIAL failures too -- e.g. the destination runs out of
            // space mid-batch. A run that wrote 10 of 32 files must not read as
            // success and ship an incomplete deliverable.
            self->lastExportFailures = juce::jmax (0, attempted - succeeded);
            if (succeeded == 0)
                self->showStatus ("Export failed"
                                  + (firstError.isNotEmpty() ? ": " + firstError : juce::String()));
            else if (self->lastExportFailures > 0)
                self->showStatus ("Export INCOMPLETE: " + juce::String (self->lastExportFailures)
                                  + " of " + juce::String (attempted) + " failed"
                                  + (firstError.isNotEmpty() ? " (" + firstError + ")" : ""));
            else
                self->showStatus ("Exported " + juce::String (succeeded)
                                  + " track(s) -> " + destName);
        });
    });
}

void MainComponent::onBounceStems()
{
    if (sessionIoBusy.load()) { showStatus ("Another session operation is already running"); return; }
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No active session to bounce"); return; }

    const auto exportDir = sessionDir.getChildFile ("Export Files");
    exportDir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Bounce edited stems to...", exportDir, "");
    juce::Component::SafePointer<MainComponent> chooserSelf (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectDirectories,
        [chooserSelf, this] (const juce::FileChooser& fc)
    {
        if (chooserSelf == nullptr) return;
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) return;
        dest.createDirectory();

        const int    nTracks = engine.getRecorder().getNumTracks();
        const auto   arrLen  = engine.getArrangementLengthSamples();
        const double sr      = engine.getPlayer().getSampleRate() > 0.0
                                 ? engine.getPlayer().getSampleRate() : 48000.0;
        if (arrLen <= 0) { showStatus ("Nothing to bounce -- record or load a session first"); return; }
        if (sessionIoBusy.exchange (true)) { showStatus ("Another session operation is already running"); return; }
        engine.setSessionTransitionActive (true);
        if (editPage != nullptr) editPage->setEnabled (false);

        showStatus ("Bouncing " + juce::String (nTracks) + " edited stems...");
        // Offline render on an OWNED background thread (joined + cancellable in
        // the destructor) so a quit mid-bounce can't leave it dereferencing a
        // freed engine. The engineer shouldn't be editing clips mid-bounce.
        joinBounceThread();   // finish any prior bounce first (one at a time)
        juce::Component::SafePointer<MainComponent> self (this);
        bounceThread = std::thread ([this, self, dest, nTracks, arrLen, sr]
        {
            const juce::ScopedLock structureGuard (engine.getRecorder().getStructureLock());
            int written = 0;
            for (int t = 0; t < nTracks; ++t)
            {
                if (bounceCancel.load (std::memory_order_relaxed)) break;
                auto& rec = engine.getRecorder();
                // A stereo pair bounces as ONE interleaved stereo stem from
                // its L half; skip the R half (already written).
                if (t > 0 && rec.getTrack (t - 1).isStereo.load (std::memory_order_relaxed))
                    continue;
                const auto& ts = rec.getTrack (t);
                // getNameThreadSafe() -- this runs on the bounce thread; a raw
                // ts.name read races setTrackName's locked reassignment (torn String).
                const auto safe = ts.getNameThreadSafe().replaceCharacter ('/', '_').replaceCharacter ('\\', '_');
                const auto outFile = dest.getChildFile (
                    juce::String::formatted ("Track_%02d - ", t + 1) + safe + ".wav");
                // Streams the edited arrangement window-by-window straight to
                // disk, so a multi-hour stem never needs a whole-track buffer.
                const bool stereo = ts.isStereo.load (std::memory_order_relaxed);
                const bool okBounce = stereo
                    ? engine.bounceStereoPairToWav (t, outFile, arrLen, sr, &bounceCancel)
                    : engine.bounceTrackArrangementToWav (t, outFile, arrLen, sr, &bounceCancel);
                if (okBounce) ++written;
            }
            // A superseding bounce (or app quit) sets bounceCancel and joins us;
            // don't post a stale "Bounced 0 stem(s)" over the new bounce's status.
            const bool cancelled = bounceCancel.load (std::memory_order_relaxed);
            juce::MessageManager::callAsync ([self, written, cancelled, dest]
            {
                if (self == nullptr) return;
                self->sessionIoBusy.store (false);
                self->engine.setSessionTransitionActive (false);
                if (self->editPage != nullptr) self->editPage->setEnabled (true);
                if (! cancelled)
                    self->showStatus ("Bounced " + juce::String (written)
                                      + " edited stem(s) -> " + dest.getFileName());
            });
        });
    });
}

void MainComponent::onBounceStereoMix()
{
    if (sessionIoBusy.load()) { showStatus ("Another session operation is already running"); return; }
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No active session to bounce"); return; }

    const auto exportDir = sessionDir.getChildFile ("Export Files");
    exportDir.createDirectory();
    const auto suggested = exportDir.getChildFile (sessionDir.getFileName() + " - Mix.wav");
    chooser = std::make_unique<juce::FileChooser> ("Bounce stereo mix to...", suggested, "*.wav");
    juce::Component::SafePointer<MainComponent> chooserSelf (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [chooserSelf, this] (const juce::FileChooser& fc)
    {
        if (chooserSelf == nullptr) return;
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) return;
        if (! dest.hasFileExtension ("wav")) dest = dest.withFileExtension ("wav");

        const auto   arrLen = engine.getArrangementLengthSamples();
        const double sr     = engine.getPlayer().getSampleRate() > 0.0
                                 ? engine.getPlayer().getSampleRate() : 48000.0;
        if (arrLen <= 0) { showStatus ("Nothing to bounce -- record or load a session first"); return; }
        if (sessionIoBusy.exchange (true)) { showStatus ("Another session operation is already running"); return; }
        engine.setSessionTransitionActive (true);
        if (editPage != nullptr) editPage->setEnabled (false);

        showStatus ("Bouncing stereo mix...");
        // Owned + joinable thread (see onBounceStems) -- never a detached
        // juce::Thread::launch that can outlive the engine on quit.
        joinBounceThread();
        juce::Component::SafePointer<MainComponent> self (this);
        bounceThread = std::thread ([this, self, dest, arrLen, sr]
        {
            const juce::ScopedLock structureGuard (engine.getRecorder().getStructureLock());
            // Streams the summed mix window-by-window straight to disk.
            const bool ok = ! bounceCancel.load (std::memory_order_relaxed)
                         && engine.bounceStereoMixToWav (dest, arrLen, sr, &bounceCancel);
            // A superseding bounce (or app quit) sets bounceCancel and joins us;
            // that returns false but is NOT a failure -- don't post a spurious
            // "bounce failed" over the new bounce's status.
            const bool cancelled = bounceCancel.load (std::memory_order_relaxed);
            juce::MessageManager::callAsync ([self, ok, cancelled, dest]
            {
                if (self == nullptr) return;
                self->sessionIoBusy.store (false);
                self->engine.setSessionTransitionActive (false);
                if (self->editPage != nullptr) self->editPage->setEnabled (true);
                if (! cancelled)
                    self->showStatus (ok ? "Bounced stereo mix -> " + dest.getFileName()
                                         : juce::String ("Stereo mix bounce failed"));
            });
        });
    });
}

void MainComponent::onSaveSessionState()
{
    const auto dir = engine.getActiveSessionDir();
    if (dir.isDirectory())
    {
        if (saveSessionStateTo (dir))
            showStatus ("Saved session state -> " + dir.getFileName());
        else
            showStatus ("Save failed");
        return;
    }
    // No active session yet -- behave like Save As so the engineer
    // can still capture the current strip / format / routing config
    // to a brand new folder.
    showStatus ("No active session -- pick a destination...");
    onSaveSessionAs();
}

void MainComponent::onImportAudioFiles()
{
    if (sessionIoBusy.load()) { showStatus ("Wait for the session file operation to finish"); return; }
    if (engine.isRecording()) { showStatus ("Stop recording before importing"); return; }

    // Accept anything the JUCE basic format manager + FLAC can read.
    // (WavAudioFormat, AiffAudioFormat, FlacAudioFormat, OggVorbisAudioFormat,
    //  MP3AudioFormat -- read-only -- when JUCE_USE_MP3AUDIOFORMAT is on.)
    const juce::String filters = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.caf";

    chooser = std::make_unique<juce::FileChooser> (
        "Pick audio files to append to the current session",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        filters);

    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectMultipleItems,
        [self, this] (const juce::FileChooser& fc)
    {
        if (self == nullptr) return;
        const auto picks = fc.getResults();
        if (picks.isEmpty()) return;

        if (sessionIoBusy.exchange (true))
        {
            showStatus ("Another session file operation is already running");
            return;
        }

        // Prepare the destination on the message thread, then leave every
        // potentially multi-hour decode/resample/write operation to the owned
        // session-I/O worker below.
        auto sessionDir = makeNewSessionDir();
        if (! sessionDir.createDirectory().wasOk())
        {
            sessionIoBusy.store (false);
            showStatus ("Import failed -- session folder is not writable");
            return;
        }

        auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
        const bool layoutReady = audioFilesDir.createDirectory().wasOk()
            && sessionDir.getChildFile ("Export Files").createDirectory().wasOk()
            && sessionDir.getChildFile ("Session File Backups").createDirectory().wasOk();
        if (! layoutReady)
        {
            sessionIoBusy.store (false);
            showStatus ("Import failed -- session folders are not writable");
            return;
        }
        engine.setActiveSessionDir (sessionDir);

        // Every imported file is written at the SESSION's sample rate, not its
        // own. Writing each source at `reader->sampleRate` left a session with
        // mixed rates on disk; SessionPlayer doesn't resample and takes the
        // LAST loaded file's rate as the session rate, so importing a 44.1 k
        // file into a 48 k session played it (or everything else) at the wrong
        // speed with no warning. Convert on the way in instead.
        const double targetSr = [this]
        {
            if (engine.getPlayer().isLoaded() && engine.getPlayer().getSampleRate() > 0.0)
                return engine.getPlayer().getSampleRate();
            if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
                if (d->getCurrentSampleRate() > 0.0) return d->getCurrentSampleRate();
            return pendingSampleRate > 0.0 ? pendingSampleRate : 48000.0;
        }();

        const int firstTrack = engine.getRecorder().getNumTracks();
        engine.setSessionTransitionActive (true);
        if (editPage != nullptr) editPage->setEnabled (false);
        showStatus ("Importing " + juce::String (picks.size()) + " audio file(s)...");

        joinExportThread();
        exportThread = std::thread ([this, self, picks, sessionDir, audioFilesDir,
                                     firstTrack, targetSr]
        {
            const auto result = zynforge::audioimport::importFiles (
                picks, audioFilesDir, firstTrack, targetSr, &exportCancel);

            juce::MessageManager::callAsync ([self, result, sessionDir, targetSr]
            {
                if (self == nullptr) return;
                self->sessionIoBusy.store (false);
                self->engine.setSessionTransitionActive (false);
                if (self->editPage != nullptr) self->editPage->setEnabled (true);
                if (result.cancelled) return;
                if (self->engine.getActiveSessionDir() != sessionDir)
                {
                    self->showStatus ("Import finished for the previous session; current session was not changed");
                    return;
                }
                if (result.tracks.empty())
                {
                    self->showStatus ("Import failed -- no readable audio files");
                    return;
                }

                int nextTrack = self->engine.getRecorder().getNumTracks();
                for (const auto& record : result.tracks)
                    nextTrack = juce::jmax (nextTrack,
                                            record.trackIndex + (record.stereo ? 2 : 1));
                if (nextTrack > self->engine.getRecorder().getNumTracks())
                    self->engine.setStripCount (nextTrack);

                for (const auto& record : result.tracks)
                {
                    self->engine.setTrackName (record.trackIndex, record.name);
                    self->engine.setTrackStereo (record.trackIndex, record.stereo);
                    if (record.stereo)
                    {
                        self->engine.setTrackName (record.trackIndex + 1, record.name + " R");
                        self->engine.setTrackStereo (record.trackIndex + 1, false);
                        self->engine.setTrackPan (record.trackIndex, -1.0f);
                        self->engine.setTrackPan (record.trackIndex + 1, 1.0f);
                        self->engine.setTrackLinkedRouting (record.trackIndex, record.trackIndex);
                        self->engine.setTrackLinkedRouting (record.trackIndex + 1,
                                                            record.trackIndex + 1);
                    }
                    else
                    {
                        self->engine.setTrackLinkedRouting (record.trackIndex, record.trackIndex);
                    }
                }

                self->lastTrackCount = -1;
                const auto savedEdits = self->engine.playlistsToJson();
                const int loaded = self->engine.loadSession (sessionDir, true);
                self->engine.loadPlaylistsFromJson (savedEdits);
                const bool saved = self->saveSessionStateTo (sessionDir);

                const int stereoCount = (int) std::count_if (
                    result.tracks.begin(), result.tracks.end(),
                    [] (const auto& record) { return record.stereo; });
                self->showStatus ("Imported " + juce::String ((int) result.tracks.size())
                    + " file(s), " + juce::String (stereoCount) + " stereo, "
                    + juce::String ((int) result.tracks.size() - stereoCount) + " mono"
                    + (result.converted > 0
                        ? " (" + juce::String (result.converted) + " resampled to "
                            + juce::String (targetSr / 1000.0, 1) + " kHz)"
                        : juce::String())
                    + (result.failed > 0
                        ? " (skipped " + juce::String (result.failed) + ")"
                        : juce::String())
                    + " -- loaded " + juce::String (loaded) + " for playback"
                    + (saved ? juce::String() : juce::String ("; WARNING: session state was not saved")));
            });
        });
    });
}

void MainComponent::onSaveSessionAs()
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before Save As"); return; }
    if (sessionIoBusy.exchange (true))
    {
        showStatus ("Another session file operation is already running");
        return;
    }
    // Source may or may not exist yet:
    //  * If the engineer made a session via File ▸ New Session... or
    //    loaded one with Open Session..., getActiveSessionDir() points
    //    at it and Save As clones the whole folder to the new spot.
    //  * If there's no active session, Save As still works as a
    //    'save current mixer state to a new folder' flow -- it just
    //    skips the directory copy.
    const auto source = engine.getActiveSessionDir();

    chooser = std::make_unique<juce::FileChooser> (
        "Save session copy in...",
        getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    juce::Component::SafePointer<MainComponent> chooserSelf (this);
    chooser->launchAsync (flags, [chooserSelf, this, source] (const juce::FileChooser& fc)
    {
        if (chooserSelf == nullptr) return;
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) { sessionIoBusy.store (false); return; }

        if (source == dest)
        {
            const bool ok = saveSessionStateTo (dest);
            sessionIoBusy.store (false);
            showStatus (ok ? "Session saved" : "Save failed -- check permissions / free space");
            return;
        }

        const bool createdDestination = ! dest.exists();
        if (createdDestination && dest.createDirectory().failed())
        { sessionIoBusy.store (false); showStatus ("Save As destination invalid"); return; }
        if (! dest.isDirectory())
        { sessionIoBusy.store (false); showStatus ("Save As destination invalid"); return; }

        // Refuse a destination INSIDE the source: copyDirectoryTo would copy
        // the growing destination into itself.
        if (source.isDirectory() && pathsafety::isSameOrDescendant (source, dest))
        {
            sessionIoBusy.store (false);
            showStatus ("Save As failed -- pick a folder outside the current session");
            return;
        }

        // Save As is a clone, never a merge.  Refusing a non-empty folder
        // prevents JUCE's copyFileTo from deleting same-named destination
        // files and prevents an old session from becoming a hybrid.
        if (! dest.findChildFiles (juce::File::findFilesAndDirectories, false).isEmpty())
        {
            sessionIoBusy.store (false);
            showStatus ("Save As needs a new or empty folder");
            return;
        }

        const bool needsCopy = source.isDirectory() && source != dest;
        if (! needsCopy)
        {
            if (! ensureSessionScaffold (dest))
            {
                removePartialCopy (dest, createdDestination);
                sessionIoBusy.store (false);
                showStatus ("Save As failed -- couldn't create the session files");
                return;
            }
            engine.setActiveSessionDir (dest);
            const bool ok = saveSessionStateTo (dest);
            sessionIoBusy.store (false);
            showStatus (ok ? "Saved As -> " + dest.getFileName()
                           : "Save As failed -- check permissions / free space");
            return;
        }

        // Freeze a complete source snapshot before the background copy begins.
        if (! saveSessionStateTo (source))
        {
            removePartialCopy (dest, createdDestination);
            sessionIoBusy.store (false);
            showStatus ("Save As failed -- couldn't save the current session");
            return;
        }

        // The folder copy carries the AUDIO -- gigabytes on a real show -- so it
        // runs on the owned export thread instead of freezing the message
        // thread for the duration. The state files are written afterwards, back
        // on the message thread, because saveSessionStateTo touches engine state.
        joinExportThread();
        engine.setSessionTransitionActive (true);
        showStatus ("Saving As -> " + dest.getFileName() + " (copying session)...");

        juce::Component::SafePointer<MainComponent> self (this);
        exportThread = std::thread ([self, source, dest, createdDestination]
        {
            const bool ok = self != nullptr
                         && copyDirectoryCancellable (source, dest, self->exportCancel);
            juce::MessageManager::callAsync ([self, ok, dest, createdDestination]
            {
                if (self == nullptr) return;
                self->engine.setSessionTransitionActive (false);
                self->sessionIoBusy.store (false);
                if (! ok)
                {
                    removePartialCopy (dest, createdDestination);
                    self->showStatus ("Save As cancelled or failed -- source session is unchanged");
                    return;
                }
                self->openSessionFolder (dest);
                self->showStatus ("Saved As -> " + dest.getFileName());
            });
        });
    });
}

// The rate the session's audio is actually AT -- what the export dialog should
// default to, so "just export it" doesn't silently resample. Prefer the loaded
// player (it read the files), fall back to the session setting, then the device.
static double sessionAudioRateFor (zynforge::AudioEngine& engine)
{
    if (const double pr = engine.getPlayer().getSampleRate(); pr > 0.0) return pr;
    if (const double ss = engine.getSessionSampleRate();      ss > 0.0) return ss;
    return engine.getDeviceSampleRate();
}

void MainComponent::onExportAllTracks()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::ExportDialog::launch ("Export all tracks", sessionAudioRateFor (engine),
        [self, this] (std::optional<zynforge::ExportOptions> opts)
    {
        if (self == nullptr || ! opts.has_value()) return;
        const auto chosenOpts = *opts;

        const auto activeSession = engine.getActiveSessionDir();
        const auto exportDir    = activeSession.isDirectory()
                                       ? activeSession.getChildFile ("Export Files")
                                       : getSessionsRoot();
        exportDir.createDirectory();
        chooser = std::make_unique<juce::FileChooser> (
            "Export all tracks to...", exportDir, "");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags,
            [self, this, chosenOpts] (const juce::FileChooser& fc)
        {
            if (self == nullptr) return;
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            std::vector<int> all;
            for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i) all.push_back (i);

            // Async: reports its own progress + completion (or the INCOMPLETE
            // / failed warning) when the worker finishes.
            startExportTracksTo (dest, all, chosenOpts);
        });
    });
}

void MainComponent::onExportIndividualTrack (int channelIndex)
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::ExportDialog::launch ("Export track", sessionAudioRateFor (engine),
        [self, this, channelIndex] (std::optional<zynforge::ExportOptions> opts)
    {
        if (self == nullptr || ! opts.has_value()) return;
        const auto chosenOpts = *opts;

        const auto activeSession = engine.getActiveSessionDir();
        const auto exportDir    = activeSession.isDirectory()
                                       ? activeSession.getChildFile ("Export Files")
                                       : getSessionsRoot();
        exportDir.createDirectory();
        chooser = std::make_unique<juce::FileChooser> (
            "Export track to...", exportDir, "");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags,
            [self, this, channelIndex, chosenOpts] (const juce::FileChooser& fc)
        {
            if (self == nullptr) return;
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            startExportTracksTo (dest, { channelIndex }, chosenOpts);
        });
    });
}

void MainComponent::onExportIndividualTracks()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    const int n = engine.getRecorder().getNumTracks();
    if (n <= 0) { showStatus ("No tracks to export"); return; }

    // Step 1: tick-box picker. ONE row per logical strip -- a stereo pair
    // shows as a single "(stereo)" entry, not two mono rows. Each row maps
    // back to its physical channel index(es) so the export collapses the
    // pair into one stereo file.
    std::vector<juce::String> names;
    std::vector<std::vector<int>> rowChannels;   // dialog row -> physical channels
    for (int i = 0; i < n; )
    {
        const auto& t = engine.getRecorder().getTrack (i);
        const bool stereo = t.isStereo.load (std::memory_order_relaxed) && (i + 1 < n);
        const auto nm = t.name.isNotEmpty() ? t.name : juce::String (i + 1);
        names.push_back (juce::String::formatted ("%02d  ", i + 1) + nm
                         + (stereo ? juce::String ("  (stereo)") : juce::String()));
        if (stereo) { rowChannels.push_back ({ i, i + 1 }); i += 2; }
        else        { rowChannels.push_back ({ i });        i += 1; }
    }

    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::TrackSelectDialog::launch ("Export individual tracks", names,
        [self, this, rowChannels] (std::optional<std::vector<int>> picked)
    {
        if (self == nullptr || ! picked.has_value() || picked->empty()) return;

        // Map picked dialog rows back to physical channel indices (a stereo
        // row expands to both halves; exportTracksTo writes them as one file).
        std::vector<int> chosenTracks;
        for (int row : *picked)
            if (row >= 0 && row < (int) rowChannels.size())
                for (int ch : rowChannels[(size_t) row])
                    chosenTracks.push_back (ch);
        if (chosenTracks.empty()) return;

        // Step 2: format / sample-rate / bit-depth (or MP3 bitrate).
        zynforge::ExportDialog::launch ("Export format", sessionAudioRateFor (engine),
            [self, this, chosenTracks] (std::optional<zynforge::ExportOptions> opts)
        {
            if (self == nullptr || ! opts.has_value()) return;
            const auto chosenOpts = *opts;

            // Step 3: choose the destination folder, then export.
            const auto activeSession = engine.getActiveSessionDir();
            const auto exportDir = activeSession.isDirectory()
                                       ? activeSession.getChildFile ("Export Files")
                                       : getSessionsRoot();
            exportDir.createDirectory();
            chooser = std::make_unique<juce::FileChooser> (
                "Export tracks to...", exportDir, "");

            const auto flags = juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectDirectories;

            chooser->launchAsync (flags,
                [self, this, chosenTracks, chosenOpts] (const juce::FileChooser& fc)
            {
                if (self == nullptr) return;
                auto dest = fc.getResult();
                if (dest.getFullPathName().isEmpty()) return;
                if (! dest.exists()) dest.createDirectory();

                startExportTracksTo (dest, chosenTracks, chosenOpts);
            });
        });
    });
}

void MainComponent::warnIfSampleRateMismatch()
{
    auto* dev = engine.getDeviceManager().getCurrentAudioDevice();
    const double sessSR = engine.getPlayer().getSampleRate();
    const double devSR  = dev != nullptr ? dev->getCurrentSampleRate() : 0.0;
    if (sessSR <= 0.0 || devSR <= 0.0) return;
    if (std::abs (sessSR - devSR) < 0.5) return;

    juce::AlertWindow::showMessageBoxAsync (
        juce::MessageBoxIconType::NoIcon,
        "Sample-rate mismatch",
        "This session was recorded at " + juce::String ((int) sessSR)
            + " Hz but your audio device is set to " + juce::String ((int) devSR)
            + " Hz.\n\nPlayback will be pitched + sped up / slowed down. "
              "Open DEVICE and switch to "
            + juce::String ((int) sessSR) + " Hz for clean playback.",
        "OK");
}

void MainComponent::applySessionSampleRate (double sr)
{
    if (sr <= 0.0) return;

    // The session's intended rate -- what the record guard + banner compare
    // the live device clock against. The 10 Hz timer keeps the engine copy
    // in lockstep from this, so updating pendingSampleRate is authoritative.
    //
    // NEVER mid-take: the device push below restarts the device, and
    // audioDeviceStopped -> recorder.release() -> stopRecording() would end the
    // recording. Changing the session rate during a take is meaningless anyway
    // -- the audio on disk is already at the device's rate.
    if (engine.isRecording())
    {
        showStatus ("Recording -- stop the take before changing the session sample rate.");
        return;
    }
    pendingSampleRate = sr;
    engine.setSessionSampleRate (sr);

    // Ask the hardware to follow. A clock-slaved device (Dante / wordclock)
    // may refuse and stay on its network rate -- in which case the mismatch
    // banner will (correctly) light, telling the engineer to fix the clock.
    auto setup = engine.getDeviceManager().getAudioDeviceSetup();
    if (! juce::approximatelyEqual (setup.sampleRate, sr))
    {
        setup.sampleRate = sr;
        engine.getDeviceManager().setAudioDeviceSetup (setup, true);
    }

    refreshFormatButton();

    const double dev = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                         ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
                         : 0.0;
    const auto khz = [] (double s)
    {
        const double k = s / 1000.0;
        return ((k == std::floor (k)) ? juce::String ((int) k) : juce::String (k, 1)) + " kHz";
    };
    if (dev > 0.0 && std::abs (dev - sr) > 1.0)
        showStatus ("Session set to " + khz (sr) + " -- but the device is locked at "
                    + khz (dev) + ". Match the clock to record.");
    else
        showStatus ("Session sample rate set to " + khz (sr));
}

void MainComponent::exportTimelineCsv()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) { showStatus ("No active session to export."); return; }

    namespace tx = zynforge::timelineexport;
    std::vector<tx::TrackEntry> tracks;
    auto& rec = engine.getRecorder();
    // Name the file that's ACTUALLY on disk. This hardcoded ".wav", so the CSV
    // handed to the mix engineer referenced files that don't exist in a FLAC or
    // AIFF session. Resolve the real container per track; fall back to the
    // current capture format's extension when the track was never recorded.
    const auto audioDir = dir.getChildFile ("Audio Files").isDirectory()
                            ? dir.getChildFile ("Audio Files") : dir;
    for (int i = 0; i < rec.getNumTracks(); ++i)
    {
        const auto stem = "Track_" + juce::String (i + 1).paddedLeft ('0', 2);
        juce::String fileName = stem + ".wav";
        for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
            if (audioDir.getChildFile (stem + ext).existsAsFile())
            { fileName = stem + ext; break; }
        tracks.push_back ({ i + 1, rec.getTrack (i).name, fileName });
    }

    std::vector<tx::MarkEntry> markers;
    auto& mk = engine.getMarkers();
    for (int i = 0; i < mk.getCount(); ++i)
    {
        const auto m = mk.getMarker (i);
        markers.push_back ({ m.sampleOffset, m.name, m.type });
    }

    std::vector<tx::MarkEntry> cueList;
    for (const auto& c : cues)
        cueList.push_back ({ c.samplePos, c.name, juce::String() });

    const double sr = engine.getSessionSampleRate() > 0.0 ? engine.getSessionSampleRate()
                                                          : pendingSampleRate;
    const auto csv = tx::buildCsv (dir.getFileName(), sr, 30, tracks, markers, cueList);

    const auto def = dir.getChildFile ("Export Files")
                        .getChildFile (dir.getFileName() + "_timeline.csv");
    chooser = std::make_unique<juce::FileChooser> ("Export session timeline (CSV)", def, "*.csv");
    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [self, this, csv] (const juce::FileChooser& fc)
        {
            if (self == nullptr) return;
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            f.getParentDirectory().createDirectory();
            showStatus (zynforge::atomicfile::writeText (f, csv)
                            ? "Timeline exported: " + f.getFileName()
                            : "Couldn't write " + f.getFileName()
                                + "; previous export was preserved");
        });
}

void MainComponent::relocateActiveSession()
{
    if (sessionIoBusy.exchange (true))
    {
        showStatus ("Another session file operation is already running");
        return;
    }
    if (engine.isRecording())
    {
        sessionIoBusy.store (false);
        showStatus ("Stop recording before moving the session.");
        return;
    }
    const auto oldDir = engine.getActiveSessionDir();
    if (! oldDir.isDirectory())
    {
        sessionIoBusy.store (false);
        showStatus ("No active session to move -- record or open one first.");
        return;
    }

    chooser = std::make_unique<juce::FileChooser> (
        "Choose a new location for this session",
        oldDir.getParentDirectory(), "");

    juce::Component::SafePointer<MainComponent> chooserSelf (this);
    chooser->launchAsync (juce::FileBrowserComponent::canSelectDirectories
                          | juce::FileBrowserComponent::openMode,
        [chooserSelf, this, oldDir] (const juce::FileChooser& fc)
        {
            if (chooserSelf == nullptr) return;
            const auto newParent = fc.getResult();
            if (! newParent.isDirectory()) { sessionIoBusy.store (false); return; }

            const auto newDir = newParent.getChildFile (oldDir.getFileName());
            if (newDir == oldDir)
            { sessionIoBusy.store (false); showStatus ("That's already the session's location."); return; }
            if (pathsafety::isSameOrDescendant (oldDir, newParent)
                || pathsafety::isSameOrDescendant (oldDir, newDir))
            {
                sessionIoBusy.store (false);
                showStatus ("Move failed -- the destination cannot be inside the session");
                return;
            }
            if (newDir.exists())
            {
                sessionIoBusy.store (false);
                showStatus ("A folder named \"" + oldDir.getFileName()
                            + "\" already exists there -- pick another location.");
                return;
            }

            if (! saveSessionStateTo (oldDir))
            {
                sessionIoBusy.store (false);
                showStatus ("Move failed -- couldn't save the current session");
                return;
            }

            // Drop any open playback handles so the files aren't busy while
            // we move them, then do the (possibly cross-volume) move off the
            // message thread so a big session can't freeze the UI.
            engine.stopPlayback();
            engine.setSessionTransitionActive (true);
            joinExportThread();
            showStatus ("Moving session to " + newParent.getFullPathName() + " ...");

            juce::Component::SafePointer<MainComponent> self (this);
            exportThread = std::thread ([self, oldDir, newDir, newParent]
            {
                bool relocated = oldDir.moveFileTo (newDir);   // instant within a volume
                bool oldCopyRemains = false;
                if (! relocated)                               // cross-volume: copy + delete
                {
                    const bool copied = self != nullptr
                                     && copyDirectoryCancellable (oldDir, newDir, self->exportCancel);
                    if (copied && ! self->exportCancel.load (std::memory_order_relaxed))
                    {
                        relocated = true; // the complete new copy is authoritative
                        oldCopyRemains = ! oldDir.deleteRecursively();
                    }
                    else
                        newDir.deleteRecursively();
                }

                juce::MessageManager::callAsync ([self, relocated, oldCopyRemains, oldDir, newDir, newParent]
                {
                    if (self == nullptr) return;
                    self->engine.setSessionTransitionActive (false);
                    self->sessionIoBusy.store (false);
                    if (! relocated)
                    {
                        self->showStatus ("Move failed -- check permissions / free space. Session left in place.");
                        return;
                    }
                    if (auto* p = self->engine.getAppProps())
                    {
                        p->setValue ("sessionsRoot", newParent.getFullPathName());
                        p->saveIfNeeded();
                    }
                    self->openSessionFolder (newDir);   // re-pin paths + reload audio from the new home
                    self->showStatus (oldCopyRemains
                        ? "Session moved, but the old folder could not be fully removed: "
                            + oldDir.getFullPathName()
                        : "Session moved to " + newDir.getFullPathName());
                });
            });
        });
}

void MainComponent::onLoadSessionClicked()
{
    if (sessionIoBusy.load()) { showStatus ("Wait for the session file operation to finish"); return; }
    chooser = std::make_unique<juce::FileChooser> (
        "Choose a session folder",
        getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (flags, [self, this] (const juce::FileChooser& fc)
    {
        if (self == nullptr) return;
        const auto dir = fc.getResult();
        if (! dir.isDirectory()) return;

        // Canonical full open (pins the active dir, restores setlist/UI/mixer,
        // loads audio, sizes the mixer incl. the no-session_mix.json fallback).
        const int n = openSessionFolder (dir);
        if (n <= 0)
        {
            const auto audioDir = dir.getChildFile ("Audio Files");
            const auto scanDir = audioDir.isDirectory() ? audioDir : dir;
            const bool hasMedia = ! scanDir.findChildFiles (
                juce::File::findFiles, false,
                "Track_*.wav;Track_*.flac;Track_*.aif;Track_*.aiff").isEmpty();
            statusLabel.setText (hasMedia
                ? "Session audio is unreadable. Existing files are protected; Record will append safely."
                : "No Track_* audio found in folder",
                juce::dontSendNotification);
        }
        playButton.setButtonText ("PLAY");
        updateTransportLabels();
    });
}

juce::File MainComponent::getSessionsRoot() const
{
    // Engineer can override via the New Session / Welcome dialog ("Local
    // Storage:") -- stored in appProps as 'sessionsRoot', falling back to
    // ~/Music/Zynforge Sessions. The resolution lives on the ENGINE so the
    // network remote-record paths (OSC, companion) share it instead of
    // hardcoding the Music folder.
    return engine.getSessionsRoot();
}

// ── Session templates ───────────────────────────────────────────────
// A template captures the engineer's per-strip layout -- count,
// names, colours, stereo pairs, input + output routings -- and
// nothing else (sample rate / device live on the audio device).
// Persisted as JSON under
//   ~/Library/Application Support/Zynforge Recording/Templates/<name>.zftemplate
// Picking "New Session from Template" applies the template to a
// fresh session.
juce::File MainComponent::templatesDir() const
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Zynforge Recording")
                    .getChildFile ("Templates");
    base.createDirectory();
    return base;
}

juce::Array<juce::File> MainComponent::listSessionTemplates() const
{
    return templatesDir().findChildFiles (juce::File::findFiles, false, "*.zftemplate");
}

void MainComponent::promptSaveSessionTemplate()
{
    auto* aw = new juce::AlertWindow ("Save session template",
                                       "Name this template:",
                                       juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    aw->addTextEditor ("name", "", {});
    aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog::primeNameEditor (*aw, "name");   // focus + select-all + Enter = Save
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw, self] (int r)
    {
        std::unique_ptr<juce::AlertWindow> dispose (aw);
        if (r != 1 || self == nullptr) return;
        const auto name = aw->getTextEditorContents ("name").trim();
        if (name.isEmpty()) return;

        juce::DynamicObject::Ptr obj (new juce::DynamicObject());
        obj->setProperty ("name",       name);
        obj->setProperty ("createdAt",  juce::Time::getCurrentTime().toISO8601 (true));
        obj->setProperty ("trackCount", self->engine.getRecorder().getNumTracks());

        juce::Array<juce::var> strips;
        for (int i = 0; i < self->engine.getRecorder().getNumTracks(); ++i)
        {
            auto& t = self->engine.getRecorder().getTrack (i);
            juce::DynamicObject::Ptr s (new juce::DynamicObject());
            s->setProperty ("name",    t.name);
            s->setProperty ("colour",  (juce::int64) t.colourARGB.load (std::memory_order_relaxed));
            s->setProperty ("inRoute", t.inputRouting .load (std::memory_order_relaxed));
            s->setProperty ("outRoute",t.outputRouting.load (std::memory_order_relaxed));
            s->setProperty ("stereo",  t.isStereo.load (std::memory_order_relaxed));
            s->setProperty ("gainDb",  (double) t.gainDb.load (std::memory_order_relaxed));
            s->setProperty ("pan",     (double) t.pan   .load (std::memory_order_relaxed));
            strips.add (juce::var (s.get()));
        }
        obj->setProperty ("strips", juce::var (strips));

        const auto safeName = name.replaceCharacters ("/:\\?*<>|\"", "         ").trim();
        const auto out = self->templatesDir().getChildFile (safeName + ".zftemplate");
        self->showStatus (zynforge::atomicfile::writeText (
                              out, juce::JSON::toString (juce::var (obj.get())))
                            ? "Template saved -> " + out.getFileName()
                            : "Template NOT saved -- check permissions / free space");
    }));
}

void MainComponent::applySessionTemplate (const juce::File& templateFile)
{
    if (engine.isRecording()) { showStatus ("Stop recording first"); return; }

    const auto parsed = juce::JSON::parse (templateFile);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) { showStatus ("Failed to read template"); return; }

    const int n = (int) obj->getProperty ("trackCount");
    if (n <= 0) { showStatus ("Template has no strips"); return; }

    // The click strip is SESSION-scoped and a template replaces the whole strip
    // layout, so any index carried over is stale. Leaving it set made the
    // "is this really a Click strip?" guard in generateOrRefreshClickTrack fail
    // forever after ("Click slot isn't a Click strip"), permanently disabling
    // Generate Click Track for the rest of the session -- every other
    // session-boundary path (open / close / new) already resets it.
    clickTrackIndex = -1;

    // Stop the live strips' meter/spectrum timers before setStripCount frees
    // the TrackStates they reference (a template with fewer strips shrinks the
    // recorder vector); the strips rebuild on the next 10 Hz tick.
    condemnAllStrips();
    engine.resetAllStripState();
    engine.setStripCount (n);

    if (auto* arr = obj->getProperty ("strips").getArray())
    {
        for (int i = 0; i < arr->size() && i < n; ++i)
        {
            auto* s = (*arr)[i].getDynamicObject();
            if (s == nullptr) continue;
            const auto nm = s->getProperty ("name").toString();
            if (nm.isNotEmpty()) engine.setTrackName (i, nm);
            const auto col = (juce::uint32) (juce::int64) s->getProperty ("colour");
            if (col != 0) engine.setTrackColour (i, juce::Colour (col));
            engine.setTrackInputRouting  (i, (int) s->getProperty ("inRoute"));
            engine.setTrackOutputRouting (i, (int) s->getProperty ("outRoute"));
            engine.setTrackStereo (i, (bool) s->getProperty ("stereo"));
            engine.setTrackGainDb (i, (float) (double) s->getProperty ("gainDb"));
            engine.setTrackPan    (i, (float) (double) s->getProperty ("pan"));
        }
    }
    lastTrackCount = -1;
    showStatus ("Applied template: " + templateFile.getFileNameWithoutExtension());
}

void MainComponent::loadUILayoutFromActiveSession()
{
    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj == juce::File{}) return;

    const auto parsed = juce::JSON::parse (proj);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return;

    const auto uiVar = obj->getProperty ("ui");
    auto* ui = uiVar.getDynamicObject();
    if (ui == nullptr) return;

    // View -- must call switchView (not just write currentView) so the
    // page visibility + automation toolbar flip correctly.
    const auto viewStr = ui->getProperty ("view").toString();
    if (viewStr.isNotEmpty())
        switchView (viewStr == "Edit" ? View::Edit : View::Mix);

    // Strip width preset.
    const auto sw = ui->getProperty ("stripWidth").toString();
    if      (sw == "XS") setStripWidthPreset (StripWidth::XS);
    else if (sw == "S")  setStripWidthPreset (StripWidth::S);
    else if (sw == "L")  setStripWidthPreset (StripWidth::L);
    else if (sw == "M")  setStripWidthPreset (StripWidth::M);

    // VCA panel visibility.
    showVcaPanel = (bool) ui->getProperty ("vcaPanel");
    if (vcaPanel != nullptr) vcaPanel->setVisible (showVcaPanel);

    // EDIT zoom level.
    if (editPage != nullptr)
        editPage->setZoom ((float) (double) ui->getProperty ("editZoom"));

    resized();
}

juce::File MainComponent::getDefaultTemplate() const
{
    if (auto* props = engine.getAppProps())
    {
        const auto path = props->getValue ("defaultTemplateFile", {});
        if (path.isNotEmpty())
        {
            juce::File f (path);
            if (f.existsAsFile()) return f;
        }
    }
    return {};
}

void MainComponent::setDefaultTemplate (const juce::File& templateFile)
{
    if (auto* props = engine.getAppProps())
    {
        props->setValue ("defaultTemplateFile", templateFile.getFullPathName());
        props->saveIfNeeded();
    }
    showStatus (templateFile == juce::File{}
                  ? juce::String ("Default template cleared")
                  : "Default template -> " + templateFile.getFileNameWithoutExtension());
}

void MainComponent::promptDeleteSessionTemplate()
{
    const auto list = listSessionTemplates();
    if (list.isEmpty()) return;

    juce::PopupMenu m;
    for (int i = 0; i < list.size(); ++i)
        m.addItem (i + 1, "Delete: " + list[i].getFileNameWithoutExtension());
    juce::Component::SafePointer<MainComponent> self (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [self, list] (int chosen)
    {
        if (chosen <= 0 || self == nullptr) return;
        const int idx = chosen - 1;
        if (idx < list.size())
        {
            list[idx].deleteFile();
            self->showStatus ("Template deleted: " + list[idx].getFileNameWithoutExtension());
        }
    });
}

juce::File MainComponent::makeNewSessionDir() const
{
    // If the engineer has created a named session via File ▸ New Session...,
    // every RECORD lands inside that folder (Audio Files/ subdirectory is
    // handled by MultitrackRecorder::startRecording). Otherwise we fall
    // back to the legacy auto-stamped folder so a bare RECORD click still
    // produces a session.
    if (auto* props = engine.getAppProps())
    {
        const auto saved = props->getValue ("activeSessionDir", {});
        if (saved.isNotEmpty())
        {
            juce::File f (saved);
            if (f.isDirectory() || f.createDirectory().wasOk())
                return f;
        }
    }
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    const auto root = getSessionsRoot();
    return root.getNonexistentChildFile ("Session_" + stamp, {}, false);
}

juce::File MainComponent::createSessionFolderStructure (const zynforge::NewSessionDialog::Result& r)
{
    // Resolve a safe folder name even if the engineer typed something
    // with slashes / colons in the picker.
    auto safeName = juce::File::createLegalFileName (r.name);
    if (safeName.isEmpty()) safeName = "Untitled-1";

    // Ensure the chosen Local Storage exists, then make the session
    // folder underneath it. If that name already exists, append a
    // numeric suffix so we don't trample on an existing session.
    if (r.location.createDirectory().failed() || ! r.location.isDirectory())
        return {};

    const juce::File sessionFolder = r.location.getNonexistentChildFile (safeName, {}, false);
    if (sessionFolder.createDirectory().failed() || ! sessionFolder.isDirectory())
        return {};

    // Subfolders -- only the ones actually wired today. Clip Groups
    // and Video Files were Pro Tools-style placeholders that nothing
    // read or wrote, so they're dropped to avoid confusing the
    // engineer with empty folders.
    const bool madeFolders = sessionFolder.getChildFile ("Audio Files")         .createDirectory().wasOk()
                          && sessionFolder.getChildFile ("Export Files")       .createDirectory().wasOk()
                          && sessionFolder.getChildFile ("Session File Backups").createDirectory().wasOk();
    if (! madeFolders)
    {
        sessionFolder.deleteRecursively();
        return {};
    }

    // Session document -- a small JSON file that ties the folder together.
    // (Equivalent of Pro Tools' .ptx; we use .zfproj for clarity.)
    {
        juce::DynamicObject::Ptr m (new juce::DynamicObject());
        m->setProperty ("zynforgeSession", true);
        m->setProperty ("name",            safeName);
        m->setProperty ("createdAt",       juce::Time::getCurrentTime().toISO8601 (true));
        m->setProperty ("sampleRate",      r.sampleRate);
        m->setProperty ("captureFormat",   (int) r.captureFormat);
        m->setProperty ("interleaved",     r.interleaved);
        m->setProperty ("ioPreset",        r.ioSettings);
        if (! zynforge::atomicfile::writeText (
                sessionFolder.getChildFile (safeName + ".zfproj"),
                juce::JSON::toString (juce::var (m.get()))))
        {
            sessionFolder.deleteRecursively();
            return {};
        }
    }

    // WaveCache.wfm is written on demand by EditPage::saveCacheToSession
    // (on close) and read back on session open. No placeholder needed.

    // Only clear the previous session after every filesystem operation has
    // succeeded; a permissions/disk failure must leave the current work open.
    engine.clearSessionState();
    engine.clearAllStripOverrides();
    clickTrackIndex = -1;
    return sessionFolder;
}
