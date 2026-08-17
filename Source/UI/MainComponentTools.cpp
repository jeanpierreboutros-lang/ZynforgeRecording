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
#include "../Audio/CrashReportScan.h"
#include "../Audio/NoiseAnalyzer.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SongDetector.h"
#include "../Audio/SpectralClassifier.h"
#include "NoiseReportDialog.h"
#include "QcReportDialog.h"
#include "SessionPropertiesDialog.h"

using namespace zynforge;

// Returns TRUE only when a click WAV was actually written. Every refusal here
// is silent apart from a status line, so the CALLER cannot assume it happened --
// ClickSettingsDialog used to switch the live click engine off on the strength
// of calling this, which killed the click mid-take and generated nothing.
bool MainComponent::generateOrRefreshClickTrack()
{
    // NEVER regenerate during a take. This deleteFile()s + synchronously renders
    // a session-length WAV and then engine.loadSession() -- which frees/reopens
    // readers on the files the recorder is actively writing (disk contention,
    // dropout risk, multi-second UI freeze). Reachable mid-record via a cue
    // recall with a differing tempo (digit keys) or a TempoBar BPM nudge; the
    // click refreshes to the new tempo after the take instead.
    if (engine.isRecording())
    {
        showStatus ("Click track refreshes after recording stops");
        return false;
    }

    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before generating a click");
        return false;
    }

    auto audioFiles = sessionDir.getChildFile ("Audio Files");
    audioFiles.createDirectory();

    auto& recorder = engine.getRecorder();
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
    }

    const double sr      = juce::jmax (8000.0, [this]
    {
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            return d->getCurrentSampleRate();
        return 48000.0;
    }());
    // Match the session's playback length, falling back to 4 hours when
    // the session is empty so the engineer always has more than enough
    // click for a show.
    auto& player = engine.getPlayer();
    juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
    if (totalSamples <= 0)
        // 1-hour fallback when there's nothing loaded yet -- plenty for
        // any single show, 4× faster to render than the old 4-hour cap.
        totalSamples = (juce::int64) (sr * 60.0 * 60.0);

    // Render the offline click using the same per-voice tone presets
    // + subdivision factors the real-time engine uses, so the file
    // matches what the engineer was hearing live before they hit
    // Generate.
    auto& cl = engine.getClickEngine();
    const auto preset1 = ClickEngine::getVoicePreset (cl.getVoice1());
    const auto preset2 = ClickEngine::getVoicePreset (cl.getVoice2());
    const auto sub1    = cl.getSub1();
    const auto sub2    = cl.getSub2();
    const double f1    = ClickEngine::subFactor (sub1);
    const double f2    = ClickEngine::subFactor (sub2);
    const float lin1   = juce::Decibels::decibelsToGain (cl.getVol1Db());
    const float lin2   = juce::Decibels::decibelsToGain (cl.getVol2Db());

    const auto& tempoMap = engine.getTempoMap();
    auto bpmAtSample = [&] (juce::int64 sample) -> float
    {
        float bpm = engine.getSessionTempoBpm();
        for (const auto& tc : tempoMap)
        {
            if (tc.samplePos <= sample) bpm = tc.bpm;
            else break;
        }
        return bpm;
    };

    // Defence in depth: NEVER deleteFile() a slot that isn't actually the
    // click strip. Require both the name AND the playback-only routing
    // (inputRouting < 0) so a recorded channel that merely happens to be named
    // "Click" (a console metronome return) can't be overwritten.
    if (clickTrackIndex < 0 || clickTrackIndex >= recorder.getNumTracks()
        || recorder.getTrack (clickTrackIndex).getNameThreadSafe() != "Click"
        || recorder.getTrack (clickTrackIndex).inputRouting.load (std::memory_order_relaxed) >= 0)
    {
        showStatus ("Click slot isn't a Click strip -- aborted to protect recordings");
        return false;
    }

    const auto trackName = juce::String::formatted ("Track_%02d", clickTrackIndex + 1);
    auto dest = audioFiles.getChildFile (trackName + ".wav");
    dest.deleteFile();

    bool rendered = false;
    if (auto* out = dest.createOutputStream().release())
    {
        juce::WavAudioFormat wav;
        juce::StringPairArray meta;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (out, sr, 1, 24, meta, 0));
        if (writer == nullptr) { delete out; showStatus ("Click write failed"); return false; }

        // Larger render chunks → fewer disk writes (8× the old size).
        constexpr int kChunk = 32768;
        juce::AudioBuffer<float> buf (1, kChunk);

        // Per-voice burst envelope state. We render a damped sine for
        // each active burst, decaying over ~50 ms.
        struct Burst { double phase=0, tSec=0, freq=1000, decay=60, gain=0; bool active=false; };
        Burst v1Burst, v2Burst;

        // Beat-scheduling counters, identical to ClickEngine's runtime.
        double samplesUntil1 = 0.0;
        double samplesUntil2 = 0.0;
        int    beat1Counter  = 0;
        int    beat2Counter  = 0;

        // Hoist tempo / per-click sample counts out of the sample loop.
        // bpmAtSample() does a tempoMap scan; running it once per sample
        // at 60M+ samples was the main reason Generate felt slow. We
        // recompute when the map says the tempo changes (rare) and
        // otherwise leave the cached values alone.
        const bool tempoIsConstant = tempoMap.empty();
        float  cachedBpm           = engine.getSessionTempoBpm();
        double samplesPerQuarter   = 60.0 * sr / juce::jmax (20.0f, cachedBpm);
        double samplesPerClick1    = (f1 > 0.0) ? (samplesPerQuarter / f1) : 0.0;
        double samplesPerClick2    = (f2 > 0.0) ? (samplesPerQuarter / f2) : 0.0;
        // Downbeat accent every N beats, N = time-signature numerator (was 4).
        const int beatsPerBar      = juce::jmax (1, engine.getTimeSignatureNumerator());

        juce::int64 written = 0;
        while (written < totalSamples)
        {
            const int thisChunk = (int) juce::jmin ((juce::int64) kChunk, totalSamples - written);
            buf.clear();
            auto* dst = buf.getWritePointer (0);

            // If the session has tempo changes, refresh the cached
            // per-click sample counts once per chunk (samples per block,
            // not per sample). For a typical session with no tempo map
            // this branch never executes.
            if (! tempoIsConstant)
            {
                const float bpm = bpmAtSample (written);
                if (bpm != cachedBpm)
                {
                    cachedBpm        = bpm;
                    samplesPerQuarter = 60.0 * sr / juce::jmax (20.0f, cachedBpm);
                    samplesPerClick1  = (f1 > 0.0) ? (samplesPerQuarter / f1) : 0.0;
                    samplesPerClick2  = (f2 > 0.0) ? (samplesPerQuarter / f2) : 0.0;
                }
            }

            for (int i = 0; i < thisChunk; ++i)
            {

                if (samplesPerClick1 > 0.0)
                {
                    samplesUntil1 -= 1.0;
                    if (samplesUntil1 <= 0.0)
                    {
                        samplesUntil1 += samplesPerClick1;
                        if ((beat1Counter % beatsPerBar) == 0)
                        {
                            v1Burst = { 0.0, 0.0, preset1.freq, preset1.decay, lin1, true };
                        }
                        ++beat1Counter;
                    }
                }
                if (samplesPerClick2 > 0.0)
                {
                    samplesUntil2 -= 1.0;
                    if (samplesUntil2 <= 0.0)
                    {
                        samplesUntil2 += samplesPerClick2;
                        const bool downAligned =
                            (sub2 == ClickEngine::Subdivision::Quarter) && (beat2Counter % beatsPerBar) == 0;
                        if (! downAligned)
                            v2Burst = { 0.0, 0.0, preset2.freq, preset2.decay, lin2, true };
                        ++beat2Counter;
                    }
                }

                auto renderBurst = [sr] (Burst& b) -> float
                {
                    if (! b.active) return 0.0f;
                    const double dt   = 1.0 / sr;
                    const double env  = std::exp (-b.tSec * b.decay);
                    const double samp = std::sin (b.phase) * env * b.gain;
                    b.phase += juce::MathConstants<double>::twoPi * b.freq * dt;
                    b.tSec  += dt;
                    if (env < 0.001) b.active = false;
                    return (float) samp;
                };

                dst[i] = renderBurst (v1Burst) + renderBurst (v2Burst);
            }

            writer->writeFromFloatArrays (buf.getArrayOfReadPointers(), 1, thisChunk);
            written += thisChunk;
        }
        rendered = true;
    }

    // The output stream couldn't be opened (disk full / read-only session
    // folder). Report the failure instead of falsely claiming success and
    // leaving an empty click strip; the strip already exists from the
    // first-press branch, so nothing further to undo.
    if (! rendered)
    {
        showStatus ("Click track NOT generated -- couldn't write to the session folder");
        return false;
    }

    // Default the click track to hardware output 1 so it's audible
    // without further routing -- the strip's OUT combo still exposes
    // every available device output so the engineer can re-patch it
    // to a dedicated cue bus (headphones, drummer's IEM, etc.).
    if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
    {
        const int outs = dev->getActiveOutputChannels().countNumberOfSetBits();
        const int outCh = juce::jlimit (0, juce::jmax (0, outs - 1), 0);
        engine.setTrackOutputRouting (clickTrackIndex, outCh);
    }
    engine.setTrackInputRouting (clickTrackIndex, -1);   // no input -- playback only

    // Same-session reload to pick up the new click file -- PRESERVE every
    // comp/split/fade edit (the default wiping open would discard them).
    engine.loadSession (sessionDir, /*preserveEdits*/ true);
    lastTrackCount = -1;

    showStatus ("Click track generated at "
                + juce::String (engine.getSessionTempoBpm(), 1) + " BPM "
                + "(routable to any output via its strip)");

    // Light up the click-beat overlay on every other EDIT row so the
    // engineer can see the metronome pulse against each track's audio.
    if (editPage != nullptr)
        editPage->setClickTrackPresent (true, clickTrackIndex);
    return true;
}

