// Session utility methods on MainComponent. Extracted from
// MainComponent.cpp as part of the 2026-05-25 god-class split (part 5).
//
// Includes: generateOrRefreshClickTrack (renders an offline click WAV
// matching the live ClickEngine voicing), togglePunchMode + servicePunch
// (loop-region punch-in/out), runNoiseAnalysis (background NoiseAnalyzer
// sweep + sortable report dialog), writeSoundcheckReport (JSON dump of
// per-track spectral classification), showSessionProperties (edit the
// .zfproj metadata).

#include "MainComponent.h"
#include "../Audio/ClickTrackRenderer.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/CrashReportScan.h"
#include "../Audio/NoiseAnalyzer.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SongDetector.h"
#include "../Audio/SpectralClassifier.h"
#include "NoiseReportDialog.h"
#include "QcReportDialog.h"
#include "SessionPropertiesDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

// Render on the owned file-I/O worker, then report actual success. The dialog
// must not switch the live click off merely because work was accepted: an open,
// encode, cancellation, or disk-full failure leaves both the live click and the
// previous offline file intact.
void MainComponent::generateOrRefreshClickTrack (std::function<void (bool)> completion)
{
    auto refuse = [this, &completion] (const juce::String& reason)
    {
        showStatus (reason);
        if (completion) completion (false);
    };

    // NEVER regenerate during a take. The finished file is installed and the
    // session reloaded, which would reopen readers the recorder is writing.
    if (engine.isRecording())
    {
        refuse ("Click track refreshes after recording stops");
        return;
    }

    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        refuse ("Open or create a session before generating a click");
        return;
    }

    if (! sessionJobs.reserve())
    {
        refuse ("Another session file operation is already running");
        return;
    }

    auto audioFiles = sessionDir.getChildFile ("Audio Files");
    if (audioFiles.createDirectory().failed())
    {
        sessionJobs.release();
        refuse ("Click write failed -- couldn't create Audio Files");
        return;
    }

    auto& recorder = engine.getRecorder();
    const bool addedClickStrip = clickTrackIndex < 0;
    const int previousTrackCount = recorder.getNumTracks();
    if (clickTrackIndex < 0)
    {
        // First press in this session -- append a fresh track at the end.
        clickTrackIndex = recorder.getNumTracks();
        engine.setStripCount (clickTrackIndex + 1);
        engine.setTrackName  (clickTrackIndex, "Click");
        // Mark it playback-only NOW. setStripCount seeds inputRouting >= 0, but
        // the defence-in-depth guard below requires inputRouting < 0 to confirm
        // this really is a click strip -- without this, a freshly-created click
        // strip trips the guard and Generate aborts on the very first press.
        engine.setTrackInputRouting (clickTrackIndex, -1);
        if (clickTrackIndex < recorder.getNumTracks())
            recorder.getTrack (clickTrackIndex).referenceMedia.store (true);
    }

    const double sr = juce::jmax (8000.0, engine.getSessionAudioRate() > 0.0
                                              ? engine.getSessionAudioRate() : 48000.0);
    // Match the session's playback length. An empty new session has no media
    // length yet, so provide three hours: enough for the stated two-hour show
    // plus setup/overtime headroom.
    auto& player = engine.getPlayer();
    juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
    if (totalSamples <= 0)
        totalSamples = (juce::int64) (sr * 60.0 * 60.0 * 3.0);

    auto& cl = engine.getClickEngine();

    // Name and routing are user-editable; only the explicit reference-media
    // marker establishes that this slot belongs to the click generator.
    if (clickTrackIndex < 0 || clickTrackIndex >= recorder.getNumTracks()
        || ! recorder.getTrack (clickTrackIndex).referenceMedia.load (std::memory_order_relaxed)
        || recorder.getTrack (clickTrackIndex).getNameThreadSafe() != "Click"
        || recorder.getTrack (clickTrackIndex).inputRouting.load (std::memory_order_relaxed) >= 0)
    {
        sessionJobs.release();
        refuse ("Click slot isn't a Click strip -- aborted to protect recordings");
        return;
    }

    const auto trackName = juce::String::formatted ("Track_%02d", clickTrackIndex + 1);
    const auto destination = audioFiles.getChildFile (trackName + ".wav");
    clickrender::Settings settings;
    settings.sampleRate = sr;
    settings.totalSamples = totalSamples;
    settings.initialBpm = engine.getSessionTempoBpm();
    settings.beatsPerBar = engine.getTimeSignatureNumerator();
    settings.voice1 = ClickEngine::getVoicePreset (cl.getVoice1());
    settings.voice2 = ClickEngine::getVoicePreset (cl.getVoice2());
    settings.subdivision1 = cl.getSub1();
    settings.subdivision2 = cl.getSub2();
    settings.gain1 = juce::Decibels::decibelsToGain (cl.getVol1Db());
    settings.gain2 = juce::Decibels::decibelsToGain (cl.getVol2Db());
    for (const auto& change : engine.getTempoMap())
        settings.tempoMap.push_back ({ change.samplePos, change.bpm });

    const int generatedTrackIndex = clickTrackIndex;
    const float generatedBpm = settings.initialBpm;
    engine.setSessionTransitionActive (true);
    if (editPage != nullptr) editPage->setEnabled (false);
    showStatus ("Generating click track in the background...");

    juce::Component::SafePointer<MainComponent> self (this);
    sessionJobs.launch ([self, destination, settings, sessionDir,
                         generatedTrackIndex, generatedBpm,
                         addedClickStrip, previousTrackCount,
                         completion = std::move (completion)] (const std::atomic<bool>& cancel)
                         -> zynforge::SessionJobRunner::Completion
    {
        const auto result = clickrender::render (destination, settings, &cancel);
        const bool cancelled = result == clickrender::Result::cancelled;
        return [self, result, cancelled, sessionDir, generatedTrackIndex,
                generatedBpm, addedClickStrip, previousTrackCount, completion] () mutable
        {
            if (self == nullptr) return;
            self->engine.setSessionTransitionActive (false);
            if (self->editPage != nullptr) self->editPage->setEnabled (! self->sessionLocked);

            const bool sameSession = self->engine.getActiveSessionDir() == sessionDir;
            if (result != clickrender::Result::succeeded || ! sameSession)
            {
                if (addedClickStrip && sameSession)
                {
                    self->engine.setStripCount (previousTrackCount);
                    self->clickTrackIndex = -1;
                }
                if (! cancelled)
                    self->showStatus (sameSession
                        ? "Click track NOT generated -- the previous file was preserved"
                        : "Click finished for the previous session; current session was not reloaded");
                if (completion) completion (false);
                return;
            }

            if (auto* device = self->engine.getDeviceManager().getCurrentAudioDevice())
            {
                const int outputs = device->getActiveOutputChannels().countNumberOfSetBits();
                self->engine.setTrackOutputRouting (generatedTrackIndex,
                    juce::jlimit (0, juce::jmax (0, outputs - 1), 0));
            }
            self->engine.setTrackInputRouting (generatedTrackIndex, -1);
            self->engine.loadSession (sessionDir, true);
            // Commit the Generate action even if the dialog was closed while
            // the worker ran; otherwise the real-time click can double the new
            // playback track.
            self->engine.getClickEngine().setEnabled (false);
            self->lastTrackCount = -1;
            self->showStatus ("Click track generated at "
                              + juce::String (generatedBpm, 1) + " BPM");
            if (self->editPage != nullptr)
                self->editPage->setClickTrackPresent (true, generatedTrackIndex);
            if (completion) completion (true);
        };
    });
}

