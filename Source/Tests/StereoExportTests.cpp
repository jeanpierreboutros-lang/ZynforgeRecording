// Headless tests for stereo-pair export (TrackExporter::exportStereoPair):
// two mono sources must interleave into ONE stereo file with the correct
// channel assignment, in each PCM format and across a sample-rate change.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/TrackExporter.h"

namespace zynforge
{
    class StereoExportTests final : public juce::UnitTest
    {
    public:
        StereoExportTests() : juce::UnitTest ("Stereo-pair export", "zynforge") {}

        // Write `value` as a constant-DC mono WAV of `len` samples at `sr`.
        static void writeMonoDc (const juce::File& f, float value, int len, double sr)
        {
            std::vector<float> s ((size_t) len, value);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
            os.release();
            const float* chans[1] = { s.data() };
            w->writeFromFloatArrays (chans, 1, len);
        }

        void runTest() override
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-stereoexp-" + juce::Uuid().toString());
            dir.createDirectory();

            const double sr = 48000.0;
            const int len = 24000;          // 0.5 s
            auto srcL = dir.getChildFile ("L.wav");
            auto srcR = dir.getChildFile ("R.wav");
            writeMonoDc (srcL,  0.30f, len, sr);
            writeMonoDc (srcR, -0.60f, len, sr);

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();

            beginTest ("exportStereoPair interleaves L->ch0, R->ch1 (WAV, same SR)");
            {
                TrackExporter ex;
                ExportOptions opts; opts.format = ExportFormat::Wav24;
                opts.sampleRate = sr; opts.bitsPerSample = 24;
                auto stem = dir.getChildFile ("pair");
                juce::String err;
                expect (ex.exportStereoPair (srcL, srcR, stem, opts, err), err);

                auto out = stem.withFileExtension (".wav");
                expect (out.existsAsFile());
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
                expect (rd != nullptr, "exported stereo file is unreadable");
                if (rd != nullptr)   // guard: expect() doesn't stop the test
                {
                    expectEquals ((int) rd->numChannels, 2, "export is not stereo");
                    expectEquals (rd->lengthInSamples, (juce::int64) len);

                    juce::AudioBuffer<float> buf (2, len);
                    rd->read (&buf, 0, len, 0, true, true);
                    // 24-bit quantisation -> small tolerance.
                    expectWithinAbsoluteError (buf.getSample (0, len / 2),  0.30f, 0.001f);
                    expectWithinAbsoluteError (buf.getSample (1, len / 2), -0.60f, 0.001f);
                    // Channels are distinct (not a doubled mono).
                    expect (std::abs (buf.getSample (0, 100) - buf.getSample (1, 100)) > 0.5f,
                            "channels look identical -- not truly stereo");
                }
            }

            beginTest ("exportStereoPair resamples to a new rate, stays stereo");
            {
                TrackExporter ex;
                ExportOptions opts; opts.format = ExportFormat::Wav24;
                opts.sampleRate = 44100.0; opts.bitsPerSample = 24;
                auto stem = dir.getChildFile ("pair44");
                juce::String err;
                expect (ex.exportStereoPair (srcL, srcR, stem, opts, err), err);

                auto out = stem.withFileExtension (".wav");
                std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
                expect (rd != nullptr);
                expectEquals ((int) rd->numChannels, 2);
                expectWithinAbsoluteError ((float) rd->sampleRate, 44100.0f, 1.0f);
                // ~0.5 s at 44.1k.
                expect (std::abs (rd->lengthInSamples - 22050) < 64);
                juce::AudioBuffer<float> buf (2, (int) rd->lengthInSamples);
                rd->read (&buf, 0, (int) rd->lengthInSamples, 0, true, true);
                const int mid = (int) rd->lengthInSamples / 2;
                expectWithinAbsoluteError (buf.getSample (0, mid),  0.30f, 0.01f);
                expectWithinAbsoluteError (buf.getSample (1, mid), -0.60f, 0.01f);
            }

            beginTest ("missing R source fails closed");
            {
                TrackExporter ex;
                ExportOptions opts;
                juce::String err;
                expect (! ex.exportStereoPair (srcL, dir.getChildFile ("nope.wav"),
                                               dir.getChildFile ("x"), opts, err));
                expect (err.isNotEmpty());
            }

            dir.deleteRecursively();
        }
    };

    static StereoExportTests stereoExportTests;
}