void MainComponent::togglePunchMode()
{
    const bool on = ! engine.isPunchModeOn();
    engine.setPunchModeOn (on);
    if (on)
    {
        // Default: punch-arm every currently-armed track. Engineer can
        // narrow by un-arming individuals via the track's right-click
        // menu later.
        auto& rec = engine.getRecorder();
        for (int i = 0; i < rec.getNumTracks(); ++i)
            engine.setTrackPunchArmed (i,
                rec.getTrack (i).armed.load (std::memory_order_relaxed));
        showStatus ("Punch mode ON -- set the loop region, then press PLAY");
    }
    else
    {
        if (engine.isRecording()) engine.stopRecording();
        restoreArmStateAfterPunch();   // leaving punch mode must not keep the forced arms
        showStatus ("Punch mode OFF");
    }
}

// Capture / restore the engineer's arm layout around a punch. servicePunch has
// to force `armed = punchArmed` so only the punch-armed tracks hit disk, but
// that write is destructive: without restoring it, narrowing the punch set (or
// arming more tracks after enabling PUNCH) silently left those channels
// disarmed for the NEXT take -- a missed-recording bug on a live rig.
void MainComponent::snapshotArmStateForPunch()
{
    if (! armStateBeforePunch.empty()) return;   // already holding a snapshot
    auto& rec = engine.getRecorder();
    const int n = rec.getNumTracks();
    armStateBeforePunch.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        armStateBeforePunch[(size_t) i] =
            rec.getTrack (i).armed.load (std::memory_order_relaxed) ? 1 : 0;
}

