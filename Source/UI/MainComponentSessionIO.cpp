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
#include "../Theme/DialogChrome.h"
#include "NewSessionDialog.h"
#include "ExportDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

bool MainComponent::saveSessionStateTo (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;

    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("captureFormat",  (int) recorder.getCaptureFormat());
    root->setProperty ("preRollSeconds", recorder.getPreRollSeconds());

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
    const bool wroteSettings = dir.getChildFile ("session_settings.json").replaceWithText (json);

    // Persist the FULL per-strip mixer state WITH the session (name, colour,
    // gain, pan, mute, solo, monitor, arm, routing, stereo, VCA + edit group)
    // so it travels per-show instead of leaking between sessions via global
    // appProps.
    engine.saveSessionMixTo (dir);

    // Persist cues + comp playlists (Takes) + automation lanes into the
    // .zfproj. These were only auto-saved on cue edits before, so drawing
    // automation and hitting Save (without touching a cue) used to lose it.
    saveSetlistToActiveSession();

    // Also persist the UI layout into the session's .zfproj so reopening
    // the show brings back the engineer's view choice, strip width,
    // VCA-panel visibility, and EDIT zoom.
    saveUILayoutToActiveSession();
    return wroteSettings;
}

void MainComponent::saveUILayoutToActiveSession()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) return;

    const auto proj = findSessionProj (dir);
    if (proj == juce::File{}) return;

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
    proj.replaceWithText (juce::JSON::toString (juce::var (obj.get())));
}

int MainComponent::exportTracksTo (const juce::File& destDir,
                                   const std::vector<int>& channelIndices,
                                   const zynforge::ExportOptions& opts)
{
    auto sourceDir = engine.getActiveSessionDir();
    if (! sourceDir.isDirectory() || ! destDir.isDirectory()) return 0;

    destDir.createDirectory();

    auto allFiles = sourceDir.findChildFiles (juce::File::findFiles, false, "Track_*");

    auto matchesIndex = [] (const juce::File& f, int index1Based) -> bool
    {
        const auto base = f.getFileNameWithoutExtension();
        const auto suffix = juce::String::formatted ("Track_%02d", index1Based);
        return base == suffix;
    };

    zynforge::TrackExporter exporter;
    int succeeded = 0;
    juce::String firstError;

    for (int i : channelIndices)
    {
        for (auto& src : allFiles)
        {
            if (! matchesIndex (src, i + 1)) continue;

            auto& trackState = engine.getRecorder().getTrack (i);
            const auto safeName = trackState.name.replaceCharacter ('/', '_')
                                                  .replaceCharacter ('\\', '_');
            const auto baseName = juce::String::formatted ("Track_%02d - ", i + 1) + safeName;
            const auto destStem = destDir.getChildFile (baseName);

            juce::String err;
            if (exporter.exportTrack (src, destStem, opts, err))
                ++succeeded;
            else if (firstError.isEmpty())
                firstError = err;
            break;
        }
    }

    if (succeeded == 0 && firstError.isNotEmpty())
        showStatus ("Export failed: " + firstError);

    return succeeded;
}

