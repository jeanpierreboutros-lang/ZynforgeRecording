// Session lifecycle + persistence + export + template methods on
// MainComponent. Extracted from MainComponent.cpp as part of the
// 2026-05-24 god-class split.
//
// Includes: saveSessionStateTo, exportTracksTo, onSaveSessionState,
// onImportAudioFiles, onSaveSessionAs, onExportAudio,
// warnIfSampleRateMismatch, onLoadSessionClicked, getSessionsRoot, templatesDir,
// promptSaveSessionTemplate, applySessionTemplate,
// loadUILayoutFromActiveSession, promptDeleteSessionTemplate,
// makeNewSessionDir, createSessionFolderStructure.
//
// showPreflightChecklist + onDeviceClicked stay in MainComponent.cpp
// because they're not really session IO (preflight is a status
// report; onDeviceClicked launches the audio-device dialog).

#include "MainComponent.h"
#include "RecordPosition.h"
#include "../Audio/AudioImport.h"
#include "../Audio/AiffRepair.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/TrackFileTransaction.h"
#include "../Audio/PathSafety.h"
#include "../Audio/ShowHandoff.h"
#include "../Theme/DialogChrome.h"
#include "../Audio/TimelineExport.h"
#include "NewSessionDialog.h"
#include "WaveCacheFile.h"
#include "ExportDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

namespace
{
juce::String applicationVersion()
{
    auto* app = juce::JUCEApplicationBase::getInstance();
    return app != nullptr ? app->getApplicationVersion() : juce::String ("unknown");
}

bool copyFileCancellable (const juce::File& source, const juce::File& target,
                          const std::atomic<bool>& cancel)
{
    auto in = source.createInputStream();
    target.deleteFile();
    auto out = target.createOutputStream();
    if (in == nullptr || out == nullptr) return false;
    const auto expectedSize = source.getSize();
    // On the heap: this runs on a job thread, whose 512 KiB stack cannot hold
    // it, and a frame that size faults the moment the function is entered.
    std::vector<char> buffer (1024 * 1024);
    while (! in->isExhausted())
    {
        if (cancel.load (std::memory_order_relaxed)) return false;
        const int got = in->read (buffer.data(), (int) buffer.size());
        if (got < 0 || (got > 0 && ! out->write (buffer.data(), (size_t) got)))
            return false;
        if (got == 0) break;
    }
    out->flush();
    return ! in->getStatus().failed() && ! out->getStatus().failed()
        && in->getPosition() == expectedSize && out->getPosition() == expectedSize
        && source.getSize() == expectedSize;
}

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
        if (zynforge::atomicfile::isStagingFile (child)) continue;
        if (child.isSymbolicLink()) return false;
        const auto target = dest.getChildFile (child.getFileName());
        if (child.isDirectory())
        {
            if (! copyDirectoryCancellable (child, target, cancel)) return false;
        }
        else if (! copyFileCancellable (child, target, cancel))
        {
            // A cache can be rewritten while this runs, and losing it costs a
            // rescan. Anything else missing would make the copy incomplete.
            if (cancel.load (std::memory_order_relaxed) || ! wavecache::isCacheFile (child)) return false;
            target.deleteFile();
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

bool MainComponent::isSessionOpen (const juce::File& dir) const
{
    // Finder and a typed path can spell one folder differently (a link, say).
    const auto current = engine.getActiveSessionDir();
    return engine.mixerBelongsTo (current)
        && pathsafety::isSameOrDescendant (current, dir)
        && pathsafety::isSameOrDescendant (dir, current);
}

int MainComponent::openSessionFolder (const juce::File& dir, bool reload)
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before switching sessions"); return -1; }
    // Every step below no-ops under finalization, so the old mixer would be
    // left in place and claimed as this session's.
    if (engine.isCaptureFinalizing())
    { showStatus ("Wait for recording finalization before switching sessions"); return -1; }
    if (! dir.isDirectory()) return 0;
    if (! reload && isSessionOpen (dir))
    {
        showStatus (dir.getFileName() + " is already open");
        return engine.getPlayer().getNumTracks();
    }
    if (! zynforge::TrackFileTransaction::recover (dir))
    { showStatus ("Session has an incomplete file move; recovery files were retained in Session File Backups"); return -1; }
    int interruptedPunches = 0, repairedHeaders = 0, unrepairedHeaders = 0;
    const auto audioDirForRecovery = dir.getChildFile ("Audio Files");
    zynforge::aiffrepair::repairTakeFolder (audioDirForRecovery.isDirectory() ? audioDirForRecovery : dir,
                                            &repairedHeaders, &unrepairedHeaders);
    if (! zynforge::MultitrackRecorder::recoverInterruptedPunches (
            audioDirForRecovery.isDirectory() ? audioDirForRecovery : dir,
            &interruptedPunches))
    {
        showStatus ("Interrupted punch could not be recovered; original sidecars were retained");
        return -1;
    }

    keepRemovedFeatureData (dir);

    // Condemn the current strips before loading -- a smaller session shrinks
    // the recorder vector and frees TrackStates the live strips still point at.
    // Before anything loads: a damaged file read as empty, and the next save
    // would then replace the only copy of what was in it.
    const auto damaged = zynforge::sessionrepair::quarantineDamagedFiles (dir);

    condemnAllStrips();
    engine.stopPlayback();
    engine.clearSessionState();
    forgetSessionUiState();
    engine.setActiveSessionDir (dir);        // pin so Save / Save As / Export stay lit
    const int n = engine.loadSession (dir);  // audio -> player + clips
    if (n < 0)
    {
        showStatus ("Session could not be loaded; its files were left untouched");
        return -1;
    }
    const bool loadedMix = engine.loadSessionMixFrom (dir);
    // Recovery fallback: a session recorded but never explicitly Saved has no
    // session_mix.json, so loadSessionMixFrom can't size the mixer and the app
    // would show "No channels yet" with every Edit / Track / Export menu grey.
    // Size one strip per loaded audio track so the channels (and the menus)
    // come back. A mix saved with no strips over a session that has audio is
    // the same case, not an authoritative empty mixer.
    if (! loadedMix || (engine.getRecorder().getNumTracks() == 0 && n > 0))
        engine.setStripCount (n);
    restoreSessionDocuments (dir);
    engine.claimMixerFor (dir);
    if (n > 0)
    {
        statusLabel.setText ("Loaded " + juce::String (n) + " tracks", juce::dontSendNotification);
        warnIfSampleRateMismatch();
    }
    if (unrepairedHeaders > 0)
        toast.show (zynforge::counted (unrepairedHeaders, "AIFF take file") + " from a crash could not be repaired;"
                    " they read short until the drive is writable", Toast::Kind::Warning);
    else if (repairedHeaders > 0)
        toast.show ("Repaired " + zynforge::counted (repairedHeaders, "AIFF take file") + " a crash left unfinished",
                    Toast::Kind::Warning);
    if (interruptedPunches > 0)
        toast.show ("Recovered " + zynforge::counted (interruptedPunches, "interrupted punch file") + "; partial audio is in Session File Backups",
                    Toast::Kind::Warning);
    if (! damaged.empty()) reportDamagedFiles (dir, damaged);
    return n;
}

void MainComponent::keepRemovedFeatureData (const juce::File& dir)
{
    const auto kept = removeddata::keepOriginals (dir);
    if (kept == removeddata::Kept::nothingToDo) return;
    const auto message = kept == removeddata::Kept::justCopied
        ? "This session used features that are gone. Its original files are in Session File Backups/"
              + juce::String (removeddata::kFolderName)
        : "! Could not copy this session's original files to Session File Backups: it cannot be saved until they are";
    // Whoever opened the session ends with its own line, which would replace this.
    juce::Component::SafePointer<MainComponent> self (this);
    juce::MessageManager::callAsync ([self, message]
    {
        if (self != nullptr) self->showStatus (message);
    });
}

void MainComponent::reportDamagedFiles (const juce::File& dir,
                                        const std::vector<zynforge::sessionrepair::DamagedFile>& files)
{
    showStatus ("! " + dir.getFileName() + " has damaged session files; defaults are in use");
    if (damagedFilesPromptForTests) { damagedFilesPromptForTests (files); return; }

    juce::String text;
    bool canRestore = false;
    for (const auto& file : files)
    {
        text << file.name << " " << file.problem << ".\n"
             << (file.keptAs != juce::File() ? "It was kept as " + file.keptAs.getFileName()
                                             : juce::String ("It could not be moved aside (the session may be read-only)"))
             << ".\n"
             << (file.backup != juce::File() ? "Newest good backup: " + file.backup.getFileName()
                                             : juce::String ("No backup holds a good copy"))
             << ".\n\n";
        canRestore = canRestore || file.backup != juce::File();
    }
    text << "Until then the session uses defaults for what is missing. Audio is not affected.";

    auto* aw = new juce::AlertWindow ("Session files are damaged", text, juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel (&laf);
    if (canRestore) aw->addButton ("Restore Backup", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton (canRestore ? "Keep Going" : "OK", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [self, aw, dir, files] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self != nullptr && result == 1) self->restoreDamagedFiles (dir, files);
        }), false);
}

