#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioImport.h"

namespace zynforge
{
    class AudioImportTests final : public juce::UnitTest
    {
    public:
        AudioImportTests() : juce::UnitTest ("Audio import worker", "zynforge") {}

        static bool writeWav (const juce::File& file, double sampleRate,
                              int channels, juce::int64 samples)
        {
            std::unique_ptr<juce::FileOutputStream> output (file.createOutputStream());
            if (output == nullptr) return false;
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (output.get(), sampleRate, (unsigned int) channels,
                                     24, {}, 0));
            if (writer == nullptr) return false;
            output.release();
            juce::AudioBuffer<float> buffer (channels, 512);
            for (int channel = 0; channel < channels; ++channel)
                buffer.setSample (channel, 0, (float) (channel + 1) * 0.1f);
            juce::int64 written = 0;
            while (written < samples)
            {
                const int count = (int) juce::jmin ((juce::int64) 512, samples - written);
                if (! writer->writeFromAudioSampleBuffer (buffer, 0, count)) return false;
                written += count;
            }
            return true;
        }

        void runTest() override
        {
            beginTest ("worker appends mono/stereo files, resamples, and reports unreadable inputs");
            const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("zf-import-" + juce::Uuid().toString());
            const auto sourceDir = root.getChildFile ("source");
            const auto audioDir = root.getChildFile ("Audio Files");
            expect (sourceDir.createDirectory().wasOk());
            expect (audioDir.createDirectory().wasOk());
            const auto mono = sourceDir.getChildFile ("Mono 44k.wav");
            const auto stereo = sourceDir.getChildFile ("Stereo 48k.wav");
            const auto bad = sourceDir.getChildFile ("not-audio.wav");
            expect (writeWav (mono, 44100.0, 1, 4410));
            expect (writeWav (stereo, 48000.0, 2, 4800));
            expect (bad.replaceWithText ("not audio"));

            juce::Array<juce::File> sources { mono, stereo, bad };
            const auto result = audioimport::importFiles (sources, audioDir, 2, 48000.0);
            expectEquals ((int) result.tracks.size(), 2);
            expectEquals (result.tracks[0].trackIndex, 2);
            expect (! result.tracks[0].stereo);
            expectEquals (result.tracks[1].trackIndex, 3);
            expect (result.tracks[1].stereo);
            expectEquals (result.converted, 1);
            expectEquals (result.failed, 1);
            expectEquals (result.importedFiles, 2);

            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> monoReader (
                formats.createReaderFor (audioDir.getChildFile ("Track_03.wav")));
            std::unique_ptr<juce::AudioFormatReader> stereoReader (
                formats.createReaderFor (audioDir.getChildFile ("Track_04.wav")));
            expect (monoReader != nullptr && monoReader->numChannels == 1);
            expect (stereoReader != nullptr && stereoReader->numChannels == 2);
            if (monoReader != nullptr)
                expectWithinAbsoluteError ((double) monoReader->lengthInSamples, 4800.0, 2.0);

            beginTest ("four-channel source imports all four mono channels");
            const auto quad = sourceDir.getChildFile ("Console 4ch.wav");
            expect (writeWav (quad, 48000.0, 4, 512));
            const auto split = audioimport::importFiles ({ quad }, audioDir, 10, 48000.0);
            expectEquals (split.importedFiles, 1);
            expectEquals ((int) split.tracks.size(), 4);
            expectEquals (split.failed, 0);
            for (int channel = 0; channel < 4; ++channel)
            {
                if (channel < (int) split.tracks.size())
                {
                    expectEquals (split.tracks[(size_t) channel].trackIndex, 10 + channel);
                    expect (! split.tracks[(size_t) channel].stereo);
                }
                const auto file = audioDir.getChildFile (
                    "Track_" + juce::String (11 + channel).paddedLeft ('0', 2) + ".wav");
                std::unique_ptr<juce::AudioFormatReader> chReader (formats.createReaderFor (file));
                expect (chReader != nullptr);
                if (chReader != nullptr)
                {
                    expectEquals ((int) chReader->numChannels, 1);
                    juce::AudioBuffer<float> sample (1, 1);
                    expect (chReader->read (&sample, 0, 1, 0, true, false));
                    expectWithinAbsoluteError (sample.getSample (0, 0),
                                               (float) (channel + 1) * 0.1f, 0.001f);
                }
            }

            beginTest ("unexpected destination collision preserves existing session media");
            const auto collision = audioDir.getChildFile ("Track_09.wav");
            expect (collision.replaceWithText ("existing-take"));
            const auto collided = audioimport::importFiles ({ mono }, audioDir, 8, 48000.0);
            expect (collided.tracks.empty());
            expectEquals (collided.failed, 1);
            expectEquals (collision.loadFileAsString(), juce::String ("existing-take"));
            expectEquals (audioDir.findChildFiles (juce::File::findFiles, false,
                                                   "*.partial.wav").size(), 0);

            beginTest ("pre-cancelled import writes no partial take");
            std::atomic<bool> cancel { true };
            const auto cancelled = audioimport::importFiles ({ mono }, audioDir, 8, 48000.0, &cancel);
            expect (cancelled.cancelled);
            expect (cancelled.tracks.empty());
            expectEquals (collision.loadFileAsString(), juce::String ("existing-take"));
            root.deleteRecursively();
        }
    };

    static AudioImportTests audioImportTests;
}
