// Integration test for punch-in RECORDING through the real recorder: record a
// base take, then armPunchIn + record a fresh take into the SAME session, and
// confirm the on-disk Track_NN.wav is base[0,punchIn) + new + base[after],
// sample-accurate, with the session report's length + SHA describing the
// SPLICED file (not the pre-splice fresh take). Proves the stash-on-start /
// splice-on-stop wiring, not just the splice primitive.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/MultitrackRecorder.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/FastHash.h"
#include "../UI/RecordPosition.h"

namespace zynforge
{
    class PunchRecordTests final : public juce::UnitTest
    {
    public:
        PunchRecordTests() : juce::UnitTest ("Punch record", "zynforge") {}

        // Record `numBlocks` blocks of constant DC `value` on a 1-track armed
        // recorder into `dir`. punchInSample >= 0 arms a punch-in first.
        static void recordDc (const juce::File& dir, float value, int numBlocks,
                              int block, double sr, juce::int64 punchInSample)
        {
            MultitrackRecorder rec;
            rec.prepare (sr, block, 1);
            rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
            if (punchInSample >= 0) rec.armPunchIn (punchInSample);
            rec.startRecording (dir);

            std::vector<float> buf ((size_t) block, value);
            const float* ptr = buf.data();
            for (int b = 0; b < numBlocks; ++b)
                rec.processBlock (&ptr, 1, block);
            rec.stopRecording();
        }