bool MainComponent::restoreDamagedFiles (const juce::File& dir,
                                         const std::vector<zynforge::sessionrepair::DamagedFile>& files)
{
    // The dialog can sit open across a remote RECORD or a session change.
    if (engine.isRecording() || sessionIoBusy.load() || ! isSessionOpen (dir))
    {
        showStatus ("Restore cancelled -- a recording, a save or another session is active");
        return false;
    }
    const auto failed = zynforge::sessionrepair::restoreFromBackups (dir, files);
    if (! failed.isEmpty())
    {
        showStatus ("Could not restore " + failed.joinIntoString (", ") + " -- check the session volume");
        return false;
    }
    if (openSessionFolder (dir, true) < 0) return false;
    showStatus ("Restored from backup: " + dir.getFileName());
    return true;
}

void MainComponent::restoreSessionDocuments (const juce::File& dir)
{
    // Playlists must be restored after loadSession seeds its default
    // clips; loading them first caused the seed to erase the restored edits.
    loadSetlistFromActiveSession();
    loadUILayoutFromActiveSession();
    const auto projectSettings = juce::JSON::parse (findSessionProj (dir));
    if ((double) projectSettings["sampleRate"] > 0.0)
        pendingSampleRate = (double) projectSettings["sampleRate"];
    const auto restoreFormat = [this] (const juce::var& format)
    {
        engine.getRecorder().setCaptureFormat ((CaptureFormat) juce::jlimit (0, (int) CaptureFormat::Flac24, (int) format));
    };
    // New Session records its format in the project. The settings file only
    // exists after the first save, and wins from then on.
    if (projectSettings.hasProperty ("captureFormat")) restoreFormat (projectSettings["captureFormat"]);
    const auto settings = juce::JSON::parse (dir.getChildFile ("session_settings.json"));
    if (auto* s = settings.getDynamicObject())
    {
        if (s->hasProperty ("captureFormat")) restoreFormat (s->getProperty ("captureFormat"));
        if (s->hasProperty ("preRollSeconds"))
            engine.getRecorder().setPreRollSeconds (juce::jlimit (0, 30, (int) s->getProperty ("preRollSeconds")));
        if (s->hasProperty ("loopStart") && s->hasProperty ("loopEnd"))
        {
            // A saved range still loops and edits, but nobody selected it in
            // this run, so it never arms a punch.
            const juce::int64 start = s->getProperty ("loopStart"), end = s->getProperty ("loopEnd");
            auto& player = engine.getPlayer();
            if (! savedRegionSpansTimeline (start, end, player.getTotalLengthSamples()))
                player.setLoopRegion (start, end);
        }
        if (s->hasProperty ("sampleRate")) pendingSampleRate = (double) s->getProperty ("sampleRate");
    }
    if (engine.getPlayer().isLoaded()) pendingSampleRate = engine.getPlayer().getSampleRate();
    engine.setSessionSampleRate (pendingSampleRate);
    lastTrackCount = -1;                      // force a strip rebuild on the next tick
    undoManager.clearUndoHistory();           // a fresh session starts with a clean undo stack
    rebaselineMixerUndo();                    // don't record the load itself as a mixer undo step
}

// A daemon take outlives the GUI that started it, so a relaunch finds the
// session pinned by the daemon's status and no mixer in memory. This is the
// session open without its media: the audio is still being written, its files
// and punch sidecars belong to the daemon, and STOP loads them. Runs before
// the engine reports a take, because strips cannot be resized under one.
void MainComponent::adoptDaemonTakeSession (const zynforge::EngineStatus& status)
{
    const juce::File dir (status.sessionPath);
    if (status.sessionPath.isEmpty() || engine.isRecording() || engine.mixerBelongsTo (dir))
        return;

    keepRemovedFeatureData (dir);

    // A session opened before the daemon was found still holds unsaved work.
    if (const auto held = engine.getMixerSession(); held.isDirectory())
        saveSessionStateTo (held);

    condemnAllStrips();
    engine.stopPlayback();
    engine.clearSessionState();
    engine.setActiveSessionDir (dir);
    if (! engine.loadSessionMixFrom (dir) || engine.getRecorder().getNumTracks() == 0)
    {
        // Nothing saved to load (a take started by an older build): the daemon
        // still knows the channels it is recording.
        const int strips = (int) status.tracks.size();
        engine.setStripCount (strips);
        for (int i = 0; i < juce::jmin (strips, engine.getRecorder().getNumTracks()); ++i)
        {
            const auto& track = status.tracks[(size_t) i];
            if (track.name.isNotEmpty()) engine.setTrackName (i, track.name);
            engine.setTrackStereo (i, track.stereoLeft);
            engine.setTrackArmed (i, track.armed);
        }
    }
    restoreSessionDocuments (dir);
    if (status.sampleRate > 0.0)
    {
        pendingSampleRate = status.sampleRate;
        engine.setSessionSampleRate (pendingSampleRate);
    }
    engine.getMarkers().setContext (dir, pendingSampleRate);
    engine.claimMixerFor (dir);
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
    if (isSessionOpen (dir))
    {
        explicitDocumentOpened = true;
        showStatus (dir.getFileName() + " is already open");
        return true;
    }
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
    if (sessionIoBusy.load()) { showStatus ("Wait for session saving to finish"); return; }
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before replacing the session"); return; }
    const auto dir = engine.getActiveSessionDir();
    // Pinned but never loaded means nothing in memory belongs to it.
    if (! dir.isDirectory() || ! engine.mixerBelongsTo (dir)) { continuation(); return; }

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
            if (result == 1)
            {
                self->metadataContinuationReserved = true;
                self->setEnabled (false); // reserve edits until Save & Continue resolves
                self->enqueueMetadataSave (dir, {}, [self, continuation = std::move (continuation)] (bool ok)
                {
                    if (self == nullptr) return;
                    self->metadataContinuationReserved = false;
                    self->setEnabled (true);
                    if (ok) continuation();
                });
                return;
            }
            continuation();
        }), false);
}

void MainComponent::closeSession()
{
    if (sessionIoBusy.load()) { showStatus ("Wait for session saving to finish"); return; }
    if (engine.isRecording())
    {
        showStatus ("Stop recording before closing the session");
        return;
    }

    // Closing loses any UNSAVED mixer / edit / cue changes
    // (recorded audio is always already on disk). Offer Save & Close /
    // Close Without Saving / Cancel -- the same choice as quitting, but it
    // only unloads the session and returns to Welcome (the app stays open).
    juce::Component::SafePointer<MainComponent> self (this);
    const auto sessionDir = engine.getActiveSessionDir();

    auto* aw = new juce::AlertWindow (
        "Close session?",
        "Return to the Welcome screen without quitting the app.\n\n"
        "Recorded audio is always on disk. Unsaved mixer / edit / cue "
        "changes since the last Save will be lost unless you save.",
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

            if (self->engine.isRecording() || self->sessionIoBusy.load())
            { self->showStatus ("Close cancelled -- recording or saving is in progress"); return; }
            auto finishClose = [self, result]
            {
                if (self == nullptr) return;
            // Forget the session so showStartupWelcome won't auto-reopen it,
            // and reset every session-scoped bit of state to the fresh-empty
            // slate (mirrors a launch with no session).
            auto& engine = self->engine;
            engine.clearSessionState();
            self->forgetSessionUiState();
            engine.clearAllStripOverrides();
            self->condemnAllStrips();   // stop strip timers before setStripCount(0) frees the TrackStates
            engine.setStripCount (0);

            self->undoManager.clearUndoHistory();
            self->lastTrackCount = -1;            // force a strip rebuild (now empty)
            self->updateTransportLabels();
            self->showStatus (result == 1 ? "Saved + closed session" : "Session closed");

            // activeSessionDir is empty now -> this shows the Welcome dialog
            // (New / Open) instead of auto-reopening the last session. A
            // Finder/command-line open only suppresses the LAUNCH Welcome.
            self->explicitDocumentOpened = false;
            self->showStartupWelcome();
            };
            if (result == 1)
            {
                self->metadataContinuationReserved = true;
                self->setEnabled (false);
                self->enqueueMetadataSave (sessionDir, {}, [self, finishClose] (bool ok)
                {
                    if (self == nullptr) return;
                    self->metadataContinuationReserved = false;
                    self->setEnabled (true);
                    if (ok) finishClose();
                });
            }
            else if (result == 2) finishClose();
        }), false);
}

zynforge::SessionMetadataSnapshot MainComponent::captureMetadataSnapshot (const juce::File& dir, bool backup)
{
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
        t->setProperty ("name",  recorder.getTrack (i).getNameThreadSafe());
        t->setProperty ("colourARGB",
                        (int) recorder.getTrack (i).colourARGB.load (std::memory_order_relaxed));
        trackArr.add (juce::var (t.get()));
    }
    root->setProperty ("tracks", trackArr);

    zynforge::SessionMetadataSnapshot snapshot;
    snapshot.directory = dir;
    snapshot.settingsJson = juce::JSON::toString (juce::var (root.get()), true);
    snapshot.mixJson = engine.captureSessionMixJson();
    snapshot.mixerOwner = engine.getMixerSession();
    auto patch = juce::JSON::parse (captureProjectPatchJson());
    patch.getDynamicObject()->setProperty ("ui", captureUILayoutJson());
    snapshot.projectPatchJson = juce::JSON::toString (patch);
    snapshot.backup = backup;
    return snapshot;
}

