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
                    rec.armContinue();
                    rec.startRecording (cdir);
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
        }
    };

    static PunchRecordTests punchRecordTests;
}