void MainComponent::restoreArmStateAfterPunch()
{
    if (armStateBeforePunch.empty()) return;
    auto& rec = engine.getRecorder();
    const int n = juce::jmin ((int) armStateBeforePunch.size(), rec.getNumTracks());
    for (int i = 0; i < n; ++i)
        rec.getTrack (i).armed.store (armStateBeforePunch[(size_t) i] != 0,
                                      std::memory_order_relaxed);
    armStateBeforePunch.clear();
}

void MainComponent::servicePunch()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded()) return;
    const auto pos    = player.getPositionSamples();
    const auto inside = (pos >= player.getLoopStart() && pos < player.getLoopEnd());

    if (inside && ! wasInsidePunch && player.isPlaying())
    {
        // Crossed into the punch window -- punch-in on every punch-armed track:
        // record fresh audio into the CURRENT session and splice it into the
        // existing take at the punch-in point (audio before the punch-in and
        // after the punch-out is preserved). The rest keep playing back.
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
            // Multi-part takes (auto-split, or built up by continue-recording)
            // are fine now: the splice reads the whole take via ConcatReader and
            // flattens it on stop, so no refusal here.
            auto& rec = engine.getRecorder();

            // Force-arm only the punch-armed tracks for the punch -- but keep
            // the engineer's real arm layout so it can be put back on punch-out
            // (see snapshotArmStateForPunch).
            snapshotArmStateForPunch();
            for (int i = 0; i < rec.getNumTracks(); ++i)
                rec.getTrack (i).armed.store (engine.isTrackPunchArmed (i),
                                              std::memory_order_relaxed);
            // Splice at the ACTUAL playhead where recording begins (one timer
            // tick past loopStart), so the captured audio lands at the right
            // timeline position instead of being nudged earlier to loopStart.
            engine.armPunchIn (player.getPositionSamples());
            if (! engine.startRecording (sessionDir))
            {
                engine.getRecorder().cancelPunchIn();
                restoreArmStateAfterPunch();
                showStatus ("Punch-in failed -- existing take was restored");
                wasInsidePunch = inside;
                return;
            }
        }
    }
    else if (! inside && wasInsidePunch && engine.isRecording())
    {
        // Crossed out -- stop recording cleanly (the splice replaces the
        // punched region; audio after the punch-out is preserved), then put the
        // engineer's arm layout back exactly as it was before the punch.
        engine.stopRecording();
        restoreArmStateAfterPunch();
    }
    wasInsidePunch = inside;
}