bool MainComponent::saveSessionStateTo (const juce::File& dir)
{
    // Transactional callers must defer rather than wait on an interactive save
    // on the message thread. The immutable writer is shared by both paths.
    if (pendingMetadataSaves != 0)
    {
        lastMetadataSaveError = "another save is still finishing";
        return false;
    }
    const auto result = zynforge::writeSessionMetadata (captureMetadataSnapshot (dir));
    lastMetadataSaveError = result.error;
    lastMetadataSaveNotice = result.notice;
    if (result.notice.isNotEmpty()) toast.show (result.notice, Toast::Kind::Warning);
    if (result.ok) lastSavedUndoUnits = undoManager.getNumberOfUnitsTakenUpByStoredCommands();
    return result.ok;
}

void MainComponent::enqueueMetadataSave (const juce::File& dir, const juce::String& success,
                                        std::function<void (bool)> completion, bool automatic,
                                        bool backup)
{
    enqueueMetadataSnapshot (captureMetadataSnapshot (dir, backup), success,
                             std::move (completion), automatic);
}

void MainComponent::enqueueMetadataSnapshot (zynforge::SessionMetadataSnapshot snapshot,
                                            const juce::String& success,
                                            std::function<void (bool)> completion, bool automatic)
{
    const auto dir = snapshot.directory;
    if (dir == juce::File() || (sessionIoBusy.load() && pendingMetadataSaves == 0 && ! captureMetadataPending))
    {
        showStatus ("Save failed -- a session operation is in progress or no session is open");
        if (completion) completion (false);
        return;
    }
    const bool fullSave = snapshot.settingsJson.isNotEmpty() && snapshot.mixJson.isNotEmpty();
    // A layout-only write must not suppress the completion of an explicit
    // full save. Only a newer full save supersedes that save's status/clean mark.
    const auto revision = fullSave ? ++metadataRevision : metadataRevision;
    const auto savedUndoUnits = undoManager.getNumberOfUnitsTakenUpByStoredCommands();
    // updatedAt is generated per snapshot and is not an edit. Comparing actual
    // serialized content catches mixer/routing/cue edits outside the UndoManager.
    auto stableProject = [] (const juce::String& json)
    {
        auto value = juce::JSON::parse (json);
        if (auto* object = value.getDynamicObject()) object->removeProperty ("updatedAt");
        return juce::JSON::toString (value);
    };
    const auto expectedSettings = snapshot.settingsJson;
    const auto expectedMix = snapshot.mixJson;
    const auto expectedProject = fullSave ? stableProject (snapshot.projectPatchJson) : juce::String();
    ++pendingMetadataSaves;
    sessionIoBusy.store (true);
    engine.setSessionTransitionActive (true);
    if (! automatic) showStatus ("Saving session metadata...");
    juce::Component::SafePointer<MainComponent> self (this);
    metadataWriter.enqueue (std::move (snapshot), automatic,
        [self, dir, revision, savedUndoUnits, success, fullSave, expectedSettings, expectedMix,
         expectedProject, stableProject, completion = std::move (completion)]
        (zynforge::SessionMetadataResult result)
    {
        auto deliver = [self, dir, revision, savedUndoUnits, success, fullSave, expectedSettings,
                        expectedMix, expectedProject, stableProject,
                        completion, result = std::move (result)] () mutable
        {
            if (self == nullptr) return;
            --self->pendingMetadataSaves;
            if (self->pendingMetadataSaves == 0 && ! self->captureMetadataPending)
            {
                self->sessionIoBusy.store (false);
                self->engine.setSessionTransitionActive (false);
            }
            if (result.superseded) return; // retired layout request; replacement still owns persistence
            self->lastMetadataSaveError = result.error;
            self->lastMetadataSaveNotice = result.notice;
            if (result.notice.isNotEmpty()) self->toast.show (result.notice, Toast::Kind::Warning);
            const bool sameSession = self->engine.getActiveSessionDir() == dir;
            bool snapshotStillCurrent = true;
            if (sameSession && result.ok && fullSave)
            {
                const auto current = self->captureMetadataSnapshot (dir, false);
                snapshotStillCurrent = current.settingsJson == expectedSettings
                    && current.mixJson == expectedMix
                    && stableProject (current.projectPatchJson) == expectedProject;
            }
            if (sameSession)
            {
                if (! result.ok) self->showStatus ("Save failed -- " + result.error);
                else if (fullSave && revision == self->metadataRevision)
                {
                    if (snapshotStillCurrent)
                    {
                        self->lastSavedUndoUnits = savedUndoUnits;
                        if (success.isNotEmpty()) self->showStatus (success);
                    }
                    else
                    {
                        self->lastSavedUndoUnits = -1;
                        self->showStatus ("Saved earlier session state; newer changes remain unsaved");
                    }
                }
            }
            if (completion) completion (result.ok && sameSession
                && (! self->metadataContinuationReserved || snapshotStillCurrent));
            self->releaseHeldRecordPress();
        };
        // Replaced layout requests retire synchronously during enqueue on the
        // message thread; worker completions are marshalled back normally.
        if (juce::MessageManager::getInstance()->isThisTheMessageThread()) deliver();
        else juce::MessageManager::callAsync (std::move (deliver));
    });
}

void MainComponent::requestUILayoutSave()
{
    if (restoringUILayout || engine.isRecording()) return;
    const auto dir = engine.getActiveSessionDir();
    if (! engine.mixerBelongsTo (dir)) return; // never replace a layout that was not loaded
    // A session job (export, bounce, import, Save As...) owns the I/O. Layout
    // is cosmetic: save it when the job ends instead of failing loudly.
    if (sessionIoBusy.load() && pendingMetadataSaves == 0 && ! captureMetadataPending)
    { uiLayoutSavePending = true; return; }
    uiLayoutSavePending = false;
    zynforge::SessionMetadataSnapshot snapshot;
    snapshot.directory = dir;
    juce::DynamicObject::Ptr patch (new juce::DynamicObject());
    patch->setProperty ("ui", captureUILayoutJson());
    patch->setProperty ("updatedAt", juce::Time::getCurrentTime().toISO8601 (true));
    snapshot.projectPatchJson = juce::JSON::toString (juce::var (patch.get()));
    snapshot.backup = false;
    enqueueMetadataSnapshot (std::move (snapshot), {}, {}, true);
}

void MainComponent::saveCueChanges (const juce::String& success)
{
    enqueueMetadataSave (engine.getActiveSessionDir(), success);
}

// The session is open but its folder is gone, usually a drive pulled. The pin
// stays so nothing silently retargets it; saving has to wait for the drive.
bool MainComponent::sessionFolderIsMissing() const
{
    const auto dir = engine.getActiveSessionDir();
    return dir != juce::File() && ! dir.isDirectory() && engine.mixerBelongsTo (dir);
}

// 15 s, then doubling up to the autosave interval. A dead drive must keep being
// reported, but not with a toast every 15 seconds for the rest of the show.
juce::uint32 MainComponent::nextAutosaveRetryMs (juce::uint32 intervalMs)
{
    const auto doublings = juce::jlimit (0, 10, autosaveFailures++);
    return juce::jmin ((juce::uint32) 15000u << doublings, intervalMs);
}

void MainComponent::serviceAutosave()
{
    if (sessionIoBusy.load()) return;
    // Nothing goes to the take's volume during a take, and a save would refuse
    // M meanwhile. STOP saves, and the interval starts again from there.
    if (engine.isRecording())
    {
        lastAutosaveMs = juce::Time::getMillisecondCounter();
        return;
    }
    auto* props = engine.getAppProps();
    if (props == nullptr) return;

    const int mins = props->getIntValue ("autosaveMinutes", 5);   // default: every 5 min
    if (mins <= 0) return;                                          // Off

    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory() && ! sessionFolderIsMissing()) return; // nothing to save into

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
    lastAutosaveMs = now; // coalesce subsequent ticks while the worker owns I/O
    // Pinned but not opened: the launch pin under a dialog, or a failed reopen.
    // A retry can't help, so this waits for the next interval and says why.
    if (! engine.mixerBelongsTo (dir))
    {
        showStatus ("Auto-save skipped -- " + dir.getFileName()
                    + " is not loaded; open it to save it");
        return;
    }
    const auto intervalMs = (juce::uint32) mins * 60000u;
    if (sessionFolderIsMissing())
    {
        lastAutosaveMs = now - intervalMs + nextAutosaveRetryMs (intervalMs);
        showStatus ("! AUTO-SAVE FAILED -- session folder is missing: " + dir.getFullPathName()
                    + "; reconnect its drive");
        return;
    }
    juce::Component::SafePointer<MainComponent> self (this);
    enqueueMetadataSave (dir, "Auto-saved " + juce::Time::getCurrentTime().formatted ("%H:%M:%S"),
        [self, intervalMs] (bool ok)
        {
            if (self == nullptr) return;
            if (ok) { self->autosaveFailures = 0; return; }
            const auto retryMs = self->nextAutosaveRetryMs (intervalMs);
            self->lastAutosaveMs = juce::Time::getMillisecondCounter() - intervalMs + retryMs;
            self->showStatus ("AUTO-SAVE FAILED -- check the session volume; retrying shortly");
        }, true);

}

void MainComponent::showAutosaveSettings()
{
    auto* aw = new juce::AlertWindow ("Auto-Save & Backup",
        "Automatically save the session (mix, cues, layout) on a timer and keep a "
        "timestamped backup snapshot (10 newest) in 'Session File Backups'. Recordings are always "
        "written separately; keep independent media backups for important takes.",
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

        zynforge::updateSettings (self->engine.getAppProps(), [mins] (juce::PropertiesFile& p)
                                  { p.setValue ("autosaveMinutes", mins); });
        self->lastAutosaveMs = 0;   // restart the clock against the new interval
        self->showStatus (mins > 0 ? ("Auto-save every " + juce::String (mins)
                                      + (mins == 1 ? " minute" : " minutes"))
                                   : juce::String ("Auto-save turned off"));
    }), true);
}