void MainComponent::onSaveSessionState()
{
    const auto dir = engine.getActiveSessionDir();
    if (dir.isDirectory())
    {
        if (saveSessionStateTo (dir))
            showStatus ("Saved session state → " + dir.getFileName());
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
    if (engine.isRecording()) { showStatus ("Stop recording before importing"); return; }

    // Accept anything the JUCE basic format manager + FLAC can read.
    // (WavAudioFormat, AiffAudioFormat, FlacAudioFormat, OggVorbisAudioFormat,
    //  MP3AudioFormat -- read-only -- when JUCE_USE_MP3AUDIOFORMAT is on.)
    const juce::String filters = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.caf";

    chooser = std::make_unique<juce::FileChooser> (
        "Pick audio files to import as a new session",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        filters);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectMultipleItems,
        [this] (const juce::FileChooser& fc)
    {
        const auto picks = fc.getResults();
        if (picks.isEmpty()) return;

        // Convert each picked file into one or two Track_NN.wav files
        // (mono per track) inside the active session's Audio Files/ dir.
        // Stereo source files become a stereo PAIR -- two consecutive
        // mono WAVs whose L track gets isStereo=true so the UI collapses
        // them into one strip. The session is then loaded for VSC playback.
        auto sessionDir = makeNewSessionDir();
        sessionDir.createDirectory();

        // Pro Tools-style: imported audio lives under Audio Files/.
        // makeNewSessionDir() either returns the engineer-named session
        // (from appProps) or freshly auto-stamps one -- either way we
        // want Track files inside the subfolder, not loose at the root.
        auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
        audioFilesDir.createDirectory();
        // Also seed the rest of the Pro Tools-style layout so loose
        // imports look like a real session if the engineer hadn't
        // already created one via File ▸ New Session....
        sessionDir.getChildFile ("Export Files")       .createDirectory();
        sessionDir.getChildFile ("Session File Backups").createDirectory();
        engine.setActiveSessionDir (sessionDir);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        // Per imported file: remember (start_strip_index, was_stereo).
        struct ImportRecord { int trackIndex; bool stereo; juce::String name; };
        std::vector<ImportRecord> records;
        // Start *after* the existing strips so multi-file import APPENDS
        // to the session instead of overwriting Track_01.wav onwards.
        int nextTrack = engine.getRecorder().getNumTracks();
        int failed    = 0;

        auto writeMono = [] (juce::AudioFormatReader& reader, int channelIndex,
                             const juce::File& dst, double sr) -> bool
        {
            dst.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> out (dst.createOutputStream());
            if (out == nullptr) return false;
            juce::StringPairArray meta;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (out.get(), sr, 1, 24, meta, 0));
            if (writer == nullptr) return false;
            out.release();

            // Stream samples in chunks so we don't allocate a huge buffer.
            constexpr int chunk = 16384;
            juce::AudioBuffer<float> buf ((int) reader.numChannels, chunk);
            juce::int64 pos = 0;
            while (pos < reader.lengthInSamples)
            {
                const int n = (int) juce::jmin ((juce::int64) chunk,
                                                reader.lengthInSamples - pos);
                if (! reader.read (&buf, 0, n, pos, true, true)) return false;
                const float* mono[1] = { buf.getReadPointer (channelIndex) };
                if (! writer->writeFromFloatArrays (mono, 1, n)) return false;
                pos += n;
            }
            return true;
        };

        for (int i = 0; i < picks.size(); ++i)
        {
            const auto& src = picks.getReference (i);
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src));
            if (reader == nullptr) { ++failed; continue; }

            const bool isStereoFile = (reader->numChannels >= 2);
            const auto baseName = src.getFileNameWithoutExtension();

            const int lTrack = nextTrack;
            const auto lDst = audioFilesDir.getChildFile (
                "Track_" + juce::String (lTrack + 1).paddedLeft ('0', 2) + ".wav");
            const bool lOk = writeMono (*reader, 0, lDst, reader->sampleRate);

            bool rOk = true;
            if (isStereoFile)
            {
                const int rTrack = nextTrack + 1;
                const auto rDst = audioFilesDir.getChildFile (
                    "Track_" + juce::String (rTrack + 1).paddedLeft ('0', 2) + ".wav");
                rOk = writeMono (*reader, 1, rDst, reader->sampleRate);
            }

            if (! lOk || ! rOk) { ++failed; continue; }

            records.push_back ({ lTrack, isStereoFile, baseName });
            nextTrack += isStereoFile ? 2 : 1;
        }

        if (records.empty())
        {
            showStatus ("Import failed -- no readable audio files");
            return;
        }

        // Resize the mixer to fit every imported strip.
        if (nextTrack > engine.getRecorder().getNumTracks())
            engine.setStripCount (nextTrack);

        // Apply stereo pair flags + names + routing to each imported strip.
        for (auto& rec : records)
        {
            engine.setTrackName    (rec.trackIndex, rec.name);
            engine.setTrackStereo  (rec.trackIndex, rec.stereo);
            if (rec.stereo)
            {
                engine.setTrackName  (rec.trackIndex + 1, rec.name + " R");
                engine.setTrackStereo (rec.trackIndex + 1, false);

                // Route the stereo pair to a sensible pair of inputs +
                // outputs so the strip's combos read 'In 1-2' / 'Out 1-2'
                // rather than (unrouted). L gets the even slot, R gets
                // the next one up.
                engine.setTrackLinkedRouting (rec.trackIndex,     rec.trackIndex);
                engine.setTrackLinkedRouting (rec.trackIndex + 1, rec.trackIndex + 1);
            }
            else
            {
                engine.setTrackLinkedRouting (rec.trackIndex, rec.trackIndex);
            }
        }

        // setTrackStereo doesn't change the track count, so the mixer
        // wouldn't otherwise rebuild -- force the next timer tick to
        // re-iterate logical strips (collapse stereo pairs into one).
        lastTrackCount = -1;

        const int loaded = engine.loadSession (sessionDir);
        const int stereoCount = (int) std::count_if (records.begin(), records.end(),
                                                     [] (const ImportRecord& r) { return r.stereo; });
        showStatus ("Imported " + juce::String ((int) records.size())
                    + " file(s), " + juce::String (stereoCount) + " stereo, "
                    + juce::String ((int) records.size() - stereoCount) + " mono"
                    + (failed > 0 ? " (skipped " + juce::String (failed) + ")"
                                  : juce::String())
                    + " -- loaded " + juce::String (loaded) + " for playback");
    });
}