void MainComponent::servicePunchSession()
{
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

    if (engine.isRecording()) engine.stopRecording();   // safety net
    restoreArmStateAfterPunch();   // no-op if servicePunch already restored it
    engine.stopPlayback();
    engine.setPunchModeOn (false);
    punchSessionActive = false;
    recordButton.setButtonText ("RECORD");
    showStatus ("Punch complete");
}

void MainComponent::runNoiseAnalysis()
{
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
    juce::Thread::launch ([self, sessionDir, names]
    {
        const auto findings = zynforge::NoiseAnalyzer::analyseSession (
            sessionDir,
            [&names] (int idx) -> juce::String
            {
                return (idx >= 0 && idx < names.size())
                    ? names[idx]
                    : juce::String ("Track ") + juce::String (idx + 1);
            });

        // Build a one-line summary + a popup with per-track detail.
        int humCount = 0, bumpCount = 0;
        for (const auto& f : findings)
        {
            if (f.humFundamentalHz > 0) ++humCount;
            if (f.bumpCount > 0) ++bumpCount;
        }
        juce::String summary = juce::String (findings.size()) + " track"
            + (findings.size() == 1 ? juce::String() : juce::String ("s"))
            + " analysed -- " + juce::String (humCount) + " with hum, "
            + juce::String (bumpCount) + " with mic bumps. Report saved to noise_report.json.";

        juce::MessageManager::callAsync ([self, summary, findings]
        {
            if (self == nullptr) return;
            self->showStatus (summary);
            // Sortable table replaces the text dump -- engineer clicks
            // column headers to sort by worst hum / most bumps /
            // highest noise floor.
            zynforge::NoiseReportDialog::launch (findings);
        });
    });
}