juce::var MainComponent::captureUILayoutJson() const
{
    juce::DynamicObject::Ptr ui (new juce::DynamicObject());
    ui->setProperty ("view",         currentView == View::Mix ? "Mix" : "Edit");
    ui->setProperty ("stripWidth",
                      stripWidthPreset == StripWidth::XS ? "XS"
                    : stripWidthPreset == StripWidth::S  ? "S"
                    : stripWidthPreset == StripWidth::L  ? "L"
                                                         : "M");
    ui->setProperty ("editZoom",     editPage != nullptr ? (double) editPage->getZoom() : 1.0);
    return juce::var (ui.get());
}

void MainComponent::startExportTracksTo (const juce::File& destDir,
                                         const std::vector<int>& channelIndices,
                                         const zynforge::ExportOptions& opts)
{
    if (engine.isRecording()) { showStatus ("Stop recording before exporting tracks"); return; }
    auto sourceDir = engine.getActiveSessionDir();
    if (! sourceDir.isDirectory()) { showStatus ("No active session to export"); return; }
    if (destDir.createDirectory().failed())
    {
        showStatus ("Export failed: cannot create " + destDir.getFullPathName());
        return;
    }

    // Imported/generated media can coexist with legacy root-level takes.
    // Prefer the modern file for each track, then fall back to its legacy file.
    const auto audioDir = sourceDir.getChildFile ("Audio Files");
    // Bare Track_* also matches Track_NN.punchbase.<ext> (a crash-orphaned punch
    // sidecar). matchesIndex below demands an EXACT "Track_NN" stem so one can't
    // slip through today, but exclude it here too rather than depend on that.
    auto allFiles = audioDir.findChildFiles (juce::File::findFiles, false, "Track_*");
    allFiles.addArray (sourceDir.findChildFiles (juce::File::findFiles, false, "Track_*"));
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
    int missingSources = 0;
    juce::AudioFormatManager sourceFormats;
    sourceFormats.registerBasicFormats();

    auto& rec = engine.getRecorder();
    // The channels were chosen before the select / format / folder dialogs; a
    // session open or the mirror poll can shrink the strip list meanwhile.
    for (int i : channelIndices)
        if (i < 0 || i >= rec.getNumTracks())
        {
            showStatus ("The channel list changed while the export was set up -- choose the channels again");
            return;
        }
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
        const auto baseName = juce::String::formatted ("Track_%02d - ", i + 1)
                            + zynforge::songexport::safeName (trackState.getNameThreadSafe());

        ExportJob job;
        job.destStem = destDir.getChildFile (baseName);
        if (stereo)
        {
            const auto srcL = findTrackFile (i + 1);
            const auto srcR = findTrackFile (i + 2);   // legacy: separate R mono file
            // A linked pair may have an old R file after relinking. Source
            // channel count, not the mere presence of R, decides the layout.
            std::unique_ptr<juce::AudioFormatReader> left (
                srcL.existsAsFile() ? sourceFormats.createReaderFor (srcL) : nullptr);
            if (left != nullptr && left->numChannels >= 2)
            {
                job.srcL = srcL;
            }
            else if (left != nullptr && left->numChannels == 1 && srcR.existsAsFile())
            {
                std::unique_ptr<juce::AudioFormatReader> right (sourceFormats.createReaderFor (srcR));
                if (right != nullptr && right->numChannels == 1)
                {
                    job.srcL = srcL; job.srcR = srcR; job.stereoPair = true;
                }
                else { ++missingSources; continue; }
            }
            else if (srcL.existsAsFile())
            {
                // A stereo strip with a missing/unreadable partner must never
                // silently export a one-channel file as the full pair.
                ++missingSources;
                continue;
            }
        }
        else
        {
            job.srcL = findTrackFile (i + 1);
        }
        if (job.srcL.existsAsFile()) jobs.push_back (std::move (job));
        else
        {
            // After unlinking a native pair, the right strip's audio is the
            // second channel of the left's file, which this export writes.
            int fileChannel = 0;
            if (i > 0 && std::find (channelIndices.begin(), channelIndices.end(), i - 1) != channelIndices.end()
                && engine.getTrackAudioFile (i, &fileChannel).existsAsFile() && fileChannel == 1)
                continue;

            // Empty, never-recorded strips are expected in Export All. A
            // selected strip, a previously loaded source, or a dangling
            // continuation part is missing media and makes the batch partial.
            const auto stem = juce::String::formatted ("Track_%02d", i + 1);
            const bool hasPart = ! audioDir.findChildFiles (juce::File::findFiles, false,
                                                            stem + "_part*").isEmpty()
                              || ! sourceDir.findChildFiles (juce::File::findFiles, false,
                                                             stem + "_part*").isEmpty();
            const auto* clips = engine.tryClipsFor (i);
            const bool hasArrangement = clips != nullptr && ! clips->empty();
            if (channelIndices.size() != (size_t) rec.getNumTracks()
                || engine.getPlayer().getTrackLengthSamples (i) > 0
                || hasArrangement || hasPart)
                ++missingSources;
        }
    }

    if (jobs.empty())
    {
        showStatus (missingSources > 0
            ? "Export failed: " + zynforge::counted (missingSources, "selected source file") + " missing"
            : "Export failed: no matching take files");
        return;
    }

    // ── Phase 2 (BACKGROUND): decode / resample / encode ───────────────────
    // Owned + joinable thread (same pattern as the bounce) so a quit mid-export
    // can't leave a detached worker running against freed state.
    if (! sessionJobs.reserve())
    {
        showStatus ("Another session file operation is already running");
        return;
    }
    showStatus ("Exporting " + zynforge::counted ((int) jobs.size(), "track") + "...");

    juce::Component::SafePointer<MainComponent> self (this);
    const auto destName = destDir.getFileName();
    sessionJobs.launch ([self, jobs, opts, destName, missingSources] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        zynforge::TrackExporter exporter;
        int succeeded = 0, attempted = 0;
        juce::String firstError;

        for (const auto& j : jobs)
        {
            // Returning without a completion is safe: the runner releases the
            // reservation on every path. (This early return used to skip the
            // only release, leaving the session busy after a cancelled export.)
            if (cancel.load (std::memory_order_relaxed)) return {};
            juce::String err;
            const bool ok = j.stereoPair
                ? exporter.exportStereoPair (j.srcL, j.srcR, j.destStem, opts, err)
                : exporter.exportTrack      (j.srcL,         j.destStem, opts, err);
            ++attempted;
            if (ok) ++succeeded;
            else if (firstError.isEmpty() && err.isNotEmpty()) firstError = err;
        }

        const bool cancelled = cancel.load (std::memory_order_relaxed);
        return [self, succeeded, attempted, firstError, destName, cancelled, missingSources]
        {
            if (self == nullptr || cancelled) return;
            // Surface PARTIAL failures too -- e.g. the destination runs out of
            // space mid-batch. A run that wrote 10 of 32 files must not read as
            // success and ship an incomplete deliverable.
            self->lastExportFailures = missingSources + juce::jmax (0, attempted - succeeded);
            if (succeeded == 0)
                self->showStatus ("Export failed"
                                  + (firstError.isNotEmpty() ? ": " + firstError : juce::String()));
            else if (self->lastExportFailures > 0)
                self->showStatus ("Export INCOMPLETE: " + juce::String (self->lastExportFailures)
                                  + " of " + juce::String (attempted + missingSources) + " failed or missing"
                                  + (firstError.isNotEmpty() ? " (" + firstError + ")" : ""));
            else
                self->showStatus ("Exported " + zynforge::counted (succeeded, "track") + " -> " + destName);
        };
    });
}

void MainComponent::onBounceStems (std::vector<int> tracks)
{
    if (sessionIoBusy.load()) { showStatus ("Another session operation is already running"); return; }
    if (engine.isRecording()) { showStatus ("Stop recording before bouncing stems"); return; }
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No active session to bounce"); return; }

    const auto exportDir = sessionDir.getChildFile ("Export Files");
    exportDir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Bounce edited stems to...", exportDir, "");
    juce::Component::SafePointer<MainComponent> chooserSelf (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectDirectories,
        [chooserSelf, this, tracks] (const juce::FileChooser& fc)
    {
        if (chooserSelf == nullptr) return;
        completeBounceStems (fc.getResult(), tracks);
    });
}

