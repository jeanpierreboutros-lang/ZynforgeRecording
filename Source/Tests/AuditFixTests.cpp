// Regression guards for the 2026-08-11 whole-codebase bug hunt.
//
// One test per fix that is reachable headlessly. The UI-thread fixes
// (threaded export / Save-As, the async SMART poll, the pre-flight probe
// guard, punch arm restore) need a MainComponent + message loop and are
// covered by the smoke test instead -- see CHANGELOG for the full list.

#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/TrackExporter.h"
#include "../Audio/ProcessSearch.h"

namespace zynforge
{
    class AuditFixTests final : public juce::UnitTest
    {
    public:
        AuditFixTests() : UnitTest ("Audit fixes 2026-08-11", "zynforge") {}

        static juce::File scratchDir (const juce::String& name)
        {
            auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("zf_auditfix_" + name);
            d.deleteRecursively();
            d.createDirectory();
            return d;
        }

        // Write a mono WAV of `len` samples at `sr` holding a constant value.
        static bool writeWav (const juce::File& f, double sr, juce::int64 len,
                              float value = 0.25f, int channels = 1)
        {
            f.getParentDirectory().createDirectory();
            f.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            if (os == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, (unsigned int) channels, 24, {}, 0));
            if (w == nullptr) return false;
            os.release();

            const int block = 4096;
            juce::AudioBuffer<float> buf (channels, block);
            juce::int64 done = 0;
            while (done < len)
            {
                const int n = (int) juce::jmin ((juce::int64) block, len - done);
                for (int c = 0; c < channels; ++c)
                    juce::FloatVectorOperations::fill (buf.getWritePointer (c), value, n);
                if (! w->writeFromAudioSampleBuffer (buf, 0, n)) return false;
                done += n;
            }
            return true;
        }

        static juce::int64 lengthOf (const juce::File& f)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            return r != nullptr ? r->lengthInSamples : -1;
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            // ── Stereo-mix bounce with MORE audio files than mixer strips ────
            // forEachStereoMixWindow bounded its TrackState loops on
            // jmax(recorder, player), so a session with more Track_NN files on
            // disk than strips (delete a strip after recording) indexed
            // recorder.getTrack() past the end of the vector. This is the crash
            // guard: the bounce must complete without reading OOB.
            beginTest ("Stereo-mix bounce is safe when the player has more tracks than the mixer");
            {
                auto dir = scratchDir ("bounce_oob");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                // Six takes on disk...
                for (int i = 1; i <= 6; ++i)
                    expect (writeWav (audio.getChildFile (
                                juce::String::formatted ("Track_%02d.wav", i)), 48000.0, 24000),
                            "could not write source take");

                AudioEngine eng;
                eng.prepareForTests (48000.0, 512);
                eng.setActiveSessionDir (dir);
                expect (eng.loadSession (dir) >= 6, "player should load all six takes");

                // ...but only TWO strips in the mixer (the shrink the old code
                // walked past). setStripCount refuses to grow the player back.
                eng.setStripCount (2);
                expectEquals (eng.getRecorder().getNumTracks(), 2);
                expect (eng.getPlayer().getNumTracks() > eng.getRecorder().getNumTracks(),
                        "test precondition: player must out-count the recorder");

                const auto out = dir.getChildFile ("Export Files").getChildFile ("mix.wav");
                out.getParentDirectory().createDirectory();
                // Pre-fix this indexed tracks[2..5] on a 2-element vector.
                const bool ok = eng.bounceStereoMixToWav (out, 24000, 48000.0, nullptr);
                expect (ok, "stereo-mix bounce should succeed");
                expectEquals (lengthOf (out), (juce::int64) 24000);

                // A cancelled replacement must not erase the last good file.
                expect (out.replaceWithText ("known-good-bounce"));
                std::atomic<bool> cancelled { true };
                expect (! eng.bounceStereoMixToWav (out, 24000, 48000.0, &cancelled));
                expectEquals (out.loadFileAsString(), juce::String ("known-good-bounce"));
                expectEquals (out.getParentDirectory().findChildFiles (
                                  juce::File::findFiles, false, "*.partial.wav").size(), 0);
                dir.deleteRecursively();
            }