void MainComponent::togglePunchMode()
{
    const bool on = ! engine.isPunchModeOn();
    if (on && daemonModeActive())
    {
        showStatus ("PUNCH mode needs in-process capture -- the capture daemon cannot punch");
        return;
    }
    engine.setPunchModeOn (on);
    if (on)
    {
        auto& player = engine.getPlayer();
        const auto pos = player.getPositionSamples();
        // Only crossing into the range punches, never turning the mode on
        // inside it.
        wasInsidePunch = player.isRangeArmed() && pos >= player.getLoopStart() && pos < player.getLoopEnd();
        showStatus (player.isRangeArmed()
                        ? "Punch mode ON -- press PLAY; the armed tracks punch through the selected range"
                        : "Punch mode ON -- select a range in EDIT, then press PLAY");
    }
    else
    {
        // Only a punch ends here. A live take keeps its two-tap STOP.
        const bool punchRolling = (punchWindowTakeActive || punchSessionActive) && engine.isRecording();
        if (punchRolling) stopActiveCaptureAsync (false);
        if (! punchRolling && ! captureMetadataPending) showStatus ("Punch mode OFF");
    }
}

// A punch take holds the playhead (SessionPlayer::setSeekLocked), so the
// player ignores jumps. Say so rather than doing nothing visible.
bool MainComponent::playheadLockedForPunch()
{
    if (! engine.getPlayer().isSeekLocked()) return false;
    showStatus ("Punch recording -- the playhead stays put until STOP");
    return true;
}

void MainComponent::servicePunch()
{
    if (sessionIoBusy.load()) return;
    auto& player = engine.getPlayer();
    if (! player.isLoaded()) return;
    const auto pos    = player.getPositionSamples();
    if (punchSessionActive)
    {
        // Selection recording opened before pre-roll began. The audio callback
        // admitted exactly [in,out), so this timer only finalises the file and
        // starts post-roll; it no longer decides which samples hit disk.
        if (engine.isRecording() && (pos >= punchOutSample || ! player.isPlaying()))
        {
            juce::Component::SafePointer<MainComponent> self (this);
            stopActiveCaptureAsync (false, [self] (bool saved)
            {
                if (self == nullptr || ! self->punchSessionActive) return;
                if (! saved)
                {
                    self->engine.stopPlayback();
                    self->engine.setPunchModeOn (false);
                    self->punchSessionActive = false;
                    return;
                }
                auto& playback = self->engine.getPlayer();
                playback.setPositionSamples (self->punchOutSample);
                self->engine.startPlayback();
            });
        }
        return;
    }
    // A range nobody selected this run never punches, even in PUNCH mode.
    const auto inside = player.isRangeArmed() && pos >= player.getLoopStart() && pos < player.getLoopEnd();

    if (inside && ! wasInsidePunch && player.isPlaying())
    {
        // Crossed into the punch window -- punch-in on every track armed right
        // now, as the R buttons show: startRecording freezes those arms as in
        // any take. Fresh audio goes into the CURRENT session and is spliced
        // into the existing take at the punch-in point (audio before the
        // punch-in and after the punch-out is preserved). The rest keep
        // playing back.
        if (! engine.isRecording())
        {
            const auto sessionDir = engine.getActiveSessionDir();
            if (! sessionDir.isDirectory())
            {
                // No take to punch into -- punch-in only overdubs an existing
                // session. (A fresh recording uses normal RECORD.)
                showStatus ("Punch-in needs a loaded session with a take");
                wasInsidePunch = inside;
                return;
            }
            if (sessionLocked || daemonModeActive())
            {
                showStatus (sessionLocked ? "Punch-in refused -- session is LOCKed"
                                          : "Punch-in refused -- the capture daemon cannot punch");
                wasInsidePunch = inside;
                return;
            }
            // Multi-part takes (auto-split, or built up by continue-recording)
            // are fine now: the splice reads the whole take via ConcatReader and
            // flattens it on stop, so no refusal here.
            // Splice at the ACTUAL playhead where recording begins (one timer
            // tick past loopStart), so the captured audio lands at the right
            // timeline position instead of being nudged earlier to loopStart.
            engine.armPunchIn (player.getPositionSamples());
            if (! engine.startRecording (sessionDir))
            {
                engine.getRecorder().cancelPunchIn();
                const auto why = engine.getRecorder().getStartError();
                showStatus ("Punch-in failed -- existing take is untouched"
                            + (why.isNotEmpty() ? " (" + why + ")" : juce::String()));
                wasInsidePunch = inside;
                return;
            }
            punchWindowTakeActive = true;
            lastCaptureFinalizationError.clear();
        }
    }
    else if ((! inside || ! player.isPlaying()) && punchWindowTakeActive && engine.isRecording())
    {
        // Crossed out, or the transport stopped short of the punch-out -- stop
        // recording cleanly (the splice replaces the punched region; audio
        // after the punch-out is preserved). A take this window did not open
        // is not its to end.
        juce::Component::SafePointer<MainComponent> self (this);
        stopActiveCaptureAsync (false, [self] (bool saved)
        {
            if (self == nullptr || saved) return;
            self->engine.stopPlayback();
            self->engine.setPunchModeOn (false);
            self->punchSessionActive = false;
        });
    }
    wasInsidePunch = inside;
}

