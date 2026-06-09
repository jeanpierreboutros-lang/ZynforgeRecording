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

void MainComponent::generateOrRefreshClickTrack()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before generating a click");
        return;
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

    const auto trackName = juce::String::formatted ("Track_%02d", clickTrackIndex + 1);
    auto dest = audioFiles.getChildFile (trackName + ".wav");
    dest.deleteFile();

    if (auto* out = dest.createOutputStream().release())
    {
        juce::WavAudioFormat wav;
        juce::StringPairArray meta;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (out, sr, 1, 24, meta, 0));
        if (writer == nullptr) { delete out; showStatus ("Click write failed"); return; }

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

    engine.loadSession (sessionDir);
    lastTrackCount = -1;

    showStatus ("Click track generated at "
                + juce::String (engine.getSessionTempoBpm(), 1) + " BPM "
                + "(routable to any output via its strip)");

    // Light up the click-beat overlay on every other EDIT row so the
    // engineer can see the metronome pulse against each track's audio.
    if (editPage != nullptr)
        editPage->setClickTrackPresent (true, clickTrackIndex);
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
        showStatus ("Punch mode OFF");
    }
}

void MainComponent::servicePunch()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded()) return;
    const auto pos    = player.getPositionSamples();
    const auto inside = (pos >= player.getLoopStart() && pos < player.getLoopEnd());

    if (inside && ! wasInsidePunch && player.isPlaying())
    {
        // Crossed into the punch window -- drop into record on every
        // punch-armed track, leave the rest playing back as normal.
        if (! engine.isRecording())
        {
            // Save each strip's pre-punch arm state, then force-arm
            // only the punch-armed ones for the duration of the punch.
            auto& rec = engine.getRecorder();
            for (int i = 0; i < rec.getNumTracks(); ++i)
                rec.getTrack (i).armed.store (engine.isTrackPunchArmed (i),
                                              std::memory_order_relaxed);
            engine.startRecording (makeNewSessionDir());
        }
    }
    else if (! inside && wasInsidePunch && engine.isRecording())
    {
        // Crossed out -- stop recording cleanly.
        engine.stopRecording();
    }
    wasInsidePunch = inside;
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
        showStatus ("No recorded audio to analyse -- record a take first.");
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
            auto q = zynforge::qc::analyzeFile (f);
            const auto stem = f.getFileNameWithoutExtension();          // Track_NN
            const int idx = stem.fromLastOccurrenceOf ("_", false, false).getIntValue() - 1;
            q.trackIndex = juce::jmax (0, idx);
            q.name = (idx >= 0 && idx < names.size() && names[idx].isNotEmpty())
                       ? names[idx] : stem;
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
    juce::Thread::launch ([self, files]
    {
        const zynforge::songs::Params params;        // -40 dB / 4 s gap / 60 s min / quorum 2
        std::vector<std::vector<char>> flags;
        double sr = 48000.0;
        for (const auto& f : files)
        {
            double fileSr = 0.0;
            auto fl = zynforge::songs::envelopeFlags (f, params.thresholdDb,
                                                      params.windowSec, fileSr);
            if (fileSr > 0.0) sr = fileSr;
            if (! fl.empty()) flags.push_back (std::move (fl));
        }
        const auto found = zynforge::songs::detect (flags, sr, params);

        juce::MessageManager::callAsync ([self, found]
        {
            if (self == nullptr) return;
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