void MainComponent::completeBounceStems (juce::File dest, const std::vector<int>& tracks)
{
    if (dest.getFullPathName().isEmpty()) return;
    // The asynchronous picker may have been opened before a remote RECORD.
    if (engine.isRecording() || captureSupervisor.isDaemonRecording() || engine.isCaptureFinalizing())
    { showStatus ("Stop recording and wait for finalization before bouncing stems"); return; }
    dest.createDirectory();

    const int    nTracks = engine.getRecorder().getNumTracks();
    const auto   arrLen  = engine.getArrangementLengthSamples();
    const double sr      = engine.getPlayer().getSampleRate() > 0.0
                             ? engine.getPlayer().getSampleRate() : 48000.0;
    if (arrLen <= 0) { showStatus ("Nothing to bounce -- record or load a session first"); return; }
    // The strips were ticked before the folder dialog; a session open or a
    // remote can change the list meanwhile.
    for (int t : tracks)
        if (t < 0 || t >= nTracks)
        {
            showStatus ("The channel list changed while the export was set up -- choose the channels again");
            return;
        }
    if (! sessionJobs.reserve()) { showStatus ("Another session operation is already running"); return; }
    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);

    showStatus ("Bouncing " + juce::String ((int) tracks.size()) + " edited stems...");
    // Resolve every stem's name and layout HERE, on the message thread, and
    // hand the worker plain values. The worker used to read TrackStates under
    // the recorder's structure lock for the WHOLE bounce -- and the 10 Hz UI
    // timer's captureStatus() takes that same lock, so the entire app froze
    // until the last stem was written. The render itself reads only clips and
    // files, never a TrackState.
    struct Stem { int track; bool stereo; juce::File file; };
    std::vector<Stem> stems;
    {
        auto& rec = engine.getRecorder();
        for (int t : tracks)
        {
            // A stereo pair bounces as ONE interleaved stereo stem from its
            // L half; skip the R half.
            if (t > 0 && rec.getTrack (t - 1).isStereo.load (std::memory_order_relaxed))
                continue;
            const auto& ts = rec.getTrack (t);
            const auto safe = zynforge::songexport::safeName (ts.getNameThreadSafe());
            stems.push_back ({ t, ts.isStereo.load (std::memory_order_relaxed),
                               dest.getChildFile (juce::String::formatted ("Track_%02d - ", t + 1) + safe + ".wav") });
        }
    }
    juce::Component::SafePointer<MainComponent> self (this);
    sessionJobs.launch ([self, &eng = engine, stems, dest, arrLen, sr] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        int written = 0, attempted = 0;
        for (const auto& stem : stems)
        {
            if (cancel.load (std::memory_order_relaxed)) break;
            // Streams the edited arrangement window-by-window straight to
            // disk, so a multi-hour stem never needs a whole-track buffer.
            const bool okBounce = stem.stereo
                ? eng.bounceStereoPairToWav (stem.track, stem.file, arrLen, sr, &cancel)
                : eng.bounceTrackArrangementToWav (stem.track, stem.file, arrLen, sr, &cancel);
            ++attempted;
            if (okBounce) ++written;
        }
        // A quit cancels us; don't post a stale "Bounced 0 stems".
        const bool cancelled = cancel.load (std::memory_order_relaxed);
        return [self, written, attempted, cancelled, dest]
        {
            if (self == nullptr) return;
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);
            if (! cancelled)
            {
                if (attempted == 0)
                    self->showStatus ("Bounce failed: no eligible stems");
                else if (written == attempted)
                    self->showStatus ("Bounced " + zynforge::counted (written, "edited stem") + " -> " + dest.getFileName());
                else
                    self->showStatus ((written == 0 ? "Bounce failed: " : "Bounce INCOMPLETE: ")
                                      + juce::String (attempted - written) + " of "
                                      + zynforge::counted (attempted, "stem") + " failed");
            }
        };
    });
}

void MainComponent::onBounceStereoMix()
{
    if (sessionIoBusy.load()) { showStatus ("Another session operation is already running"); return; }
    if (engine.isRecording()) { showStatus ("Stop recording before bouncing a mix"); return; }
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
        completeBounceStereoMix (fc.getResult());
    });
}

void MainComponent::completeBounceStereoMix (juce::File dest)
{
    if (dest.getFullPathName().isEmpty()) return;
    // The asynchronous picker may have been opened before a remote RECORD.
    if (engine.isRecording() || captureSupervisor.isDaemonRecording() || engine.isCaptureFinalizing())
    { showStatus ("Stop recording and wait for finalization before bouncing a mix"); return; }
    if (! dest.hasFileExtension ("wav")) dest = dest.withFileExtension ("wav");

    const auto   arrLen = engine.getArrangementLengthSamples();
    const double sr     = engine.getPlayer().getSampleRate() > 0.0
                             ? engine.getPlayer().getSampleRate() : 48000.0;
    if (arrLen <= 0) { showStatus ("Nothing to bounce -- record or load a session first"); return; }
    if (! sessionJobs.reserve()) { showStatus ("Another session operation is already running"); return; }
    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);

    showStatus ("Bouncing stereo mix...");
    const bool soloActive = engine.isAnySoloActive();
    // No structure lock here: the mix renderer snapshots every strip under a
    // brief lock of its own and then renders lock-free. Holding the lock for
    // the whole render froze the UI timer (captureStatus) until it finished.
    juce::Component::SafePointer<MainComponent> self (this);
    sessionJobs.launch ([self, &eng = engine, dest, arrLen, sr, soloActive] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        // Streams the summed mix window-by-window straight to disk.
        float peak = 0.0f;
        const bool ok = ! cancel.load (std::memory_order_relaxed)
                     && eng.bounceStereoMixToWav (dest, arrLen, sr, &cancel, &peak);
        // A quit cancels us; that returns false but is NOT a failure -- don't
        // post a spurious "bounce failed".
        const bool cancelled = cancel.load (std::memory_order_relaxed);
        return [self, ok, cancelled, dest, peak, soloActive]
        {
            if (self == nullptr) return;
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);
            if (cancelled) return;
            if (! ok) { self->showStatus ("Stereo mix bounce failed"); return; }
            const auto warning = zynforge::songexport::mixRenderWarning (peak <= 0.0f, soloActive);
            self->showStatus ("Bounced stereo mix -> " + dest.getFileName()
                              + (warning.isNotEmpty() ? " -- " + warning : juce::String()));
        };
    });
}

void MainComponent::onSaveSessionState()
{
    if (captureMetadataPending) { showStatus ("Wait for capture finalization before saving again"); return; }
    if (engine.isRecording()) { showStatus ("Stop recording before saving session state"); return; }
    const auto dir = engine.getActiveSessionDir();
    if (sessionFolderIsMissing())
    {
        showStatus ("! Session folder is missing: " + dir.getFullPathName() + "; reconnect its drive, then save");
        return;
    }
    if (dir.isDirectory())
    {
        enqueueMetadataSave (dir, "Saved session state -> " + dir.getFileName());
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
        completeAudioImport (fc.getResults());
    });
}

void MainComponent::completeAudioImport (const juce::Array<juce::File>& picks)
{
    juce::Component::SafePointer<MainComponent> self (this);
    if (picks.isEmpty()) return;
    // Initial command validation expires while the asynchronous picker is open.
    if (engine.isRecording() || captureSupervisor.isDaemonRecording() || engine.isCaptureFinalizing())
    { showStatus ("Stop recording and wait for finalization before importing"); return; }
    if (sessionFolderIsMissing())
    {
        showStatus ("! Session folder is missing: " + engine.getActiveSessionDir().getFullPathName()
                    + "; reconnect its drive before importing");
        return;
    }

    if (! sessionJobs.reserve())
    {
        showStatus ("Another session file operation is already running");
        return;
    }

    // Prepare the destination on the message thread, then leave every
    // potentially multi-hour decode/resample/write operation to the owned
    // session-I/O worker below.
    const auto originalSession = engine.getActiveSessionDir();
    const bool newSession = ! originalSession.isDirectory();
    // The new strips would land in a session whose own mixer was never loaded,
    // and its state could not be saved afterwards.
    if (! newSession && ! engine.mixerBelongsTo (originalSession))
    {
        sessionJobs.release();
        showStatus ("Import refused -- " + originalSession.getFileName() + " is not loaded; open it first");
        return;
    }
    // makeNewSessionDir may reuse a persisted named folder even when the
    // engine has no active session. Import must start in its own unique
    // folder in that case, never write into an unrelated old take.
    auto sessionDir = newSession
        ? getSessionsRoot().getNonexistentChildFile (
            "Imported_" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S"),
            {}, false)
        : originalSession;
    const bool deleteOnCancel = newSession && ! sessionDir.exists();
    if (! sessionDir.createDirectory().wasOk())
    {
        sessionJobs.release();
        showStatus ("Import failed -- session folder is not writable");
        return;
    }

    auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
    const bool layoutReady = audioFilesDir.createDirectory().wasOk()
        && sessionDir.getChildFile ("Export Files").createDirectory().wasOk()
        && sessionDir.getChildFile ("Session File Backups").createDirectory().wasOk();
    if (! layoutReady)
    {
        sessionJobs.release();
        showStatus ("Import failed -- session folders are not writable");
        return;
    }
    // Keep the current session pinned until an import has actually
    // produced playable media. A failed or cancelled import must not
    // redirect Save, Export or the next recording to an empty folder.

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

    int firstTrack = engine.getRecorder().getNumTracks();
    for (const auto& file : audioFilesDir.findChildFiles (juce::File::findFiles, false,
                                                           "Track_*"))
    {
        const auto stem = file.getFileNameWithoutExtension();
        if (! stem.startsWith ("Track_")) continue;
        const int index = stem.fromFirstOccurrenceOf ("Track_", false, false)
                              .upToFirstOccurrenceOf ("_part", false, false).getIntValue();
        firstTrack = juce::jmax (firstTrack, index);
    }
    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);
    showStatus ("Importing " + zynforge::counted (picks.size(), "audio file") + "...");

    sessionJobs.launch ([self, picks, sessionDir, originalSession,
                         newSession, deleteOnCancel, audioFilesDir, firstTrack, targetSr]
                        (const std::atomic<bool>& cancel) -> zynforge::SessionJobRunner::Completion
    {
        const auto result = zynforge::audioimport::importFiles (
            picks, audioFilesDir, firstTrack, targetSr, &cancel);

        return [self, result, sessionDir, originalSession,
                newSession, deleteOnCancel, audioFilesDir, targetSr]
        {
            if (self == nullptr) return;
            if (result.cancelled || self->engine.getActiveSessionDir() != originalSession
                || result.tracks.empty())
            {
                for (const auto& track : result.tracks)
                    audioFilesDir.getChildFile (juce::String::formatted ("Track_%02d.wav",
                                                                        track.trackIndex + 1)).deleteFile();
                if (deleteOnCancel) sessionDir.deleteRecursively();
                if (! result.cancelled)
                    self->showStatus (result.tracks.empty()
                        ? "Import failed -- no readable audio files"
                        : "Import finished for the previous session; current session was not changed");
                self->engine.setSessionTransitionActive (false);
                if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);
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
            const auto savedEdits = newSession ? juce::var() : self->engine.playlistsToJson();
            const int loaded = self->engine.loadSession (sessionDir, ! newSession);
            if (! newSession) self->engine.loadPlaylistsFromJson (savedEdits);
            else if (loaded > 0) self->engine.claimMixerFor (sessionDir);
            const bool saved = self->saveSessionStateTo (sessionDir);
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);

            const int stereoCount = (int) std::count_if (
                result.tracks.begin(), result.tracks.end(),
                [] (const auto& record) { return record.stereo; });
            self->showStatus ("Imported " + zynforge::counted (result.importedFiles, "file") + " as " + zynforge::counted ((int) result.tracks.size(), "track") + ", " + juce::String (stereoCount) + " stereo, "
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
        };
    });
}