void MainComponent::servicePunchSession()
{
    if (captureMetadataPending) return;
    // Drives a RECORD-button-triggered selection punch through its tail.
    // servicePunch() above does the actual in/out record; this just ends the
    // session: once recording has stopped (punched out) and the post-roll has
    // played, OR the take reached its end, OR the user stopped, stop the
    // transport and tear the punch arming down.
    auto& player = engine.getPlayer();
    const auto pos = player.getPositionSamples();
    const bool punchedOut = ! engine.isRecording();
    const bool done = (! player.isPlaying())
                   || (punchedOut && pos >= punchPostRollEnd)
                   || (pos >= player.getTotalLengthSamples());
    if (! done) return;

    const bool hadCapture = engine.isRecording();
    if (hadCapture) stopActiveCaptureAsync (false); // safety net
    engine.stopPlayback();
    engine.setPunchModeOn (false);
    punchSessionActive = false;
    recordButton.setButtonText ("RECORD");
    if (! hadCapture && lastCaptureFinalizationError.isEmpty()) showStatus ("Punch complete");
}

juce::String MainComponent::noiseAnalysisSummary (const std::vector<zynforge::NoiseFinding>& findings)
{
    int humCount = 0, bumpCount = 0, failedCount = 0;
    for (const auto& f : findings)
    {
        if (f.error.isNotEmpty()) { ++failedCount; continue; }
        if (f.humFundamentalHz > 0) ++humCount;
        if (f.bumpCount > 0) ++bumpCount;
    }
    const auto completed = findings.size() - (size_t) failedCount;
    const auto count = failedCount == 0 ? juce::String (findings.size())
        : juce::String (completed) + " of " + juce::String (findings.size());
    const auto failures = failedCount == 0 ? juce::String()
        : juce::String (failedCount) + (failedCount == 1 ? " analysis failed; " : " analyses failed; ");
    return count + " track"
        + (findings.size() == 1 ? juce::String() : juce::String ("s"))
        + " analysed -- " + failures + juce::String (humCount) + " with hum, "
        + juce::String (bumpCount) + " with mic bumps. Report saved to noise_report.json.";
}

// Analysis reads every take file in full on the capture drive. Never while a
// take (local or daemon) is writing there, or beside another session job.
bool MainComponent::refuseAnalysisNow()
{
    if (engine.isRecording() || captureMetadataPending)
    { showStatus ("Analysis runs after recording stops -- it reads every take file"); return true; }
    if (sessionIoBusy.load())
    { showStatus ("Wait for the session operation to finish"); return true; }
    if (analysisBusy.load())
    { showStatus ("An analysis is already running"); return true; }
    return false;
}

// RECORD must never wait for a report, so a scan holds its own reservation
// rather than the session's, and the take cancels the scan. Polled from the
// timer because a take can start from the button, a remote or a punch window.
void MainComponent::cancelAnalysisForTake()
{
    if (analysisBusy.load() && engine.isRecording()) analysisJobs.cancel();
}

// A report dialog is modal: over a live take it would stand between the
// engineer and STOP.
bool MainComponent::analysisResultWanted (const juce::File& scannedSession)
{
    if (engine.isRecording() || captureMetadataPending)
    {
        showStatus ("Analysis stopped for the take -- run it again after recording");
        return false;
    }
    if (engine.getActiveSessionDir() != scannedSession)
    {
        showStatus ("Analysis finished for the previous session; nothing was changed");
        return false;
    }
    return true;
}