            // ── Clip lists must not be destroyed by a reseed ─────────────────
            // seedDefaultClips did trackClips.resize(player.getNumTracks()),
            // shrinking away the clip list of any strip above the highest
            // recorded Track_NN -- so a clip on a never-recorded strip vanished
            // on every stop-record / reload even with preserveEdits = true.
            beginTest ("seedDefaultClips preserves clips above the last recorded track");
            {
                auto dir = scratchDir ("seed_grow_only");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                expect (writeWav (audio.getChildFile ("Track_01.wav"), 48000.0, 24000), "write");

                AudioEngine eng;
                eng.prepareForTests (48000.0, 512);
                eng.setActiveSessionDir (dir);
                eng.loadSession (dir);
                eng.setStripCount (8);

                // Put a clip on strip 5 -- well past the single recorded take.
                Clip c;
                c.timelineStartSamples = 1000;
                c.fileStartSamples     = 0;
                c.fileLengthSamples    = 5000;
                c.audioFile            = audio.getChildFile ("Track_01.wav");
                eng.clipsFor (5).clear();
                eng.clipsFor (5).push_back (c);
                expectEquals ((int) eng.clipsFor (5).size(), 1);

                eng.seedDefaultClips (/*preserveEdits*/ true);
                expectEquals ((int) eng.clipsFor (5).size(), 1,
                              "the high-index clip must survive a preserving reseed");
                expectEquals (eng.clipsFor (5)[0].timelineStartSamples, (juce::int64) 1000);
                dir.deleteRecursively();
            }

            // ── Disk-rate estimate must count mirror destinations ────────────
            beginTest ("Disk-rate estimate includes mirror destinations");
            {
                auto root = scratchDir ("mirror_rate");
                MultitrackRecorder rec;
                rec.prepare (48000.0, 512, 4);
                for (int i = 0; i < 4; ++i)
                    rec.getTrack (i).armed.store (true, std::memory_order_relaxed);
                rec.setCaptureFormat (CaptureFormat::Wav24);

                const auto plain = rec.estimateBytesPerSecondForArmedTracks();
                expect (plain > 0, "baseline rate should be non-zero");

                std::vector<MultitrackRecorder::MirrorConfig> mirrors;
                MultitrackRecorder::MirrorConfig m;
                m.root   = root.getChildFile ("mirrorA");
                m.root.createDirectory();
                m.format = CaptureFormat::Wav24;
                mirrors.push_back (m);
                rec.setMirrors (mirrors);

                const auto withMirror = rec.estimateBytesPerSecondForArmedTracks();
                expectEquals (withMirror, plain * 2,
                              "one same-format mirror doubles the projected write rate");
                root.deleteRecursively();
            }

            // ── A continue must extend the take in its EXISTING container ────
            // The continue scan only looked for the CURRENT capture format's
            // extension, so changing format between takes wrote a second file
            // (Track_01.flac) alongside Track_01.wav as "part 1".
            beginTest ("Continue-record adopts the existing take's container");
            {
                auto dir = scratchDir ("continue_format");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                const auto take = audio.getChildFile ("Track_01.wav");
                expect (writeWav (take, 48000.0, 4800), "write base take");
                const auto baseLen = lengthOf (take);

                MultitrackRecorder rec;
                rec.prepare (48000.0, 512, 1);
                rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                // Engineer flipped the format to FLAC between takes.
                rec.setCaptureFormat (CaptureFormat::Flac24);
                rec.armContinue (0);
                expect (rec.startRecording (dir), "continue start");

                std::vector<float> silence (512, 0.0f);
                const float* ins[1] = { silence.data() };
                for (int b = 0; b < 8; ++b) rec.processBlock (ins, 1, 512);
                // The timeline base must have been read off the EXISTING take.
                expectEquals (rec.getRecordBaseSamples(), baseLen,
                              "continue base should come from the existing .wav take");
                rec.stopRecording();

                expect (! audio.getChildFile ("Track_01.flac").existsAsFile(),
                        "a continue must NOT fork the take into a second container");
                expect (audio.getChildFile ("Track_01_part02.wav").existsAsFile(),
                        "the continuation should be a .wav part of the existing take");
                dir.deleteRecursively();
            }