void MainComponent::onSaveSessionAs()
{
    if (engine.isRecording() || captureSupervisor.isDaemonRecording())
    { showStatus ("Stop recording before Save As"); return; }
    // Its audio is on that drive: a copy made now would be a session with none.
    if (sessionFolderIsMissing())
    {
        showStatus ("! Session folder is missing: " + engine.getActiveSessionDir().getFullPathName()
                    + "; reconnect its drive before Save As");
        return;
    }
    if (! sessionJobs.reserve())
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
        const auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) { sessionJobs.release(); return; }
        completeSaveAs (source, dest);
    });
}

void MainComponent::completeSaveAs (const juce::File& source, const juce::File& dest)
{
    if (source == dest)
    {
        const bool ok = saveSessionStateTo (dest);
        sessionJobs.release();
        showStatus (ok ? "Session saved" : "Save failed -- check permissions / free space");
        return;
    }

    const bool createdDestination = ! dest.exists();
    if (createdDestination && dest.createDirectory().failed())
    { sessionJobs.release(); showStatus ("Save As destination invalid"); return; }
    if (! dest.isDirectory())
    { sessionJobs.release(); showStatus ("Save As destination invalid"); return; }

    // Refuse a destination INSIDE the source: copyDirectoryTo would copy
    // the growing destination into itself.
    if (source.isDirectory() && pathsafety::isSameOrDescendant (source, dest))
    {
        sessionJobs.release();
        showStatus ("Save As failed -- pick a folder outside the current session");
        return;
    }

    // Save As is a clone, never a merge.  Refusing a non-empty folder
    // prevents JUCE's copyFileTo from deleting same-named destination
    // files and prevents an old session from becoming a hybrid.
    if (! dest.findChildFiles (juce::File::findFilesAndDirectories, false).isEmpty())
    {
        sessionJobs.release();
        showStatus ("Save As needs a new or empty folder");
        return;
    }

    const bool needsCopy = source.isDirectory() && source != dest;
    if (! needsCopy)
    {
        if (! ensureSessionScaffold (dest))
        {
            removePartialCopy (dest, createdDestination);
            sessionJobs.release();
            showStatus ("Save As failed -- couldn't create the session files");
            return;
        }
        engine.setActiveSessionDir (dest);
        engine.claimMixerFor (dest);
        const bool ok = saveSessionStateTo (dest);
        sessionJobs.release();
        showStatus (ok ? "Saved As -> " + dest.getFileName()
                       : "Save As failed -- check permissions / free space");
        return;
    }

    // The folder copy carries the AUDIO -- gigabytes on a real show -- so it
    // runs on the owned export thread instead of freezing the message
    // thread for the duration. The state files are written afterwards, back
    // on the message thread, because saveSessionStateTo touches engine state.
    juce::Component::SafePointer<MainComponent> self (this);
    auto copySession = [self, source, dest, createdDestination] (bool carryEdits)
    {
        if (self == nullptr) return;
        self->engine.setSessionTransitionActive (true);
        self->showStatus ("Saving As -> " + dest.getFileName() + " (copying session)...");
        const bool watchReport = self->daemonModeActive();
        self->sessionJobs.launch ([self, source, dest, createdDestination, carryEdits, watchReport]
                                  (const std::atomic<bool>& cancel)
                                  -> zynforge::SessionJobRunner::Completion
        {
            const bool ok = zynforge::MultitrackRecorder::waitForReportHashes (source, cancel, watchReport)
                         && copyDirectoryCancellable (source, dest, cancel);
            return [self, ok, dest, createdDestination, carryEdits]
            {
                if (self == nullptr) return;
                self->engine.setSessionTransitionActive (false);
                if (! ok)
                {
                    removePartialCopy (dest, createdDestination);
                    self->showStatus ("Save As cancelled or failed -- source session is unchanged");
                    return;
                }
                const bool carried = ! carryEdits || self->carryMixerInto (dest);
                self->openSessionFolder (dest);
                self->showStatus (carried ? "Saved As -> " + dest.getFileName()
                                          : "Saved As -> " + dest.getFileName() + "; unsaved changes could not be carried over");
            };
        });
    };

    // Freeze a complete source snapshot before the background copy begins.
    if (saveSessionStateTo (source)) { copySession (false); return; }
    // Nothing in memory is this session's, so there is nothing to lose.
    if (! engine.mixerBelongsTo (source)) { copySession (false); return; }
    offerContinueFromDisk ("Save As", "Your unsaved changes will be saved into the copy.",
                           [copySession] { copySession (true); },
                           [self, dest, createdDestination]
                           {
                               if (self == nullptr) return;
                               removePartialCopy (dest, createdDestination);
                               self->sessionJobs.release();
                               self->showStatus ("Save As cancelled -- source session is unchanged");
                           });
}

// A session on read-only media keeps its edits only in memory. The copy made
// from it is the first place they can be saved, so they go there.
bool MainComponent::carryMixerInto (const juce::File& copy)
{
    engine.claimMixerFor (copy);
    return saveSessionStateTo (copy);
}

void MainComponent::offerContinueFromDisk (const juce::String& action, const juce::String& carried,
                                           std::function<void()> proceed, std::function<void()> cancel)
{
    const auto text = action + " could not save the current state into the session first"
        + (lastMetadataSaveError.isNotEmpty() ? " (" + lastMetadataSaveError + ")" : juce::String())
        + ".\n\nContinue from the files as they are on disk? " + carried;
    if (continueFromDiskPromptForTests)
    {
        if (continueFromDiskPromptForTests (text)) proceed(); else cancel();
        return;
    }
    auto* aw = new juce::AlertWindow (action + " needs a decision", text, juce::MessageBoxIconType::NoIcon, this);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Continue", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel",   0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw, proceed = std::move (proceed), cancel = std::move (cancel)] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result == 1) proceed(); else cancel();
        }), false);
}

// The rate the session's audio is actually AT -- what the export dialog should
// default to, so "just export it" doesn't silently resample. Prefer the loaded
// player (it read the files), fall back to the session setting, then the device.
static double sessionAudioRateFor (zynforge::AudioEngine& engine)
{
    return engine.getSessionAudioRate();
}

void MainComponent::onExportAudio()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    const int n = engine.getRecorder().getNumTracks();
    if (n <= 0) { showStatus ("No tracks to export"); return; }

    // ONE row per logical strip -- a stereo pair shows as a single "(stereo)"
    // entry, not two mono rows. Each row maps back to its physical channel
    // index(es) so the export collapses the pair into one stereo file.
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
    zynforge::ExportAudioDialog::launch (sessionAudioRateFor (engine), names,
        [self, rowChannels] (std::optional<zynforge::ExportAudioContent::Result> choice)
    {
        if (self == nullptr || ! choice.has_value()) return;

        // Map ticked rows back to physical channel indices (a stereo row
        // expands to both halves; the export writes them as one file).
        std::vector<int> tracks;
        for (int row : choice->rows)
            if (row >= 0 && row < (int) rowChannels.size())
                for (int ch : rowChannels[(size_t) row])
                    tracks.push_back (ch);

        using Source = zynforge::ExportAudioContent::Source;
        if (choice->source == Source::stereoMix)         self->onBounceStereoMix();
        else if (tracks.empty())                         return;
        else if (choice->source == Source::editedTracks) self->onBounceStems (tracks);
        else                                             self->chooseRawTakesFolder (tracks, choice->options);
    });
}

