// Capture-path regressions: what a crash leaves on disk, what a punch writes,
// how a lost daemon behaves, and how copies stay out of each other's way.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AiffRepair.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/FlacRepair.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/PreflightProbes.h"
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
            flacCrashSurvivor();
            hashesFinishBeforeAMove();
            refusedStarts();
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

            beginTest ("Closing the recorder waits for a slow write instead of killing its thread");
            {
                struct SlowClient final : public juce::TimeSliceClient
                {
                    int useTimeSlice() override
                    {
                        started.store (true);
                        juce::Thread::sleep (2500);
                        finished.store (true);
                        return -1;
                    }
                    std::atomic<bool> started { false }, finished { false };
                } slow;
                {
                    MultitrackRecorder rec;
                    expect (! rec.writerThreads.empty());
                    rec.writerThreads.front()->addTimeSliceClient (&slow);
                    for (int i = 0; i < 500 && ! slow.started.load(); ++i)
                        juce::Thread::sleep (10);
                    expect (slow.started.load(), "the writer thread never ran");
                }
                expect (slow.finished.load(), "the writer thread was killed mid-write");
            }

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

        void flacCrashSurvivor()
        {
            const auto totalIn = [] (const juce::File& file)
            {
                juce::MemoryBlock head;
                file.loadFileAsData (head);
                const auto* b = static_cast<const juce::uint8*> (head.getData());
                return ((juce::int64) (b[21] & 0x0f) << 32) | ((juce::int64) b[22] << 24)
                     | ((juce::int64) b[23] << 16) | ((juce::int64) b[24] << 8) | (juce::int64) b[25];
            };
            beginTest ("A FLAC take a crash left opens without being decoded");
            {
                const auto dir = tempDir ("flac");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setCaptureFormat (CaptureFormat::Flac24);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (dir));
                feedRamp (rec, 0, 400);

                const auto survivor = dir.getChildFile ("Track_01.flac");
                expect (dir.getChildFile ("Audio Files").getChildFile ("Track_01.flac").copyFileTo (survivor));
                rec.stopRecording();
                expectEquals (totalIn (survivor), (juce::int64) 0, "the fixture already has a length");

                const auto scanned = lengthOf (survivor);   // JUCE's own whole-file scan
                expectGreaterThan (scanned, (juce::int64) 0);
                expect (MultitrackRecorder::recoverInterruptedPunches (dir), "the open failed");
                expectEquals (totalIn (survivor), scanned, "the open did not store the length");
                const auto audio = samplesOf (survivor);
                expectEquals ((juce::int64) audio.size(), scanned);
                int wrong = 0;
                for (int i = 0; i < (int) audio.size(); ++i)
                    if (std::abs (audio[(size_t) i] - rampAt (i)) > 0.0002f) ++wrong;
                expectEquals (wrong, 0, "the repaired file reads differently");

                const auto before = bytesOf (survivor);
                expect (flacrepair::repairStaleLength (survivor) == flacrepair::Result::untouched);
                expect (bytesOf (survivor) == before, "a finished length was rewritten");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A torn last FLAC frame is left out of the length");
            {
                const auto dir = tempDir ("flac-torn");
                const auto file = dir.getChildFile ("Track_01.flac");
                {
                    auto stream = file.createOutputStream();
                    std::unique_ptr<juce::AudioFormatWriter> writer (
                        juce::FlacAudioFormat().createWriterFor (stream.get(), kRate, 2, 24, {}, 5));
                    stream.release();
                    writeRamp (*writer, 2, 40000);
                }
                const auto whole = bytesOf (file);
                juce::MemoryBlock cut (whole.getData(), whole.getSize() - 100);
                for (int b : { 21, 22, 23, 24, 25 })
                    static_cast<juce::uint8*> (cut.getData())[b] = b == 21 ? (juce::uint8) (static_cast<const juce::uint8*> (whole.getData())[21] & 0xf0) : (juce::uint8) 0;
                file.replaceWithData (cut.getData(), cut.getSize());

                expect (flacrepair::repairStaleLength (file) == flacrepair::Result::repaired);
                const auto repaired = lengthOf (file);
                expectGreaterThan (repaired, (juce::int64) 30000);
                expectLessThan (repaired, (juce::int64) 40000);
                expectEquals (repaired % 4096, (juce::int64) 0, "the length is not a whole number of frames");
                expectEquals ((int) samplesOf (file).size(), (int) repaired);
                dir.deleteRecursively();
            }

            beginTest ("FLAC repair leaves other files alone");
            {
                const auto dir = tempDir ("flac-none");
                const auto file = dir.getChildFile ("Track_01.flac");
                file.replaceWithText ("fLaC, but not really a FLAC file at all, and long enough to read a header from");
                const auto before = bytesOf (file);
                expect (flacrepair::repairStaleLength (file) == flacrepair::Result::untouched);
                expect (bytesOf (file) == before);
                dir.deleteRecursively();
            }
        }

        void hashesFinishBeforeAMove()
        {
            beginTest ("A move waits for the integrity hashes, and can be cancelled");
            const auto dir = tempDir ("hash-wait");
            juce::WaitableEvent held;
            std::atomic<bool> cancel { false }, returned { false };
            auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
            hooks->beforeScan = [&held] { held.wait (20000); };
            {
                MultitrackRecorder rec;
                rec.reportTestHooks = hooks;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (dir));
                feed (rec, 10, 1);
                rec.stopRecording();
            }

            cancel.store (true);
            expect (! MultitrackRecorder::waitForReportHashes (dir, cancel), "a raised cancel did not end the wait");
            cancel.store (false);

            std::thread mover ([&]
            {
                MultitrackRecorder::waitForReportHashes (dir, cancel);
                returned.store (true);
            });
            juce::Thread::sleep (300);
            expect (! returned.load(), "the wait did not hold for the running scan");
            held.signal();
            mover.join();
            expect (returned.load());
            const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
            expect (! (bool) report["sha256Pending"], "the report was still pending after the wait");
            MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
        }

        void refusedStarts()
        {
            beginTest ("A refused start keeps the last take's failure on screen");
            {
                const auto dir = tempDir ("refused");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (dir));
                feed (rec, 4, 1);
                rec.stopRecording();
                rec.latches->primaryFailed.store (true);   // what a failed take leaves
                // A fresh take cannot replace the existing one.
                expect (! rec.startRecording (dir), "the fixture should refuse");
                expect (rec.hasPrimaryFailed(), "the refusal cleared the banner");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A refused continue leaves no new files on any copy");
            {
                const auto dir = tempDir ("refused-parts");
                const auto audio = dir.getChildFile ("Audio Files");
                const auto arm = [] (MultitrackRecorder& rec)
                {
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (0).armed.store (true);
                    rec.getTrack (1).armed.store (true);
                };
                {
                    MultitrackRecorder rec;
                    arm (rec);
                    expect (rec.startRecording (dir));
                    feed (rec, 4, 2);
                    rec.stopRecording();
                }
                // Track 2's next part cannot open: its name is taken.
                expect (audio.getChildFile ("Track_02_part02.wav").createDirectory().wasOk());
                MultitrackRecorder rec;
                arm (rec);
                rec.armContinue (4 * kBlock);
                expect (! rec.startRecording (dir), "the fixture should refuse");
                expect (! audio.getChildFile ("Track_01_part02.wav").exists(), "a refused start left a part file");
                audio.getChildFile ("Track_02_part02.wav").deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A drive with nothing free is full, not unknown");
            expect (preflight::freeSpaceKnown (0, 1000), "a full volume read as unknown");
            expect (! preflight::freeSpaceKnown (0, 0), "a failed query read as full");
            expect (preflight::freeSpaceKnown (500, 1000));
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
                expect (main.daemonTakeAdopted && main.engine.isRecording(), "the window lost the rolling take");

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
                expect (! main.daemonTakeAdopted, "the window still holds the ended take");
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
                expect (! main.daemonTakeAdopted, "the window still holds the ended take");
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
                expect (main.daemonLinkLost, "an unanswered take must stay unconfirmed");

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

                // The write goes behind the take, on its own thread.
                expect (pumpUntil ([&]
                {
                    return rig.session.getChildFile ("markers.json").loadFileAsString().contains ("Scene 1");
                }, 5000), "the take has no marker file");
                expectEquals (previous.getChildFile ("markers.json").loadFileAsString(), before,
                              "the marker went into the previous session");

                bool done = false;
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expect (pumpUntil ([&] { return done; }, 8000));
                expect (main.engine.getMarkers().getCount() >= 1, "the marker did not survive STOP");
            }

            beginTest ("A drive that stops answering cannot hold the daemon or its STOP");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                // The probe's thread is detached and outlives this test, so it
                // must own what it waits on.
                auto released = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard letGo { [released] { released->signal(); } };
                auto& recorder = rig.daemon.getRecorder();
                recorder.volumeProbePeriodMs = 10;
                recorder.volumeStaleAfterMs = 200.0;
                recorder.volumeReaderForTests = [released] (const juce::File&)
                {
                    released->wait (-1);
                    return VolumeProbe::Reading();
                };
                if (! startTake (rig, rig.session)) return;

                // Status can only keep arriving if nothing under the daemon's
                // command lock is waiting on the drive.
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    const auto status = main.engine.captureStatus();
                    return status.source == "daemon" && status.primaryDiskStale;
                }, 8000), "the stale reading never reached the window");
                const auto status = main.engine.captureStatus();
                expectEquals (status.minutesRemaining, 0, "a stale drive still showed a number");
                expectEquals (status.primaryFreeBytes, (juce::int64) -1);

                main.updateCaptureWarnings (status);
                expect (main.alarmBanner.getText().contains ("PRIMARY DRIVE NOT ANSWERING"),
                        main.alarmBanner.getText());

                bool done = false;
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expect (pumpUntil ([&] { return done; }, 8000), "STOP waited on the dead drive");
            }

            beginTest ("A hole in the primary reaches the window from the daemon");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                auto& recorder = rig.daemon.getRecorder();
                recorder.copyRingFramesForTests = MultitrackRecorder::CaptureCopy::kChunkFrames;
                juce::WaitableEvent released { true };
                const juce::ScopeGuard letGo { [&released] { released.signal(); } };
                recorder.beforeCopyWriteForTests = [&released] (MultitrackRecorder::CaptureCopy& copy)
                {
                    if (copy.copyIndex == 0) released.wait (-1);
                };
                if (! startTake (rig, rig.session)) return;

                std::vector<float> samples ((size_t) kBlock, 0.25f);
                const float* inputs[] { samples.data() };
                expect (pumpUntil ([&]
                {
                    for (int b = 0; b < 200; ++b) rig.daemon.processTestBlock (inputs, 1, kBlock);
                    main.serviceCaptureDaemonLink();
                    const auto status = main.engine.captureStatus();
                    // The audio FIFO can overflow before the copy's ring does, so a
                    // missed sample alone doesn't mean the copy has said anything yet.
                    return status.source == "daemon" && status.missedSamples > 0
                        && status.copyProblems.joinIntoString ("|").contains ("no other copy has it");
                }, 8000), "the daemon called a primary with a hole clean");
                released.signal();
                bool done = false;
                main.stopActiveCaptureAsync (false, [&] (bool) { done = true; });
                expect (pumpUntil ([&] { return done; }, 8000));
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
                expect (pumpUntil ([&]
                {
                    return rig.session.getChildFile ("markers.json").loadFileAsString().contains ("After the crash");
                }, 5000), "the marker never reached the take's folder");
            }

            beginTest ("Missed samples and copy problems reach the banner");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                EngineStatus status;
                status.recording = true;
                status.missedSamples = 480;
                main.updateCaptureWarnings (status);
                expect (main.statusLabel.getText().contains ("480 SAMPLES MISSED"), main.statusLabel.getText());
                expect (main.alarmBanner.isVisible(), "missed samples alone did not raise the banner");
                expect (main.alarmBanner.getText().contains ("480 SAMPLES MISSED"), main.alarmBanner.getText());
                expect (main.alarmBanner.getLevel() == AlarmBanner::Level::Danger, "missed samples are not a danger");

                status.missedSamples = 0;
                status.copyProblems.add ("Mirror /Volumes/NAS lost 2.0 s of audio; the other copies have it");
                main.updateCaptureWarnings (status);
                expect (main.statusLabel.getText().contains ("/Volumes/NAS"), main.statusLabel.getText());
                expect (main.alarmBanner.getText().contains ("/Volumes/NAS"), main.alarmBanner.getText());
                expect (main.alarmBanner.getLevel() == AlarmBanner::Level::Danger, "a copy losing audio is not a danger");

                status.copyProblems.clear();
                main.updateCaptureWarnings (status);
                expect (! main.alarmBanner.isVisible(), "the banner outlived the condition");
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
                expect (! main.engine.isRecording(), "the interrupted take still reads as rolling");
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
                main.requestDaemonTake (rig.session, 1, arms, 1, true);
                expect (main.engine.isRecording(), "no answer must not read as a failed RECORD");
                expect (main.daemonStartUnansweredMs != 0 && ! main.daemonTakeAdopted, "the unanswered take was called settled");
                releaseStart.store (true);
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.daemonTakeAdopted;
                }, 10000), "the take that did start was never adopted");
                expect (rig.daemon.getRecorder().isRecording());
                expectEquals ((int) main.daemonStartUnansweredMs, 0, "the adopted take still reads as unanswered");
                expectEquals (main.engine.getActiveSessionDir().getFullPathName(), rig.session.getFullPathName(),
                              "the window lost the session the daemon is writing");
            }

            beginTest ("The chosen backup format reaches the daemon");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                expect (main.engine.setBackupFormat (CaptureFormat::Flac24));
                auto config = main.captureDaemonSettings();
                config.getDynamicObject()->setProperty ("sampleRate", kRate);
                expect (main.captureSupervisor.configureCapture (config));
                expect (rig.daemon.getRecorder().getBackupCaptureFormat() == CaptureFormat::Flac24,
                        "the daemon took a backup format the window did not choose");
                expect (main.engine.setBackupFormat (CaptureFormat::Wav24));
                updateSettings (main.engine.getAppProps(), [] (juce::PropertiesFile& p) { p.removeValue ("backupFormat"); });
            }

            beginTest ("A daemon refusal keeps its reason");
            {
                Rig rig;
                if (! bringUp (rig)) return;
                auto& main = *rig.main;
                rig.daemon.getRecorder().getTrack (0).armed.store (true);
                juce::BigInteger arms; arms.setBit (0);
                main.requestDaemonTake (juce::File(), 1, arms, 1, true);
                expect (main.alarmBanner.getText().contains ("no sessionDir"), main.alarmBanner.getText());
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
                player.selectRange (in, outPoint);
                engine.setPunchModeOn (true);
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

                player.selectRange (100 * kBlock, 200 * kBlock);
                engine.setPunchModeOn (true);
                player.setPositionSamples (90 * kBlock);
                engine.startPlayback();
                expect (player.isPlaying());
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
                expect (! main.daemonTakeAdopted, "the window still holds the ended take");
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
            std::atomic<bool> holding { true };
            juce::WaitableEvent entered, released { true };
        };
        using Copy = MultitrackRecorder::CaptureCopy;

        static void hold (MultitrackRecorder& rec, const std::shared_ptr<Stall>& stall)
        {
            rec.beforeCopyWriteForTests = [stall] (Copy& copy)
            {
                if (copy.copyIndex != stall->copyIndex || ! stall->holding.load()) return;
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

        static juce::int64 lengthOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            return reader != nullptr ? reader->lengthInSamples : -1;
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
            hungVolumeStats();
            markersBehindTheTake();
            refusedStartLeavesNothing();
            punchesWithHoles();
            boundedFinishing();
            primaryHolesAreCounted();
            copiesOpenBehindTheTake();
            slowLeadIns();
            emptyPassOwesNoLeadIn();
            openingCopyIsNotExpectedToWrite();
            punchPutBackOnACopy();
            hashesWaitForTheNextTake();
            refusedStartResumesHashes();
        }

        // A lead-in is silence the length of the take so far: gigabytes on a
        // long show, minutes on a slow drive. Every block of it is progress.
        void slowLeadIns()
        {
            beginTest ("A copy writing a long silent lead-in after STOP is making progress and is not left behind");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks (320, { 0 });                 // five 16384-frame blocks of lead-in
                rec.stopRecording();
                const auto firstSet = feeder.frame;

                // The mirror joins for the second set and owes the first as silence.
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                rec.copyStallLimitMs = 250.0;
                MultitrackRecorder::silenceBlockDelayMsForTests.store (80);
                const juce::ScopeGuard fast { [] { MultitrackRecorder::silenceBlockDelayMsForTests.store (0); } };
                rec.armContinue (firstSet);
                expect (rec.startRecording (dirs.session), rec.getStartError());
                feeder.blocks (20, { 0, 2 });
                rec.stopRecording();

                const auto problems = rec.getCopyProblems().joinIntoString ("|");
                expect (! problems.contains ("left behind"), problems);
                expect (! rec.anyMirrorFailed(), "a healthy mirror was reported failed");
                expect (! rec.isStillLeftBehind (dirs.mirror), "the next take would leave the mirror out");
                const auto mirrorAudio = dirs.mirror.getChildFile ("take").getChildFile ("Audio Files");
                expectEquals (lengthOf (mirrorAudio.getChildFile ("Track_01.wav")), firstSet, "the lead-in is short");
                expectEquals (lengthOf (mirrorAudio.getChildFile ("Track_01_part02.wav")), (juce::int64) 20 * kBlock);
                expect (! mirrorAudio.getChildFile ("Pending Lead-ins").exists(), "the lead-in is still owed");
            }

            beginTest ("A copy finishing an owed lead-in while it joins a take is making progress, not hung");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks (20, { 0, 2 });
                rec.stopRecording();

                // A crash left the mirror owing a lead-in of 81920 frames.
                const auto mirrorAudio = dirs.mirror.getChildFile ("take").getChildFile ("Audio Files");
                MultitrackRecorder::PendingLeadIn lead;
                lead.audioDir = mirrorAudio; lead.trackName = "Track_09"; lead.ext = ".wav";
                lead.firstPart = 1; lead.parts = 1; lead.container = 0; lead.bitDepth = 24; lead.channels = 1;
                lead.sampleRate = kRate; lead.priorLength = 0; lead.timelineStart = 5 * 16384;
                expect (MultitrackRecorder::writeLeadInMarker (lead));
                expect (mirrorAudio.getChildFile ("Track_09_part02.wav").replaceWithText ("audio"));

                rec.copyStallLimitMs = 250.0;
                MultitrackRecorder::silenceBlockDelayMsForTests.store (80);
                const juce::ScopeGuard fast { [] { MultitrackRecorder::silenceBlockDelayMsForTests.store (0); } };
                rec.armContinue (feeder.frame);
                expect (rec.startRecording (dirs.session), rec.getStartError());
                feeder.blocks (4, { 0 });
                rec.stopRecording();                       // while the mirror still writes that silence

                const auto problems = rec.getCopyProblems().joinIntoString ("|");
                expect (! problems.contains ("left behind"), problems);
                expect (! problems.contains ("not in this take"), problems);
                expect (! rec.isStillLeftBehind (dirs.mirror));
                expectEquals (lengthOf (mirrorAudio.getChildFile ("Track_09.wav")), lead.timelineStart);
                expectEquals (lengthOf (mirrorAudio.getChildFile ("Track_01_part02.wav")), (juce::int64) 4 * kBlock,
                              "the mirror did not get the take");
            }
        }

        // A pass that captured nothing has nothing to line up.
        void emptyPassOwesNoLeadIn()
        {
            beginTest ("A continue stopped before any audio writes no lead-in for a newly armed track, on any copy");
            Dirs dirs;
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 2);
            rec.getTrack (0).armed.store (true);
            rec.setBackupDirectory (dirs.backup);
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec, 2 };
            feeder.blocks (40, { 0, 1 });
            rec.stopRecording();

            rec.getTrack (1).armed.store (true);            // the encore's guest mic
            rec.armContinue (feeder.frame);
            expect (rec.startRecording (dirs.session), rec.getStartError());
            rec.stopRecording();                            // before a single block

            for (const auto& audio : { dirs.session.getChildFile ("Audio Files"),
                                       dirs.backup.getChildFile ("take").getChildFile ("Audio Files") })
            {
                const auto written = audio.findChildFiles (juce::File::findFiles, false, "Track_02*");
                expect (written.isEmpty(), audio.getFullPathName() + " holds " + juce::String (written.size())
                                               + " Track_02 file(s) from a pass that captured nothing");
                expect (! audio.getChildFile ("Pending Lead-ins").exists(), "a lead-in is still owed");
                expectEquals (lengthOf (audio.getChildFile ("Track_01.wav")), (juce::int64) 40 * kBlock,
                              "the first set changed");
            }
            expect (! rec.hasPrimaryFailed() && ! rec.hasBackupFailed());
            const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
            expect (report["stampFailures"].size() == 0, juce::JSON::toString (report["stampFailures"]));
        }

        // The expected write rate is what is writing: a copy still opening
        // writes nothing yet, and counting it read as a struggling disk.
        void openingCopyIsNotExpectedToWrite()
        {
            beginTest ("A copy still opening is not counted in the disk rate the take expects");
            Dirs dirs;
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.setBackupDirectory (dirs.backup);
            expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
            struct Gate { ~Gate() { released.signal(); } juce::WaitableEvent released { true }; };
            auto gate = std::make_shared<Gate>();
            auto opening = std::make_shared<std::atomic<int>> (0);
            rec.beforeCopyOpenForTests = [gate, opening] (Copy&)
            {
                opening->fetch_add (1);
                gate->released.wait (-1);
            };
            expect (rec.startRecording (dirs.session));
            for (int i = 0; i < 5000 && opening->load() < 2; ++i) juce::Thread::sleep (1);
            expectEquals (opening->load(), 2, "both copies should be opening");
            const auto primaryOnly = (juce::int64) (kRate * 3);   // one mono 24-bit track
            expectEquals (rec.estimateBytesPerSecondForArmedTracks(), primaryOnly,
                          "copies that are still opening were expected to write");
            gate->released.signal();
            Feeder feeder { rec, 1 };
            feeder.blocks (10, { 0, 1, 2 });
            expectEquals (rec.estimateBytesPerSecondForArmedTracks(), primaryOnly * 3, "the copies that opened were left out");
            rec.stopRecording();
        }

        void punchPutBackOnACopy()
        {
            beginTest ("A copy that had an interrupted punch put back sits out one take and joins the next");
            Dirs dirs;
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec, 1 };
            feeder.blocks (20, { 0, 2 });
            rec.stopRecording();

            // A crash in the middle of a punch on the mirror.
            const auto mirrorAudio = dirs.mirror.getChildFile ("take").getChildFile ("Audio Files");
            const auto take = mirrorAudio.getChildFile ("Track_01.wav");
            expect (take.copyFileTo (mirrorAudio.getChildFile ("Track_01.punchbase.wav")));

            rec.armContinue (feeder.frame);
            expect (rec.startRecording (dirs.session), rec.getStartError());
            feeder.blocks (10, { 0 });
            const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (! rec.anyMirrorFailed() && juce::Time::getMillisecondCounterHiRes() < end) juce::Thread::sleep (2);
            rec.stopRecording();
            const auto problems = rec.getCopyProblems().joinIntoString ("|");
            expect (problems.contains ("interrupted punch put back"), problems);
            expect (problems.contains ("joins the next"), "the line does not say what happens next: " + problems);
            expect (! problems.contains ("reopen"), "reopening the session does nothing for a copy: " + problems);

            rec.armContinue (feeder.frame);
            expect (rec.startRecording (dirs.session), rec.getStartError());
            feeder.blocks (10, { 0, 2 });
            rec.stopRecording();
            expect (! rec.anyMirrorFailed(), "the mirror did not join the next take: "
                                                 + rec.getCopyProblems().joinIntoString ("|"));
        }

        // The last take's hashes read the drive the next take writes to. In
        // another folder nothing used to stop them: every helper take is one.
        void hashesWaitForTheNextTake()
        {
            beginTest ("The last take's hash pass reads nothing while the next take, in another folder, captures");
            {
                Dirs first, second;
                auto gate      = std::make_shared<juce::WaitableEvent> (true);
                auto published = std::make_shared<std::atomic<bool>> (false);
                auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                hooks->beforeScan   = [gate] { gate->wait (10000); };
                hooks->afterPublish = [published] { published->store (true); };
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.reportTestHooks = hooks;
                expect (rec.startRecording (first.session));
                Feeder feeder { rec, 1 };
                feeder.blocks (40, { 0 });
                rec.stopRecording();
                rec.reportTestHooks = {};

                expect (rec.startRecording (second.session), rec.getStartError());
                gate->signal();                            // the first take's pass may read now
                Feeder next { rec, 1 };
                for (int i = 0; i < 30; ++i) { next.blocks (2, { 0 }); juce::Thread::sleep (10); }
                expect (! published->load(), "the hash pass read the drive while the next take captured");
                expect ((bool) juce::JSON::parse (first.session.getChildFile ("session.report.json"))["sha256Pending"]);
                rec.stopRecording();

                const auto end = juce::Time::getMillisecondCounterHiRes() + 10000.0;
                while (! published->load() && juce::Time::getMillisecondCounterHiRes() < end) juce::Thread::sleep (5);
                expect (published->load(), "the hash pass never came back after the take");
                expect (! (bool) juce::JSON::parse (first.session.getChildFile ("session.report.json"))["sha256Pending"]);
            }

            beginTest ("A resumed hash pass reads nothing while a take captures, and finishes after it");
            {
                Dirs first, second;
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (first.session));
                    Feeder feeder { rec, 1 };
                    feeder.blocks (40, { 0 });
                    rec.stopRecording();
                }
                std::atomic<bool> stop { false };
                expect (MultitrackRecorder::waitForReportHashes (first.session, stop, true));
                expect (MultitrackRecorder::editSessionReport (first.session, [] (juce::DynamicObject& live)
                { live.setProperty ("sha256Pending", true); }));

                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (second.session));
                auto done = std::make_shared<juce::WaitableEvent> (true);
                MultitrackRecorder::resumeReportHashes (first.session, [done] { done->signal(); });
                Feeder feeder { rec, 1 };
                for (int i = 0; i < 30; ++i) { feeder.blocks (2, { 0 }); juce::Thread::sleep (10); }
                expect (! done->wait (0), "the resumed pass read the drive while a take captured");
                rec.stopRecording();
                expect (done->wait (10000), "the resumed pass never finished");
                expect (! (bool) juce::JSON::parse (first.session.getChildFile ("session.report.json"))["sha256Pending"]);
            }
        }

        void refusedStartResumesHashes()
        {
            beginTest ("A RECORD refused after it touched the take puts the last take's hashes back to work");
            Dirs dirs;
            auto gate = std::make_shared<juce::WaitableEvent> (true);
            auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
            hooks->beforeScan = [gate] { gate->wait (10000); };
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.reportTestHooks = hooks;
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec, 1 };
            feeder.blocks (40, { 0 });
            rec.stopRecording();
            rec.reportTestHooks = {};

            // The continuation's file cannot be created: RECORD is refused after
            // it claimed the session's report.
            const auto blocker = dirs.session.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav");
            expect (blocker.createDirectory().wasOk());
            rec.armContinue (feeder.frame);
            expect (! rec.startRecording (dirs.session), "the fixture did not refuse the start");
            gate->signal();
            expect (blocker.deleteRecursively());

            const auto report = [&dirs] { return juce::JSON::parse (dirs.session.getChildFile ("session.report.json")); };
            const auto end = juce::Time::getMillisecondCounterHiRes() + 10000.0;
            while ((bool) report()["sha256Pending"] && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep (10);
            expect (! (bool) report()["sha256Pending"], "the last take's hashes stayed pending after a refused RECORD");
            expectEquals (report()["tracks"][0]["sha256"][0].toString().length(), 64);
        }

        void copiesOpenBehindTheTake()
        {
            struct Gate
            {
                ~Gate() { released.signal(); }
                std::atomic<bool> holding { true };
                juce::WaitableEvent entered, released { true };
            };
            const auto holdOpen = [] (MultitrackRecorder& rec, const std::shared_ptr<Gate>& gate, int copyIndex)
            {
                rec.beforeCopyOpenForTests = [gate, copyIndex] (Copy& copy)
                {
                    if (copy.copyIndex != copyIndex || ! gate->holding.load()) return;
                    gate->entered.signal();
                    gate->released.wait (-1);
                };
            };

            beginTest ("RECORD waits for no copy, and a slow copy still gets every frame");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                auto gate = std::make_shared<Gate>();
                holdOpen (rec, gate, 1);

                std::atomic<bool> started { false };
                expect (finishesWithin (3000, [&rec, &dirs, &started] { started = rec.startRecording (dirs.session); }),
                        "RECORD waited for the backup");
                expect (started.load());
                expect (gate->entered.wait (5000), "the backup never began opening");
                Feeder feeder { rec, 1 };
                feeder.blocks (80, { 0 });

                gate->holding.store (false);
                gate->released.signal();
                feeder.blocks (20, { 0, 1 });
                rec.stopRecording();
                expect (! rec.hasBackupFailed(), "a copy inside its ring lost nothing");
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                expectIntact (dirs.backupTake(), feeder.frame, "the backup that opened late");
            }

            beginTest ("A copy that opens later than its ring lasts pads silence, in step");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                auto gate = std::make_shared<Gate>();
                holdOpen (rec, gate, 1);
                expect (rec.startRecording (dirs.session));
                expect (gate->entered.wait (5000));
                Feeder feeder { rec, 1 };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 0 });
                gate->holding.store (false);
                gate->released.signal();
                feeder.blocks (20, { 0, 1 });
                rec.stopRecording();

                expect (rec.hasBackupFailed(), "the lost audio was not reported");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("lost"), "the banner did not say so");
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                const auto backup = read (dirs.backupTake());
                expectEquals ((juce::int64) backup.getNumSamples(), feeder.frame, "out of step with the primary");
                if (backup.getNumSamples() == feeder.frame)
                {
                    expectEquals (backup.getSample (0, 0), 0.0f, "the lost head should be silence");
                    int wrong = 0;
                    for (int i = backup.getNumSamples() - 20 * kBlock; i < backup.getNumSamples(); ++i)
                        if (std::abs (backup.getSample (0, i) - sampleAt (i, 0)) > 0.0002f) ++wrong;
                    expectEquals (wrong, 0, "the end moved");
                }
            }

            beginTest ("A dead mount can hang neither RECORD nor STOP, and sits out the next take");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.copyStallLimitMs = 300.0;
                auto gate = std::make_shared<Gate>();
                holdOpen (rec, gate, 1);
                expect (rec.startRecording (dirs.session));
                expect (gate->entered.wait (5000));
                Feeder feeder { rec, 1 };
                feeder.blocks (40, { 0 });
                expect (finishesWithin (5000, [&rec] { rec.stopRecording(); }), "STOP waited on the dead mount");
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("left behind"));
                expect (rec.isStillLeftBehind (dirs.backup));

                rec.armContinue (0);
                expect (rec.startRecording (dirs.session), "a dead backup must not refuse the next take");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("still not responding"));
                rec.stopRecording();
                gate->released.signal();
            }

            beginTest ("A copy that fails the rules leaves by name, and the take rolls on");
            {
                Dirs dirs;
                const auto foreign = dirs.mirror.getChildFile ("take").getChildFile ("Audio Files")
                                                .getChildFile ("Track_01.wav");
                expect (foreign.getParentDirectory().createDirectory().wasOk());
                expect (foreign.replaceWithText ("someone else's audio"));
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } }));
                expect (rec.startRecording (dirs.session), "one copy's folder must not refuse the take");
                const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
                while (! rec.anyMirrorFailed() && juce::Time::getMillisecondCounterHiRes() < end)
                    juce::Thread::sleep (5);
                expect (rec.anyMirrorFailed(), "the refused copy was not reported");
                expect (rec.getCopyProblems().joinIntoString ("|").contains (dirs.mirror.getFullPathName()),
                        "the warning did not name the copy");
                expectEquals (rec.getMirrorsSkippedAtStart(), 1);
                Feeder feeder { rec, 1 };
                feeder.blocks (40, { 0 });
                rec.stopRecording();
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                expectEquals (foreign.loadFileAsString(), juce::String ("someone else's audio"), "the foreign audio moved");
            }

            beginTest ("A drive that will not answer refuses RECORD in bounded time");
            {
                expect (VolumeProbe::answersWithin (juce::File::getSpecialLocation (juce::File::tempDirectory), 5000));
                juce::WaitableEvent released { true };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                expect (! VolumeProbe::answersWithin ({}, 100, [&released] (const juce::File&)
                {
                    released.wait (5000);
                    return VolumeProbe::Reading();
                }));
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 3000.0);
                released.signal();
                juce::Thread::sleep (20);
            }
        }

        void primaryHolesAreCounted()
        {
            beginTest ("A hole in the primary reads as missed samples");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                auto stall = std::make_shared<Stall> (0);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 1 });
                const auto missed = rec.getMissedSamples();
                expectGreaterThan (missed, (juce::int64) 0, "a live hole showed no missed samples");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("the other copies have it"),
                        "the backup is healthy and has it");
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();

                expectGreaterThan (rec.getMissedSamples(), (juce::int64) 0);
                const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expectGreaterThan ((juce::int64) report.getProperty ("missedSamples", 0), (juce::int64) 0,
                                   "the report called the take clean");
            }

            beginTest ("With no healthy copy nothing claims the audio is safe elsewhere");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                auto stall = std::make_shared<Stall> (0);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, {});
                const auto problems = rec.getCopyProblems().joinIntoString ("|");
                expect (problems.contains ("no other copy has it"), problems);
                expect (! problems.contains ("the other copies have it"), problems);
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();
            }

            beginTest ("A copy's own hole is not a hole in the take");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                auto stall = std::make_shared<Stall> (1);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 0 });
                expectEquals (rec.getMissedSamples(), (juce::int64) 0, "the primary has every frame");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("the other copies have it"));
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();
            }
        }

        void boundedFinishing()
        {
            beginTest ("A copy that keeps writing slowly still has a total limit at STOP");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.copyDrainLimitMs = 400.0;
                // Each chunk is progress, so this drive never trips the stall
                // limit, however long the ring takes to come out. The copy is
                // held until STOP, so the backlog is there on any machine.
                auto slow = std::make_shared<std::atomic<bool>> (false);
                auto stopping = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard letGo { [stopping] { stopping->signal(); } };
                rec.beforeCopyWriteForTests = [slow, stopping] (Copy& copy)
                {
                    if (copy.copyIndex != 1 || ! slow->load()) return;
                    stopping->wait (-1);
                    juce::Thread::sleep (100);
                };
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec, 1 };
                feeder.blocks (20, { 0, 1 });
                slow->store (true);
                feeder.blocks ((Copy::kChunkFrames * 40) / kBlock, { 0 });

                const auto started = juce::Time::getMillisecondCounterHiRes();
                stopping->signal();
                rec.stopRecording();
                expectLessThan (juce::Time::getMillisecondCounterHiRes() - started, 3000.0,
                                "STOP sat through the whole drain");
                expect (rec.hasBackupFailed());
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("left behind"));
                expectIntact (dirs.primaryTake(), feeder.frame, "the primary");
                slow->store (false);
            }

            beginTest ("A copy stuck in its post-take work costs the primary nothing");
            {
                PunchRig rig;
                if (! rig.recordBase (*this)) return;
                auto rec = std::make_shared<MultitrackRecorder>();
                rig.configure (*rec);
                rec->copyStallLimitMs = 500.0;
                auto entered  = std::make_shared<juce::WaitableEvent>();
                auto released = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard letGo { [released] { released->signal(); } };
                rec->beforeCopyFinishForTests = [entered, released] (Copy& copy)
                {
                    if (copy.copyIndex != 2) return;
                    entered->signal();
                    released->wait (-1);
                };
                rec->armPunchIn (10 * kBlock);
                expect (rec->startRecording (rig.dirs.session));
                Feeder feeder { *rec, 1 };
                feeder.frame = 1000000;
                feeder.blocks (20, { 0, 1, 2 });

                auto stopped = std::make_shared<juce::WaitableEvent> (true);
                std::thread ([rec, stopped] { rec->stopRecording(); stopped->signal(); }).detach();
                expect (entered->wait (10000), "the stuck copy was never reached");
                expect (! rig.dirs.session.getChildFile ("Audio Files")
                              .getChildFile ("Track_01.punchbase.wav").existsAsFile(),
                        "the primary waited for a copy before installing its punch");
                expect (stopped->wait (10000), "STOP waited on the stuck copy");
                if (! stopped->wait (0)) return;

                expect (rec->anyMirrorFailed());
                expect (rec->getCopyProblems().joinIntoString ("|").contains ("left behind"));
                expect (PunchRig::bytesOf (rig.dirs.backupTake()) != rig.backupBase,
                        "the backup should have finished its punch");
                const auto report = juce::JSON::parse (rig.dirs.session.getChildFile ("session.report.json"));
                expect (report.isObject(), "no report");
                expect (report.getProperty ("mirrorFailed", false), "the report missed the stuck mirror");
                expect (rec->isStillLeftBehind (rig.dirs.mirror), "its drive would be used again");

                released->signal();
                const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
                while (rec->isStillLeftBehind (rig.dirs.mirror) && juce::Time::getMillisecondCounterHiRes() < end)
                    juce::Thread::sleep (5);
                expect (! rec->isStillLeftBehind (rig.dirs.mirror), "the drive never came back");
            }

            beginTest ("A copy's hash on a dead drive holds no primary hash, this take's or the next");
            {
                PunchRig rig;
                auto entered  = std::make_shared<juce::WaitableEvent>();
                auto released = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard letGo { [released] { released->signal(); } };
                auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                hooks->beforeCopyScan = [entered, released]
                {
                    entered->signal();
                    released->wait (-1);
                };
                {
                    MultitrackRecorder rec;
                    rig.configure (rec);
                    rec.reportTestHooks = hooks;
                    expect (rec.startRecording (rig.dirs.session));
                    Feeder feeder { rec, 1 };
                    feeder.blocks (20, { 0, 1, 2 });
                    rec.stopRecording();
                }
                expect (entered->wait (10000), "the copies were hashed first");
                const auto report = juce::JSON::parse (rig.dirs.session.getChildFile ("session.report.json"));
                expect ((bool) report.getProperty ("sha256Pending", false), "the copies' hashes are not in yet");
                expect ((bool) report.getProperty ("sha256Failed", true) == false);
                const auto* tracks = report.getProperty ("tracks", {}).getArray();
                expect (tracks != nullptr && ! tracks->isEmpty());
                if (tracks != nullptr && ! tracks->isEmpty())
                    expect (tracks->getReference (0).getProperty ("sha256", {})[0].toString().length() == 64,
                            "the primary's hash waited for a copy");

                // Another session's primary still gets its hash.
                Dirs next;
                auto secondHooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                auto published = std::make_shared<juce::WaitableEvent> (true);
                secondHooks->afterPublish = [published] { published->signal(); };
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.reportTestHooks = secondHooks;
                    expect (rec.startRecording (next.session));
                    Feeder feeder { rec, 1 };
                    feeder.blocks (20, { 0 });
                    rec.stopRecording();
                }
                expect (published->wait (10000), "a stuck copy held the next take's primary hash");
                released->signal();
            }
        }

        struct PunchRig
        {
            Dirs dirs;
            juce::int64 baseFrames { 0 };
            juce::MemoryBlock primaryBase, backupBase, mirrorBase;

            static juce::MemoryBlock bytesOf (const juce::File& file)
            {
                juce::MemoryBlock block;
                file.loadFileAsData (block);
                return block;
            }

            void configure (MultitrackRecorder& rec) const
            {
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.setMirrors ({ { dirs.mirror, CaptureFormat::Wav24 } });
            }

            // A take on every copy, whose exact bytes the punch must not cost.
            bool recordBase (UnitTest& test)
            {
                MultitrackRecorder rec;
                configure (rec);
                if (! rec.startRecording (dirs.session)) return false;
                Feeder feeder { rec, 1 };
                feeder.blocks (60, { 0, 1, 2 });
                baseFrames = feeder.frame;
                rec.stopRecording();
                primaryBase = bytesOf (dirs.primaryTake());
                backupBase  = bytesOf (dirs.backupTake());
                mirrorBase  = bytesOf (dirs.mirrorTake());
                test.expect (primaryBase.getSize() > 0 && backupBase.getSize() > 0 && mirrorBase.getSize() > 0);
                return primaryBase.getSize() > 0;
            }

            int archivedFrames (const juce::File& copyRoot) const
            {
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                int frames = 0;
                for (const auto& file : copyRoot.getChildFile ("Session File Backups")
                                                .findChildFiles (juce::File::findFiles, true, "Track_01*.wav"))
                    if (std::unique_ptr<juce::AudioFormatReader> reader { fm.createReaderFor (file) })
                        frames += (int) reader->lengthInSamples;
                return frames;
            }
        };

        void punchesWithHoles()
        {
            beginTest ("A punch with a FIFO gap keeps the original on every copy");
            {
                PunchRig rig;
                if (! rig.recordBase (*this)) return;
                MultitrackRecorder rec;
                rig.configure (rec);
                rec.armPunchIn (10 * kBlock);
                expect (rec.startRecording (rig.dirs.session));
                Feeder feeder { rec, 1 };
                feeder.frame = 1000000;
                feeder.blocks (20, { 0, 1, 2 });
                rec.missedSamples.fetch_add (kBlock);   // what a full FIFO adds
                rec.stopRecording();

                expect (rec.hasPunchSpliceFailed(), "the failure was not latched");
                expect (PunchRig::bytesOf (rig.dirs.primaryTake()) == rig.primaryBase, "the primary lost its original");
                expect (PunchRig::bytesOf (rig.dirs.backupTake()) == rig.backupBase, "the backup lost its original");
                expect (PunchRig::bytesOf (rig.dirs.mirrorTake()) == rig.mirrorBase, "the mirror lost its original");
                expectEquals (rig.archivedFrames (rig.dirs.session), 20 * kBlock, "the punch audio was not kept");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("Punch discarded"),
                        "the banner did not say so");
                const auto report = rig.dirs.session.getChildFile ("session.report.json").loadFileAsString();
                expect (report.contains ("Punch discarded"), "the report did not say so");
                expect (report.contains ("\"punchSpliceFailed\": true"), "the report missed the failure");
                expect (! rig.dirs.session.getChildFile ("Audio Files")
                                 .getChildFile ("Track_01.punchbase.wav").existsAsFile(), "a sidecar stayed behind");
            }

            beginTest ("A punch the primary lost audio from keeps the original on every copy");
            {
                PunchRig rig;
                if (! rig.recordBase (*this)) return;
                MultitrackRecorder rec;
                rig.configure (rec);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                auto stall = std::make_shared<Stall> (0);
                hold (rec, stall);
                rec.armPunchIn (10 * kBlock);
                expect (rec.startRecording (rig.dirs.session));
                Feeder feeder { rec, 1 };
                feeder.frame = 1000000;
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 1, 2 });
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();

                expect (rec.hasPunchSpliceFailed(), "the failure was not latched");
                expect (PunchRig::bytesOf (rig.dirs.primaryTake()) == rig.primaryBase, "the primary lost its original");
                expect (PunchRig::bytesOf (rig.dirs.backupTake()) == rig.backupBase, "the backup lost its original");
                expect (PunchRig::bytesOf (rig.dirs.mirrorTake()) == rig.mirrorBase, "the mirror lost its original");
                expectGreaterThan (rig.archivedFrames (rig.dirs.session), 0, "the punch audio was not kept");
            }

            beginTest ("A copy with a hole in the punch keeps its take; the others take the punch");
            {
                PunchRig rig;
                if (! rig.recordBase (*this)) return;
                MultitrackRecorder rec;
                rig.configure (rec);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                auto stall = std::make_shared<Stall> (2);
                hold (rec, stall);
                rec.armPunchIn (10 * kBlock);
                expect (rec.startRecording (rig.dirs.session));
                Feeder feeder { rec, 1 };
                feeder.frame = 1000000;
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 0, 1 });
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();

                expect (! rec.hasPunchSpliceFailed(), "a copy's hole must not cost the primary its punch");
                expect (PunchRig::bytesOf (rig.dirs.primaryTake()) != rig.primaryBase, "the primary did not take the punch");
                expect (PunchRig::bytesOf (rig.dirs.backupTake()) != rig.backupBase, "the backup did not take the punch");
                expect (PunchRig::bytesOf (rig.dirs.mirrorTake()) == rig.mirrorBase, "the mirror took a punch with a hole");
                expect (rec.anyMirrorFailed(), "the mirror was not reported failed");
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("kept its pre-punch take"),
                        "the banner did not name the copy");
            }
        }

        // A stat that can be stopped from answering.
        struct Volume
        {
            Volume (juce::int64 freeBytes, juce::uint64 id)
            {
                answer.freeBytes = freeBytes;
                answer.totalBytes = freeBytes * 2;
                answer.volumeId = id;
            }
            ~Volume() { released.signal(); }
            VolumeProbe::Reading read()
            {
                if (holding.load()) released.wait (-1);
                return answer;
            }
            VolumeProbe::Reading answer;
            std::atomic<bool> holding { false };
            juce::WaitableEvent released { true };
        };

        // Run on a thread of its own: code that wrongly waits on the hung stat
        // has to fail this test, not wedge the whole suite.
        static bool finishesWithin (int timeoutMs, std::function<void()> work)
        {
            auto finished = std::make_shared<juce::WaitableEvent> (true);
            std::thread ([finished, work = std::move (work)]
            {
                work();
                finished->signal();
            }).detach();
            return finished->wait (timeoutMs);
        }

        bool readsUntil (MultitrackRecorder& rec,
                         const std::function<bool (const MultitrackRecorder::DiskEstimate&)>& wanted,
                         MultitrackRecorder::DiskEstimate& out)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (juce::Time::getMillisecondCounterHiRes() < end)
            {
                auto reading = std::make_shared<MultitrackRecorder::DiskEstimate>();
                if (! finishesWithin (1000, [&rec, reading] { *reading = rec.readDiskEstimate(); }))
                {
                    expect (false, "reading the estimate waited on the drive");
                    return false;
                }
                out = *reading;
                if (wanted (out)) return true;
                juce::Thread::sleep (5);
            }
            return false;
        }

        void hungVolumeStats()
        {
            using Estimate = MultitrackRecorder::DiskEstimate;
            // 8,640,000 bytes a minute: one 24-bit channel at 48 kHz.
            constexpr juce::int64 kPerMinute = 8640000;

            beginTest ("A drive that stops answering costs only its own reading");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.volumeProbePeriodMs = 10;
                rec.volumeStaleAfterMs = 200.0;
                // The backup would fill first, so a held-over backup figure shows.
                auto primary = std::make_shared<Volume> (kPerMinute * 1000, 1);
                auto backup  = std::make_shared<Volume> (kPerMinute * 100, 2);
                rec.volumeReaderForTests = [primary, backup, session = dirs.session] (const juce::File& root)
                {
                    return (root == session ? primary : backup)->read();
                };
                expect (! rec.readDiskEstimate().watching, "nothing to watch before a take");
                expect (rec.startRecording (dirs.session));

                Estimate seen;
                expect (readsUntil (rec, [] (const Estimate& e) { return e.minutes == 100; }, seen),
                        "the first volume to fill should set the figure");
                expect (seen.watching);
                expectEquals (seen.primaryFreeBytes, kPerMinute * 1000);
                expect (! seen.primaryStale && ! seen.copyStale);

                backup->holding.store (true);
                backup->released.reset();
                expect (readsUntil (rec, [] (const Estimate& e) { return e.copyStale; }, seen),
                        "a copy's dead drive never read as stale");
                expectEquals (seen.minutes, 1000, "the primary's figure must stand alone");
                expectEquals (seen.primaryFreeBytes, kPerMinute * 1000);
                expect (! seen.primaryStale, "a copy's stat decided the primary's reading");

                backup->holding.store (false);
                backup->released.signal();
                expect (readsUntil (rec, [] (const Estimate& e) { return ! e.copyStale && e.minutes == 100; }, seen),
                        "a drive that came back should read as live again");

                primary->holding.store (true);
                primary->released.reset();
                expect (readsUntil (rec, [] (const Estimate& e) { return e.primaryStale; }, seen),
                        "a primary that stopped answering never read as stale");
                expectEquals (seen.minutes, 0, "a stale primary still gave a confident number");
                expectEquals (seen.primaryFreeBytes, (juce::int64) -1);

                expect (finishesWithin (20000, [&rec] { rec.stopRecording(); }), "STOP waited on the stat");
                expect (! rec.readDiskEstimate().watching, "the probes outlived the take");
                primary->released.signal();
            }

            beginTest ("Copies on one drive drain it together");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                rec.volumeProbePeriodMs = 10;
                auto shared = std::make_shared<Volume> (kPerMinute * 1000, 7);
                rec.volumeReaderForTests = [shared] (const juce::File&) { return shared->read(); };
                expect (rec.startRecording (dirs.session));
                Estimate seen;
                expect (readsUntil (rec, [] (const Estimate& e) { return e.minutes == 500; }, seen),
                        "two copies on one volume should halve its time");
                rec.stopRecording();
            }

            beginTest ("Two real paths on one drive read as one volume");
            {
                Dirs dirs;
                const auto a = VolumeProbe::readVolume (dirs.session);
                const auto b = VolumeProbe::readVolume (dirs.backup);
                expect (a.volumeId != 0, "no volume id");
                expect (a.volumeId == b.volumeId);
                expect (a.totalBytes > 0);
                const auto missing = VolumeProbe::readVolume (dirs.session.getChildFile ("a").getChildFile ("b"));
                expect (missing.volumeId == a.volumeId, "a folder not made yet is on its parent's drive");
            }
        }

        void markersBehindTheTake()
        {
            beginTest ("A marker during a take never waits on the take's drive");
            Dirs dirs;
            MarkersManager markers;
            markers.setContext (dirs.session, kRate);
            markers.deferDiskWhile = [] { return true; };
            auto entered  = std::make_shared<juce::WaitableEvent>();
            auto released = std::make_shared<juce::WaitableEvent> (true);
            const juce::ScopeGuard letGo { [released] { released->signal(); } };
            markers.setBeforeDeferredWriteForTests ([entered, released]
            {
                entered->signal();
                released->wait (-1);
            });

            expect (finishesWithin (2000, [&markers]
            {
                markers.drop (10, "One");
            }), "the marker waited on the drive");
            expect (entered->wait (5000), "the save never left this thread");
            expect (finishesWithin (2000, [&markers]
            {
                markers.drop (20, "Two");
                markers.renameMarker (0, "First");
            }), "a marker during a stuck write waited on it");
            expectEquals (markers.getCount(), 2);
            expect (markers.hasContext(), "a stuck drive lost the session");

            const auto file = dirs.session.getChildFile ("markers.json");
            expect (! file.existsAsFile(), "the writer was held");
            released->signal();

            // The writer thread may need a moment; only the newest list counts.
            const auto hasBoth = [&file]
            {
                const auto text = file.loadFileAsString();
                return text.contains ("First") && text.contains ("Two");
            };
            const auto end = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (! hasBoth() && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep (5);
            expect (hasBoth(), "the newest list never reached the disk");

            // Out of the take the next save lands in order with the last one.
            markers.deferDiskWhile = nullptr;
            markers.drop (30, "Three");
            const auto settled = juce::Time::getMillisecondCounterHiRes() + 5000.0;
            while (! file.loadFileAsString().contains ("Three")
                   && juce::Time::getMillisecondCounterHiRes() < settled)
                juce::Thread::sleep (5);
            expect (file.loadFileAsString().contains ("Three"));
            expect (file.loadFileAsString().contains ("First"));
        }

        void refusedStartLeavesNothing()
        {
            beginTest ("A refused RECORD leaves the markers it found and no stereo mix file");
            Dirs dirs;
            AudioEngine::setTestModeSkipAudioInit (true);
            AudioEngine engine;
            engine.prepareForTests (kRate, kBlock);
            engine.setStripCount (1);
            engine.getRecorder().getTrack (0).armed.store (true);
            auto& markers = engine.getMarkers();
            const auto previous = dirs.root.getChildFile ("previous");
            previous.createDirectory();
            markers.setContext (previous, kRate);
            markers.drop (5, "Keep");
            engine.setRecordStereoMix (true);

            // The recorder refuses this one after the engine has prepared for it:
            // a fresh take must never land on media already in the folder.
            const auto mix = dirs.session.getChildFile ("Export Files").getChildFile ("StereoMix.wav");
            const auto existing = dirs.session.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
            expect (existing.create().wasOk());
            expect (! engine.startRecording (dirs.session), "a fresh take over existing media must refuse");
            expect (! mix.existsAsFile(), "a refused start left a stereo mix file");
            expect (markers.getSessionDir() == previous, "a refused start moved the markers");
            expectEquals (markers.getCount(), 1);
            expectEquals (markers.getLast().name, juce::String ("Keep"));
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
                rec->copyRingFramesForTests = Copy::kChunkFrames;
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

                expectEquals (rec->missedSamples.load(), (juce::int64) 0, "the audio FIFO dropped: every copy lost it");
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
            rec.copyRingFramesForTests = Copy::kChunkFrames;
            auto stall = std::make_shared<Stall> (0);
            stall->holding.store (false);
            hold (rec, stall);
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec, 1 };

            // Healthy first: a reading that freezes must have something to hold.
            const auto healthyBy = juce::Time::getMillisecondCounterHiRes() + 10000.0;
            while (rec.getDiskBytesPerSec() <= 0.0f && juce::Time::getMillisecondCounterHiRes() < healthyBy)
            {
                feeder.block();
                rec.drainPendingForTests();
                juce::Thread::sleep (5);
            }
            expectGreaterThan (rec.getDiskBytesPerSec(), 0.0f, "the take never showed any throughput");

            stall->holding.store (true);
            feeder.block();
            rec.drainOnce();
            expect (stall->entered.wait (5000));

            const auto expected = rec.estimateBytesPerSecondForArmedTracks();
            const auto end = juce::Time::getMillisecondCounterHiRes() + 20000.0;
            while ((! rec.isDiskStruggling() || rec.getDiskBytesPerSec() > 0.0f)
                   && juce::Time::getMillisecondCounterHiRes() < end)
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
