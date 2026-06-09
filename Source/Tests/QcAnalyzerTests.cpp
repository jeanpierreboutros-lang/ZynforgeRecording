// Headless tests for the post-show QC scan (Source/Audio/QcAnalyzer.h).
// Synthesises a WAV with known peak / clipping / silence content and
// verifies the analyzer reports exactly what was written.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/QcAnalyzer.h"

namespace zynforge
{
    class QcAnalyzerTests final : public juce::UnitTest
    {
    public:
        QcAnalyzerTests() : juce::UnitTest ("Post-show QC analyzer", "zynforge") {}

        void runTest() override
        {
            const double sr = 48000.0;
            const int total = (int) (3.0 * sr);          // 3 s mono

            // 0.0-1.0 s silence | 1.0-2.0 s sine at 0.5 | clip run of 100
            // samples of 1.0 at 2.5 s | silence to the end.
            std::vector<float> samples ((size_t) total, 0.0f);
            for (int i = 0; i < (int) sr; ++i)
                samples[(size_t) ((int) sr + i)] =
                    0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr);
            const int clipStart = (int) (2.5 * sr);
            for (int i = 0; i < 100; ++i)
                samples[(size_t) (clipStart + i)] = 1.0f;

            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-qc-" + juce::Uuid().toString());
            dir.createDirectory();
            auto wavFile = dir.getChildFile ("Track_07.wav");
            {
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::FileOutputStream> os (wavFile.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (
                    wav.createWriterFor (os.get(), sr, 1, 32, {}, 0));   // 32f: exact values
                expect (w != nullptr);
                os.release();
                const float* chans[1] = { samples.data() };
                w->writeFromFloatArrays (chans, 1, total);
            }

            beginTest ("analyzeFile reports peak, clip events and noise floor");
            {
                const auto q = qc::analyzeFile (wavFile);
                expectEquals (q.lengthSamples, (juce::int64) total);
                expectWithinAbsoluteError (q.peakDb, 0.0f, 0.1f);           // the 1.0 run
                expectEquals (q.clipEventCount, 1);
                expect (! q.clipEvents.empty());
                if (! q.clipEvents.empty())
                {
                    expectEquals (q.clipEvents.front().startSample, (juce::int64) clipStart);
                    expectEquals (q.clipEvents.front().lengthSamples, 100);
                }
                // 1/3 of the file is digital silence -> 10th-percentile
                // window RMS sits at the floor.
                expect (q.noiseFloorDb < -90.0f, "noise floor should be deep for digital silence");
                // A 0.5 sine for a third of the file lands somewhere sane.
                expect (q.integratedLufs > -40.0f && q.integratedLufs < 0.0f,
                        "integrated loudness out of plausible range: "
                        + juce::String (q.integratedLufs));
            }

            beginTest ("clean audio yields zero clip events");
            {
                auto cleanFile = dir.getChildFile ("Track_08.wav");
                {
                    std::vector<float> clean ((size_t) sr, 0.0f);
                    for (size_t i = 0; i < clean.size(); ++i)
                        clean[i] = 0.9f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 100.0 * (double) i / sr);
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::FileOutputStream> os (cleanFile.createOutputStream());
                    std::unique_ptr<juce::AudioFormatWriter> w (
                        wav.createWriterFor (os.get(), sr, 1, 32, {}, 0));
                    os.release();
                    const float* chans[1] = { clean.data() };
                    w->writeFromFloatArrays (chans, 1, (int) clean.size());
                }
                const auto q = qc::analyzeFile (cleanFile);
                expectEquals (q.clipEventCount, 0);
                expectWithinAbsoluteError (q.peakDb, -0.9f, 0.2f);          // 0.9 = -0.92 dBFS
            }

            beginTest ("timecode formats sample positions");
            {
                expectEquals (qc::timecode (48000, 48000.0),  juce::String ("00:00:01.000"));
                expectEquals (qc::timecode (0, 48000.0),      juce::String ("00:00:00.000"));
                expectEquals (qc::timecode ((juce::int64) (3661.5 * 48000.0), 48000.0),
                              juce::String ("01:01:01.500"));
                expectEquals (qc::timecode (1, 0.0), juce::String ("--"));
            }

            beginTest ("reportText flags clipping and dropouts");
            {
                const auto q = qc::analyzeFile (wavFile);
                const auto txt = qc::reportText ("TestShow", { q }, 0);
                expect (txt.contains ("CLIPPING: 1 event"));
                expect (txt.contains ("missed samples: 0  [OK]"));
                const auto bad = qc::reportText ("TestShow", { q }, 1234);
                expect (bad.contains ("[!!] DROPOUTS"));
            }

            beginTest ("missing file fails closed");
            {
                const auto q = qc::analyzeFile (dir.getChildFile ("nope.wav"));
                expectEquals (q.lengthSamples, (juce::int64) 0);
                expectEquals (q.clipEventCount, 0);
            }

            dir.deleteRecursively();
        }
    };

    static QcAnalyzerTests qcAnalyzerTests;
}