void MainComponent::runNoiseAnalysis()
{
    if (refuseAnalysisNow()) return;
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or record a session before analysing");
        return;
    }

    // Nothing to analyse until a take is on disk -- the analyser reads the
    // recorded Track_NN.* files. Tell the engineer plainly instead of running
    // a background scan that finds nothing and pops an empty report.
    const auto audioDir = sessionDir.getChildFile ("Audio Files").isDirectory()
                            ? sessionDir.getChildFile ("Audio Files") : sessionDir;
    if (audioDir.findChildFiles (juce::File::findFiles, false, "Track_*.wav;Track_*.aif;Track_*.aiff;Track_*.flac").isEmpty())
    {
        // Spell out what the tool does + why it can't run yet, instead of a
        // status line that's easy to miss (the "it does nothing" report).
        juce::AlertWindow::showMessageBoxAsync (
            juce::MessageBoxIconType::InfoIcon,
            "Nothing to analyse yet",
            "Analyse for noise scans your RECORDED tracks for electrical hum "
            "(50 / 60 Hz), mic bumps / thumps, and each track's noise floor, "
            "then shows a sortable report (and saves noise_report.json).\n\n"
            "This session has no recorded or imported audio yet -- record a "
            "take or import audio first, then run it.");
        return;
    }

    showStatus ("Analysing tracks for noise / hum / bumps...");

    // Snapshot track names so the worker doesn't touch engine state
    // from the background thread.
    juce::StringArray names;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        names.add (engine.getRecorder().getTrack (i).name);

    juce::Component::SafePointer<MainComponent> self (this);
    analysisJobs.start ([self, sessionDir, names] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        const auto findings = zynforge::NoiseAnalyzer::analyseSession (
            sessionDir,
            [&names] (int idx) -> juce::String
            {
                return (idx >= 0 && idx < names.size())
                    ? names[idx]
                    : juce::String ("Track ") + juce::String (idx + 1);
            }, &cancel);
        if (cancel.load (std::memory_order_relaxed))
            return [self] { if (self != nullptr) self->showStatus ("Noise analysis stopped: a take started"); };

        const auto summary = noiseAnalysisSummary (findings);

        return [self, summary, findings, sessionDir]
        {
            if (self == nullptr || ! self->analysisResultWanted (sessionDir)) return;
            self->showStatus (summary);
            // Sortable table replaces the text dump -- engineer clicks
            // column headers to sort by worst hum / most bumps /
            // highest noise floor.
            zynforge::NoiseReportDialog::launch (findings);
        };
    });
}

void MainComponent::runQcAnalysis()
{
    if (refuseAnalysisNow()) return;
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before running QC");
        return;
    }

    const auto audioDir = sessionDir.getChildFile ("Audio Files").isDirectory()
                            ? sessionDir.getChildFile ("Audio Files") : sessionDir;
    auto files = audioDir.findChildFiles (juce::File::findFiles, false,
                                          "Track_*.wav;Track_*.aif;Track_*.aiff;Track_*.flac");
    if (files.isEmpty())
    {
        showStatus ("No recorded audio to QC -- record a take first.");
        return;
    }
    files.sort();

    showStatus ("Running post-show QC (peak / LUFS / clipping / noise floor)...");

    // Snapshot names so the worker never touches engine state.
    juce::StringArray names;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        names.add (engine.getRecorder().getTrack (i).name);

    juce::Component::SafePointer<MainComponent> self (this);
    analysisJobs.start ([self, sessionDir, audioDir, files, names] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        const auto stopped = [self]
        { if (self != nullptr) self->showStatus ("QC stopped: a take started. No report was written."); };

        std::vector<zynforge::qc::TrackQc> results;
        for (const auto& f : files)
        {
            const auto stem = f.getFileNameWithoutExtension();          // "Track_NN" or "Track_NN_partXX"
            // Skip continuation parts: analyzeFile scans one file at a time, so
            // a _part would produce a phantom mislabeled row (getIntValue on the
            // last "_" segment -> 0 -> track 1) and miss the later audio. Parse
            // the index from the Track_NN PREFIX, matching NoiseAnalyzer/song
            // detect (the fix pass fixed those two analyzers but not this one).
            // Also skips punch sidecars and Track_NN_consolidated_K renders.
            const int idx = zynforge::mainTakeTrackNumber (f) - 1;
            if (idx < 0) continue;
            auto q = zynforge::qc::analyzeFile (f, &cancel);
            if (cancel.load (std::memory_order_relaxed)) return stopped;
            q.trackIndex = idx;
            q.name = (idx < names.size() && names[idx].isNotEmpty()) ? names[idx] : stem;
            results.push_back (std::move (q));
        }

        // Recorder dropout count from the integrity manifest, if present.
        juce::int64 missed = -1;
        const auto reportFile = sessionDir.getChildFile ("session.report.json");
        if (reportFile.existsAsFile())
        {
            const auto v = juce::JSON::parse (reportFile);
            if (v.hasProperty ("missedSamples"))
                missed = (juce::int64) v.getProperty ("missedSamples", -1);
        }

        if (cancel.load (std::memory_order_relaxed)) return stopped;

        // Write the morning-after text report next to the exports.
        const auto exportDir = sessionDir.getChildFile ("Export Files");
        exportDir.createDirectory();
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H%M%S");
        const auto reportTxt = exportDir.getChildFile ("QC Report " + stamp + ".txt");
        const bool reportSaved = zynforge::atomicfile::writeText (
            reportTxt, zynforge::qc::reportText (sessionDir.getFileName(), results, missed));

        juce::String summary = zynforge::qc::statusSummary (results, missed)
            + (reportSaved ? ". Saved " + reportTxt.getFileName() + "."
                           : ". WARNING: report file could not be saved.");

        return [self, summary, results, sessionDir] () mutable
        {
            if (self == nullptr || ! self->analysisResultWanted (sessionDir)) return;
            self->showStatus (summary);
            zynforge::QcReportDialog::launch (std::move (results));
        };
    });
}