void MainComponent::chooseRawTakesFolder (std::vector<int> tracks, const zynforge::ExportOptions& options)
{
    const auto activeSession = engine.getActiveSessionDir();
    const auto exportDir = activeSession.isDirectory()
                               ? activeSession.getChildFile ("Export Files")
                               : getSessionsRoot();
    exportDir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Export tracks to...", exportDir, "");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (flags, [self, this, tracks, options] (const juce::FileChooser& fc)
    {
        if (self == nullptr) return;
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) return;
        if (! dest.exists()) dest.createDirectory();

        // Async: reports its own progress + completion (or the INCOMPLETE /
        // failed warning) when the worker finishes.
        startExportTracksTo (dest, tracks, options);
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

juce::String MainComponent::buildTimelineCsv()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) return {};

    namespace tx = zynforge::timelineexport;
    auto& rec = engine.getRecorder();
    // Name the files a strip ACTUALLY reads, in their real container: the
    // R strip of a native-stereo take reads channel 2 of the L file, and a
    // continued take lists every part. A never-recorded strip names none.
    juce::StringArray names;
    for (int i = 0; i < rec.getNumTracks(); ++i)
        names.add (rec.getTrack (i).name);
    const auto tracks = tx::trackEntries (names, [this] (int track, int& channel)
                                          { return engine.getTrackAudioFile (track, &channel); });

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
    return tx::buildCsv (dir.getFileName(), sr,
                         tx::frameRateFor (engine.getTimecodeChase().getFps()),
                         tracks, markers, cueList);
}

void MainComponent::exportTimelineCsv()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) { showStatus ("No active session to export."); return; }
    const auto csv = buildTimelineCsv();

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

// A project file holds no audio, so this writes in moments, but it opens every
// take's headers to map clips onto part files: on a big show on a slow drive
// that is real time, so it runs as a job like the rest.
void MainComponent::exportDawProject (zynforge::projectexport::Format format)
{
    namespace px = zynforge::projectexport;
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No active session to export"); return; }
    if (const auto refusal = engine.exportRefusal(); refusal.isNotEmpty()) { showStatus (refusal); return; }
    if (sessionLocked) { showStatus ("Unlock the session before exporting"); return; }
    if (! sessionJobs.reserve()) { showStatus ("Another session operation is already running"); return; }

    const auto exportDir = sessionDir.getChildFile ("Export Files");
    if (exportDir.createDirectory().failed())
    {
        sessionJobs.release();
        showStatus ("Could not create " + exportDir.getFullPathName());
        return;
    }
    const auto input = engine.captureInterchange();
    const auto dest  = px::uniqueDestination (exportDir, input.sessionName, format);
    const auto version = applicationVersion();

    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);
    showStatus ("Writing " + dest.getFileName() + "...");

    juce::Component::SafePointer<MainComponent> self (this);
    sessionJobs.launch ([self, input, format, dest, version] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        const auto result = px::write (input, format, dest, version, cancel);
        const bool cancelled = cancel.load (std::memory_order_relaxed);
        return [self, result, cancelled]
        {
            if (self == nullptr) return;
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);
            if (cancelled) return;
            if (! result.ok)
            {
                self->showStatus ("Export failed: " + result.error);
                return;
            }
            self->showStatus ("Exported " + result.file.getFileName()
                              + (result.notes.isEmpty() ? juce::String()
                                                        : " -- " + zynforge::counted (result.notes.size(), "note")));
            if (! result.notes.isEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon,
                                                        "Exported " + result.file.getFileName(),
                                                        result.notes.joinIntoString ("\n\n"), "OK");
        };
    });
}

void MainComponent::exportSongs()
{
    if (! engine.getActiveSessionDir().isDirectory()) { showStatus ("No active session to export"); return; }
    if (const auto refusal = engine.exportRefusal(); refusal.isNotEmpty()) { showStatus (refusal); return; }

    const auto songs = zynforge::songexport::rangesFromMarkers (engine.getMarkers().getAll(),
                                                                engine.getArrangementLengthSamples());
    if (songs.empty())
    {
        showStatus ("No songs to export: add markers, or run Detect songs first");
        return;
    }
    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::ExportSongsDialog::launch (songs, sessionAudioRateFor (engine),
        [self] (std::optional<zynforge::songexport::Choice> choice)
    {
        if (self != nullptr && choice.has_value()) self->startSongExport (std::move (*choice));
    });
}

void MainComponent::startSongExport (zynforge::songexport::Choice choice)
{
    namespace sx = zynforge::songexport;
    // The dialog can sit open across a remote RECORD or a session change.
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No active session to export"); return; }
    if (const auto refusal = engine.exportRefusal(); refusal.isNotEmpty()) { showStatus (refusal); return; }
    if (sessionLocked) { showStatus ("Unlock the session before exporting"); return; }
    if (! sessionJobs.reserve()) { showStatus ("Another session operation is already running"); return; }

    const auto exportDir = sessionDir.getChildFile ("Export Files");
    const auto plan = sx::buildPlan (engine, choice, exportDir);
    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);
    songExportRunning = true;
    showStatus ("Exporting songs: " + zynforge::counted (plan.totalFiles(), "file") + "...");

    juce::Component::SafePointer<MainComponent> self (this);
    sessionJobs.launch ([self, &eng = engine, plan] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        // The render reads clips under its own brief locks, as the
        // whole-session bounces do, so the UI timer keeps running.
        const auto outcome = sx::run (eng, plan, cancel, [self] (int done, int total, const juce::String& song)
        {
            juce::MessageManager::callAsync ([self, done, total, song]
            {
                if (self != nullptr)
                    self->showStatus ("Exporting songs: " + juce::String (done) + " of "
                                      + juce::String (total) + " files -- " + song
                                      + " (File > Export > Cancel Song Export to stop)");
            });
        });
        return [self, outcome]
        {
            if (self == nullptr) return;
            self->songExportRunning = false;
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);
            self->lastExportFailures = juce::jmax (0, outcome.attempted - outcome.written);
            self->showStatus (sx::statusLine (outcome, "Export Files/Songs"));
        };
    });
}

void MainComponent::exportShowHandoff()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory() || engine.isRecording() || sessionLocked)
    { showStatus ("Stop recording and unlock the session before creating a handoff"); return; }
    if (! sessionJobs.reserve())
    { showStatus ("Another session operation is already running"); return; }

    chooser = std::make_unique<juce::FileChooser> (
        "Create a new show handoff folder...",
        source.getSiblingFile (source.getFileName() + " Handoff"), "");
    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectDirectories;
    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (flags, [self, source] (const juce::FileChooser& fc)
    {
        if (self == nullptr) return;
        const auto dest = fc.getResult();
        if (dest == juce::File()) { self->sessionJobs.release(); return; }
        self->completeShowHandoff (source, dest);
    });
}

void MainComponent::completeShowHandoff (const juce::File& source, const juce::File& dest)
{
    if (pathsafety::isSameOrDescendant (source, dest) || dest.isSymbolicLink())
    {
        sessionJobs.release();
        showStatus ("Choose a new handoff folder outside the source session");
        return;
    }
    if (dest.exists() && (! dest.isDirectory()
                          || ! dest.findChildFiles (juce::File::findFilesAndDirectories, false).isEmpty()))
    {
        sessionJobs.release();
        showStatus ("Handoff needs a new or empty folder; nothing was overwritten");
        return;
    }
    if (! dest.isDirectory() && dest.createDirectory().failed())
    {
        sessionJobs.release();
        showStatus ("Could not create the handoff folder");
        return;
    }
    if (saveSessionStateTo (source) || ! engine.mixerBelongsTo (source))
    {
        startHandoffCopy (source, dest);
        return;
    }
    juce::Component::SafePointer<MainComponent> self (this);
    offerContinueFromDisk ("The handoff",
                           "The handoff is verified against the source, so unsaved changes will not be in it.",
                           [self, source, dest] { if (self != nullptr) self->startHandoffCopy (source, dest); },
                           [self]
                           {
                               if (self == nullptr) return;
                               self->sessionJobs.release();
                               self->showStatus ("Handoff cancelled: session metadata could not be saved");
                           });
}

void MainComponent::startHandoffCopy (const juce::File& source, const juce::File& dest)
{
    const auto timelineCsv = buildTimelineCsv();
    if (timelineCsv.isEmpty())
    {
        sessionJobs.release();
        showStatus ("Handoff cancelled: session timeline is unavailable");
        return;
    }
    const auto interchange = engine.captureInterchange();
    if (const auto outside = projectexport::firstMediaOutsideSession (interchange);
        outside != juce::File())
    {
        sessionJobs.release();
        showStatus ("Handoff cancelled: a clip reads " + outside.getFullPathName()
                    + ", which is outside this session and would not be in the copy");
        return;
    }
    const auto version = applicationVersion();
    // A crash or quit mid-copy must leave a visible incomplete marker,
    // not a plausible-looking folder with a few truncated recordings.
    const auto incomplete = dest.getChildFile ("HANDOFF INCOMPLETE.txt");
    if (! atomicfile::writeText (incomplete,
         "This show handoff was interrupted or failed. Do not deliver it as verified.\n"))
    {
        sessionJobs.release();
        showStatus ("Could not create the handoff safety marker");
        return;
    }

    engine.setSessionTransitionActive (true);
    showStatus ("Creating handoff in background: " + dest.getFileName());
    juce::Component::SafePointer<MainComponent> self (this);
    const bool watchReport = daemonModeActive();
    sessionJobs.launch ([self, source, dest, incomplete, timelineCsv, interchange, version, watchReport]
                        (const std::atomic<bool>& cancel) -> zynforge::SessionJobRunner::Completion
    {
        showhandoff::Result result;
        // The report changing during verification reads as a damaged copy.
        if (zynforge::MultitrackRecorder::waitForReportHashes (source, cancel, watchReport)
            && copyDirectoryCancellable (source, dest, cancel))
            result = showhandoff::verifyAndWrite (source, dest, timelineCsv, cancel,
                [&] (const juce::File& copy, juce::String& warning)
                { return projectexport::writeForHandoff (interchange, copy, version, cancel, warning); });
        else
            result = { false, "Copy cancelled or failed" };

        if (result.ok && ! incomplete.deleteFile())
            result = { false, "Could not remove the incomplete marker" };

        if (! result.ok)
            atomicfile::writeText (incomplete,
                "This show handoff is incomplete: " + result.message + "\nDo not deliver it as verified.\n");
        return [self, dest, result]
        {
            if (self == nullptr) return;
            self->engine.setSessionTransitionActive (false);
            if (result.ok)
                self->showStatus ((result.captureReportWarning
                    ? "! Handoff copy verified; CAPTURE REPORT NEEDS REVIEW: "
                    : "Verified handoff ready: ") + dest.getFullPathName());
            else
                self->showStatus ("Handoff incomplete at " + dest.getFullPathName()
                                  + " -- " + result.message);
        };
    });
}