void MainComponent::runQcAnalysis()
{
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
    juce::Thread::launch ([self, sessionDir, audioDir, files, names]
    {
        std::vector<zynforge::qc::TrackQc> results;
        for (const auto& f : files)
        {
            const auto stem = f.getFileNameWithoutExtension();          // "Track_NN" or "Track_NN_partXX"
            // Skip continuation parts: analyzeFile scans one file at a time, so
            // a _part would produce a phantom mislabeled row (getIntValue on the
            // last "_" segment -> 0 -> track 1) and miss the later audio. Parse
            // the index from the Track_NN PREFIX, matching NoiseAnalyzer/song
            // detect (the fix pass fixed those two analyzers but not this one).
            if (stem.containsIgnoreCase ("_part")) continue;
            const int idx = stem.fromFirstOccurrenceOf ("Track_", false, false).getIntValue() - 1;
            if (idx < 0) continue;
            auto q = zynforge::qc::analyzeFile (f);
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

        // Write the morning-after text report next to the exports.
        const auto exportDir = sessionDir.getChildFile ("Export Files");
        exportDir.createDirectory();
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H%M%S");
        const auto reportTxt = exportDir.getChildFile ("QC Report " + stamp + ".txt");
        reportTxt.replaceWithText (
            zynforge::qc::reportText (sessionDir.getFileName(), results, missed));

        int clipped = 0, totalEvents = 0;
        for (const auto& q : results)
            if (q.clipEventCount > 0) { ++clipped; totalEvents += q.clipEventCount; }
        juce::String summary = juce::String ((int) results.size()) + " track"
            + (results.size() == 1 ? juce::String() : juce::String ("s")) + " QC'd -- "
            + (clipped == 0 ? juce::String ("no clipping")
                            : juce::String (clipped) + " with clipping ("
                              + juce::String (totalEvents) + " events)")
            + (missed > 0 ? juce::String (", DROPOUTS: ") + juce::String (missed) + " samples"
                          : juce::String())
            + ". Saved " + reportTxt.getFileName() + ".";

        juce::MessageManager::callAsync ([self, summary, results]() mutable
        {
            if (self == nullptr) return;
            self->showStatus (summary);
            zynforge::QcReportDialog::launch (std::move (results));
        });
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

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, profiles] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;
            const auto host = aw->getTextEditorContents ("host").trim();
            if (host.isEmpty()) return;

            const int profileIdx = aw->getComboBoxComponent ("console") != nullptr
                ? juce::jlimit (0, (int) profiles.size() - 1,
                                aw->getComboBoxComponent ("console")->getSelectedItemIndex())
                : 0;
            consoleLink.setProfile (profiles[(size_t) profileIdx].kind);

            consoleLink.onStatus = [this] (const juce::String& s) { showStatus (s); };

            // ── READ TIER ────────────────────────────────────────────────
            // The two things a recorder actually wants from a console, both
            // read-only and therefore safe on any desk, confirmed or not.

            // A channel name lands on the matching strip, so takes arrive
            // labelled instead of "1, 2, 3". Routed through the engine's
            // capture-aware path so "New session from console" can also
            // collect names for channels that don't exist yet.
            consoleLink.onChannelName = [this] (int ch1, const juce::String& name)
            {
                if (name.trim().isEmpty()) return;
                engine.onConsoleChannelName (ch1, name.trim());
            };

            // A scene / snapshot recall drops a marker. This is what makes a
            // show navigable the next morning -- jump straight to the song.
            // Markers need a session context; if there isn't one yet the recall
            // is simply ignored rather than dropped at a meaningless position.
            consoleLink.onSceneRecalled = [this] (int sceneIdx, const juce::String& sceneName)
            {
                if (! engine.getMarkers().hasContext()) return;
                const auto label = sceneName.trim().isNotEmpty()
                                     ? sceneName.trim()
                                     : "Scene " + juce::String (sceneIdx);
                if (engine.dropMarkerAtCurrentPosition (label) >= 0)
                    showStatus ("Console scene \"" + label + "\" -- marker dropped");
            };

            if (! consoleLink.connect (host))
            {
                showStatus ("Console link failed -- check the IP and the network");
                return;
            }
            if (auto* p = engine.getAppProps())
            {
                p->setValue ("consoleHost", host);
                p->setValue ("consoleProfile", profileIdx);
                p->saveIfNeeded();
            }
            // Pull the channel names straight away so the session labels
            // itself the moment the desk is reachable.
            if (consoleLink.getProfile().canReadNames)
            {
                const int n = juce::jlimit (1, 64,
                    juce::jmax (8, engine.getRecorder().getNumTracks()));
                consoleLink.requestChannelNames (n);
            }

            // For a native-VSC desk, point the engineer at the console.
            if (! consoleLink.getProfile().canRepatch
                && consoleLink.getProfile().note.isNotEmpty())
                showStatus (consoleLink.getProfile().displayName + " -- "
                            + consoleLink.getProfile().note);
            // Show-night state saved with the session (stage patch +
            // gains) comes back automatically on VSC day.
            const auto sess = engine.getActiveSessionDir();
            if (sess.isDirectory() && consoleLink.loadFrom (sess))
                showStatus ("Console link up: " + host
                            + " -- loaded saved patch + gains from session");
        }), false);
}