void MainComponent::promptConsoleConnect()
{
    if (consoleLink.isConnected())
    {
        consoleLink.disconnect();
        showStatus ("Console link closed");
        return;
    }

    auto* props = engine.getAppProps();
    const auto lastHost = props != nullptr
        ? props->getValue ("consoleHost", "192.168.1.10")
        : juce::String ("192.168.1.10");
    const int lastProfile = props != nullptr ? props->getIntValue ("consoleProfile", 0) : 0;

    auto* aw = new juce::AlertWindow ("Connect to console",
                                      "Pick your console family and enter its IP. The X32 / M32 "
                                      "is driven fully over OSC (repatch + head-amp gains); the "
                                      "large-format desks use their own Virtual Soundcheck mode.",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default

    const auto profiles = zynforge::consoleProfiles();
    juce::StringArray profileNames;
    for (auto& p : profiles) profileNames.add (p.displayName);
    aw->addComboBox ("console", profileNames, "Console:");
    if (auto* cb = aw->getComboBoxComponent ("console"))
        cb->setSelectedItemIndex (juce::jlimit (0, profileNames.size() - 1, lastProfile),
                                  juce::dontSendNotification);

    aw->addTextEditor ("host", lastHost, "Console IP:");
    dialog::primeNameEditor (*aw, "host");
    aw->addButton ("Connect", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel",  0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, profiles] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1 || self == nullptr) return;
            const auto host = aw->getTextEditorContents ("host").trim();
            if (host.isEmpty()) return;

            const int profileIdx = aw->getComboBoxComponent ("console") != nullptr
                ? juce::jlimit (0, (int) profiles.size() - 1,
                                aw->getComboBoxComponent ("console")->getSelectedItemIndex())
                : 0;
            self->consoleLink.setProfile (profiles[(size_t) profileIdx].kind);

            if (! self->consoleLink.connect (host))
            {
                self->showStatus (zynforge::oscwire::normaliseSourceIp (host).isEmpty()
                    ? "Enter the console's IP address as numbers, like 192.168.1.50"
                    : "Console link failed -- check the IP and the network");
                return;
            }
            if (auto* p = self->engine.getAppProps())
            {
                p->setValue ("consoleHost", host);
                p->setValue ("consoleProfile", profileIdx);
                p->saveIfNeeded();
            }
            // Pull the channel names straight away so the session labels
            // itself the moment the desk is reachable.
            if (self->consoleLink.getProfile().canReadNames)
            {
                const int n = juce::jlimit (1, 64,
                    juce::jmax (8, self->engine.getRecorder().getNumTracks()));
                self->consoleLink.requestChannelNames (n);
            }

            // For a native-VSC desk, point the engineer at the console.
            if (! self->consoleLink.getProfile().canRepatch
                && self->consoleLink.getProfile().note.isNotEmpty())
                self->showStatus (self->consoleLink.getProfile().displayName + " -- "
                            + self->consoleLink.getProfile().note);
            // Show-night state saved with the session (stage patch +
            // gains) comes back automatically on VSC day.
            const auto sess = self->engine.getActiveSessionDir();
            self->consoleLink.setSessionDir (sess);
            if (sess.isDirectory() && self->consoleLink.loadFrom (sess))
                self->showStatus ("Console link up: " + host
                            + " -- loaded saved patch + gains from session");
        }), false);
}

void MainComponent::wireConsoleLink()
{
    juce::Component::SafePointer<MainComponent> self (this);
    consoleLink.onStatus = [self] (const juce::String& s)
    { if (self != nullptr) self->showStatus (s); };

    // A channel name lands on the matching strip, so takes arrive labelled
    // instead of "1, 2, 3". Routed through the engine's capture-aware path so
    // "New session from console" can also collect names for channels that
    // don't exist yet.
    consoleLink.onChannelName = [self] (int ch1, const juce::String& name)
    {
        if (self == nullptr || name.trim().isEmpty()) return;
        self->engine.getRemotes().onConsoleChannelName (ch1, name.trim());
    };

    consoleLink.onSceneRecalled = [self] (int sceneIdx, const juce::String& sceneName)
    {
        if (self != nullptr)
            self->engine.getRemotes().onConsoleScene (sceneIdx, sceneName, zynforge::scenefollow::Path::ConsoleLink);
    };

    consoleLink.onStereoLink = [self] (int leftCh1, bool linked)
    { if (self != nullptr) self->engine.getRemotes().onConsoleStereoLink (leftCh1, linked); };

    consoleLink.onTraffic = [self] (const juce::String& line)
    { if (self != nullptr) self->engine.getRemotes().logOsc (line); };
}

// A repatch or a head-amp write changes what the take records, so neither goes
// to the desk while one rolls, whoever asks.
bool MainComponent::refuseConsoleWriteDuringTake()
{
    if (! engine.isRecording() && ! captureMetadataPending) return false;
    showStatus ("Console changes wait until the take has stopped");
    return true;
}

