#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioImport.h"
#include "FailingAudioReader.h"

// Internal seam into the same conversion routine used by importFiles. The
// public import API and its default reader selection are unchanged.
namespace zynforge::audioimport::detail
{
    bool writeConverted (juce::AudioFormatReader&, const juce::File&, double,
                         int, int, const std::atomic<bool>*, bool* clipped = nullptr);
}

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
            for (double targetRate : { 48000.0, 44100.0 })
            {
                beginTest ("Import rejects source read failure at " + juce::String (targetRate, 0)
                           + " Hz without publishing partial media");
                testaudio::ReadEvidence evidence;
                testaudio::FailingReader reader (evidence);
                const auto destination = audioDir.getChildFile ("fault-import.wav");
                destination.deleteFile();
                expect (! audioimport::detail::writeConverted (
                    reader, destination, targetRate, 1, 0, nullptr));
                expectEquals (evidence.successfulReads, 1);
                expect (evidence.failedReads > 0, "fault was not exercised");
                expect (! destination.exists(), "failed read became session media");
                expectEquals (audioDir.findChildFiles (juce::File::findFiles, false,
                                                       "*.partial.wav").size(), 0);
            }
            testPolyFileReadOnce (sourceDir);
            root.deleteRecursively();
        }

        // Every channel a different signal, so a channel landing on the wrong
        // track or a sample shifted shows. Below half scale: JUCE reads 24-bit
        // as n / 2^23 and writes x * (2^23 - 1), so louder samples come back one
        // step lower on any 24-bit round trip, import or not.
        static float polySample (int channel, juce::int64 frame)
        {
            return (float) (((frame * (channel + 3) + channel * 977) % 2001) - 1000) / 2048.0f;
        }

        static bool writePoly (const juce::File& file, double sampleRate, int channels, juce::int64 frames)
        {
            std::unique_ptr<juce::FileOutputStream> output (file.createOutputStream());
            if (output == nullptr) return false;
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (output.get(), sampleRate, (unsigned int) channels, 24, {}, 0));
            if (writer == nullptr) return false;
            output.release();
            juce::AudioBuffer<float> block (channels, 1000);
            for (juce::int64 done = 0; done < frames;)
            {
                const int count = (int) juce::jmin ((juce::int64) 1000, frames - done);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < count; ++i) block.setSample (c, i, polySample (c, done + i));
                if (! writer->writeFromAudioSampleBuffer (block, 0, count)) return false;
                done += count;
            }
            return true;
        }

        void testPolyFileReadOnce (const juce::File& sourceDir)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const auto readAll = [&formats] (const juce::File& file, juce::AudioBuffer<float>& into)
            {
                std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                if (reader == nullptr) return false;
                into.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
                return reader->read (&into, 0, (int) reader->lengthInSamples, 0, true, true);
            };

            beginTest ("A poly file is read once, not once per channel, and every channel stays exact");
            {
                constexpr int channels = 8;
                constexpr juce::int64 frames = 48000;
                const auto poly = sourceDir.getChildFile ("Show 8ch.wav");
                const auto audioDir = sourceDir.getSiblingFile ("Poly Audio Files");
                expect (audioDir.createDirectory().wasOk());
                expect (writePoly (poly, 48000.0, channels, frames));
                audioimport::detail::sourceFramesReadForTests = 0;
                const auto result = audioimport::importFiles ({ poly }, audioDir, 0, 48000.0);
                expectEquals (result.importedFiles, 1);
                expectEquals ((int) result.tracks.size(), channels);
                expect (audioimport::detail::sourceFramesReadForTests.load() <= frames + 16384,
                        "the source was read " + juce::String (audioimport::detail::sourceFramesReadForTests.load())
                        + " frames for a file of " + juce::String (frames));
                juce::AudioBuffer<float> source;
                expect (readAll (poly, source));
                for (int c = 0; c < channels; ++c)
                {
                    juce::AudioBuffer<float> take;
                    const auto file = audioDir.getChildFile ("Track_" + juce::String (c + 1).paddedLeft ('0', 2) + ".wav");
                    expect (readAll (file, take), file.getFileName());
                    expectEquals (take.getNumChannels(), 1);
                    expectEquals (take.getNumSamples(), (int) frames);
                    int wrong = 0;
                    for (int i = 0; i < juce::jmin (take.getNumSamples(), source.getNumSamples()); ++i)
                        if (take.getSample (0, i) != source.getSample (c, i)) ++wrong;
                    expectEquals (wrong, 0, "channel " + juce::String (c + 1) + " changed on the way in");
                }
                audioDir.deleteRecursively();
            }

            beginTest ("A poly file at another rate gives each channel what its own conversion would");
            {
                constexpr int channels = 4;
                constexpr juce::int64 frames = 44100;
                const auto poly = sourceDir.getChildFile ("Show 4ch 44k.wav");
                const auto audioDir = sourceDir.getSiblingFile ("Poly 44k Audio Files");
                const auto alone = sourceDir.getSiblingFile ("Alone Audio Files");
                expect (audioDir.createDirectory().wasOk() && alone.createDirectory().wasOk());
                expect (writePoly (poly, 44100.0, channels, frames));
                const auto result = audioimport::importFiles ({ poly }, audioDir, 0, 48000.0);
                expectEquals ((int) result.tracks.size(), channels);
                juce::AudioBuffer<float> source;
                expect (readAll (poly, source));
                for (int c = 0; c < channels; ++c)
                {
                    // The same channel, on its own, through the mono path.
                    const auto single = alone.getChildFile ("ch" + juce::String (c) + ".wav");
                    {
                        std::unique_ptr<juce::FileOutputStream> output (single.createOutputStream());
                        juce::WavAudioFormat wav;
                        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (output.get(), 44100.0, 1, 24, {}, 0));
                        expect (writer != nullptr);
                        if (writer == nullptr) continue;
                        output.release();
                        const float* plane[1] = { source.getReadPointer (c) };
                        expect (writer->writeFromFloatArrays (plane, 1, source.getNumSamples()));
                    }
                    const auto monoDir = alone.getChildFile ("out" + juce::String (c));
                    expect (monoDir.createDirectory().wasOk());
                    audioimport::importFiles ({ single }, monoDir, 0, 48000.0);
                    juce::AudioBuffer<float> fromPoly, fromMono;
                    expect (readAll (audioDir.getChildFile ("Track_0" + juce::String (c + 1) + ".wav"), fromPoly));
                    expect (readAll (monoDir.getChildFile ("Track_01.wav"), fromMono));
                    expectEquals (fromPoly.getNumSamples(), fromMono.getNumSamples());
                    int wrong = 0;
                    for (int i = 0; i < juce::jmin (fromPoly.getNumSamples(), fromMono.getNumSamples()); ++i)
                        if (fromPoly.getSample (0, i) != fromMono.getSample (0, i)) ++wrong;
                    expectEquals (wrong, 0, "channel " + juce::String (c + 1) + " differs from its own conversion");
                }
                audioDir.deleteRecursively();
                alone.deleteRecursively();
            }
        }
    };

    static AudioImportTests audioImportTests;
}
