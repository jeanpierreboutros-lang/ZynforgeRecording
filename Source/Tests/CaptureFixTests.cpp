// Capture-path regressions: what a crash leaves on disk, what a punch writes,
// how a lost daemon behaves, and how copies stay out of each other's way.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AiffRepair.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/MultitrackRecorder.h"

#include <cmath>
#include <vector>

namespace zynforge
{
    class CaptureFixTests final : public juce::UnitTest
    {
    public:
        CaptureFixTests() : UnitTest ("Capture fixes", "zynforge") {}

        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 256;

        static juce::File tempDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-capture-fix-" + tag + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        static void feed (MultitrackRecorder& rec, int blocks, int channels, float value = 0.25f)
        {
            std::vector<float> samples ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) channels, samples.data());
            for (int b = 0; b < blocks; ++b)
            {
                rec.processBlock (inputs.data(), channels, kBlock);
                rec.drainPendingForTests();
            }
        }

        static juce::int64 lengthOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            return reader != nullptr ? reader->lengthInSamples : -1;
        }

        static std::vector<float> samplesOf (const juce::File& file, int channel = 0)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr) return {};
            juce::AudioBuffer<float> buffer ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
            std::vector<float> out ((size_t) buffer.getNumSamples());
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                out[(size_t) i] = buffer.getSample (channel, i);
            return out;
        }

        static juce::MemoryBlock bytesOf (const juce::File& file)
        {
            juce::MemoryBlock block;
            file.loadFileAsData (block);
            return block;
        }

        static void writeRamp (juce::AudioFormatWriter& writer, int channels, int frames)
        {
            juce::AudioBuffer<float> buffer (channels, frames);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < frames; ++i)
                    buffer.setSample (c, i, (float) (i % 2000) / 2500.0f - 0.4f + (float) c * 0.01f);
            writer.writeFromAudioSampleBuffer (buffer, 0, frames);
        }

        static void writeAiff (const juce::File& file, int bits, int channels, int frames)
        {
            file.deleteFile();
            auto stream = file.createOutputStream();
            std::unique_ptr<juce::AudioFormatWriter> writer (
                juce::AiffAudioFormat().createWriterFor (stream.get(), kRate, (unsigned int) channels, bits, {}, 0));
            stream.release();
            writeRamp (*writer, channels, frames);
        }

        // Puts the bytes back the way an older build left them after a crash:
        // the header it wrote at open, then the audio.
        static void staleHeaderFromOpen (const juce::File& file, bool aifc)
        {
            juce::FileOutputStream out (file);
            const int headerBytes = aifc ? 72 : 54;
            out.setPosition (4);                       out.writeIntBigEndian (headerBytes - 8);
            out.setPosition (aifc ? 34 : 22);          out.writeIntBigEndian (0);
            out.setPosition (aifc ? 60 : 42);          out.writeIntBigEndian (8);
        }

        void runTest() override
        {
            aiffCrashSurvival();
        }

        void aiffCrashSurvival()
        {
            for (const auto format : { CaptureFormat::Aiff16, CaptureFormat::Aiff24 })
                for (const bool stereo : { false, true })
                {
                    const auto label = juce::String (format == CaptureFormat::Aiff16 ? "16" : "24")
                                     + (stereo ? " stereo" : " mono");
                    beginTest ("AIFF " + label + " is readable on disk during the take");
                    const auto dir = tempDir ("aiff");
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.setCaptureFormat (format);
                    rec.getTrack (0).armed.store (true);
                    if (stereo) { rec.getTrack (0).isStereo.store (true); rec.getTrack (1).armed.store (true); }
                    expect (rec.startRecording (dir));
                    feed (rec, 40, 2);
                    rec.flushOpenWritersForTests();

                    const auto take = dir.getChildFile ("Audio Files").getChildFile ("Track_01.aif");
                    const auto survivor = dir.getChildFile ("survivor.aif");
                    expect (take.copyFileTo (survivor));
                    expectEquals (lengthOf (survivor), (juce::int64) 40 * kBlock, "the survivor read short");
                    const auto audio = samplesOf (survivor, stereo ? 1 : 0);
                    expect (audio.size() == 40 * kBlock && std::abs (audio.back() - 0.25f) < 0.001f,
                            "the survivor's audio is not the take");

                    feed (rec, 10, 2);
                    rec.stopRecording();
                    expectEquals (lengthOf (take), (juce::int64) 50 * kBlock, "the closed file lost frames");
                    MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
                }

            beginTest ("The live AIFF writer produces the same file as JUCE's");
            for (const int bits : { 16, 24 })
                for (const int channels : { 1, 2 })
                    for (const int frames : { 1000, 1001 })
                    {
                        const auto dir = tempDir ("bytes");
                        const auto ours = dir.getChildFile ("ours.aif");
                        {
                            auto stream = ours.createOutputStream();
                            std::unique_ptr<juce::AudioFormatWriter> writer (
                                PcmAiffWriter::create (stream.get(), kRate, (unsigned int) channels, bits));
                            expect (writer != nullptr);
                            stream.release();
                            writeRamp (*writer, channels, frames / 2);
                            expect (writer->flush(), "a mid-take flush failed");
                            writeRamp (*writer, channels, frames - frames / 2);
                            expect (flushFloatAiffBeforeClose (writer.get()));
                        }
                        const auto split = dir.getChildFile ("split.aif");
                        {
                            auto stream = split.createOutputStream();
                            std::unique_ptr<juce::AudioFormatWriter> writer (
                                juce::AiffAudioFormat().createWriterFor (stream.get(), kRate, (unsigned int) channels, bits, {}, 0));
                            stream.release();
                            writeRamp (*writer, channels, frames / 2);
                            writeRamp (*writer, channels, frames - frames / 2);
                        }
                        expect (bytesOf (ours) == bytesOf (split),
                                juce::String (bits) + " bit, " + juce::String (channels) + " ch, "
                                    + juce::String (frames) + " frames differ from JUCE's file");
                        dir.deleteRecursively();
                    }

            beginTest ("Opening a session repairs an AIFF a crash left with an old header");
            for (const int bits : { 16, 24 })
                for (const int frames : { 4000, 4001 })
                {
                    const auto audioDir = tempDir ("repair").getChildFile ("Audio Files");
                    audioDir.createDirectory();
                    const auto take = audioDir.getChildFile ("Track_01.aif");
                    writeAiff (take, bits, 1, frames);
                    const auto expected = samplesOf (take);
                    staleHeaderFromOpen (take, false);
                    expectEquals (lengthOf (take), (juce::int64) 0, "the fixture is not stale");

                    expect (MultitrackRecorder::recoverInterruptedPunches (audioDir));
                    expectEquals (lengthOf (take), (juce::int64) frames, "the repair missed frames");
                    expect (samplesOf (take) == expected, "the repair changed the audio");

                    const auto repaired = bytesOf (take);
                    expect (MultitrackRecorder::recoverInterruptedPunches (audioDir));
                    expect (bytesOf (take) == repaired, "a second open rewrote a repaired file");
                    audioDir.getParentDirectory().deleteRecursively();
                }

            beginTest ("A float AIFF with an old header is repaired too");
            {
                const auto audioDir = tempDir ("repair-float").getChildFile ("Audio Files");
                audioDir.createDirectory();
                const auto take = audioDir.getChildFile ("Track_01.aif");
                {
                    auto stream = take.createOutputStream();
                    std::unique_ptr<juce::AudioFormatWriter> writer (FloatAiffWriter::create (stream.get(), kRate, 1));
                    stream.release();
                    writeRamp (*writer, 1, 4000);
                    expect (flushFloatAiffBeforeClose (writer.get()));
                }
                const auto expected = samplesOf (take);
                staleHeaderFromOpen (take, true);
                expectEquals (lengthOf (take), (juce::int64) 0, "the fixture is not stale");
                expect (MultitrackRecorder::recoverInterruptedPunches (audioDir));
                expect (samplesOf (take) == expected, "the float repair lost audio");
                audioDir.getParentDirectory().deleteRecursively();
            }

            beginTest ("Repair stops at the last whole frame and works mid-file");
            {
                const auto dir = tempDir ("repair-tail");
                const auto take = dir.getChildFile ("Track_01.aif");
                writeAiff (take, 24, 1, 1000);
                staleHeaderFromOpen (take, false);
                { juce::FileOutputStream out (take); out.writeByte (1); out.writeByte (2); }  // 2/3 of a frame
                expect (aiffrepair::repairStaleHeader (take) == aiffrepair::Result::repaired);
                expectEquals (lengthOf (take), (juce::int64) 1000);
                dir.deleteRecursively();
            }

            beginTest ("Repair leaves everything else alone");
            {
                const auto dir = tempDir ("repair-none");
                const auto good = dir.getChildFile ("Track_01.aif");
                writeAiff (good, 24, 1, 1000);
                const auto before = bytesOf (good);
                expect (aiffrepair::repairStaleHeader (good) == aiffrepair::Result::untouched);
                expect (bytesOf (good) == before, "a finished file was rewritten");

                // A chunk after the audio, then unknown bytes: not what a crash leaves.
                const auto chunked = dir.getChildFile ("Track_02.aif");
                {
                    good.copyFileTo (chunked);
                    const auto form = (int) good.getSize() - 8 + 12;
                    juce::FileOutputStream out (chunked);
                    out.write ("NAME", 4); out.writeIntBigEndian (4); out.write ("take", 4);
                    out.write ("extra", 5);
                    out.setPosition (4); out.writeIntBigEndian (form);
                }
                const auto chunkedBefore = bytesOf (chunked);
                expect (aiffrepair::repairStaleHeader (chunked) == aiffrepair::Result::untouched);
                expect (bytesOf (chunked) == chunkedBefore, "a file with trailing chunks was rewritten");

                const auto wav = dir.getChildFile ("Track_03.aif");
                wav.replaceWithText ("RIFF....WAVEfmt not an aiff at all");
                const auto wavBefore = bytesOf (wav);
                expect (aiffrepair::repairStaleHeader (wav) == aiffrepair::Result::untouched);
                expect (bytesOf (wav) == wavBefore, "a non-AIFF was rewritten");
                dir.deleteRecursively();
            }
        }
    };

    static CaptureFixTests captureFixTests;
}