void MainComponent::consoleToggleSoundcheck()
{
    if (refuseConsoleWriteDuringTake()) return;
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
    const auto sess = engine.getActiveSessionDir();
    consoleLink.setSessionDir (sess);
    if (consoleLink.getPatch() == zynforge::ConsoleLink::Patch::Soundcheck
        || (consoleLink.getPatch() == zynforge::ConsoleLink::Patch::Unknown && consoleLink.hasStagePatch()))
        consoleLink.exitSoundcheck();
    else if (! sess.isDirectory())
        showStatus ("Open a session first -- the show patch is saved with it before the console moves");
    else
        consoleLink.enterSoundcheck();
}

void MainComponent::consoleCaptureGains()
{
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
    consoleLink.setSessionDir (engine.getActiveSessionDir());
    // jlimit(1, ...) turned an empty session into a 1-channel poll, which then
    // "captured" a single head-amp and saved it as the show's gains.
    const int tracks = engine.getRecorder().getNumTracks();
    if (tracks <= 0)
    {
        showStatus ("No channels yet -- build the session before capturing head-amp gains");
        return;
    }
    const int n = juce::jlimit (1, 32, tracks);
    consoleLink.captureGains (n);
    // Persist once the replies have had time to land (UDP round trip on
    // a local network is milliseconds; 1.5 s is generous).
    juce::Component::SafePointer<MainComponent> self (this);
    juce::Timer::callAfterDelay (1500, [self]
    {
        if (self == nullptr) return;
        // Only persist a COMPLETE capture. "Not empty" wasn't enough: this
        // timer and ConsoleLink's own reply watchdog are both 1500 ms, so on a
        // lossy link they race -- a partial capture (a handful of replies)
        // would pass the emptiness check and overwrite console_state.json as
        // if it were the whole show, leaving the un-captured channels to be
        // "restored" to whatever the desk happened to have at soundcheck.
        if (self->consoleLink.getCapturedGains().empty())
        {
            self->showStatus ("No head-amp replies from the console -- gains not saved");
            return;
        }
        if (! self->consoleLink.isGainCaptureComplete())
        {
            self->showStatus (juce::String ((int) self->consoleLink.getCapturedGains().size())
                              + " of the requested head-amp gains came back -- PARTIAL capture "
                                "NOT saved. Check the console link and capture again.");
            return;
        }
        const auto sess = self->engine.getActiveSessionDir();
        if (sess.isDirectory() && self->consoleLink.saveTo (sess))
            self->showStatus (juce::String ((int) self->consoleLink.getCapturedGains().size())
                              + " head-amp gains captured -> "
                              + zynforge::ConsoleLink::kStateFileName);
    });
}

void MainComponent::consoleRestoreGains()
{
    if (refuseConsoleWriteDuringTake()) return;
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
    consoleLink.setSessionDir (engine.getActiveSessionDir());
    if (consoleLink.getCapturedGains().empty())
    {
        showStatus ("No captured gains in this session -- capture them on show night first");
        return;
    }
    consoleLink.restoreGains();
}

void MainComponent::connectCaptureDaemon (bool announceReattach)
{
    captureSupervisor.onDaemonDied = [this] (bool wasRecording)
    {
        // The link may have dropped while the daemon continues to capture.
        // Retain the transport guard, but never show its old metrics as live.
        engine.invalidateExternalCaptureStatus();
        engine.setExternalRecording (wasRecording);
        showStatus (wasRecording
            ? "!! CAPTURE DAEMON UNAVAILABLE MID-TAKE -- may still be recording. Reconnect and verify before touching storage."
            : "Capture daemon connection lost or process exited.");
        recordButton.setButtonText (wasRecording ? "REC ?" : "RECORD");
    };

    if (! captureSupervisor.connectOrLaunch (kCaptureDaemonPort))
    {
        useCaptureDaemon = false;
        showStatus ("Capture daemon unavailable (binary not found / port busy) -- in-process recording stays active.");
        return;
    }

    // Phase 2: if a daemon left rolling by a previous GUI is mid-take,
    // adopt it -- the engineer sees the take immediately.
    juce::Thread::sleep (250);   // let the first status push land
    if (captureSupervisor.isDaemonRecording())
    {
        const auto status = captureSupervisor.lastStatus();
        engine.setExternalCaptureStatus (status, captureSupervisor.lastStatusAtMs());
        engine.setExternalRecording (true);
        if (status.sessionPath.isNotEmpty()) engine.setActiveSessionDir (juce::File (status.sessionPath));
        recordButton.setButtonText ("STOP");
        if (announceReattach)
            showStatus ("REATTACHED to a rolling capture-daemon take -- STOP closes it normally.");
    }
    else
        showStatus (captureSupervisor.didLaunch()
            ? "Capture daemon launched (experimental) -- RECORD now runs out-of-process."
            : "Attached to running capture daemon -- RECORD now runs out-of-process.");
}

void MainComponent::toggleCaptureDaemon()
{
    if (useCaptureDaemon)
    {
        if (captureSupervisor.isDaemonRecording())
        {
            showStatus ("Daemon take rolling -- STOP it before disabling the capture daemon.");
            return;
        }
        captureSupervisor.shutdown();
        useCaptureDaemon = false;
        showStatus ("Capture daemon disabled -- recording is in-process again.");
    }
    else
    {
        useCaptureDaemon = true;
        connectCaptureDaemon (true);
    }
    if (auto* p = engine.getAppProps())
    {
        p->setValue ("useCaptureDaemon", useCaptureDaemon);
        p->saveIfNeeded();
    }
}

