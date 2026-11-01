// Capture-path regressions: what a crash leaves on disk, what a punch writes,
// how a lost daemon behaves, and how copies stay out of each other's way.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AiffRepair.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Capture/CaptureDaemon.h"
#include "../UI/MainComponent.h"

#include <cmath>
#include <vector>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

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
            AudioEngine::setTestModeSkipAudioInit (true);
            aiffCrashSurvival();
            punchPastATracksEnd();
            punchAcrossTheRollCeiling();
            continuedTakeSurvivesACrash();
        }

        static void recordOn (AudioEngine& engine, const juce::File& dir, int blocks, float value, bool continuing)
        {
            auto& rec = engine.getRecorder();
            rec.getTrack (0).armed.store (true);
            if (continuing) engine.armContinue (engine.getCaptureLengthSamples());
            if (! engine.startRecording (dir)) return;
            std::vector<float> samples ((size_t) kBlock, value);
            const float* inputs[] { samples.data() };
            for (int b = 0; b < blocks; ++b) { rec.processBlock (inputs, 1, kBlock); rec.drainPendingForTests(); }
            engine.stopRecording();
        }

        static juce::int64 clipEnd (AudioEngine& engine)
        {
            juce::int64 end = 0;
            for (const auto& clip : engine.clipsFor (0))
                end = juce::jmax (end, clip.timelineStartSamples + clip.fileLengthSamples);
            return end;
        }

        static void trimEnd (AudioEngine& engine, juce::int64 frames)
        {
            engine.clipsFor (0)[0].fileLengthSamples -= frames;
            engine.syncActiveTake (0);
        }

        // The project on disk from before the second pass, then the take as a
        // crash left it: its new part is there, nothing saved it into a clip.
        void continuedTakeSurvivesACrash()
        {
            const auto run = [this] (bool trimmed, bool withLengths)
            {
                const auto dir = tempDir ("continue-crash");
                const juce::int64 first = 40 * kBlock, second = 30 * kBlock, trim = 10 * kBlock;
                juce::var playlists, lengths;
                {
                    AudioEngine engine;
                    engine.setStripCount (1);
                    engine.prepareForTests (kRate, kBlock);
                    recordOn (engine, dir, 40, 0.25f, false);
                    if (trimmed) trimEnd (engine, trim);
                    playlists = engine.playlistsToJson();
                    if (withLengths) lengths = engine.mediaLengthsToJson();
                    recordOn (engine, dir, 30, 0.5f, true);
                }

                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                expectEquals (engine.loadSession (dir), 1);
                engine.loadPlaylistsFromJson (playlists);
                expectEquals (clipEnd (engine), first - (trimmed ? trim : 0), "the fixture lost the old project's clips");
                engine.reconcileClipsWithMedia (lengths, true);

                expectEquals (clipEnd (engine), first + second, "the new part is missing from the timeline");
                const auto& clips = engine.clipsFor (0);
                if (trimmed)
                {
                    expectEquals ((int) clips.size(), withLengths ? 2 : 1);
                    if (withLengths) expectEquals (clips[0].fileLengthSamples, first - trim, "the trim was undone");
                }
                else
                    expectEquals ((int) clips.size(), 1, "a plain take should stay one clip");

                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            };

            beginTest ("A crash in a continued take leaves its new audio on the timeline");
            run (false, true);
            beginTest ("Clip edits survive the rebuild, and the new audio still lands");
            run (true, true);
            beginTest ("An older project with no saved lengths is rebuilt from its clips after a crash");
            run (false, false);

            beginTest ("A deliberate end trim is left alone when nothing was recorded after it");
            {
                const auto dir = tempDir ("trim-kept");
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                recordOn (engine, dir, 40, 0.25f, false);
                trimEnd (engine, 10 * kBlock);
                engine.reconcileClipsWithMedia (engine.mediaLengthsToJson(), false);
                expectEquals (clipEnd (engine), (juce::int64) 30 * kBlock, "reconciling undid a trim");
                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        static std::unique_ptr<MultitrackRecorder> recorderFor (const juce::File& dir, float value, int blocks,
                                                                std::function<void (MultitrackRecorder&)> arm = {})
        {
            auto rec = std::make_unique<MultitrackRecorder>();
            rec->prepare (kRate, kBlock, 1);
            rec->getTrack (0).armed.store (true);
            if (arm) arm (*rec);
            if (rec->startRecording (dir)) feed (*rec, blocks, 1, value);
            rec->stopRecording();
            return rec;
        }

        void punchPastATracksEnd()
        {
            beginTest ("A punch past a track's end lands where it was played");
            {
                const auto dir = tempDir ("punch-past");
                recorderFor (dir, 0.25f, 40);
                const auto take = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                const juce::int64 base = 40 * kBlock, punchAt = 100 * kBlock;
                expectEquals (lengthOf (take), base);

                recorderFor (dir, 0.75f, 20, [&] (MultitrackRecorder& rec) { rec.armPunchIn (punchAt); });

                expectEquals (lengthOf (take), punchAt + 20 * kBlock, "the take did not grow to the punch's end");
                const auto audio = samplesOf (take);
                expect (std::abs (audio[(size_t) base - 1] - 0.25f) < 0.001f, "the base was damaged");
                expect (audio[(size_t) base] == 0.0f && audio[(size_t) punchAt - 1] == 0.0f,
                        "the gap before the punch is not silence");
                expect (std::abs (audio[(size_t) punchAt] - 0.75f) < 0.001f, "the punch landed early");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A punch past the end of a multi-part take keeps every part and its place");
            {
                const auto dir = tempDir ("punch-past-parts");
                recorderFor (dir, 0.25f, 10);
                recorderFor (dir, 0.5f, 10, [] (MultitrackRecorder& rec) { rec.armContinue (10 * kBlock); });
                const auto audioDir = dir.getChildFile ("Audio Files");
                expect (audioDir.getChildFile ("Track_01_part02.wav").existsAsFile());

                const juce::int64 punchAt = 40 * kBlock;
                recorderFor (dir, 0.75f, 10, [&] (MultitrackRecorder& rec) { rec.armPunchIn (punchAt); });

                const auto take = audioDir.getChildFile ("Track_01.wav");
                expectEquals (lengthOf (take), punchAt + 10 * kBlock);
                const auto audio = samplesOf (take);
                expect (std::abs (audio[15 * kBlock] - 0.5f) < 0.001f, "the second part was lost");
                expect (audio[30 * kBlock] == 0.0f, "the gap is not silence");
                expect (std::abs (audio[(size_t) punchAt] - 0.75f) < 0.001f, "the punch landed early");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        // Every value identifies its position, so a part in the wrong place shows.
        static float rampAt (juce::int64 frame) noexcept
        {
            return (float) (frame % 30011) / 40000.0f - 0.35f;
        }

        static void feedRamp (MultitrackRecorder& rec, juce::int64 startFrame, int blocks)
        {
            std::vector<float> samples ((size_t) kBlock);
            const float* inputs[] { samples.data() };
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                    samples[(size_t) i] = rampAt (startFrame + b * (juce::int64) kBlock + i);
                rec.processBlock (inputs, 1, kBlock);
                rec.drainPendingForTests();
            }
        }

        static std::vector<float> takeSamples (const juce::File& main)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            const auto reader = ConcatReader::create (fm, findTakeParts (main));
            if (reader == nullptr) return {};
            juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
            reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false);
            return { buffer.getReadPointer (0), buffer.getReadPointer (0) + buffer.getNumSamples() };
        }

        void punchAcrossTheRollCeiling()
        {
            // 24000 bytes is 12000 frames of 16-bit mono.
            MultitrackRecorder::setAutoSplitThresholdBytesForTests (24000);
            const juce::ScopeGuard clear { [] { MultitrackRecorder::setAutoSplitThresholdBytesForTests (0); } };
            const auto record = [] (const juce::File& dir, juce::int64 at, int blocks, std::function<void (MultitrackRecorder&)> arm)
            {
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setCaptureFormat (CaptureFormat::Aiff16);
                rec.getTrack (0).armed.store (true);
                if (arm) arm (rec);
                if (! rec.startRecording (dir)) return false;
                feedRamp (rec, at, blocks);
                rec.stopRecording();
                return ! rec.hasPunchSpliceFailed();
            };

            beginTest ("A punch longer than the roll point lands whole, in order");
            {
                const auto dir = tempDir ("punch-roll");
                const auto audio = dir.getChildFile ("Audio Files");
                expect (record (dir, 0, 120, {}));
                expect (audio.getChildFile ("Track_01_part02.aif").existsAsFile(), "the base did not roll");

                const juce::int64 in = 20 * kBlock, punchLength = 70 * kBlock;
                expect (record (dir, in + 1000000, 70, [&] (MultitrackRecorder& rec) { rec.armPunchIn (in); }));

                const auto samples = takeSamples (audio.getChildFile ("Track_01.aif"));
                expectEquals ((int) samples.size(), 120 * kBlock, "the take changed length");
                int wrong = 0;
                for (int i = 0; i < (int) samples.size(); ++i)
                {
                    const bool punched = i >= in && i < in + punchLength;
                    const auto expected = rampAt (punched ? 1000000 + (juce::int64) i : (juce::int64) i);
                    if (std::abs (samples[(size_t) i] - expected) > 0.0002f) ++wrong;
                }
                expectEquals (wrong, 0, "audio moved in the splice");

                for (const auto& file : audio.findChildFiles (juce::File::findFiles, false, "*"))
                {
                    expect (! file.getFileName().containsIgnoreCase (".punch"), "left behind: " + file.getFileName());
                    expect (file.getSize() <= 24000 + 54 + 2, "a file crossed the ceiling: " + file.getFileName());
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A failed splice puts every base part back and keeps every punch part");
            {
                const auto dir = tempDir ("punch-roll-fail");
                const auto audio = dir.getChildFile ("Audio Files");
                expect (record (dir, 0, 60, {}));
                const auto basePart = audio.getChildFile ("Track_01_part02.aif");
                const auto before = bytesOf (basePart);

                const juce::int64 in = 20 * kBlock;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setCaptureFormat (CaptureFormat::Aiff16);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (in);
                expect (rec.startRecording (dir));
                feedRamp (rec, 1000000, 70);
                // The splice cannot read this base.
                audio.getChildFile ("Track_01.punchbase.aif").replaceWithText ("not audio any more");
                rec.stopRecording();

                expect (rec.hasPunchSpliceFailed(), "the fixture did not fail the splice");
                expect (bytesOf (basePart) == before, "a punch part replaced a base part");
                const auto archives = dir.getChildFile ("Session File Backups")
                    .findChildFiles (juce::File::findFiles, true, "Track_01*.aif");
                expectGreaterThan (archives.size(), 1, "the rolled punch parts were not kept");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Crash recovery takes a rolled punch part off the restored take");
            {
                const auto dir = tempDir ("punch-roll-crash");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                writeAiff (audio.getChildFile ("Track_01.punchbase.aif"), 16, 1, 100);
                writeAiff (audio.getChildFile ("Track_01.aif"), 16, 1, 200);
                writeAiff (audio.getChildFile ("Track_01_part02.aif"), 16, 1, 300);
                audio.getChildFile ("Track_01.punchtmp.aif").replaceWithText ("half a splice");
                expect (MultitrackRecorder::recoverInterruptedPunches (audio));
                expectEquals (lengthOf (audio.getChildFile ("Track_01.aif")), (juce::int64) 100, "the base did not come back");
                expect (! audio.getChildFile ("Track_01_part02.aif").existsAsFile(), "the rolled part stayed on the take");
                expect (! audio.getChildFile ("Track_01.punchtmp.aif").existsAsFile(), "the temp file stayed");
                expectEquals (dir.getChildFile ("Session File Backups")
                                  .findChildFiles (juce::File::findFiles, true, "Track_01*.aif").size(), 2,
                              "the punch's audio was not kept");
                dir.deleteRecursively();
            }

            beginTest ("A base part not yet moved aside when a punch died is never taken off");
            {
                const auto dir = tempDir ("punch-stash-crash");
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                writeAiff (audio.getChildFile ("Track_01.punchbase.aif"), 16, 1, 100);
                writeAiff (audio.getChildFile ("Track_01_part02.aif"), 16, 1, 300);
                expect (MultitrackRecorder::recoverInterruptedPunches (audio));
                expectEquals (lengthOf (audio.getChildFile ("Track_01_part02.aif")), (juce::int64) 300);
                dir.deleteRecursively();
            }
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

    // A real MainComponent over a real socket to an in-process daemon: what
    // the window does when the daemon behind it goes away.
    class CaptureDaemonLinkTests final : public juce::UnitTest
    {
    public:
        CaptureDaemonLinkTests() : UnitTest ("Capture daemon link", "zynforge") {}

        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 256;

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
               #else
                juce::Thread::sleep (10);
               #endif
            }
            return predicate();
        }

        struct Rig
        {
            capture::CaptureDaemon daemon;
            int port { 0 };
            juce::File root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-daemon-link-" + juce::Uuid().toString());
            juce::File session = root.getChildFile ("take");
            std::unique_ptr<MainComponent> main;

            ~Rig()
            {
                if (main != nullptr) main->engine.clearAllStripOverrides();
                main.reset();
                daemon.stop();
                MainComponent::s_testConstruct = false;
                root.deleteRecursively();
            }
        };

        bool bringUp (Rig& rig)
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            rig.root.createDirectory();
            rig.daemon.setTestModeNoDevice (true);
            juce::Random random;
            for (int attempt = 0; attempt < 20 && rig.port == 0; ++attempt)
            {
                const int candidate = 50000 + random.nextInt (9000);
                if (rig.daemon.start (candidate, 1)) rig.port = candidate;
            }
            expect (rig.port != 0);
            if (rig.port == 0) return false;
            rig.daemon.prepareForTests (kRate, kBlock, 1);
            rig.main = std::make_unique<MainComponent>();
            rig.main->engine.clearAllStripOverrides();
            rig.main->engine.setStripCount (1);
            rig.main->engine.prepareForTests (kRate, kBlock);
            rig.main->useCaptureDaemon = true;
            rig.main->captureDaemonPort = rig.port;
            rig.main->connectCaptureDaemon (false);
            expect (rig.main->captureSupervisor.isAttached());
            return rig.main->captureSupervisor.isAttached();
        }

        // Rolling, as far as the window knows: it has seen the daemon's status.
        bool startTake (Rig& rig, const juce::File& session)
        {
            auto& main = *rig.main;
            juce::BigInteger arms; arms.setBit (0);
            expect (main.captureSupervisor.startRecording (session, 1, (int) CaptureFormat::Wav24, arms));
            const bool adopted = pumpUntil ([&]
            {
                main.serviceCaptureDaemonLink();
                return main.engine.isExternalRecording();
            }, 5000);
            expect (adopted, "the window never saw the take");
            std::vector<float> samples ((size_t) kBlock, 0.25f);
            const float* inputs[] { samples.data() };
            for (int b = 0; b < 20; ++b) rig.daemon.processTestBlock (inputs, 1, kBlock);
            return adopted;
        }

        void runTest() override
        {
            beginTest ("A dropped link comes back by itself, and STOP still ends the take");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                main.statusLabel.setText ({}, juce::dontSendNotification);
                rig.daemon.dropLinkForTests();
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.statusLabel.getText().contains ("REATTACHED");
                }, 8000), "the window did not come back to the rolling take");
                expect (rig.daemon.getRecorder().isRecording(), "the take must not have stopped");
                expectEquals (main.recordButton.getButtonText(), juce::String ("STOP"));

                bool done = false;
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expect (pumpUntil ([&] { return done; }, 30000), "STOP never finished");
                // On a busy machine the daemon can drop a late write and take
                // the link with it; the status path then finishes the job.
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return ! main.engine.isRecording() && ! main.captureMetadataPending;
                }, 30000), "the take never came back");
                expect (! rig.daemon.getRecorder().isRecording(), "the daemon is still rolling");
                expect (! main.engine.isRecording());
                expectEquals (main.recordButton.getButtonText(), juce::String ("RECORD"));
            }

            beginTest ("A daemon that is gone ends the take, says so, and can be turned off");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                rig.daemon.stop();
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return ! main.engine.isRecording() && ! main.captureMetadataPending;
                }, 8000), "the window still calls a dead daemon's take rolling");
                expect (main.lastCaptureFinalizationError.contains ("GONE"), main.lastCaptureFinalizationError);
                expectEquals (main.recordButton.getButtonText(), juce::String ("RECORD"));
                expect (! main.captureSupervisor.isDaemonRecording(), "the stale status survived");

                main.toggleCaptureDaemon();
                expect (! main.useCaptureDaemon, "a dead daemon could not be turned off");
            }

            beginTest ("A daemon that will not answer is never called stopped, and has ways out");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                const int port = rig.port;
                rig.daemon.stop();
                // Something holds the port and never completes the handshake.
                juce::StreamingSocket squatter;
                expect (squatter.createListener (port, "127.0.0.1"));
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.daemonLinkLost;
                }, 8000), "the loss was never noticed");
                main.serviceCaptureDaemonLink();
                expect (main.lostDaemonTakeUnknown());
                expect (main.engine.isRecording(), "an unanswered daemon's take was called over");
                expectEquals (main.recordButton.getButtonText(), juce::String ("REC ?"));

                bool done = false, ok = true;
                main.stopActiveCaptureAsync (false, [&] (bool saved) { done = true; ok = saved; });
                expect (pumpUntil ([&] { return done; }, 8000));
                expect (! ok, "STOP claimed a take it could not reach");
                expect (main.engine.isRecording(), "STOP dropped the guard on an unconfirmed take");
                expect (main.statusLabel.getText().contains ("did not confirm"), main.statusLabel.getText());

                main.leaveLostDaemon();
                main.disableCaptureDaemon();
                expect (! main.useCaptureDaemon);
                expect (! main.engine.isRecording(), "walking away must release this window");
            }

            beginTest ("Markers during a daemon take land in that take, whatever was open before");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                const auto previous = rig.root.getChildFile ("previous");
                previous.createDirectory();
                auto& markers = main.engine.getMarkers();
                markers.setContext (previous, kRate);
                markers.drop (100, "Old song");
                const auto before = previous.getChildFile ("markers.json").loadFileAsString();

                if (! startTake (rig, rig.session)) return;
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.engine.captureStatus().source == "daemon";
                }, 5000), "no fresh daemon status");
                expect (main.engine.dropMarkerAtCurrentPosition ("Scene 1") > 0, "the marker was refused");

                expectEquals (previous.getChildFile ("markers.json").loadFileAsString(), before,
                              "the marker went into the previous session");
                expect (rig.session.getChildFile ("markers.json").loadFileAsString().contains ("Scene 1"),
                        "the take has no marker file");

                bool done = false;
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expect (pumpUntil ([&] { return done; }, 8000));
                expect (main.engine.getMarkers().getCount() >= 1, "the marker did not survive STOP");
            }

            beginTest ("A window that reattaches can mark the take it found");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                main.statusLabel.setText ({}, juce::dontSendNotification);
                rig.daemon.dropLinkForTests();
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.statusLabel.getText().contains ("REATTACHED")
                        && main.engine.captureStatus().source == "daemon";
                }, 8000));
                expect (main.engine.dropMarkerAtCurrentPosition ("After the crash") > 0, "the marker was refused");
                expect (rig.session.getChildFile ("markers.json").loadFileAsString().contains ("After the crash"));
            }

            beginTest ("Missed samples and copy problems reach the status line");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                EngineStatus status;
                status.recording = true;
                status.missedSamples = 480;
                status.copyProblems.add ("Mirror /Volumes/NAS lost 2.0 s of audio; the other copies have it");
                main.updateCaptureWarnings (status);
                expect (main.statusLabel.getText().contains ("480 SAMPLES MISSED"), main.statusLabel.getText());
                expect (main.statusLabel.getText().contains ("/Volumes/NAS"), main.statusLabel.getText());
            }

            beginTest ("A take the device ended resets the transport and saves the session");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                main.useCaptureDaemon = false;
                main.captureSupervisor.disconnect();
                auto& rec = main.engine.getRecorder();
                rec.getTrack (0).armed.store (true);
                expect (main.engine.startRecording (rig.session));
                main.recordButton.setButtonText ("STOP");
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                for (int b = 0; b < 20; ++b) rec.processBlock (inputs, 1, kBlock);

                rec.beforeFinalizationForTests = [] { juce::Thread::sleep (300); };
                main.engine.audioDeviceStopped();
                expect (main.engine.isCaptureFinalizing(), "finalization ran on the caller's thread");
                main.onRecordClicked();
                expect (! main.engine.isRecording(), "a press during finalization started a take");

                expect (pumpUntil ([&] { return ! main.engine.isCaptureFinalizing() && ! main.captureMetadataPending; }, 15000),
                        "the interrupted take never finished");
                expectEquals (main.recordButton.getButtonText(), juce::String ("RECORD"));
                expect (main.engine.getPlayer().isLoaded(), "the take was not loaded");
                expect (rig.session.getChildFile ("session_mix.json").existsAsFile(), "the session was not saved");
                expect (main.lastCaptureFinalizationError.contains ("device"), main.lastCaptureFinalizationError);
            }

            beginTest ("STOP keeps the window alive while the daemon closes its files");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                std::atomic<bool> releaseStop { false };
                rig.daemon.getRecorder().beforeFinalizationForTests = [&]
                {
                    while (! releaseStop.load()) juce::Thread::sleep (5);
                };
                bool done = false;
                const auto started = juce::Time::getMillisecondCounterHiRes();
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 2000.0, "STOP held the message thread");
                expect (! done);
                expect (main.engine.isRecording(), "the take was called stopped before the daemon said so");
                expect (main.captureMetadataPending);
                main.onStopClicked();
                expect (main.statusLabel.getText().contains ("pending"), main.statusLabel.getText());

                releaseStop.store (true);
                expect (pumpUntil ([&] { return done; }, 30000), "STOP never finished");
                expect (! main.engine.isRecording());
                expect (main.engine.getPlayer().isLoaded(), "the take was not read back");
                expect (rig.session.getChildFile ("session_mix.json").existsAsFile(), "the session was not saved");
            }

            beginTest ("A RECORD with no answer is held, and adopted when the daemon rolls");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                std::atomic<bool> releaseStart { false };
                rig.daemon.beforeStartForTests = [&] { while (! releaseStart.load()) juce::Thread::sleep (5); };
                main.captureSupervisor.startReplyTimeoutMs = 50;
                juce::BigInteger arms; arms.setBit (0);
                main.requestDaemonTake (rig.session, 1, arms, 1);
                expect (main.engine.isRecording(), "no answer must not read as a failed RECORD");
                expectEquals (main.recordButton.getButtonText(), juce::String ("REC ?"));
                releaseStart.store (true);
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.recordButton.getButtonText() == "STOP";
                }, 10000), "the take that did start was never adopted");
                expect (rig.daemon.getRecorder().isRecording());
                expectEquals (main.engine.getActiveSessionDir().getFullPathName(), rig.session.getFullPathName(),
                              "the window lost the session the daemon is writing");
            }

            beginTest ("A daemon refusal keeps its reason");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                rig.daemon.getRecorder().getTrack (0).armed.store (true);
                juce::BigInteger arms; arms.setBit (0);
                main.requestDaemonTake (juce::File(), 1, arms, 1);
                expect (main.statusLabel.getText().contains ("no sessionDir"), main.statusLabel.getText());
                expect (! main.engine.isRecording());
            }

            beginTest ("Reattaching to a rolling take opens no dialog over it");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                const int before = juce::ModalComponentManager::getInstance()->getNumModalComponents();
                main.showStartupWelcome();
                expectEquals (juce::ModalComponentManager::getInstance()->getNumModalComponents(), before,
                              "a dialog opened over the rolling take");
            }

            beginTest ("A daemon that cannot be reached at launch says why");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                const int port = rig.port;
                main.captureSupervisor.disconnect();
                rig.daemon.stop();
                juce::StreamingSocket squatter;
                expect (squatter.createListener (port, "127.0.0.1"));
                main.useCaptureDaemon = true;
                main.connectCaptureDaemon (true);
                expect (! main.useCaptureDaemon);
                expect (main.statusLabel.getText().contains ("no crash protection"), main.statusLabel.getText());
                expect (main.captureSupervisor.lastConnectError.contains (juce::String (port)),
                        main.captureSupervisor.lastConnectError);
            }

            beginTest ("PUNCH mode ends at the out point however late the timer runs");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                main.useCaptureDaemon = false;
                main.captureSupervisor.disconnect();
                auto& engine = main.engine;
                auto& rec = engine.getRecorder();
                auto& player = engine.getPlayer();
                rec.getTrack (0).armed.store (true);
                engine.setTrackInputRouting (0, 0);
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                juce::AudioBuffer<float> out (2, kBlock);
                const auto callback = [&]
                { engine.audioDeviceIOCallbackWithContext (inputs, 1, out.getArrayOfWritePointers(), 2, kBlock, {}); };

                expect (engine.startRecording (rig.session));
                for (int b = 0; b < 400; ++b) { callback(); rec.drainPendingForTests(); }
                engine.stopRecording();
                expect (player.isLoaded());

                const juce::int64 in = 100 * kBlock, outPoint = 200 * kBlock;
                player.setLoopRegion (in, outPoint);
                engine.setPunchModeOn (true);
                engine.setTrackPunchArmed (0, true);
                player.setPositionSamples (90 * kBlock);
                engine.startPlayback();
                std::fill (samples.begin(), samples.end(), 0.75f);
                while (player.getPositionSamples() < in + kBlock) callback();
                main.servicePunch();
                expect (engine.isRecording(), "the punch never started");
                // The timer stalls well past the out point.
                while (player.getPositionSamples() < 260 * kBlock)
                {
                    callback();
                    rec.drainPendingForTests();
                }
                main.servicePunch();
                expect (pumpUntil ([&] { return ! engine.isRecording() && ! engine.isCaptureFinalizing()
                                             && ! main.captureMetadataPending; }, 15000));
                engine.setPunchModeOn (false);

                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (
                    rig.session.getChildFile ("Audio Files").getChildFile ("Track_01.wav")));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> take (1, (int) reader->lengthInSamples);
                    reader->read (&take, 0, take.getNumSamples(), 0, true, false);
                    expect (std::abs (take.getSample (0, 150 * kBlock) - 0.75f) < 0.001f, "the punch did not land");
                    expect (std::abs (take.getSample (0, (int) outPoint - 1) - 0.75f) < 0.001f, "the punch ended early");
                    expect (std::abs (take.getSample (0, (int) outPoint) - 0.25f) < 0.001f, "the punch ran past the out point");
                    expect (std::abs (take.getSample (0, 230 * kBlock) - 0.25f) < 0.001f, "audio after the out point was replaced");
                }
            }

            beginTest ("A punch-in a save ran through is reported, not skipped in silence");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                main.useCaptureDaemon = false;
                main.captureSupervisor.disconnect();
                auto& engine = main.engine;
                auto& rec = engine.getRecorder();
                auto& player = engine.getPlayer();
                rec.getTrack (0).armed.store (true);
                expect (engine.startRecording (rig.session));
                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                for (int b = 0; b < 400; ++b) { rec.processBlock (inputs, 1, kBlock); rec.drainPendingForTests(); }
                engine.stopRecording();

                player.setLoopRegion (100 * kBlock, 200 * kBlock);
                engine.setPunchModeOn (true);
                engine.setTrackPunchArmed (0, true);
                engine.startPlayback();
                main.sessionIoBusy.store (true);
                player.setPositionSamples (150 * kBlock);
                main.servicePunch();
                expect (! engine.isRecording());
                player.setPositionSamples (250 * kBlock);
                main.servicePunch();
                main.sessionIoBusy.store (false);
                engine.stopPlayback();
                engine.setPunchModeOn (false);
                expect (main.statusLabel.getText().contains ("Punch-in missed"), main.statusLabel.getText());
            }

            beginTest ("Auto-save stays off the take's drive until the take ends");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                main.engine.getAppProps()->setValue ("autosaveMinutes", 1);
                main.lastAutosaveMs = juce::Time::getMillisecondCounter() - 120000u;
                main.serviceAutosave();
                expectEquals (main.pendingMetadataSaves, 0, "auto-save ran during a take");
                expect (! main.sessionIoBusy.load(), "M is refused during a take");
                expect (! rig.session.getChildFile ("session_mix.json").existsAsFile(), "a save reached the take volume");
            }

            beginTest ("A take the daemon ended by itself comes back to the window");
            {
                Rig rig;
                if (! bringUp (rig) || ! startTake (rig, rig.session)) return;
                auto& main = *rig.main;
                rig.daemon.audioDeviceStopped();
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return ! main.engine.isRecording() && ! main.captureMetadataPending;
                }, 8000), "the window never noticed the take had ended");
                expectEquals (main.recordButton.getButtonText(), juce::String ("RECORD"));
                expect (main.lastCaptureFinalizationError.contains ("device"), main.lastCaptureFinalizationError);
                expect (main.engine.getPlayer().isLoaded(), "the ended take was not loaded");
            }
        }
    };

    static CaptureDaemonLinkTests captureDaemonLinkTests;

    // Each copy of a take has its own thread and ring. These hold one copy's
    // thread in a write, the way a dead mount would, and watch the others.
    class CaptureCopyTests final : public juce::UnitTest
    {
    public:
        CaptureCopyTests() : UnitTest ("Capture copies", "zynforge") {}

        static constexpr double kRate = 48000.0;
        static constexpr int kBlock = 256;

        struct Stall
        {
            explicit Stall (int index) : copyIndex (index) {}
            ~Stall() { released.signal(); }
            int copyIndex;
            juce::WaitableEvent entered, released { true };
        };
        using Copy = MultitrackRecorder::CaptureCopy;

        static void hold (MultitrackRecorder& rec, const std::shared_ptr<Stall>& stall)
        {
            rec.beforeCopyWriteForTests = [stall] (Copy& copy)
            {
                if (copy.copyIndex != stall->copyIndex) return;
                stall->entered.signal();
                stall->released.wait (-1);
            };
        }

        struct Dirs
        {
            juce::File root, session, backup, mirror;
            explicit Dirs (const juce::File& base = juce::File::getSpecialLocation (juce::File::tempDirectory))
                : root (base.getChildFile ("zf-copies-" + juce::Uuid().toString())),
                  session (root.getChildFile ("primary").getChildFile ("take")),
                  backup (root.getChildFile ("backup")), mirror (root.getChildFile ("mirror"))
            {
                session.createDirectory(); backup.createDirectory(); mirror.createDirectory();
            }
            ~Dirs()
            {
                MultitrackRecorder::deleteSessionAfterCancellingReports (session);
                root.deleteRecursively();
            }
            juce::File primaryTake (const juce::String& ext = ".wav") const
            { return session.getChildFile ("Audio Files").getChildFile ("Track_01" + ext); }
            juce::File backupTake (const juce::String& ext = ".wav") const
            { return backup.getChildFile ("take").getChildFile ("Audio Files").getChildFile ("Track_01" + ext); }
            juce::File mirrorTake (const juce::String& ext = ".wav") const
            { return mirror.getChildFile ("take").getChildFile ("Audio Files").getChildFile ("Track_01" + ext); }
        };

        static float sampleAt (juce::int64 frame, int channel) noexcept
        {
            return 0.4f * (float) std::sin ((double) frame * (0.01 + 0.002 * channel));
        }

        // Audio as the callback delivers it, every frame identifiable. The
        // recorder's own counters are the only thing that decides the pace.
        struct Feeder
        {
            MultitrackRecorder& rec;
            int channels;
            juce::int64 frame { 0 };

            void block()
            {
                std::vector<std::vector<float>> audio ((size_t) channels, std::vector<float> ((size_t) kBlock));
                std::vector<const float*> inputs;
                for (int c = 0; c < channels; ++c)
                {
                    for (int i = 0; i < kBlock; ++i) audio[(size_t) c][(size_t) i] = sampleAt (frame + i, c);
                    inputs.push_back (audio[(size_t) c].data());
                }
                rec.processBlock (inputs.data(), channels, kBlock);
                frame += kBlock;
            }

            // The named copies stay caught up; a held one cannot.
            void blocks (int count, const std::vector<int>& keepUp)
            {
                for (int b = 0; b < count; ++b)
                {
                    block();
                    rec.drainOnce();
                    for (const int index : keepUp)
                        for (auto& copy : rec.copies)
                            if (copy->copyIndex == index) copy->waitUntilWritten (5000);
                }
            }
        };

        static juce::AudioBuffer<float> read (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr) return {};
            juce::AudioBuffer<float> out ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&out, 0, out.getNumSamples(), 0, true, true);
            return out;
        }

        // Every frame is the frame played at that position.
        void expectIntact (const juce::File& file, juce::int64 frames, const juce::String& what,
                           int channel = 0, int fileChannel = 0)
        {
            const auto audio = read (file);
            expectEquals ((juce::int64) audio.getNumSamples(), frames, what + ": wrong length");
            if (audio.getNumSamples() != frames) return;
            int wrong = 0;
            for (int i = 0; i < audio.getNumSamples(); ++i)
                if (std::abs (audio.getSample (fileChannel, i) - sampleAt (i, channel)) > 0.0002f) ++wrong;
            expectEquals (wrong, 0, what + ": frames differ from what was played");
        }

        void runTest() override
        {
            stalledCopies();
            leftBehindAtStop();
            unwritableCopies();
            everyCopyMatches();
            ringMemory();
            stuckDriveReadings();
        }

        void stalledCopies()
        {
            for (const int held : { 2, 1, 0 })
            {
                const auto name = juce::String (held == 0 ? "primary" : held == 1 ? "backup" : "mirror");
                beginTest ("A stalled " + name + " costs only that copy");
                Dirs dirs;
                auto rec = std::make_unique<MultitrackRecorder>();
                rec->prepare (kRate, kBlock, 1);
                rec->getTrack (0).armed.store (true);
                rec->setBackupDirectory (dirs.backup);
                expect (rec->setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                rec->copyRingFramesForTests = 4096;
                auto stall = std::make_shared<Stall> (held);
                hold (*rec, stall);
                expect (rec->startRecording (dirs.session));
                expectEquals ((int) rec->copies.size(), 3);

                Feeder feeder { *rec, 1 };
                feeder.block();
                rec->drainOnce();
                expect (stall->entered.wait (5000), "the copy never reached its write");
                feeder.blocks (79, held == 0 ? std::vector<int> { 1, 2 }
                                 : held == 1 ? std::vector<int> { 0, 2 } : std::vector<int> { 0, 1 });
                const juce::int64 total = 80 * kBlock;

                expectEquals (rec->getMissedSamples(), (juce::int64) 0, "the audio FIFO dropped: every copy lost it");
                expect (held == 0 ? rec->hasPrimaryFailed() : ! rec->hasPrimaryFailed(), "primary failure latch");
                expect (held == 1 ? rec->hasBackupFailed()  : ! rec->hasBackupFailed(),  "backup failure latch");
                expect (held == 2 ? rec->anyMirrorFailed()  : ! rec->anyMirrorFailed(),  "mirror failure latch");
                const auto problems = rec->getCopyProblems();
                expectEquals (problems.size(), 1, "the banner has no named copy");
                if (problems.size() == 1)
                    expect (problems[0].containsIgnoreCase (held == 2 ? dirs.mirror.getFullPathName() : name), problems[0]);

                stall->released.signal();
                rec->stopRecording();

                for (const int index : { 0, 1, 2 })
                {
                    const auto file = index == 0 ? dirs.primaryTake() : index == 1 ? dirs.backupTake() : dirs.mirrorTake();
                    if (index != held) { expectIntact (file, total, "a healthy copy"); continue; }

                    // What the ring kept lands where it was played; the rest is
                    // silence, so this copy still lines up with the others.
                    const auto audio = read (file);
                    expectEquals ((juce::int64) audio.getNumSamples(), total, "the held copy lost its alignment");
                    if (audio.getNumSamples() != total) continue;
                    expect (std::abs (audio.getSample (0, (int) total - 1) - sampleAt (total - 1, 0)) < 0.0002f,
                            "the tail of the held copy moved");
                    expectEquals (audio.getSample (0, 40 * kBlock), 0.0f, "the lost span is not silence");
                }

                const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expect ((bool) report[held == 0 ? "primaryFailed" : held == 1 ? "backupFailed" : "mirrorFailed"],
                        "the report does not flag the copy");
                const auto* listed = report["copyProblems"].getArray();
                expect (listed != nullptr && listed->size() == 1, "the report does not name the copy");
            }
        }

        void leftBehindAtStop()
        {
            beginTest ("A copy that never comes back does not hold up STOP");
            {
                Dirs dirs;
                auto rec = std::make_unique<MultitrackRecorder>();
                rec->prepare (kRate, kBlock, 1);
                rec->getTrack (0).armed.store (true);
                expect (rec->setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                rec->copyStallLimitMs = 200.0;
                auto stall = std::make_shared<Stall> (2);
                hold (*rec, stall);
                expect (rec->startRecording (dirs.session));
                Feeder feeder { *rec, 1 };
                feeder.blocks (1, { 0 });
                expect (stall->entered.wait (5000));
                feeder.blocks (19, { 0 });
                std::weak_ptr<Copy> leftBehind = rec->copies.back();

                const auto started = juce::Time::getMillisecondCounterHiRes();
                rec->stopRecording();
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 20000.0, "STOP waited on the dead copy");

                expectIntact (dirs.primaryTake(), 20 * kBlock, "the primary");
                expect (rec->anyMirrorFailed());
                const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expect ((bool) report["mirrorFailed"], "the report does not flag the mirror");
                expect (report["copyProblems"].toString().isNotEmpty() || report["copyProblems"].size() == 1,
                        "the report does not name the mirror");
                auto mirrors = report["tracks"][0]["mirrors"];
                expect (mirrors.size() == 1 && (bool) mirrors[0]["failed"], "the track does not list the mirror as failed");

                // The next take on the same recorder must work with that
                // thread still stuck, and the recorder may go first.
                rec->beforeCopyWriteForTests = {};
                rec->armContinue (20 * kBlock);
                expect (rec->startRecording (dirs.session), rec->getStartError());
                expectEquals (rec->getMirrorsSkippedAtStart(), 1, "the stuck mirror was not left out");
                expect (rec->getCopyProblems().joinIntoString (" ").contains (dirs.mirror.getFullPathName()),
                        "the stuck mirror is not named");
                Feeder next { *rec, 1 };
                next.blocks (4, { 0 });
                rec->stopRecording();
                expect (dirs.session.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav").existsAsFile(),
                        "the second take did not land");
                expect (! leftBehind.expired(), "the fixture released the stuck thread too early");
                rec.reset();

                stall->released.signal();
                const auto end = juce::Time::getMillisecondCounterHiRes() + 10000.0;
                while (! leftBehind.expired() && juce::Time::getMillisecondCounterHiRes() < end)
                    juce::Thread::sleep (5);
                expect (leftBehind.expired(), "the stuck thread never ended");
            }

            beginTest ("A primary that stalls at STOP is waited for, never left behind");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.copyStallLimitMs = 50.0;
                auto stall = std::make_shared<Stall> (0);
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.block();
                rec.drainOnce();
                expect (stall->entered.wait (5000));
                std::atomic<bool> stopped { false };
                std::thread stopper ([&] { rec.stopRecording(); stopped.store (true); });
                juce::Thread::sleep (400);
                expect (! stopped.load(), "STOP walked away from the primary");
                stall->released.signal();
                stopper.join();
                expectIntact (dirs.primaryTake(), kBlock, "the primary");
            }
        }

        void unwritableCopies()
        {
            for (const bool useBackup : { false, true })
            {
                beginTest (juce::String ("A read-only ") + (useBackup ? "backup" : "mirror")
                           + " does not refuse the take, and is named");
                Dirs dirs (juce::File ("/private/tmp"));   // the test home may have no permissions to take away
                const auto record = [&] (bool continuing)
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    if (useBackup) rec.setBackupDirectory (dirs.backup);
                    else           expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                    if (continuing) rec.armContinue (10 * kBlock);
                    expect (rec.startRecording (dirs.session), rec.getStartError());
                    Feeder feeder { rec, 1 };
                    feeder.blocks (10, { 0 });
                    const auto problems = rec.getCopyProblems();
                    rec.stopRecording();
                    return problems;
                };
                expect (record (false).isEmpty(), "a healthy copy was reported");

                const auto copy = (useBackup ? dirs.backup : dirs.mirror).getChildFile ("take");
                expect (copy.setReadOnly (true, false));
                const auto problems = record (true);
                copy.setReadOnly (false, false);

                expectEquals (problems.size(), 1, "the dropped copy is not named");
                expect (problems.joinIntoString (" ").contains ((useBackup ? dirs.backup : dirs.mirror).getFullPathName()));
                expect (dirs.session.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav").existsAsFile(),
                        "the primary did not record");
                expect (! copy.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav").existsAsFile(),
                        "the dropped copy was written");
            }

            beginTest ("A folder that holds another session's audio still refuses the take");
            {
                Dirs dirs;
                const auto foreign = dirs.mirror.getChildFile ("take").getChildFile ("Audio Files");
                foreign.createDirectory();
                foreign.getChildFile ("Track_01.wav").replaceWithText ("someone else's take");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                expect (! rec.startRecording (dirs.session), "a foreign copy was adopted");
                expect (rec.getStartError().contains ("ownership"), rec.getStartError());
                expectEquals (foreign.getChildFile ("Track_01.wav").loadFileAsString(), juce::String ("someone else's take"));
            }
        }

        void everyCopyMatches()
        {
            beginTest ("Pre-roll and live audio reach every copy in every format, in step");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 3);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (1).armed.store (true);
                rec.getTrack (0).isStereo.store (true);
                rec.getTrack (2).armed.store (true);
                rec.setCaptureFormat (CaptureFormat::Wav24);
                rec.setBackupDirectory (dirs.backup);
                rec.setBackupCaptureFormat (CaptureFormat::Flac24);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Aiff24 } }));

                Feeder feeder { rec, 3 };
                for (int b = 0; b < 60; ++b) feeder.block();   // history before RECORD
                expect (rec.startRecording (dirs.session));
                for (int b = 0; b < 40; ++b) { feeder.block(); rec.drainPendingForTests(); }
                rec.stopRecording();

                const auto total = feeder.frame;
                for (const auto& [file, name] : { std::pair { dirs.primaryTake(), "primary" },
                                                  std::pair { dirs.backupTake (".flac"), "backup" },
                                                  std::pair { dirs.mirrorTake (".aif"), "mirror" } })
                {
                    expectIntact (file, total, name, 0, 0);
                    expectIntact (file, total, name, 1, 1);
                    expectIntact (file.getSiblingFile ("Track_03" + file.getFileExtension()), total, name, 2, 0);
                }
                expectEquals (rec.getMissedSamples(), (juce::int64) 0);
                expect (! rec.hasPrimaryFailed() && ! rec.hasBackupFailed() && ! rec.anyMirrorFailed());
            }

            beginTest ("Thirty-two tracks on three copies stay exact at live pace");
            {
                constexpr int kTracks = 32, kBlocks = 300;
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, kTracks);
                for (int t = 0; t < kTracks; ++t) rec.getTrack (t).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, kTracks };
                for (int b = 0; b < kBlocks; ++b)
                {
                    feeder.block();
                    juce::Thread::sleep (3);
                }
                rec.stopRecording();
                expectEquals (rec.getMissedSamples(), (juce::int64) 0);
                for (const auto& take : { dirs.primaryTake(), dirs.backupTake(), dirs.mirrorTake() })
                    for (int t = 0; t < kTracks; ++t)
                        expectIntact (take.getSiblingFile (juce::String::formatted ("Track_%02d.wav", t + 1)),
                                      feeder.frame, take.getFullPathName(), t);
            }

            beginTest ("A copy stuck in its pre-roll holds up nothing else");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                auto stall = std::make_shared<Stall> (2);
                hold (rec, stall);

                Feeder feeder { rec, 1 };
                for (int b = 0; b < 60; ++b) feeder.block();
                expect (rec.startRecording (dirs.session));
                expect (stall->entered.wait (5000), "the mirror never reached its dump");
                feeder.blocks (40, { 0 });
                expectEquals (rec.copies[0]->framesDone.load(), rec.copies[0]->framesPushed.load(),
                              "the primary waited on the mirror's dump");
                stall->released.signal();
                rec.stopRecording();
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                expectIntact (dirs.mirrorTake(), feeder.frame, "the mirror");
            }
        }

        void stuckDriveReadings()
        {
            beginTest ("A stuck drive shows in the readings while it is stuck");
            Dirs dirs;
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.copyRingFramesForTests = 4096;
            auto stall = std::make_shared<Stall> (0);
            hold (rec, stall);
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec, 1 };
            feeder.block();
            rec.drainOnce();
            expect (stall->entered.wait (5000));

            const auto expected = rec.estimateBytesPerSecondForArmedTracks();
            const auto end = juce::Time::getMillisecondCounterHiRes() + 20000.0;
            while (! rec.isDiskStruggling() && juce::Time::getMillisecondCounterHiRes() < end)
            {
                feeder.block();
                rec.drainOnce();
                rec.updateDiskHealth (expected);
                juce::Thread::sleep (20);
            }
            expect (rec.isDiskStruggling(), "the warning never fired during the stall");
            expectEquals (rec.getDiskBytesPerSec(), 0.0f, "throughput held its last healthy value");
            expectGreaterThan (rec.getRingFillPct(), 0.0f, "the ring reading did not move");
            stall->released.signal();
            rec.stopRecording();
        }

        void ringMemory()
        {
            beginTest ("Copy rings stay bounded at any channel count");
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            const auto bytes = [&] (int copies, int channels)
            { return (juce::int64) rec.copyRingFrames (copies, channels) * 4 * copies * channels; };
            expectEquals (rec.copyRingFrames (3, 64), 1 << 18, "a normal rig gets the full ring");
            expectLessOrEqual (bytes (3, 64), (juce::int64) 1 << 30);
            expectLessOrEqual (bytes (5, 256), (juce::int64) 1 << 30);
            expectGreaterOrEqual (rec.copyRingFrames (5, 256), (int) kRate, "at least a second of ring");
        }
    };

    static CaptureCopyTests captureCopyTests;
}
