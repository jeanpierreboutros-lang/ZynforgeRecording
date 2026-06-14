// Headless tests for the offline punch-in splice (PunchSplice.h). The splice
// is the capture-critical core of punch-record: it must place the new audio
// EXACTLY in the punched region and preserve every sample before the punch-in
// and after the punch-out, sample-accurately, without ever touching the inputs.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/PunchSplice.h"

namespace zynforge
{
    class PunchSpliceTests final : public juce::UnitTest
    {
    public:
        PunchSpliceTests() : juce::UnitTest ("Punch splice", "zynforge") {}

        static void writeDc (const juce::File& f, float value, int len, double sr, int chans = 1)
        {
            juce::AudioBuffer<float> buf (chans, len);
            for (int c = 0; c < chans; ++c)
                juce::FloatVectorOperations::fill (buf.getWritePointer (c), value, len);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, (unsigned int) chans, 24, {}, 0));
            os.release();
            w->writeFromAudioSampleBuffer (buf, 0, len);
        }

        // Mean of channel 0 over [start, start+len).
        static float regionMean (juce::AudioFormatManager& fm, const juce::File& f,
                                 juce::int64 start, juce::int64 len)
        {
            std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
            if (r == nullptr || len <= 0) return 0.0f;
            juce::AudioBuffer<float> buf ((int) r->numChannels, (int) len);
            r->read (&buf, 0, (int) len, start, true, true);
            double sum = 0.0;
            for (int i = 0; i < (int) len; ++i) sum += buf.getSample (0, i);
            return (float) (sum / (double) len);
        }

        void runTest() override
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-punch-" + juce::Uuid().toString());
            dir.createDirectory();
            juce::AudioFormatManager fm; fm.registerBasicFormats();

            const double sr = 48000.0;

            beginTest ("punch in the middle: before + new + after preserved, exact lengths");
            {
                const int baseLen = 48000;       // 1.0 s base  @ 0.50
                const int insLen  = 12000;       // 0.25 s insert @ -0.30
                const juce::int64 punchIn = 18000;   // 0.375 s
                auto base   = dir.getChildFile ("base.wav");
                auto insert = dir.getChildFile ("ins.wav");
                auto out    = dir.getChildFile ("out.wav");
                writeDc (base,   0.50f, baseLen, sr);
                writeDc (insert,-0.30f, insLen,  sr);

                expect (splicePunchFile (fm, base, insert, punchIn, out));

                // Length unchanged: punchIn + insLen (30000) < baseLen, so the
                // tail after the punch-out keeps the file at baseLen.
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out));
                expect (r != nullptr);
                expectEquals ((int) r->lengthInSamples, baseLen);

                // Three regions, sample-accurate.
                expectWithinAbsoluteError (regionMean (fm, out, 0,             punchIn),        0.50f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, out, punchIn,       insLen),        -0.30f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, out, punchIn+insLen,
                                                       baseLen-(punchIn+insLen)),               0.50f, 0.001f);

                // Inputs were not mutated.
                expectWithinAbsoluteError (regionMean (fm, base,   0, baseLen),  0.50f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, insert, 0, insLen),  -0.30f, 0.001f);
            }

            beginTest ("punch past the end extends the take (append)");
            {
                const int baseLen = 24000, insLen = 12000;
                auto base = dir.getChildFile ("b2.wav"), insert = dir.getChildFile ("i2.wav"),
                     out  = dir.getChildFile ("o2.wav");
                writeDc (base, 0.20f, baseLen, sr);
                writeDc (insert, -0.40f, insLen, sr);
                // punchIn well past the end -> clamps to baseLen -> append.
                expect (splicePunchFile (fm, base, insert, 999999, out));
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out));
                expectEquals ((int) r->lengthInSamples, baseLen + insLen);
                expectWithinAbsoluteError (regionMean (fm, out, 0, baseLen),        0.20f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, out, baseLen, insLen),  -0.40f, 0.001f);
            }

            beginTest ("insert longer than the remaining tail overruns cleanly");
            {
                const int baseLen = 24000, insLen = 20000;
                const juce::int64 punchIn = 18000;   // punchIn+insLen = 38000 > baseLen
                auto base = dir.getChildFile ("b3.wav"), insert = dir.getChildFile ("i3.wav"),
                     out  = dir.getChildFile ("o3.wav");
                writeDc (base, 0.10f, baseLen, sr);
                writeDc (insert, -0.50f, insLen, sr);
                expect (splicePunchFile (fm, base, insert, punchIn, out));
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out));
                // Result = base[0,punchIn) + insert  ->  punchIn + insLen.
                expectEquals ((int) r->lengthInSamples, (int) (punchIn + insLen));
                expectWithinAbsoluteError (regionMean (fm, out, 0, punchIn),        0.10f, 0.001f);
                expectWithinAbsoluteError (regionMean (fm, out, punchIn, insLen),  -0.50f, 0.001f);
            }

            beginTest ("channel-count mismatch is refused (no corruption)");
            {
                auto base = dir.getChildFile ("bm.wav"), insert = dir.getChildFile ("im.wav"),
                     out  = dir.getChildFile ("om.wav");
                writeDc (base, 0.50f, 24000, sr, /*chans*/ 2);
                writeDc (insert, -0.30f, 12000, sr, /*chans*/ 1);
                expect (! splicePunchFile (fm, base, insert, 6000, out));
                expect (! out.existsAsFile());   // refused, nothing written
            }

            beginTest ("stereo punch preserves both channels");
            {
                const int baseLen = 24000, insLen = 8000;
                const juce::int64 punchIn = 8000;
                auto base = dir.getChildFile ("bs.wav"), insert = dir.getChildFile ("is.wav"),
                     out  = dir.getChildFile ("os.wav");
                writeDc (base, 0.40f, baseLen, sr, 2);
                writeDc (insert, -0.20f, insLen, sr, 2);
                expect (splicePunchFile (fm, base, insert, punchIn, out));
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (out));
                expect (r != nullptr && r->numChannels == 2);
                expectEquals ((int) r->lengthInSamples, baseLen);
                expectWithinAbsoluteError (regionMean (fm, out, punchIn, insLen), -0.20f, 0.001f);
            }

            dir.deleteRecursively();
        }
    };

    static PunchSpliceTests punchSpliceTests;
}