bool MainComponent::configureCaptureDaemon()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    auto xml = engine.getDeviceManager().createStateXml();
    if (device == nullptr || xml == nullptr) return false;
    auto* o = new juce::DynamicObject();
    juce::var config (o);
    o->setProperty ("deviceState", xml->toString());
    o->setProperty ("sampleRate", device->getCurrentSampleRate());
    auto& rec = engine.getRecorder();
    o->setProperty ("preRoll", rec.getPreRollSeconds());
    o->setProperty ("backup", rec.getBackupDirectory().getFullPathName());
    o->setProperty ("backupFormat", (int) rec.getBackupCaptureFormat());
    juce::Array<juce::var> tracks, mirrors;
    for (int i = 0; i < rec.getNumTracks(); ++i)
    {
        auto& t = rec.getTrack (i); auto* s = new juce::DynamicObject();
        s->setProperty ("name", t.getNameThreadSafe()); s->setProperty ("uid", t.stripId);
        s->setProperty ("input", engine.getEffectiveInputRouting (i)); s->setProperty ("armed", t.armed.load());
        s->setProperty ("stereo", t.isStereo.load()); s->setProperty ("bus", t.isBus.load());
        tracks.add (juce::var (s));
    }
    for (const auto& m : rec.getMirrors())
    {
        auto* s = new juce::DynamicObject(); s->setProperty ("root", m.root.getFullPathName());
        s->setProperty ("format", (int) m.format); mirrors.add (juce::var (s));
    }
    o->setProperty ("tracks", tracks); o->setProperty ("mirrors", mirrors);
    return captureSupervisor.configureCapture (config);
}

void MainComponent::scanForCrashReports()
{
    auto* props = engine.getAppProps();
    if (props == nullptr) return;

    // First-ever run: don't dump historic reports on a fresh install --
    // just set the baseline and start watching from here.
    const double lastMs = props->getDoubleValue ("lastCrashScanMs", 0.0);
    props->setValue ("lastCrashScanMs",
                     (double) juce::Time::getCurrentTime().toMilliseconds());
    props->saveIfNeeded();
    if (lastMs <= 0.0) return;

    const auto reports = zynforge::crashscan::findNewReports (
        zynforge::crashscan::reportsDirectory(), juce::Time ((juce::int64) lastMs));
    if (! reports.isEmpty())
        showCrashReportNotice (reports);
}

void MainComponent::showCrashReportNotice (const juce::Array<juce::File>& reports)
{
    if (reports.isEmpty()) return;
    const auto newest = reports.getFirst();
    juce::String body;
    body << "ZynForge Recording crashed since the last run ("
         << reports.size() << " report" << (reports.size() == 1 ? "" : "s") << ").\n\n"
         << zynforge::crashscan::summarize (newest)
         << "\nCopy the details into a GitHub issue, or reveal the report in Finder.";

    auto* aw = new juce::AlertWindow ("Crash detected", body,
                                      juce::MessageBoxIconType::WarningIcon);
    aw->setLookAndFeel (&laf);
    aw->addButton ("Copy details",     1);
    aw->addButton ("Reveal in Finder", 2);
    aw->addButton ("Dismiss",          0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw, newest] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result == 1)
                juce::SystemClipboard::copyTextToClipboard (
                    zynforge::crashscan::summarize (newest)
                    + "\nFull report: " + newest.getFullPathName());
            else if (result == 2)
                newest.revealToUser();
        }));
}

void MainComponent::detectSongsToMarkers()
{
    if (refuseAnalysisNow()) return;
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before detecting songs");
        return;
    }

    const auto voices = engine.songScanVoices();
    if (voices.empty())
    {
        showStatus ("No recorded audio to scan -- record a take first.");
        return;
    }
    const auto arrangement = juce::JSON::toString (engine.playlistsToJson());

    showStatus ("Detecting songs (multi-track quorum scan)...");

    juce::Component::SafePointer<MainComponent> self (this);
    analysisJobs.start ([self, voices, sessionDir, arrangement] (const std::atomic<bool>& cancel)
                        -> zynforge::SessionJobRunner::Completion
    {
        const zynforge::songs::Params params;        // -40 dB / 4 s gap / 60 s min / quorum 2
        double sr = 48000.0;
        const auto flags = zynforge::songs::scanVoices (voices, params, sr, &cancel);
        if (cancel.load (std::memory_order_relaxed))
            return [self] { if (self != nullptr) self->showStatus ("Song detection stopped: a take started"); };
        const auto found = zynforge::songs::detect (flags, sr, params);

        return [self, found, sessionDir, arrangement]
        {
            if (self == nullptr || ! self->analysisResultWanted (sessionDir)) return;
            if (found.empty())
            {
                self->showStatus ("No songs detected -- try Marker List to add them by hand.");
                return;
            }
            // Markers are a timeline edit: a take may have started during the scan.
            if (! self->canMutateTimeline ("adding song markers")) return;
            if (juce::JSON::toString (self->engine.playlistsToJson()) != arrangement)
            {
                self->showStatus ("The arrangement changed during the scan; no markers were added");
                return;
            }
            auto& markers = self->engine.getMarkers();
            int n = 0;
            for (const auto& s : found)
                markers.drop (s.startSample, "Song " + juce::String (++n), "song");
            self->showStatus (juce::String (n) + " song marker" + (n == 1 ? "" : "s")
                              + " dropped -- rename them in the Marker List.");
        };
    });
}