void MainComponent::consoleToggleSoundcheck()
{
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
    if (consoleLink.getPatch() == zynforge::ConsoleLink::Patch::Soundcheck)
        consoleLink.exitSoundcheck();
    else
        consoleLink.enterSoundcheck();
}

void MainComponent::consoleCaptureGains()
{
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
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
    if (! consoleLink.isConnected()) { showStatus ("Connect to the console first"); return; }
    if (consoleLink.getCapturedGains().empty())
    {
        const auto sess = engine.getActiveSessionDir();
        if (! (sess.isDirectory() && consoleLink.loadFrom (sess)))
        {
            showStatus ("No captured gains -- capture them on show night first");
            return;
        }
    }
    consoleLink.restoreGains();
}

void MainComponent::connectCaptureDaemon (bool announceReattach)
{
    captureSupervisor.onDaemonDied = [this] (bool wasRecording)
    {
        showStatus (wasRecording
            ? "!! CAPTURE DAEMON DIED MID-TAKE -- audio up to the last flush is on disk. NOT relaunching automatically."
            : "Capture daemon exited.");
        recordButton.setButtonText ("RECORD");
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
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before detecting songs");
        return;
    }

    const auto audioDir = sessionDir.getChildFile ("Audio Files").isDirectory()
                            ? sessionDir.getChildFile ("Audio Files") : sessionDir;
    auto files = audioDir.findChildFiles (juce::File::findFiles, false,
                                          "Track_*.wav;Track_*.aif;Track_*.aiff;Track_*.flac");
    if (files.isEmpty())
    {
        showStatus ("No recorded audio to scan -- record a take first.");
        return;
    }
    files.sort();

    showStatus ("Detecting songs (multi-track quorum scan)...");

    juce::Component::SafePointer<MainComponent> self (this);
    juce::Thread::launch ([self, files, sessionDir]
    {
        const zynforge::songs::Params params;        // -40 dB / 4 s gap / 60 s min / quorum 2
        std::vector<std::vector<char>> flags;
        double sr = 48000.0;
        for (const auto& f : files)
        {
            // Skip continuation parts: envelopeFlags treats every file as
            // starting at t=0, so a Track_NN_partXX would fold set-2 audio onto
            // the timeline START and add a phantom voice to the quorum, landing
            // song markers all over the session. Scan main files only.
            if (f.getFileNameWithoutExtension().containsIgnoreCase ("_part"))
                continue;
            double fileSr = 0.0;
            auto fl = zynforge::songs::envelopeFlags (f, params.thresholdDb,
                                                      params.windowSec, fileSr);
            if (fileSr > 0.0) sr = fileSr;
            if (! fl.empty()) flags.push_back (std::move (fl));
        }
        const auto found = zynforge::songs::detect (flags, sr, params);

        juce::MessageManager::callAsync ([self, found, sessionDir]
        {
            if (self == nullptr) return;
            if (self->engine.getActiveSessionDir() != sessionDir)
            {
                self->showStatus ("Song detection finished for the previous session; no markers were changed");
                return;
            }
            if (found.empty())
            {
                self->showStatus ("No songs detected -- try Marker List to add them by hand.");
                return;
            }
            auto& markers = self->engine.getMarkers();
            int n = 0;
            for (const auto& s : found)
                markers.drop (s.startSample, "Song " + juce::String (++n));
            self->showStatus (juce::String (n) + " song marker" + (n == 1 ? "" : "s")
                              + " dropped -- rename them in the Marker List.");
        });
    });
}