        static float regionMean (juce::AudioFormatManager& fm, const juce::File& f,
                                 juce::int64 start, juce::int64 len)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr || len <= 0) return 999.0f;
            len = juce::jmin (len, r->lengthInSamples - start);
            juce::AudioBuffer<float> buf (1, (int) juce::jmax ((juce::int64) 1, len));
            r->read (&buf, 0, (int) len, start, true, false);
            double sum = 0.0;
            for (int i = 0; i < (int) len; ++i) sum += buf.getSample (0, i);
            return (float) (sum / (double) len);
        }

        void runTest() override
        {
            beginTest ("rolling punch uses live playhead, not stale edit cursor");
            expectEquals (manualRecordPosition (true, 24000, 0), (juce::int64) 24000);
            expectEquals (manualRecordPosition (true, 24000, 12000), (juce::int64) 24000);
            expectEquals (manualRecordPosition (false, 24000, 12000), (juce::int64) 12000);
            expectEquals (manualRecordPosition (false, 24000, -1), (juce::int64) 24000);

            const double sr = 48000.0;
            const int    block = 512;
            juce::AudioFormatManager fm; fm.registerBasicFormats();

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-punchrec-" + juce::Uuid().toString());
            dir.deleteRecursively();

            const int baseBlocks  = 90;                       // base take
            const int baseLen     = baseBlocks * block;        // 46080
            const juce::int64 punchIn = 20 * block;            // 10240
            const int punchBlocks = 15;                        // punch take
            const int punchLen    = punchBlocks * block;       // 7680
            const float baseVal  =  0.50f;
            const float punchVal = -0.30f;

            auto track = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");

            beginTest ("base take then punch-in: on-disk file is before+new+after");
            {
                recordDc (dir, baseVal, baseBlocks, block, sr, /*punchIn*/ -1);
                expect (track.existsAsFile(), "base take not written");

                recordDc (dir, punchVal, punchBlocks, block, sr, punchIn);
                expect (track.existsAsFile(), "punched take missing");

                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (track));
                expect (r != nullptr);
                // punchIn + punchLen (17920) < baseLen -> length stays baseLen.
                expectEquals ((int) r->lengthInSamples, baseLen, "spliced length wrong");

                // Three regions, DC means are immune to a small capture offset.
                expectWithinAbsoluteError (regionMean (fm, track, 0,              punchIn - 64),  baseVal,  0.02f);
                expectWithinAbsoluteError (regionMean (fm, track, punchIn + 64,   punchLen - 128),punchVal, 0.02f);
                expectWithinAbsoluteError (regionMean (fm, track, punchIn + punchLen + 64,
                                                       baseLen - (punchIn + punchLen) - 64),      baseVal,  0.02f);

                // No sidecar / temp left behind.
                expect (! track.getSiblingFile ("Track_01.punchbase.wav").existsAsFile(),
                        "punch sidecar leaked");
                expect (! track.getSiblingFile ("Track_01.punchtmp.wav").existsAsFile(),
                        "punch temp leaked");
            }

            beginTest ("punch keeps the original file when capture format changed");
            {
                const auto changedDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-format-" + juce::Uuid().toString());
                const auto original = changedDir.getChildFile ("Audio Files/Track_01.wav");
                recordDc (changedDir, baseVal, baseBlocks, block, sr, -1);

                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.setCaptureFormat (CaptureFormat::Flac16);
                rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                rec.armPunchIn (punchIn);
                expect (rec.startRecording (changedDir), "format-changed punch did not start");
                std::vector<float> samples ((size_t) block, punchVal);
                const float* data = samples.data();
                for (int b = 0; b < punchBlocks; ++b)
                    rec.processBlock (&data, 1, block);
                rec.stopRecording();

                expect (original.existsAsFile(), "original WAV went missing");
                expect (! changedDir.getChildFile ("Audio Files/Track_01.flac").existsAsFile(),
                        "punch created a second base file instead of splicing the WAV");
                expectWithinAbsoluteError (regionMean (fm, original, punchIn + 64,
                                                       punchLen - 128), punchVal, 0.02f);
                changedDir.deleteRecursively();
            }

            beginTest ("format-changed punch keeps primary, backup and mirror in their original files");
            {
                const auto id = juce::Uuid().toString();
                const auto copyDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-copies-" + id);
                const auto backupRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-backup-" + id);
                const auto mirrorRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-mirror-" + id);
                backupRoot.createDirectory();
                mirrorRoot.createDirectory();

                auto capture = [&] (bool punch)
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.setCaptureFormat (punch ? CaptureFormat::Flac24 : CaptureFormat::Wav24);
                    rec.setBackupDirectory (backupRoot);
                    rec.setBackupCaptureFormat (punch ? CaptureFormat::Wav16 : CaptureFormat::Flac16);
                    expect (rec.setMirrors ({ { mirrorRoot, punch ? CaptureFormat::Flac16
                                                            : CaptureFormat::Aiff24 } }));
                    rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                    if (punch) rec.armPunchIn (punchIn);
                    expect (rec.startRecording (copyDir));
                    std::vector<float> samples ((size_t) block, punch ? punchVal : baseVal);
                    const float* data = samples.data();
                    for (int b = 0; b < (punch ? punchBlocks : baseBlocks); ++b)
                        rec.processBlock (&data, 1, block);
                    rec.stopRecording();
                    expect (! rec.hasPunchSpliceFailed(), "copy splice rolled back");
                };
                capture (false);
                capture (true);

                const auto primary = copyDir.getChildFile ("Audio Files/Track_01.wav");
                const auto backup = backupRoot.getChildFile (copyDir.getFileName())
                                               .getChildFile ("Audio Files/Track_01.flac");
                const auto mirror = mirrorRoot.getChildFile (copyDir.getFileName())
                                               .getChildFile ("Audio Files/Track_01.aif");
                for (const auto& file : { primary, backup, mirror })
                {
                    expect (file.existsAsFile(), "original copy file missing: " + file.getFileName());
                    expectWithinAbsoluteError (regionMean (fm, file, punchIn + 64,
                                                           punchLen - 128), punchVal, 0.03f);
                }
                expect (! copyDir.getChildFile ("Audio Files/Track_01.flac").existsAsFile());
                expect (! backup.getSiblingFile ("Track_01.wav").existsAsFile());
                expect (! mirror.getSiblingFile ("Track_01.flac").existsAsFile());
                copyDir.deleteRecursively();
                backupRoot.deleteRecursively();
                mirrorRoot.deleteRecursively();
            }

            beginTest ("session report length + SHA describe the SPLICED file");
            {
                // The SHA is hashed on a background thread after stop; poll until
                // the report is final (sha256Pending == false).
                const auto reportFile = dir.getChildFile ("session.report.json");
                juce::var report;
                for (int t = 0; t < 60; ++t)
                {
                    report = juce::JSON::parse (reportFile.loadFileAsString());
                    if (report.isObject()
                        && ! (bool) report.getProperty ("sha256Pending", true))
                        break;
                    juce::Thread::sleep (100);
                }
                expect (report.isObject(), "no report");
                expect (! (bool) report.getProperty ("sha256Pending", true),
                        "report SHA never finalised");

                auto* tracks = report.getProperty ("tracks", juce::var()).getArray();
                expect (tracks != nullptr && tracks->size() >= 1, "no tracks in report");
                if (tracks != nullptr && ! tracks->isEmpty())
                {
                    const auto& t0 = tracks->getReference (0);
                    expectEquals ((int) (juce::int64) t0.getProperty ("totalSamplesPrimary", 0),
                                  baseLen, "report length not the spliced length");
                    if (auto* shas = t0.getProperty ("sha256", juce::var()).getArray())
                        if (! shas->isEmpty())
                            expectEquals (shas->getReference (0).toString(),
                                          juce::String (zynforge::hashing::fileSha256 (track)),
                                          "report SHA != spliced-file SHA");
                }
            }

            dir.deleteRecursively();

            // ── Continue recording = a NEW PART, original untouched ──────────
            auto cdir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("zf-continue-" + juce::Uuid().toString());
            cdir.deleteRecursively();
            auto cTrack = cdir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
            auto cPart2 = cdir.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav");
            const int contBlocks = 20;
            const int contLen    = contBlocks * block;

            beginTest ("continue appends a new part; the original take is untouched");
            {
                recordDc (cdir, baseVal, baseBlocks, block, sr, /*punchIn*/ -1);
                expect (cTrack.existsAsFile(), "base take missing");
                const auto baseSha = juce::String (zynforge::hashing::fileSha256 (cTrack));
                std::unique_ptr<juce::AudioFormatReader> br (fm.createReaderFor (cTrack));
                const int origLen = br != nullptr ? (int) br->lengthInSamples : 0;
                br.reset();

                // Continue: arm the SAME track, armContinue, record more.
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                    rec.armContinue ((juce::int64) origLen);   // timeline base = existing take
                    rec.startRecording (cdir);
                    // The continue's timeline position = base + samples captured.
                    expectEquals ((int) rec.getRecordBaseSamples(), origLen,
                                  "continue base should be the existing take length");
                    std::vector<float> buf ((size_t) block, punchVal);
                    const float* ptr = buf.data();
                    for (int b = 0; b < contBlocks; ++b) rec.processBlock (&ptr, 1, block);
                    rec.stopRecording();
                }

                // Original Track_01.wav is byte-identical (never touched).
                expect (cTrack.existsAsFile(), "original take vanished");
                expectEquals (juce::String (zynforge::hashing::fileSha256 (cTrack)), baseSha,
                              "continue MODIFIED the original take");
                // A new part exists with the continuation audio.
                expect (cPart2.existsAsFile(), "continuation part not written");
                expectWithinAbsoluteError (regionMean (fm, cPart2, 0, contLen), punchVal, 0.02f);

                // The player stitches them: total = base + continuation.
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                expectEquals (eng.loadSession (cdir), 1, "parts should load as one track");
                expectEquals ((int) eng.getPlayer().getTotalLengthSamples(), origLen + contLen,
                              "take length should span base + continuation");
            }

            cdir.deleteRecursively();

            // ── Punch into a MULTI-PART take: flatten + splice ───────────────
            // Build a take with a continuation part, then punch INSIDE the first
            // part. The splice must read the whole take (part1 ++ part2) as the
            // base, drop the punch in, and FLATTEN to a single Track_01.wav with
            // the part deleted -- so you can punch anywhere on a continued take.
            auto mdir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("zf-mppunch-" + juce::Uuid().toString());
            mdir.deleteRecursively();

            beginTest ("interrupted punch restores original and archives partial replacement");
            {
                const auto recoveryDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-recovery-" + juce::Uuid().toString());
                const auto partialDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-partial-" + juce::Uuid().toString());
                recordDc (recoveryDir, baseVal, baseBlocks, block, sr, -1);
                recordDc (partialDir, punchVal, punchBlocks, block, sr, -1);
                const auto audio = recoveryDir.getChildFile ("Audio Files");
                const auto original = audio.getChildFile ("Track_01.wav");
                const auto sidecar = audio.getChildFile ("Track_01.punchbase.wav");
                const auto originalHash = juce::String (hashing::fileSha256 (original));
                expect (original.moveFileTo (sidecar));
                expect (partialDir.getChildFile ("Audio Files/Track_01.wav").copyFileTo (original));
                int restored = 0;
                expect (MultitrackRecorder::recoverInterruptedPunches (audio, &restored));
                expectEquals (restored, 1);
                expectEquals (juce::String (hashing::fileSha256 (original)), originalHash);
                expect (! sidecar.exists());
                const auto archive = recoveryDir.getChildFile ("Session File Backups");
                expectEquals (archive.findChildFiles (juce::File::findFiles, true,
                                                      "Track_01.wav").size(), 1);
                recoveryDir.deleteRecursively();
                partialDir.deleteRecursively();
            }

            beginTest ("legacy root-level punch recovery archives inside its session");
            {
                const auto legacyDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-legacy-" + juce::Uuid().toString());
                recordDc (legacyDir, baseVal, baseBlocks, block, sr, -1);
                const auto base = legacyDir.getChildFile ("Track_01.wav");
                const auto sidecar = legacyDir.getChildFile ("Track_01.punchbase.wav");
                expect (legacyDir.getChildFile ("Audio Files/Track_01.wav").moveFileTo (sidecar));
                expect (sidecar.copyFileTo (base)); // interrupted replacement placeholder
                int restored = 0;
                expect (MultitrackRecorder::recoverInterruptedPunches (legacyDir, &restored));
                expectEquals (restored, 1);
                expect (base.existsAsFile());
                expectEquals (legacyDir.getChildFile ("Session File Backups")
                                  .findChildFiles (juce::File::findFiles, true,
                                                   "Track_01.wav").size(), 1);
                legacyDir.deleteRecursively();
            }

            beginTest ("occupied punch sidecar refuses start without touching original");
            {
                const auto safeDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-sidecar-" + juce::Uuid().toString());
                recordDc (safeDir, baseVal, baseBlocks, block, sr, -1);
                const auto original = safeDir.getChildFile ("Audio Files/Track_01.wav");
                const auto hash = juce::String (hashing::fileSha256 (original));
                const auto occupied = safeDir.getChildFile ("Audio Files/Track_01.punchbase.wav");
                expect (occupied.createDirectory().wasOk());
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (punchIn);
                expect (! rec.startRecording (safeDir));
                expectEquals (juce::String (hashing::fileSha256 (original)), hash);
                safeDir.deleteRecursively();
            }

            beginTest ("new backup without original take refuses punch");
            {
                const auto copyDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-missingcopy-" + juce::Uuid().toString());
                const auto backupRoot = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-newbackup-" + juce::Uuid().toString());
                recordDc (copyDir, baseVal, baseBlocks, block, sr, -1);
                const auto original = copyDir.getChildFile ("Audio Files/Track_01.wav");
                const auto hash = juce::String (hashing::fileSha256 (original));
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.setBackupDirectory (backupRoot);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (punchIn);
                expect (! rec.startRecording (copyDir));
                expectEquals (juce::String (hashing::fileSha256 (original)), hash);
                copyDir.deleteRecursively();
                backupRoot.deleteRecursively();
            }

            beginTest ("capture pre-roll does not shift a punch splice");
            {
                const auto preDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-preroll-" + juce::Uuid().toString());
                recordDc (preDir, baseVal, baseBlocks, block, sr, -1);
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                std::vector<float> history ((size_t) block, 0.8f);
                const float* h = history.data();
                for (int b = 0; b < 10; ++b) rec.processBlock (&h, 1, block);
                rec.armPunchIn (punchIn);
                expect (rec.startRecording (preDir));
                std::vector<float> fresh ((size_t) block, punchVal);
                const float* p = fresh.data();
                for (int b = 0; b < punchBlocks; ++b) rec.processBlock (&p, 1, block);
                rec.stopRecording();
                const auto file = preDir.getChildFile ("Audio Files/Track_01.wav");
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
                expect (r != nullptr);
                if (r != nullptr) expectEquals ((int) r->lengthInSamples, baseLen);
                expectWithinAbsoluteError (regionMean (fm, file, punchIn + 64,
                                                       punchLen - 128), punchVal, 0.02f);
                expectWithinAbsoluteError (regionMean (fm, file, punchIn + punchLen + 64,
                                                       baseLen - punchIn - punchLen - 128), baseVal, 0.02f);
                preDir.deleteRecursively();
            }

            beginTest ("newly armed continuation track begins at the existing take end");
            {
                const auto alignedDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-continue-newtrack-" + juce::Uuid().toString());
                recordDc (alignedDir, baseVal, baseBlocks, block, sr, -1);
                MultitrackRecorder rec;
                rec.prepare (sr, block, 2);
                rec.getTrack (1).armed.store (true);
                rec.armContinue (0); // daemon's no-player-hint path
                expect (rec.startRecording (alignedDir));
                expectEquals ((int) rec.getRecordBaseSamples(), baseLen);
                std::vector<float> silence ((size_t) block, 0.0f);
                std::vector<float> fresh ((size_t) block, punchVal);
                const float* inputs[2] { silence.data(), fresh.data() };
                for (int b = 0; b < 4; ++b) rec.processBlock (inputs, 2, block);
                rec.stopRecording();
                const auto file = alignedDir.getChildFile ("Audio Files/Track_02.wav");
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
                expect (r != nullptr);
                if (r != nullptr) expectEquals ((int) r->lengthInSamples, baseLen + 4 * block);
                expectWithinAbsoluteError (regionMean (fm, file, baseLen - block, block), 0.0f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, file, baseLen + 64,
                                                       4 * block - 128), punchVal, 0.02f);
                alignedDir.deleteRecursively();
            }

            beginTest ("new track punched into a session has a silent lead-in");
            {
                const auto alignedDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("zf-punch-newtrack-" + juce::Uuid().toString());
                recordDc (alignedDir, baseVal, baseBlocks, block, sr, -1);
                MultitrackRecorder rec;
                rec.prepare (sr, block, 2);
                rec.getTrack (1).armed.store (true);
                rec.armPunchIn (punchIn);
                expect (rec.startRecording (alignedDir));
                std::vector<float> silence ((size_t) block, 0.0f);
                std::vector<float> fresh ((size_t) block, punchVal);
                const float* inputs[2] { silence.data(), fresh.data() };
                for (int b = 0; b < 4; ++b) rec.processBlock (inputs, 2, block);
                rec.stopRecording();
                const auto file = alignedDir.getChildFile ("Audio Files/Track_02.wav");
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (file));
                expect (r != nullptr);
                if (r != nullptr) expectEquals ((int) r->lengthInSamples, (int) punchIn + 4 * block);
                expectWithinAbsoluteError (regionMean (fm, file, punchIn - block, block), 0.0f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, file, punchIn + 64,
                                                       4 * block - 128), punchVal, 0.02f);
                alignedDir.deleteRecursively();
            }
            auto mTrack = mdir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
            auto mPart2 = mdir.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav");

            beginTest ("punch into a multi-part take flattens to one spliced file");
            {
                const float contVal = 0.20f;
                // Part 1: baseBlocks of baseVal.
                recordDc (mdir, baseVal, baseBlocks, block, sr, /*punchIn*/ -1);
                // Part 2: continuation of contVal -> Track_01_part02.wav.
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true, std::memory_order_relaxed);
                    rec.armContinue ((juce::int64) baseLen);
                    rec.startRecording (mdir);
                    std::vector<float> buf ((size_t) block, contVal);
                    const float* p = buf.data();
                    for (int b = 0; b < contBlocks; ++b) rec.processBlock (&p, 1, block);
                    rec.stopRecording();
                }
                expect (mPart2.existsAsFile(), "continuation part missing before punch");
                const int fullLen = baseLen + contLen;   // whole take, both parts

                // Punch INSIDE part 1, recording punchVal.
                recordDc (mdir, punchVal, punchBlocks, block, sr, /*punchIn*/ punchIn);

                // Flattened: one file, no part02, length == the whole take.
                expect (mTrack.existsAsFile(), "flattened take missing");
                expect (! mPart2.existsAsFile(), "part02 should be folded into the flattened take");
                expect (! mTrack.getSiblingFile ("Track_01.punchbase.wav").existsAsFile(),
                        "punch sidecar leaked");
                expect (! mPart2.getSiblingFile ("Track_01_part02.punchbase.wav").existsAsFile(),
                        "part sidecar leaked");
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (mTrack));
                expect (r != nullptr);
                expectEquals ((int) r->lengthInSamples, fullLen, "flattened length wrong");
                r.reset();

                // Three regions: base before the punch, punch, then the
                // CONTINUATION audio after the punch-out (the splice reached into
                // part 2 of the base).
                expectWithinAbsoluteError (regionMean (fm, mTrack, 0, punchIn - 64), baseVal, 0.02f);
                expectWithinAbsoluteError (regionMean (fm, mTrack, punchIn + 64, punchLen - 128), punchVal, 0.02f);
                expectWithinAbsoluteError (regionMean (fm, mTrack, baseLen + 64, contLen - 128), contVal, 0.02f);

                // Loads back as ONE track of the full length.
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine eng;
                expectEquals (eng.loadSession (mdir), 1, "flattened take should load as one track");
                expectEquals ((int) eng.getPlayer().getTotalLengthSamples(), fullLen,
                              "flattened take length wrong on load");
            }

            mdir.deleteRecursively();
        }
    };

    static PunchRecordTests punchRecordTests;
}