void MainComponent::onSaveSessionAs()
{
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

    chooser->launchAsync (flags, [this, source] (const juce::FileChooser& fc)
    {
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) return;

        if (! dest.exists()) dest.createDirectory();
        if (! dest.isDirectory()) { showStatus ("Save As destination invalid"); return; }

        // Seed the Pro Tools-style subfolder layout in the new spot.
        dest.getChildFile ("Audio Files")         .createDirectory();
        dest.getChildFile ("Export Files")       .createDirectory();
        dest.getChildFile ("Session File Backups").createDirectory();

        if (source.isDirectory() && source != dest)
        {
            // Copy every file inside the source session dir (audio + markers + settings).
            if (! source.copyDirectoryTo (dest))
            {
                showStatus ("Save As failed");
                return;
            }
        }

        // Pin the new folder as the active session FIRST so the parts of the
        // save that target getActiveSessionDir() (setlist / playlists /
        // automation / UI layout) land in the new location, then write
        // everything (settings + full mix state + .zfproj).
        engine.setActiveSessionDir (dest);
        saveSessionStateTo (dest);
        showStatus ("Saved As → " + dest.getFileName());
    });
}

void MainComponent::onExportAllTracks()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    zynforge::ExportDialog::launch ("Export all tracks",
        [this] (std::optional<zynforge::ExportOptions> opts)
    {
        if (! opts.has_value()) return;
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
            [this, chosenOpts] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            std::vector<int> all;
            for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i) all.push_back (i);

            showStatus ("Exporting " + juce::String ((int) all.size()) + " tracks...");
            const int n = exportTracksTo (dest, all, chosenOpts);
            showStatus ("Exported " + juce::String (n) + " tracks → " + dest.getFileName());
        });
    });
}