            beginTest ("Fresh recording refuses every existing Track container and preserves it");
            {
                auto dir = scratchDir ("fresh_collision");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                const auto take = audio.getChildFile ("Track_01.flac");
                {
                    juce::FlacAudioFormat flac;
                    std::unique_ptr<juce::FileOutputStream> os (take.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> writer (
                        flac.createWriterFor (os.get(), 48000.0, 1, 24, {}, 5));
                    expect (writer != nullptr);
                    if (writer != nullptr)
                    {
                        os.release();
                        juce::AudioBuffer<float> b (1, 1024);
                        juce::FloatVectorOperations::fill (b.getWritePointer (0), 0.35f, 1024);
                        expect (writer->writeFromAudioSampleBuffer (b, 0, 1024));
                    }
                }
                const auto bytesBefore = take.getSize();

                MultitrackRecorder rec;
                rec.prepare (48000.0, 256, 1);
                rec.getTrack (0).armed.store (true);
                rec.setCaptureFormat (CaptureFormat::Wav24); // different extension must still collide
                expect (! rec.startRecording (dir), "fresh start must not create a rival Track_01.wav");
                expectEquals (take.getSize(), bytesBefore);
                expect (! audio.getChildFile ("Track_01.wav").existsAsFile());

                rec.armContinue (0);
                expect (rec.startRecording (dir), "explicit continuation should remain allowed");
                std::vector<float> samples (256, 0.2f);
                const float* input[] = { samples.data() };
                rec.processBlock (input, 1, 256);
                rec.stopRecording();
                expect (audio.getChildFile ("Track_01_part02.flac").existsAsFile());
                expectEquals (take.getSize(), bytesBefore, "continuation must not rewrite the base take");
                dir.deleteRecursively();
            }

            beginTest ("Unavailable backup fails loud while primary capture remains valid");
            {
                auto root = scratchDir ("backup_open_failure");
                const auto session = root.getChildFile ("Take");
                const auto badBackup = root.getChildFile ("not-a-folder");
                expect (badBackup.replaceWithText ("occupied by a file"));

                MultitrackRecorder rec;
                rec.prepare (48000.0, 256, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (badBackup);
                expect (rec.startRecording (session), "primary should still start");
                expect (rec.hasBackupFailed(), "backup open failure must be latched");
                expect (! rec.isBackupActive(), "a failed backup must not be shown as active");
                std::vector<float> samples (256, 0.2f);
                const float* input[] = { samples.data() };
                rec.processBlock (input, 1, 256);
                rec.stopRecording();
                expect (session.getChildFile ("Audio Files/Track_01.wav").existsAsFile());
                root.deleteRecursively();
            }

            beginTest ("Backup destination persists across engine restart");
            {
                auto chosen = scratchDir ("persisted_backup");
                juce::File previous;
                {
                    AudioEngine eng;
                    eng.prepareForTests (48000.0, 256);
                    previous = eng.getRecorder().getBackupDirectory();
                    eng.setBackupDirectory (chosen);
                    expect (eng.getRecorder().getBackupDirectory() == chosen);
                }
                {
                    AudioEngine restarted;
                    restarted.prepareForTests (48000.0, 256);
                    expect (restarted.getRecorder().getBackupDirectory() == chosen,
                            "backup folder was forgotten after restart");
                    restarted.setBackupDirectory (previous);
                }
                chosen.deleteRecursively();
            }

            beginTest ("Strip Silence analyzes the edited arrangement and commits all-silent emptiness");
            {
                auto dir = scratchDir ("strip_arrangement");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                const auto own = audio.getChildFile ("Track_01.wav");
                const auto cross = audio.getChildFile ("cross.wav");
                expect (writeWav (own, 48000.0, 4800, 0.5f));
                expect (writeWav (cross, 48000.0, 4800, 0.7f));

                AudioEngine eng;
                eng.prepareForTests (48000.0, 256);
                eng.setActiveSessionDir (dir);
                expect (eng.loadSession (dir) > 0);
                Clip first;
                first.timelineStartSamples = 0;
                first.fileLengthSamples = 4800;
                first.gainDb = -3.0f;
                Clip second;
                second.audioFile = cross;
                second.sourceChannel = 0;
                second.timelineStartSamples = 9600;
                second.fileLengthSamples = 4800;
                second.gainDb = -6.0f;
                eng.clipsFor (0) = { first, second };
                eng.getPlayer().setTrackClips (0, eng.clipsFor (0));
                eng.syncActiveTake (0);

                expectEquals (eng.stripSilence (0, -40.0f, 1000, 100, 0), 2);
                expectEquals ((int) eng.clipsFor (0).size(), 2);
                expect (eng.clipsFor (0)[1].audioFile == cross,
                        "cross-track source must survive Strip Silence");
                expectEquals (eng.clipsFor (0)[1].sourceChannel, 0);
                expectWithinAbsoluteError (eng.clipsFor (0)[1].gainDb, -6.0f, 0.001f);

                auto silentDir = scratchDir ("strip_all_silent");
                const auto silentAudio = silentDir.getChildFile ("Audio Files");
                silentAudio.createDirectory();
                expect (writeWav (silentAudio.getChildFile ("Track_01.wav"),
                                  48000.0, 4800, 0.0f));
                AudioEngine silent;
                silent.prepareForTests (48000.0, 256);
                silent.setActiveSessionDir (silentDir);
                silent.loadSession (silentDir);
                expectEquals (silent.stripSilence (0, -40.0f, 100, 100, 0), 0);
                expect (silent.isTrackArrangementEmpty (0),
                        "an all-silent result must be an explicit empty arrangement");
                silentDir.deleteRecursively();
                dir.deleteRecursively();
            }

            beginTest ("External capture blocks local playback starts");
            {
                auto dir = scratchDir ("external_record_playback_guard");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                expect (writeWav (audio.getChildFile ("Track_01.wav"), 48000.0, 4800));
                AudioEngine eng;
                eng.prepareForTests (48000.0, 256);
                eng.setActiveSessionDir (dir);
                eng.loadSession (dir);
                eng.setExternalRecording (true);
                eng.startPlayback();
                expect (! eng.isPlaying());
                eng.setExternalRecording (false);
                eng.startPlayback();
                expect (eng.isPlaying());
                eng.stopPlayback();
                dir.deleteRecursively();
            }

            beginTest ("Consolidation numbering never overwrites slot 999");
            {
                auto dir = scratchDir ("consolidate_1000");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                expect (writeWav (audio.getChildFile ("Track_01.wav"), 48000.0, 4800, 0.4f));
                AudioEngine eng;
                eng.prepareForTests (48000.0, 256);
                eng.setActiveSessionDir (dir);
                eng.loadSession (dir);

                for (int i = 1; i <= 999; ++i)
                    expect (audio.getChildFile ("Track_01_consolidated_" + juce::String (i) + ".wav")
                                  .replaceWithText ("occupied"));
                const auto sentinel = audio.getChildFile ("Track_01_consolidated_999.wav");
                expect (eng.consolidateRange (0, 0, 100));
                expectEquals (sentinel.loadFileAsString(), juce::String ("occupied"));
                expect (audio.getChildFile ("Track_01_consolidated_1000.wav").existsAsFile());
                dir.deleteRecursively();
            }

            // ── takeIsMultiPart must ignore .punchbase sidecars ──────────────
            beginTest ("takeIsMultiPart ignores punchbase sidecars");
            {
                auto dir = scratchDir ("multipart_punchbase");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                expect (writeWav (audio.getChildFile ("Track_01.wav"), 48000.0, 4800), "write");
                expect (! MultitrackRecorder::takeIsMultiPart (dir, 0),
                        "a single-file take is not multi-part");

                // A leftover sidecar from an aborted punch must not count.
                expect (writeWav (audio.getChildFile ("Track_01_part02.punchbase.wav"),
                                  48000.0, 1200), "write sidecar");
                expect (! MultitrackRecorder::takeIsMultiPart (dir, 0),
                        "a .punchbase sidecar must not read as a continuation part");

                // A real part still does.
                expect (writeWav (audio.getChildFile ("Track_01_part02.wav"), 48000.0, 1200), "write");
                expect (MultitrackRecorder::takeIsMultiPart (dir, 0),
                        "a real _part file must still be detected");
                dir.deleteRecursively();
            }

            // ── FLAC export at 32-bit must clamp, not fail ───────────────────
            beginTest ("Track exporter constructs cleanly and FLAC clamps an unsupported 32-bit request");
            {
                auto dir = scratchDir ("flac_bits");
                const auto src = dir.getChildFile ("src.wav");
                expect (writeWav (src, 48000.0, 4800), "write source");

                // Construction itself is part of this regression: it used to
                // register FLAC twice, aborting Debug builds in JUCE.
                TrackExporter ex;
                ExportOptions opts;
                opts.format        = ExportFormat::Flac24;
                opts.sampleRate    = 48000.0;
                opts.bitsPerSample = 32;          // FLAC can't do this
                juce::String err;
                const auto stem = dir.getChildFile ("out");
                expect (ex.exportTrack (src, stem, opts, err),
                        "32-bit FLAC request should clamp to 24 and succeed, not fail: " + err);
                expect (stem.withFileExtension (".flac").existsAsFile(), "flac not written");
                dir.deleteRecursively();
            }

            beginTest ("Executable lookup searches PATH directly without a child process");
            {
                auto dir = scratchDir ("path_lookup");
                const auto first = dir.getChildFile ("missing");
                const auto second = dir.getChildFile ("folder with spaces");
                expect (first.createDirectory().wasOk());
                expect (second.createDirectory().wasOk());
                const auto fakeLame = second.getChildFile ("lame");
                expect (fakeLame.replaceWithText ("test executable"));

                expect (processsearch::findExecutableInPath (
                            "lame", first.getFullPathName() + ":" + second.getFullPathName())
                        == fakeLame);
                expect (processsearch::findExecutableInPath ("missing", second.getFullPathName())
                        == juce::File());
                expect (processsearch::findExecutableInPath ({}, second.getFullPathName())
                        == juce::File());
                dir.deleteRecursively();
            }

            // ── Same-rate export is a straight copy (no resampler) ───────────
            beginTest ("Same-rate export preserves length exactly");
            {
                auto dir = scratchDir ("samerate_export");
                const auto src = dir.getChildFile ("src.wav");
                expect (writeWav (src, 48000.0, 12345, 0.5f), "write source");

                TrackExporter ex;
                ExportOptions opts;
                opts.format        = ExportFormat::Wav24;
                opts.sampleRate    = 48000.0;      // identical -> copy path
                opts.bitsPerSample = 24;
                juce::String err;
                const auto stem = dir.getChildFile ("out");
                expect (ex.exportTrack (src, stem, opts, err), "export failed: " + err);
                expectEquals (lengthOf (stem.withFileExtension (".wav")), (juce::int64) 12345,
                              "a same-rate export must be sample-for-sample the same length");
                dir.deleteRecursively();
            }

            beginTest ("Failed export preserves an existing deliverable and removes partial output");
            {
                auto dir = scratchDir ("transactional_export");
                const auto src = dir.getChildFile ("src.wav");
                expect (writeWav (src, 48000.0, 4800), "write source");
                const auto stem = dir.getChildFile ("out");
                const auto existing = stem.withFileExtension (".wav");
                expect (existing.replaceWithText ("known-good-export"));

                TrackExporter ex;
                ExportOptions opts;
                opts.format = static_cast<ExportFormat> (999); // invalid persisted/input value
                opts.sampleRate = 48000.0; // output opens, then writer creation fails
                juce::String err;
                expect (! ex.exportTrack (src, stem, opts, err));
                expectEquals (existing.loadFileAsString(), juce::String ("known-good-export"));
                expectEquals (dir.findChildFiles (juce::File::findFiles, false, "*.partial.*").size(), 0);
                dir.deleteRecursively();
            }

            beginTest ("Recovery-marker and integrity-report write failures are surfaced");
            {
                auto dir = scratchDir ("metadata_failure");
                expect (dir.getChildFile ("Audio Files").createDirectory().wasOk());
                expect (dir.getChildFile ("recording.session").createDirectory().wasOk());
                expect (dir.getChildFile ("session.report.json").createDirectory().wasOk());
                MultitrackRecorder recorder;
                recorder.prepare (48000.0, 256, 1);
                recorder.getTrack (0).armed.store (true);
                expect (recorder.startRecording (dir));
                expect (recorder.hasRecoveryMarkerFailed());
                recorder.stopRecording();
                expect (recorder.hasReportWriteFailed());
                dir.deleteRecursively();
            }

            beginTest ("Optional stereo-mix open failure is surfaced while multitracks continue");
            {
                auto dir = scratchDir ("stereo_mix_failure");
                expect (dir.getChildFile ("Audio Files").createDirectory().wasOk());
                expect (dir.getChildFile ("Export Files").replaceWithText ("not-a-directory"));
                AudioEngine eng;
                eng.prepareForTests (48000.0, 256);
                eng.setStripCount (1);
                eng.getRecorder().getTrack (0).armed.store (true);
                eng.setRecordStereoMix (true);
                expect (eng.startRecording (dir));
                expect (eng.hasStereoMixWriteFailed());
                eng.stopRecording();
                eng.setRecordStereoMix (false);
                dir.deleteRecursively();
            }

            // ── Deleting a strip must shift the appProps per-index keys ──────
            // strip_stereo_N / strip_uid_N weren't shifted, so after a
            // mid-list delete the stereo flags and stable UUIDs re-attached to
            // the wrong strips on the next applyPersistedStripState().
            beginTest ("removeStripAt shifts stereo flags and strip UUIDs");
            {
                AudioEngine eng;
                eng.prepareForTests (48000.0, 512);
                eng.setStripCount (4);
                auto* props = eng.getAppProps();
                expect (props != nullptr, "no appProps");

                // Give strips distinguishable identities. These MUST be flushed
                // to disk: removeStripAt calls reloadAppPropsBeforeWrite(),
                // which is a clear()+reload() (the shared-.settings
                // reload-to-REPLACE rule), so unsaved in-memory values would be
                // thrown away before the shift runs.
                for (int i = 0; i < 4; ++i)
                    props->setValue ("strip_uid_" + juce::String (i), "uid-" + juce::String (i));
                props->saveIfNeeded();
                eng.setTrackStereo (2, true);      // writes strip_stereo_2
                expect (props->getBoolValue ("strip_stereo_2", false), "precondition");

                eng.removeStripAt (1);             // strip 2 becomes strip 1

                expectEquals (props->getValue ("strip_uid_1", {}), juce::String ("uid-2"),
                              "the UUID must follow its strip down one slot");
                expectEquals (props->getValue ("strip_uid_2", {}), juce::String ("uid-3"));
                expect (props->getBoolValue ("strip_stereo_1", false),
                        "the stereo flag must follow its strip down one slot");
                expect (! props->getBoolValue ("strip_stereo_2", false),
                        "the vacated slot must not keep the old flag");
                expect (props->getValue ("strip_uid_3", {}).isEmpty(),
                        "the orphan tail slot must be cleared");
            }

            // ── Transient cache must see non-WAV takes ───────────────────────
            beginTest ("Transient scan covers FLAC/AIFF sessions");
            {
                auto dir = scratchDir ("transient_flac");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();

                // A FLAC take with a hard transient partway through.
                const auto flacFile = audio.getChildFile ("Track_01.flac");
                {
                    juce::FlacAudioFormat flac;
                    std::unique_ptr<juce::FileOutputStream> os (flacFile.createOutputStream());
                    expect (os != nullptr, "flac stream");
                    std::unique_ptr<juce::AudioFormatWriter> w (
                        flac.createWriterFor (os.get(), 48000.0, 1, 24, {}, 5));
                    expect (w != nullptr, "flac writer");
                    os.release();
                    juce::AudioBuffer<float> buf (1, 48000);
                    buf.clear();
                    // Silence, then a loud sustained burst = one clear onset.
                    for (int i = 24000; i < 48000; ++i)
                        buf.setSample (0, i, 0.8f * std::sin (2.0f * 3.14159f * 220.0f
                                                              * (float) i / 48000.0f));
                    expect (w->writeFromAudioSampleBuffer (buf, 0, 48000), "flac write");
                }

                AudioEngine eng;
                eng.prepareForTests (48000.0, 512);
                eng.setActiveSessionDir (dir);
                eng.invalidateTransientCache();
                // Pre-fix the .wav-only glob found nothing here, so Tab was dead.
                const auto next = eng.nextTransientSample (0, 1);
                expect (next > 0, "a FLAC take must produce transients (was WAV-only)");
                dir.deleteRecursively();
            }
        }
    };

    static AuditFixTests auditFixTests;
}