void MainComponent::relocateActiveSession()
{
    if (! sessionJobs.reserve())
    {
        showStatus ("Another session file operation is already running");
        return;
    }
    if (engine.isRecording())
    {
        sessionJobs.release();
        showStatus ("Stop recording before moving the session.");
        return;
    }
    const auto oldDir = engine.getActiveSessionDir();
    if (! oldDir.isDirectory())
    {
        sessionJobs.release();
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
            if (! newParent.isDirectory()) { sessionJobs.release(); return; }
            completeRelocate (oldDir, newParent);
        });
}

void MainComponent::completeRelocate (const juce::File& oldDir, const juce::File& newParent)
{
    const auto newDir = newParent.getChildFile (oldDir.getFileName());
    if (newDir == oldDir)
    { sessionJobs.release(); showStatus ("That's already the session's location."); return; }
    if (pathsafety::isSameOrDescendant (oldDir, newParent)
        || pathsafety::isSameOrDescendant (oldDir, newDir))
    {
        sessionJobs.release();
        showStatus ("Move failed -- the destination cannot be inside the session");
        return;
    }
    if (newDir.exists())
    {
        sessionJobs.release();
        showStatus ("A folder named \"" + oldDir.getFileName()
                    + "\" already exists there -- pick another location.");
        return;
    }

    juce::Component::SafePointer<MainComponent> self (this);
    auto moveSession = [self, oldDir, newParent, newDir] (bool carryEdits)
    {
        if (self == nullptr) return;
        // Drop any open playback handles so the files aren't busy while
        // we move them, then do the (possibly cross-volume) move off the
        // message thread so a big session can't freeze the UI.
        self->engine.stopPlayback();
        self->engine.setSessionTransitionActive (true);
        self->showStatus ("Moving session to " + newParent.getFullPathName() + " ...");

        const bool watchReport = self->daemonModeActive();
        self->sessionJobs.launch ([self, oldDir, newDir, carryEdits, watchReport] (const std::atomic<bool>& cancel)
                                  -> zynforge::SessionJobRunner::Completion
        {
                // The hash thread holds absolute paths into the old folder.
                const bool hashed = zynforge::MultitrackRecorder::waitForReportHashes (oldDir, cancel, watchReport);
                bool relocated = hashed && oldDir.moveFileTo (newDir);   // instant within a volume
                bool oldCopyRemains = false;
                if (hashed && ! relocated)                     // cross-volume: copy + delete
                {
                    const bool copied = copyDirectoryCancellable (oldDir, newDir, cancel);
                    if (copied && ! cancel.load (std::memory_order_relaxed))
                    {
                        relocated = true; // the complete new copy is authoritative
                        oldCopyRemains = ! oldDir.deleteRecursively();
                    }
                    else
                        newDir.deleteRecursively();
                }

                return [self, relocated, oldCopyRemains, oldDir, newDir, carryEdits]
                {
                    if (self == nullptr) return;
                    self->engine.setSessionTransitionActive (false);
                    if (! relocated)
                    {
                        self->showStatus ("Move failed -- check permissions / free space. Session left in place.");
                        return;
                    }
                    const bool carried = ! carryEdits || self->carryMixerInto (newDir);
                    // The default sessions folder stays where it was: moving one
                    // session to an archive drive must not send every New
                    // Session, import and crash-recovery scan there too.
                    self->openSessionFolder (newDir);   // re-pin paths + reload audio from the new home
                    self->showStatus (oldCopyRemains
                        ? "Session moved, but the old folder could not be fully removed: "
                            + oldDir.getFullPathName()
                        : (carried ? "Session moved to " : "Session moved, without its unsaved changes, to ")
                            + newDir.getFullPathName());
                };
        });
    };

    if (saveSessionStateTo (oldDir) || ! engine.mixerBelongsTo (oldDir)) { moveSession (false); return; }
    offerContinueFromDisk ("Move", "Your unsaved changes will be saved in the new location.",
                           [moveSession] { moveSession (true); },
                           [self]
                           {
                               if (self == nullptr) return;
                               self->sessionJobs.release();
                               self->showStatus ("Move cancelled -- the session was not moved");
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
        if (dir.isDirectory()) completeOpenFolder (dir);
    });
}

void MainComponent::completeOpenFolder (const juce::File& dir)
{
    // Canonical full open (pins the active dir, restores setlist/UI/mixer,
    // loads audio, sizes the mixer incl. the no-session_mix.json fallback).
    // A negative result means it did not open and has said why. The session
    // that was open is still open, so a line about this folder's audio would
    // be false. Nor does it apply to reopening the session already open.
    const bool alreadyOpen = isSessionOpen (dir);
    if (openSessionFolder (dir) == 0 && ! alreadyOpen)
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
    updateTransportLabels();
}

juce::File MainComponent::getSessionsRoot() const
{
    // Engineer can override via the New Session / Welcome dialog ("Local
    // Storage:") -- stored in appProps as 'sessionsRoot', falling back to
    // ~/Music/Zynforge Sessions. The resolution lives on the ENGINE so the
    // network remote-record paths (OSC, desk macros) share it instead of
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

    // Stop the live strips' meter timers before setStripCount frees
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
    const juce::ScopedValueSetter<bool> restoring (restoringUILayout, true);
    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj == juce::File{}) return;

    const auto parsed = juce::JSON::parse (proj);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return;

    const auto uiVar = obj->getProperty ("ui");
    auto* ui = uiVar.getDynamicObject();
    if (ui == nullptr) return;

    // View -- must call switchView (not just write currentView) so the
    // page visibility + tools palette flip correctly.
    const auto viewStr = ui->getProperty ("view").toString();
    if (viewStr.isNotEmpty())
        switchView (viewStr == "Edit" ? View::Edit : View::Mix);

    // Strip width preset.
    const auto sw = ui->getProperty ("stripWidth").toString();
    if      (sw == "XS") setStripWidthPreset (StripWidth::XS);
    else if (sw == "S")  setStripWidthPreset (StripWidth::S);
    else if (sw == "L")  setStripWidthPreset (StripWidth::L);
    else if (sw == "M")  setStripWidthPreset (StripWidth::M);

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
    zynforge::updateSettings (engine.getAppProps(), [&templateFile] (juce::PropertiesFile& props)
                              { props.setValue ("defaultTemplateFile", templateFile.getFullPathName()); });
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
            // A folder pinned but never loaded has its own mixer on disk, and
            // this take must neither join those tracks nor replace that mixer.
            juce::File f (saved);
            // A folder gone from under a loaded session is not that session
            // any more, even if its path can be made again.
            const bool gone = ! f.isDirectory() && engine.isSessionLoaded();
            if (! gone && engine.mixerBelongsTo (f) && (f.isDirectory() || f.createDirectory().wasOk()))
                return f;
        }
    }
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    const auto root = getSessionsRoot();
    return root.getNonexistentChildFile ("Session_" + stamp, {}, false);
}

juce::String MainComponent::sessionCreateFailureMessage() const
{
    return engine.isRecording() || engine.isCaptureFinalizing()
        ? "Recording -- the new session was not created. Stop the take first."
        : "Couldn't create session -- check permissions / free space";
}

juce::File MainComponent::createSessionFolderStructure (const zynforge::NewSessionDialog::Result& r)
{
    // Guard at the mutation. Every New Session path (dialog, Welcome, CSV,
    // console) gets here after an async step, and a take can start meanwhile
    // (a remote RECORD). Clearing the strips would re-patch that take live,
    // and STOP would then save the wiped state over the real session.
    if (engine.isRecording() || engine.isCaptureFinalizing())
        return {};

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
        if (! zynforge::atomicfile::writeText (
                sessionFolder.getChildFile (safeName + ".zfproj"),
                juce::JSON::toString (juce::var (m.get()))))
        {
            sessionFolder.deleteRecursively();
            return {};
        }
    }

    // WaveCache.wfm is written on demand by EditPage (wavecache::save)
    // (on close) and read back on session open. No placeholder needed.

    // Only clear the previous session after every filesystem operation has
    // succeeded; a permissions/disk failure must leave the current work open.
    engine.clearSessionState();
    forgetSessionUiState();
    engine.clearAllStripOverrides();
    engine.claimMixerFor (sessionFolder);
    // The previous session's undo steps would apply to this one's files and
    // strips. Retake the mixer baseline on the next idle tick, after the
    // caller has applied its template, so that is not an undo step either.
    undoManager.clearUndoHistory();
    mixerUndoReady = false;
    return sessionFolder;
}