void MainComponent::onExportIndividualTrack (int channelIndex)
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    zynforge::ExportDialog::launch ("Export track",
        [this, channelIndex] (std::optional<zynforge::ExportOptions> opts)
    {
        if (! opts.has_value()) return;
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
            [this, channelIndex, chosenOpts] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            showStatus ("Exporting track " + juce::String (channelIndex + 1) + "...");
            const int n = exportTracksTo (dest, { channelIndex }, chosenOpts);
            showStatus (n > 0
                        ? "Exported track " + juce::String (channelIndex + 1)
                           + " → " + dest.getFileName()
                        : "Export failed");
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

void MainComponent::onLoadSessionClicked()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose a session folder",
        getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (! dir.isDirectory()) return;

        engine.stopPlayback();
        engine.setActiveSessionDir (dir);   // pin so Save / Save As stay lit
        loadSetlistFromActiveSession();
        loadUILayoutFromActiveSession();
        const int n = engine.loadSession (dir);
        // Restore this session's own full mixer state (names, colours, gains,
        // pans, mutes/solos/arm/monitor, routing, stereo, VCA + edit groups)
        // and size the mixer to match, AFTER the audio loads. Forces a strip
        // rebuild on the next tick so the restored state + badges show.
        engine.loadSessionMixFrom (dir);
        lastTrackCount = -1;
        if (n > 0)
        {
            statusLabel.setText ("Loaded " + juce::String (n) + " tracks", juce::dontSendNotification);
            warnIfSampleRateMismatch();
        }
        else
        {
            statusLabel.setText ("No Track_*.wav found in folder", juce::dontSendNotification);
        }
        playButton.setButtonText ("PLAY");
        updateTransportLabels();
    });
}

juce::File MainComponent::getSessionsRoot() const
{
    // Engineer can override via New Session... dialog ("Local Storage:")
    // -- stored in appProps as 'sessionsRoot'. Falls back to the canonical
    // ~/Music/Zynforge Sessions when unset.
    if (auto* props = engine.getAppProps())
    {
        const auto override_ = props->getValue ("sessionsRoot", {});
        if (override_.isNotEmpty())
        {
            juce::File f (override_);
            if (f.isDirectory() || f.createDirectory().wasOk())
                return f;
        }
    }
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile ("Zynforge Sessions");
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
        out.replaceWithText (juce::JSON::toString (juce::var (obj.get())));
        self->showStatus ("Template saved → " + out.getFileName());
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
    return getSessionsRoot().getChildFile ("Session_" + stamp);
}

juce::File MainComponent::createSessionFolderStructure (const zynforge::NewSessionDialog::Result& r)
{
    // Fresh session => wipe any per-strip persistence left behind by
    // the previous run (gains, pans, colours, names, routing, stereo
    // flags, VCA assignments). Without this, a hard-pan from last
    // weekend's show silently ports over to a new band.
    engine.clearAllStripOverrides();

    // Resolve a safe folder name even if the engineer typed something
    // with slashes / colons in the picker.
    auto safeName = juce::File::createLegalFileName (r.name);
    if (safeName.isEmpty()) safeName = "Untitled-1";

    // Ensure the chosen Local Storage exists, then make the session
    // folder underneath it. If that name already exists, append a
    // numeric suffix so we don't trample on an existing session.
    r.location.createDirectory();

    juce::File sessionFolder = r.location.getChildFile (safeName);
    for (int i = 2; sessionFolder.exists() && i < 9999; ++i)
        sessionFolder = r.location.getChildFile (safeName + "-" + juce::String (i));
    sessionFolder.createDirectory();

    // Subfolders -- only the ones actually wired today. Clip Groups
    // and Video Files were Pro Tools-style placeholders that nothing
    // read or wrote, so they're dropped to avoid confusing the
    // engineer with empty folders.
    sessionFolder.getChildFile ("Audio Files")         .createDirectory();
    sessionFolder.getChildFile ("Export Files")       .createDirectory();
    sessionFolder.getChildFile ("Session File Backups").createDirectory();

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
        sessionFolder.getChildFile (safeName + ".zfproj")
                     .replaceWithText (juce::JSON::toString (juce::var (m.get())));
    }

    // WaveCache.wfm is written on demand by EditPage::saveCacheToSession
    // (on close) and read back on session open. No placeholder needed.

    return sessionFolder;
}