void MainComponent::writeSoundcheckReport()
{
    // Nothing writes to the session volume while a take is rolling.
    if (engine.isRecording() || captureMetadataPending)
    { showStatus ("The soundcheck report waits until the take has stopped"); return; }
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before writing a soundcheck report");
        return;
    }

    auto& rec = engine.getRecorder();
    const double sr = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
        ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
        : 48000.0;

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("generatedAt", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("sampleRate",  sr);
    root->setProperty ("bpm",         engine.getSessionTempoBpm());

    juce::Array<juce::var> arr;
    for (int i = 0; i < rec.getNumTracks(); ++i)
    {
        const auto& t = rec.getTrack (i);
        const auto r  = SpectralClassifier::classify (t, sr);

        juce::DynamicObject::Ptr o (new juce::DynamicObject());
        o->setProperty ("track",        i + 1);
        o->setProperty ("name",         t.name);
        o->setProperty ("guess",        r.name);
        o->setProperty ("peak",         (double) t.peak.load());
        o->setProperty ("rms",          (double) t.rms .load());
        o->setProperty ("clipCount",    t.clipCount.load());
        o->setProperty ("bandSub",      (double) r.bands.sub);
        o->setProperty ("bandLow",      (double) r.bands.low);
        o->setProperty ("bandMid",      (double) r.bands.mid);
        o->setProperty ("bandHigh",     (double) r.bands.high);
        o->setProperty ("bandAir",      (double) r.bands.air);
        o->setProperty ("stereo",       t.isStereo.load());
        arr.add (juce::var (o.get()));
    }
    root->setProperty ("strips", juce::var (arr));

    const auto reportFile = sessionDir.getChildFile ("soundcheck.report.json");
    showStatus (zynforge::atomicfile::writeText (
                    reportFile, juce::JSON::toString (juce::var (root.get())))
                    ? "Soundcheck report -> " + reportFile.getFileName()
                    : "Soundcheck report NOT saved -- check permissions / free space");
}

void MainComponent::showSessionProperties()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session first");
        return;
    }

    const auto proj = findSessionProj (sessionDir);

    juce::var parsed;
    if (proj.existsAsFile())
        parsed = juce::JSON::parse (proj);
    juce::DynamicObject::Ptr obj;
    if (parsed.isObject())
        obj = parsed.getDynamicObject();
    else
        obj = new juce::DynamicObject();

    auto readString = [&] (const juce::Identifier& k, const juce::String& fallback = {}) -> juce::String
    {
        if (obj == nullptr) return fallback;
        const auto v = obj->getProperty (k);
        if (v.isString()) return v.toString();
        return fallback;
    };

    // Convert the persisted captureFormat int back to a readable label.
    auto formatLabel = [] (int code) -> juce::String
    {
        using F = CaptureFormat;
        switch ((F) code)
        {
            case F::Wav16:        return "WAV 16-bit";
            case F::Wav24:        return "WAV 24-bit";
            case F::Wav32Float:   return "WAV 32-bit float";
            case F::Aiff16:       return "AIFF 16-bit";
            case F::Aiff24:       return "AIFF 24-bit";
            case F::Aiff32Float:  return "AIFF 32-bit float";
            case F::Flac16:       return "FLAC 16-bit";
            case F::Flac24:       return "FLAC 24-bit";
        }
        return "--";
    };

    SessionPropertiesDialog::Fields fields;
    fields.name        = readString ("name", sessionDir.getFileName());
    fields.artist      = readString ("artist");
    fields.venue       = readString ("venue");
    fields.fohEngineer = readString ("fohEngineer");
    fields.date        = readString ("date",
                                     juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
    fields.notes       = readString ("notes");
    {
        const auto sr = obj ? (double) obj->getProperty ("sampleRate") : 0.0;
        fields.sampleRate = sr > 0.0
            ? juce::String (sr / 1000.0, 1) + " kHz"
            : juce::String ("--");
    }
    {
        // The project file holds the format the session was created with.
        // The recorder holds the one in use, and only this session is open.
        const int fmt = (int) engine.getRecorder().getCaptureFormat();
        fields.captureFormat = formatLabel (fmt);
        fields.bitDepth = (fmt == (int) CaptureFormat::Wav16 || fmt == (int) CaptureFormat::Aiff16 || fmt == (int) CaptureFormat::Flac16) ? "16-bit"
                        : (fmt == (int) CaptureFormat::Wav32Float || fmt == (int) CaptureFormat::Aiff32Float)              ? "32-bit float"
                                                                                                                          : "24-bit";
    }

    juce::Component::SafePointer<MainComponent> self (this);
    SessionPropertiesDialog::launch (fields,
        [self, sessionDir] (const SessionPropertiesDialog::Fields& edited)
        {
            if (self == nullptr) return;
            // Submit only fields owned by this dialog. The serialized worker
            // merges them with the latest project after preceding autosaves.
            juce::DynamicObject::Ptr merged (new juce::DynamicObject());
            merged->setProperty ("zynforgeSession", true);
            merged->setProperty ("name",            edited.name);
            merged->setProperty ("artist",          edited.artist);
            merged->setProperty ("venue",           edited.venue);
            merged->setProperty ("fohEngineer",     edited.fohEngineer);
            merged->setProperty ("date",            edited.date);
            merged->setProperty ("notes",           edited.notes);
            merged->setProperty ("updatedAt",
                                 juce::Time::getCurrentTime().toISO8601 (true));

            SessionMetadataSnapshot snapshot;
            snapshot.directory = sessionDir;
            snapshot.projectPatchJson = juce::JSON::toString (juce::var (merged.get()));
            self->enqueueMetadataSnapshot (std::move (snapshot), {},
                [self, sessionDir] (bool ok)
                {
                    if (self != nullptr && ok)
                        self->showStatus ("Saved session properties -- " + sessionDir.getFileName());
                });
        });
}