void MainComponent::writeSoundcheckReport()
{
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
    reportFile.replaceWithText (juce::JSON::toString (juce::var (root.get())));
    showStatus ("Soundcheck report -> " + reportFile.getFileName());
}

void MainComponent::showSessionProperties()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session first");
        return;
    }

    // Find the .zfproj inside the active session folder. If there's
    // more than one (shouldn't happen), pick the first match.
    juce::File proj;
    for (auto& f : sessionDir.findChildFiles (juce::File::findFiles, false, "*.zfproj"))
    {
        proj = f;
        break;
    }
    if (! proj.existsAsFile())
        proj = sessionDir.getChildFile (sessionDir.getFileName() + ".zfproj");

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
        const int fmt = obj ? (int) obj->getProperty ("captureFormat") : (int) CaptureFormat::Wav24;
        fields.captureFormat = formatLabel (fmt);
        fields.bitDepth = (fmt == (int) CaptureFormat::Wav16 || fmt == (int) CaptureFormat::Aiff16 || fmt == (int) CaptureFormat::Flac16) ? "16-bit"
                        : (fmt == (int) CaptureFormat::Wav32Float || fmt == (int) CaptureFormat::Aiff32Float)              ? "32-bit float"
                                                                                                                          : "24-bit";
    }

    SessionPropertiesDialog::launch (fields,
        [this, proj, sessionDir, obj] (const SessionPropertiesDialog::Fields& edited)
        {
            // Merge back into the existing JSON (preserves sampleRate /
            // captureFormat / createdAt that the dialog doesn't edit).
            juce::DynamicObject::Ptr merged = obj;
            if (merged == nullptr) merged = new juce::DynamicObject();
            merged->setProperty ("zynforgeSession", true);
            merged->setProperty ("name",            edited.name);
            merged->setProperty ("artist",          edited.artist);
            merged->setProperty ("venue",           edited.venue);
            merged->setProperty ("fohEngineer",     edited.fohEngineer);
            merged->setProperty ("date",            edited.date);
            merged->setProperty ("notes",           edited.notes);
            merged->setProperty ("updatedAt",
                                 juce::Time::getCurrentTime().toISO8601 (true));

            proj.replaceWithText (juce::JSON::toString (juce::var (merged.get())));
            showStatus ("Saved session properties -- " + sessionDir.getFileName());
        });
}
