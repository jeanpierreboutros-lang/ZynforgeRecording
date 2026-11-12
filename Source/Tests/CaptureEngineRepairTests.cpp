// What a take says about its copies once it has stopped, what a crash in a
// punch's last step leaves, how a take treats a session folder that moved or a
// drive that went, and where the engine puts what it recorded and played.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/CaptureCopy.h"
#include "../Audio/FastHash.h"
#include "../Audio/FloatAiffWriter.h"
#include "../Audio/Markers.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/PlaybackReader.h"
#include "../Audio/VolumeProbe.h"

#include <cmath>
#include <map>
#include <sys/stat.h>
#include <vector>

namespace zynforge
{
    namespace
    {
        constexpr double kRepairRate = 48000.0;
        constexpr int kRepairBlock = 256;

        juce::File repairDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-repair-" + tag + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        juce::MemoryBlock bytesOf (const juce::File& file)
        {
            juce::MemoryBlock block;
            file.loadFileAsData (block);
            return block;
        }

        juce::int64 lengthOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            return reader != nullptr ? reader->lengthInSamples : -1;
        }

        juce::AudioBuffer<float> audioOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            if (reader == nullptr) return {};
            juce::AudioBuffer<float> out ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&out, 0, out.getNumSamples(), 0, true, true);
            return out;
        }

        void writeFile (const juce::File& file, juce::AudioFormat& format, int frames, float value, int bits = 16)
        {
            file.getParentDirectory().createDirectory();
            file.deleteFile();
            auto stream = file.createOutputStream();
            std::unique_ptr<juce::AudioFormatWriter> writer (
                format.createWriterFor (stream.get(), kRepairRate, 1, bits, {}, 0));
            if (writer == nullptr) return;
            stream.release();
            juce::AudioBuffer<float> buffer (1, frames);
            for (int i = 0; i < frames; ++i) buffer.setSample (0, i, value);
            writer->writeFromAudioSampleBuffer (buffer, 0, frames);
        }

        void writeWav (const juce::File& file, int frames, float value)
        {
            juce::WavAudioFormat wav;
            writeFile (file, wav, frames, value, 24);
        }

        void writeAiff (const juce::File& file, int frames, float value)
        {
            juce::AiffAudioFormat aiff;
            writeFile (file, aiff, frames, value, 16);
        }

        bool archiveHolds (const juce::File& sessionDir, const juce::MemoryBlock& bytes)
        {
            for (const auto& file : sessionDir.getChildFile ("Session File Backups")
                                              .findChildFiles (juce::File::findFiles, true, "*"))
                if (bytesOf (file) == bytes) return true;
            return false;
        }

        template <typename Done>
        bool waitFor (Done done, int timeoutMs = 5000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                juce::Thread::sleep (2);
            return done();
        }
    }

    class CaptureRepairTests final : public juce::UnitTest
    {
    public:
        CaptureRepairTests() : UnitTest ("Capture repairs", "zynforge") {}

        static constexpr double kRate = kRepairRate;
        static constexpr int kBlock = kRepairBlock;
        using Copy = MultitrackRecorder::CaptureCopy;

        // Holds one copy's thread in a write, the way a dead mount would.
        struct Stall
        {
            explicit Stall (int index) : copyIndex (index) {}
            ~Stall() { released.signal(); }
            int copyIndex;
            std::atomic<bool> holding { true };
            juce::WaitableEvent entered, released { true };
        };

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
            juce::File root, session, backup;
            Dirs()
                : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("zf-repair-copies-" + juce::Uuid().toString())),
                  session (root.getChildFile ("primary").getChildFile ("take")),
                  backup (root.getChildFile ("backup"))
            {
                session.createDirectory(); backup.createDirectory();
            }
            ~Dirs()
            {
                MultitrackRecorder::deleteSessionAfterCancellingReports (session);
                root.deleteRecursively();
            }
            juce::File primaryTake() const { return session.getChildFile ("Audio Files").getChildFile ("Track_01.wav"); }
            juce::File backupTake() const
            { return backup.getChildFile ("take").getChildFile ("Audio Files").getChildFile ("Track_01.wav"); }
        };

        static float sampleAt (juce::int64 frame) noexcept
        {
            return 0.4f * (float) std::sin ((double) frame * 0.01);
        }

        struct Feeder
        {
            MultitrackRecorder& rec;
            juce::int64 frame { 0 };

            void block()
            {
                std::vector<float> audio ((size_t) kBlock);
                for (int i = 0; i < kBlock; ++i) audio[(size_t) i] = sampleAt (frame + i);
                const float* inputs[] { audio.data() };
                rec.processBlock (inputs, 1, kBlock);
                frame += kBlock;
            }

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

        static void feedConstant (MultitrackRecorder& rec, int blocks, int channels, float value)
        {
            std::vector<float> samples ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) channels, samples.data());
            for (int b = 0; b < blocks; ++b)
            {
                rec.processBlock (inputs.data(), channels, kBlock);
                rec.drainPendingForTests();
            }
        }

        // A take whose backup lost audio while the primary kept every frame.
        static void lossyBackupTake (MultitrackRecorder& rec, const Dirs& dirs)
        {
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.setBackupDirectory (dirs.backup);
            auto stall = std::make_shared<Stall> (1);
            rec.copyRingFramesForTests = Copy::kChunkFrames;
            hold (rec, stall);
            if (! rec.startRecording (dirs.session)) return;
            Feeder feeder { rec };
            feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 0 });
            stall->holding.store (false);
            stall->released.signal();
            rec.stopRecording();
            rec.beforeCopyWriteForTests = {};
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            copyLinesAfterStop();
            refusedStartKeepsCopyLines();
            refusalReasons();
            floatAiffPeriodicFlush();
            legacyRootTakes();
            takeClipCounts();
            sessionFolderMoved();
            pulledDriveReadsNoVolume();
            punchInstallCrashes();
            copyHashesApart();
        }

        void copyLinesAfterStop()
        {
            for (const int lossy : { 0, 1 })
            {
                beginTest (lossy == 0 ? "After STOP the primary's lost audio is still said to be on the backup"
                                      : "After STOP the backup's lost audio is still said to be on the primary");
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (dirs.backup);
                auto stall = std::make_shared<Stall> (lossy);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, { 1 - lossy });
                expect (rec.getCopyProblems().joinIntoString ("|").contains ("the other copies have it"),
                        "the fixture lost nothing");
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();

                const auto problems = rec.getCopyProblems().joinIntoString ("|");
                expect (problems.contains ("the other copies have it"), problems);
                expect (! problems.contains ("no other copy has it"), problems);
                const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                const auto listed = juce::JSON::toString (report.getProperty ("copyProblems", {}));
                expect (listed.contains ("the other copies have it"), listed);
                expect (! listed.contains ("no other copy has it"), listed);
            }

            beginTest ("After STOP a take with no healthy copy still says nothing else has the audio");
            {
                Dirs dirs;
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                auto stall = std::make_shared<Stall> (0);
                rec.copyRingFramesForTests = Copy::kChunkFrames;
                hold (rec, stall);
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec };
                feeder.blocks ((Copy::kChunkFrames * 3) / kBlock, {});
                stall->holding.store (false);
                stall->released.signal();
                rec.stopRecording();
                const auto problems = rec.getCopyProblems().joinIntoString ("|");
                expect (problems.contains ("no other copy has it"), problems);
                expect (! problems.contains ("the other copies have it"), problems);
            }
        }

        void refusedStartKeepsCopyLines()
        {
            beginTest ("A refused RECORD keeps the last take's copy lines on the banner");
            Dirs dirs;
            MultitrackRecorder rec;
            lossyBackupTake (rec, dirs);
            const auto before = rec.getCopyProblems().joinIntoString ("|");
            expect (before.contains ("Backup lost"), before);
            // A fresh take cannot replace the take this folder holds.
            expect (! rec.startRecording (dirs.session), "the fixture should refuse");
            expectEquals (rec.getCopyProblems().joinIntoString ("|"), before, "a refused start wiped the copy lines");
            expect (rec.hasBackupFailed(), "a refused start cleared the backup's failure");
        }

        void refusalReasons()
        {
            beginTest ("A fresh take over an existing one is refused for that reason");
            {
                const auto dir = repairDir ("fresh-over");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feedConstant (rec, 4, 1, 0.25f);
                    rec.stopRecording();
                }
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (! rec.startRecording (dir), "a fresh take replaced a take");
                const auto why = rec.getStartError();
                expect (why.contains ("already has a take"), why);
                expect (! why.contains ("mixed formats"), why);
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            for (const bool punch : { false, true })
            {
                beginTest (punch ? "A punch into a take nothing can read is refused as unreadable"
                                 : "A continue of a take nothing can read is refused as unreadable");
                const auto dir = repairDir ("unreadable");
                const auto take = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                take.getParentDirectory().createDirectory();
                expect (take.replaceWithText ("not audio at all"));
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                if (punch) rec.armPunchIn (500); else rec.armContinue (1000);
                expect (! rec.startRecording (dir), "an unreadable take was continued");
                const auto why = rec.getStartError();
                expect (why.contains ("could not be read"), why);
                expect (! why.contains ("mixed formats"), why);
                expectEquals (take.loadFileAsString(), juce::String ("not audio at all"));
                dir.deleteRecursively();
            }

            beginTest ("Takes in two formats are still refused as mixed formats");
            {
                const auto dir = repairDir ("mixed");
                const auto audio = dir.getChildFile ("Audio Files");
                writeWav (audio.getChildFile ("Track_01.wav"), 1000, 0.25f);
                writeAiff (audio.getChildFile ("Track_02.aif"), 1000, 0.25f);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 2);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (1).armed.store (true);
                rec.armContinue (1000);
                expect (! rec.startRecording (dir));
                expect (rec.getStartError().contains ("mixed formats"), rec.getStartError());
                dir.deleteRecursively();
            }
        }

        void floatAiffPeriodicFlush()
        {
            beginTest ("A float AIFF's periodic flush rewrites its header without forcing the file to disk");
            struct CountingStream final : public juce::OutputStream
            {
                juce::MemoryOutputStream inner;
                int flushes { 0 };
                void flush() override { ++flushes; }
                bool setPosition (juce::int64 position) override { return inner.setPosition (position); }
                juce::int64 getPosition() override { return inner.getPosition(); }
                bool write (const void* data, size_t bytes) override { return inner.write (data, bytes); }
            };
            auto* stream = new CountingStream();
            std::unique_ptr<juce::AudioFormatWriter> writer (FloatAiffWriter::create (stream, kRate, 1));
            expect (writer != nullptr);
            if (writer == nullptr) { delete stream; return; }
            juce::AudioBuffer<float> audio (1, 1000);
            for (int i = 0; i < 1000; ++i) audio.setSample (0, i, 0.25f);
            expect (writer->writeFromAudioSampleBuffer (audio, 0, 1000));
            for (int i = 0; i < 3; ++i) expect (writer->flush(), "a periodic flush failed");
            expectEquals (stream->flushes, 0, "the periodic flush forced the file to disk");
            const auto* bytes = static_cast<const juce::uint8*> (stream->inner.getData());
            const auto frames = (int) juce::ByteOrder::bigEndianInt (bytes + 34);
            expectEquals (frames, 1000, "the header is not current after the flush");
            expect (flushFloatAiffBeforeClose (writer.get()));
            expectEquals (stream->flushes, 1, "closing must still force the file to disk");
        }

        void legacyRootTakes()
        {
            for (int mode = 0; mode < 3; ++mode)
            {
                beginTest (mode == 0 ? "A fresh take into a session whose take sits in its root is refused"
                         : mode == 1 ? "A continue of a take in the session root is refused before anything is written"
                                     : "A punch into a take in the session root is refused before anything moves");
                const auto dir = repairDir ("legacy-root");
                const auto root = dir.getChildFile ("Track_01.wav");
                writeWav (root, 4000, 0.25f);
                const auto before = bytesOf (root);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                if (mode == 1) rec.armContinue (4000);
                if (mode == 2) rec.armPunchIn (1000);
                const bool started = rec.startRecording (dir);
                if (started) { feedConstant (rec, 4, 1, 0.5f); rec.stopRecording(); }
                expect (! started, "a take went into Audio Files over a take in the session root");
                expect (rec.getStartError().contains ("Audio Files"), rec.getStartError());
                expect (bytesOf (root) == before, "the root take changed");
                expect (! dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav").exists(),
                        "a new file now hides the root take");
                expect (! rec.isPunchInArmed() && ! rec.isContinueArmed(), "the refused arm outlived the start");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A root-level take on a strip that is not armed does not stop the others recording");
            {
                const auto dir = repairDir ("legacy-other");
                writeWav (dir.getChildFile ("Track_01.wav"), 4000, 0.25f);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 2);
                rec.getTrack (1).armed.store (true);
                expect (rec.startRecording (dir), rec.getStartError());
                feedConstant (rec, 4, 2, 0.5f);
                rec.stopRecording();
                expect (dir.getChildFile ("Audio Files").getChildFile ("Track_02.wav").existsAsFile());
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A root file behind a track's Audio Files take does not stop a continue");
            {
                const auto dir = repairDir ("legacy-shadowed");
                writeWav (dir.getChildFile ("Track_01.wav"), 4000, 0.25f);
                writeWav (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav"), 2000, 0.25f);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.armContinue (2000);
                expect (rec.startRecording (dir), rec.getStartError());
                feedConstant (rec, 4, 1, 0.5f);
                rec.stopRecording();
                expect (dir.getChildFile ("Audio Files").getChildFile ("Track_01_part02.wav").existsAsFile());
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        static juce::var trackReport (const juce::File& dir, int index)
        {
            const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
            if (auto* tracks = report.getProperty ("tracks", {}).getArray())
                if (index < tracks->size()) return tracks->getReference (index);
            return {};
        }

        void takeClipCounts()
        {
            std::vector<float> hot ((size_t) kBlock, 1.0f), clean ((size_t) kBlock, 0.25f);
            const auto block = [&] (MultitrackRecorder& rec, int channels, bool hotFirst, bool hotSecond)
            {
                const float* inputs[] { hotFirst ? hot.data() : clean.data(), hotSecond ? hot.data() : clean.data() };
                rec.processBlock (inputs, channels, kBlock);
                rec.drainPendingForTests();
            };

            beginTest ("The report counts a take's clipped blocks, not soundcheck's");
            {
                const auto dir = repairDir ("clips");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 2);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (1).monitor.store (true);   // metered, never captured
                for (int b = 0; b < 10; ++b) block (rec, 2, true, true);
                const int soundcheckClips = rec.getTrack (0).clipCount.load();
                expectGreaterThan (soundcheckClips, 0, "the fixture did not clip in soundcheck");
                expect (rec.startRecording (dir));
                for (int b = 0; b < 20; ++b) block (rec, 2, b == 12, true);
                rec.stopRecording();

                const auto armed = trackReport (dir, 0), unarmed = trackReport (dir, 1);
                expectEquals ((int) armed.getProperty ("clipCount", -9), 1);
                expectEquals ((juce::int64) armed.getProperty ("lastClipSample", -9), (juce::int64) 12 * kBlock);
                expectEquals ((int) unarmed.getProperty ("clipCount", -9), 0, "a strip that recorded nothing reported clips");
                expectEquals ((juce::int64) unarmed.getProperty ("lastClipSample", -9), (juce::int64) -1);
                expect (rec.getTrack (0).clipCount.load() >= soundcheckClips, "the take cleared the meter's clip evidence");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A clip's place in the report counts the pre-roll ahead of it, and a continue's the take before it");
            {
                const auto dir = repairDir ("clip-places");
                juce::int64 firstLength = 0;
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.setPreRollSeconds (1);
                    rec.getTrack (0).armed.store (true);
                    for (int b = 0; b < (int) (kRate * 1.2) / kBlock; ++b) block (rec, 1, false, false);
                    expect (rec.startRecording (dir));
                    for (int b = 0; b < 20; ++b) block (rec, 1, b == 5, false);
                    rec.stopRecording();
                    const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
                    const auto preRoll = (juce::int64) report.getProperty ("passPreRollSamples", 0);
                    expectGreaterThan (preRoll, (juce::int64) 0, "the fixture wrote no pre-roll");
                    expectEquals ((juce::int64) trackReport (dir, 0).getProperty ("lastClipSample", -9),
                                  preRoll + 5 * kBlock);
                    firstLength = lengthOf (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav"));
                }
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.armContinue (firstLength);
                expect (rec.startRecording (dir), rec.getStartError());
                for (int b = 0; b < 10; ++b) block (rec, 1, b == 3, false);
                rec.stopRecording();
                const auto track = trackReport (dir, 0);
                expectEquals ((juce::int64) track.getProperty ("lastClipSample", -9), firstLength + 3 * kBlock);
                expectEquals ((int) track.getProperty ("clipCount", -9), 2, "the continue forgot the first pass's clip");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void sessionFolderMoved()
        {
            beginTest ("A session folder renamed during a take is named, and STOP says the take was not finished there");
            Dirs dirs;
            const auto moved = dirs.session.getSiblingFile ("renamed");
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, 1);
            rec.getTrack (0).armed.store (true);
            rec.volumeProbePeriodMs = 10;
            expect (rec.startRecording (dirs.session));
            Feeder feeder { rec };
            feeder.blocks (10, { 0 });
            expect (dirs.session.moveFileTo (moved), "the fixture could not rename the folder");
            const auto named = waitFor ([&rec, &dirs]
            {
                return rec.getCopyProblems().joinIntoString ("|").contains (dirs.session.getFullPathName());
            });
            expect (named, "the take never said its folder had gone");
            feeder.blocks (10, { 0 });
            rec.stopRecording();

            expect (rec.hasReportWriteFailed(), "a take with no report was called clean");
            const auto problems = rec.getCopyProblems().joinIntoString ("|");
            expect (problems.contains (dirs.session.getFullPathName()), problems);
            expect (! dirs.session.exists(), "STOP made the old folder again");
            expect (moved.getChildFile ("Audio Files").getChildFile ("Track_01.wav").existsAsFile(), "the audio went");
            expect (moved.getChildFile ("recording.session").existsAsFile(), "nothing asks to recover this take");
            expect (problems.contains ("intact"), "a folder that kept every write should say its audio is intact: " + problems);
            MultitrackRecorder::deleteSessionAfterCancellingReports (moved);

            beginTest ("A session folder whose drive was pulled during a take never claims its audio is intact");
            {
                Dirs pulled;
                const auto away = pulled.session.getSiblingFile ("unplugged");
                MultitrackRecorder recorder;
                recorder.prepare (kRate, kBlock, 1);
                recorder.getTrack (0).armed.store (true);
                expect (recorder.startRecording (pulled.session));
                Feeder writes { recorder };
                writes.blocks (10, { 0 });
                expect (pulled.session.moveFileTo (away), "the fixture could not take the folder away");
                // Every write after a drive is pulled fails.
                recorder.copies.front()->writeFailed.store (true);
                recorder.stopRecording();
                expect (recorder.isLostSessionFolderIncomplete());
                const auto said = recorder.getCopyProblems().joinIntoString ("|");
                expect (said.contains (pulled.session.getFullPathName()), said);
                expect (said.contains ("unplugged"), "a pulled drive is not among the causes: " + said);
                expect (! said.contains ("intact"), "a take whose writes failed was called intact: " + said);
                MultitrackRecorder::deleteSessionAfterCancellingReports (away);

                // The drive itself gone reads the same, with or without a failed write.
                expect (MultitrackRecorder::driveIsGone (juce::File ("/Volumes/zf-no-such-drive-" + juce::Uuid().toString())
                                                             .getChildFile ("take")));
                expect (! MultitrackRecorder::driveIsGone (pulled.root), "a folder on a mounted drive read as unplugged");
            }
        }

        void pulledDriveReadsNoVolume()
        {
            beginTest ("A volume that changes under its probe reads as no volume");
            {
                auto volume = std::make_shared<std::atomic<juce::uint64>> (7);
                VolumeProbe probe (juce::File(), [volume] (const juce::File&)
                {
                    VolumeProbe::Reading reading;
                    reading.freeBytes = 1000000; reading.totalBytes = 9000000;
                    reading.volumeId = volume->load();
                    return reading;
                }, 5, true);
                expect (waitFor ([&probe] { return probe.latest().totalBytes > 0; }), "the probe never read");
                volume->store (8);
                expect (waitFor ([&probe] { return probe.latest().totalBytes == 0; }),
                        "another volume's space was reported as this one's");
            }

            beginTest ("A pulled recording drive leaves no record time, not the boot disk's");
            {
                Dirs dirs;
                auto volume = std::make_shared<std::atomic<juce::uint64>> (7);
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.volumeProbePeriodMs = 5;
                rec.volumeReaderForTests = [volume] (const juce::File&)
                {
                    VolumeProbe::Reading reading;
                    reading.freeBytes = (juce::int64) 50 * 1000 * 1000 * 1000; reading.totalBytes = reading.freeBytes * 2;
                    reading.volumeId = volume->load();
                    return reading;
                };
                expect (rec.startRecording (dirs.session));
                Feeder feeder { rec };
                feeder.blocks (4, { 0 });
                expect (waitFor ([&rec] { return rec.readDiskEstimate().minutes > 0; }), "no estimate to start with");
                volume->store (8);
                expect (waitFor ([&rec] { return rec.readDiskEstimate().minutes == 0; }), "the time left read another disk");
                expect (rec.readDiskEstimate().primaryFreeBytes < 0);
                rec.stopRecording();
            }
        }

        void punchInstallCrashes()
        {
            beginTest ("A crash between removing a punch's capture and installing its splice keeps the punch");
            {
                const auto dir = repairDir ("install-gap");
                const auto audio = dir.getChildFile ("Audio Files");
                writeAiff (audio.getChildFile ("Track_01.punchbase.aif"), 100, 0.25f);
                const auto spliced = audio.getChildFile ("Track_01.punchtmp.aif");
                writeAiff (spliced, 250, 0.5f);
                expect (audio.getChildFile ("Track_01.punchinstall").create().wasOk());
                const auto splicedBytes = bytesOf (spliced);
                expect (MultitrackRecorder::recoverInterruptedPunches (audio));
                expectEquals (lengthOf (audio.getChildFile ("Track_01.aif")), (juce::int64) 100, "the base did not come back");
                expect (! spliced.exists(), "the temp file stayed in the take's folder");
                expect (archiveHolds (dir, splicedBytes), "the spliced punch was thrown away");
                expect (! audio.getChildFile ("Track_01.punchinstall").exists(), "the install marker stayed");
                dir.deleteRecursively();
            }

            beginTest ("A crash part way through installing a splice keeps every part of it");
            {
                const auto dir = repairDir ("install-part");
                const auto audio = dir.getChildFile ("Audio Files");
                writeAiff (audio.getChildFile ("Track_01.punchbase.aif"), 100, 0.25f);
                writeAiff (audio.getChildFile ("Track_01.aif"), 250, 0.5f);           // the splice's first part, moved in
                writeAiff (audio.getChildFile ("Track_01.punchtmp02.aif"), 260, 0.6f); // its second, not yet
                expect (audio.getChildFile ("Track_01.punchinstall").create().wasOk());
                const auto firstPart = bytesOf (audio.getChildFile ("Track_01.aif"));
                const auto secondPart = bytesOf (audio.getChildFile ("Track_01.punchtmp02.aif"));
                expect (MultitrackRecorder::recoverInterruptedPunches (audio));
                expectEquals (lengthOf (audio.getChildFile ("Track_01.aif")), (juce::int64) 100, "the base did not come back");
                expect (! audio.getChildFile ("Track_01_part02.aif").exists());
                expect (archiveHolds (dir, firstPart), "the splice's first part was thrown away");
                expect (archiveHolds (dir, secondPart), "the splice's second part was thrown away");
                dir.deleteRecursively();
            }

            beginTest ("A crash after a splice was committed keeps the spliced take whole");
            {
                const auto dir = repairDir ("install-committed");
                const auto audio = dir.getChildFile ("Audio Files");
                writeAiff (audio.getChildFile ("Track_01.aif"), 250, 0.5f);                    // the spliced take
                writeAiff (audio.getChildFile ("Track_01_part02.punchbase.aif"), 300, 0.25f);  // a base part not yet deleted
                expect (audio.getChildFile ("Track_01.punchinstall").create().wasOk());
                const auto spliced = bytesOf (audio.getChildFile ("Track_01.aif"));
                const auto basePart = bytesOf (audio.getChildFile ("Track_01_part02.punchbase.aif"));
                int recovered = -1;
                expect (MultitrackRecorder::recoverInterruptedPunches (audio, &recovered));
                expect (bytesOf (audio.getChildFile ("Track_01.aif")) == spliced, "the spliced take was replaced");
                expect (! audio.getChildFile ("Track_01_part02.aif").exists(), "an old base part joined the spliced take");
                expect (! audio.getChildFile ("Track_01_part02.punchbase.aif").exists());
                expect (archiveHolds (dir, basePart), "the old base part was deleted rather than kept");
                expect (! audio.getChildFile ("Track_01.punchinstall").exists(), "the install marker stayed");
                expectEquals (recovered, 0, "nothing was put back, so nothing needs reopening");
                dir.deleteRecursively();
            }
        }

        void copyHashesApart()
        {
            beginTest ("A backup unplugged while its files are hashed leaves the primary's verdict alone");
            {
                Dirs dirs;
                const auto copyFolder = dirs.backup.getChildFile ("take");
                const auto away = dirs.root.getChildFile ("unplugged");
                auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                auto done = std::make_shared<juce::WaitableEvent> (true);
                hooks->beforeCopyScan = [copyFolder, away] { copyFolder.moveFileTo (away); };
                hooks->finished = [done] { done->signal(); };
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.setBackupDirectory (dirs.backup);
                    rec.reportTestHooks = hooks;
                    expect (rec.startRecording (dirs.session));
                    Feeder feeder { rec };
                    feeder.blocks (20, { 0, 1 });
                    rec.stopRecording();
                    expect (done->wait (10000), "the hashes never finished");
                    expect (! rec.hasAsyncReportFailure(), "a missing backup failed the take's report");
                }
                auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expect (! (bool) report.getProperty ("sha256Failed", true), "the primary's hashes were called failed");
                expect ((bool) report.getProperty ("copySha256Missing", false), "nothing says the backup's hashes are owed");
                expect (! (bool) report.getProperty ("copySha256Failed", true));
                expect (trackReport (dirs.session, 0).getProperty ("sha256", {})[0].toString().length() == 64,
                        "the primary has no hash");

                // The drive comes back, and the next open finishes the backup.
                expect (away.moveFileTo (copyFolder));
                juce::WaitableEvent resumed;
                MultitrackRecorder::resumeReportHashes (dirs.session, [&resumed] { resumed.signal(); }, {});
                expect (resumed.wait (10000), "the owed hashes never ran");
                report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expect (! (bool) report.getProperty ("copySha256Missing", true), "the backup's hashes are still owed");
                expect (! (bool) report.getProperty ("sha256Failed", true));
                expect (trackReport (dirs.session, 0).getProperty ("backupSha256", {})[0].toString().length() == 64,
                        "the backup has no hash");
                expect (trackReport (dirs.session, 0).getProperty ("sha256", {})[0].toString().length() == 64,
                        "the primary's hash went");
            }

            beginTest ("A backup file that cannot be read fails the copy's hash, not the take's");
            {
                Dirs dirs;
                const auto backupFile = dirs.backupTake();
                auto hooks = std::make_shared<MultitrackRecorder::ReportTestHooks>();
                auto done = std::make_shared<juce::WaitableEvent> (true);
                hooks->beforeCopyScan = [backupFile] { ::chmod (backupFile.getFullPathName().toRawUTF8(), 0); };
                hooks->finished = [done] { done->signal(); };
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.setBackupDirectory (dirs.backup);
                    rec.reportTestHooks = hooks;
                    expect (rec.startRecording (dirs.session));
                    Feeder feeder { rec };
                    feeder.blocks (20, { 0, 1 });
                    rec.stopRecording();
                    expect (done->wait (10000), "the hashes never finished");
                    expect (! rec.hasAsyncReportFailure(), "an unreadable backup failed the take's report");
                }
                ::chmod (backupFile.getFullPathName().toRawUTF8(), 0644);
                const auto report = juce::JSON::parse (dirs.session.getChildFile ("session.report.json"));
                expect (! (bool) report.getProperty ("sha256Failed", true), "the primary's hashes were called failed");
                expect ((bool) report.getProperty ("copySha256Failed", false), "nothing says the backup could not be read");
            }
        }
    };

    static CaptureRepairTests captureRepairTests;

    // ── The engine ──────────────────────────────────────────────────────
    namespace
    {
        // A device the test plugs in and out, with rates and buffer sizes of
        // its own, so a reopen can be asked what it was opened at.
        struct RepairRig
        {
            std::map<juce::String, int> inputs;
            juce::String defaultInput { "Mic" };
            std::map<juce::String, juce::Array<double>> rates;
            std::map<juce::String, double> ownRate;
            std::map<juce::String, juce::Array<int>> buffers;
            std::map<juce::String, int> defaultBuffer;
            std::map<juce::String, std::pair<double, int>> openedAt;

            juce::StringArray inputNames() const
            {
                juce::StringArray names;
                for (const auto& device : inputs) names.add (device.first);
                return names;
            }
        };

        class RepairDevice final : public juce::AudioIODevice
        {
        public:
            RepairDevice (const juce::String& nameIn, RepairRig& rigIn)
                : juce::AudioIODevice (nameIn, "Repair"), rig (rigIn),
                  inputs (rig.inputs.count (nameIn) > 0 ? rig.inputs[nameIn] : 0) {}
            ~RepairDevice() override { stop(); }

            juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
            juce::StringArray getInputChannelNames() override
            {
                juce::StringArray names;
                for (int i = 0; i < inputs; ++i) names.add (juce::String (i + 1));
                return names;
            }
            juce::Array<double> getAvailableSampleRates() override
            { return rig.rates.count (getName()) > 0 ? rig.rates[getName()] : juce::Array<double> { kRepairRate }; }
            juce::Array<int> getAvailableBufferSizes() override
            { return rig.buffers.count (getName()) > 0 ? rig.buffers[getName()] : juce::Array<int> { kRepairBlock }; }
            int getDefaultBufferSize() override
            { return rig.defaultBuffer.count (getName()) > 0 ? rig.defaultBuffer[getName()] : kRepairBlock; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double rate, int buffer) override
            {
                opened = true;
                openRate = rate; openBuffer = buffer;
                rig.openedAt[getName()] = { rate, buffer };
                return {};
            }
            void close() override { stop(); opened = false; }
            bool isOpen() override { return opened; }
            void start (juce::AudioIODeviceCallback* cb) override
            {
                callback = cb;
                if (callback != nullptr) callback->audioDeviceAboutToStart (this);
            }
            void stop() override
            {
                if (auto* cb = std::exchange (callback, nullptr)) cb->audioDeviceStopped();
            }
            bool isPlaying() override { return callback != nullptr; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return opened ? openBuffer : getDefaultBufferSize(); }
            double getCurrentSampleRate() override
            {
                if (opened) return openRate;
                return rig.ownRate.count (getName()) > 0 ? rig.ownRate[getName()] : kRepairRate;
            }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return 3; }
            juce::BigInteger getActiveInputChannels() const override
            {
                juce::BigInteger bits;
                bits.setRange (0, inputs, true);
                return bits;
            }

        private:
            RepairRig& rig;
            int inputs;
            bool opened { false };
            double openRate { 0.0 };
            int openBuffer { 0 };
            juce::AudioIODeviceCallback* callback { nullptr };
        };

        class RepairType final : public juce::AudioIODeviceType
        {
        public:
            explicit RepairType (RepairRig& rigIn) : juce::AudioIODeviceType ("Repair"), rig (rigIn) {}
            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool wantInputs) const override
            { return wantInputs ? rig.inputNames() : juce::StringArray { "Speakers" }; }
            int getDefaultDeviceIndex (bool forInput) const override
            { return forInput ? juce::jmax (0, rig.inputNames().indexOf (rig.defaultInput)) : 0; }
            int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
            bool hasSeparateInputsAndOutputs() const override { return true; }
            juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
            { return new RepairDevice (input.isNotEmpty() ? input : output, rig); }
            void listChanged() { callDeviceChangeListeners(); }

        private:
            RepairRig& rig;
        };

        // The engine on a device of the test's own, its callback attached the
        // way it is outside a test process.
        struct OnRepairRig
        {
            AudioEngine& engine;
            RepairType* type { nullptr };

            OnRepairRig (AudioEngine& engineIn, RepairRig& rig) : engine (engineIn)
            {
                auto owned = std::make_unique<RepairType> (rig);
                type = owned.get();
                manager().addAudioDeviceType (std::move (owned));
                manager().setCurrentAudioDeviceType ("Repair", true);
                engine.openChosenDevice();
                manager().addAudioCallback (&engine);
            }
            ~OnRepairRig()
            {
                manager().removeAudioCallback (&engine);
                manager().closeAudioDevice();
            }
            juce::AudioDeviceManager& manager() { return engine.getDeviceManager(); }
            juce::String input() { return manager().getAudioDeviceSetup().inputDeviceName; }
            void choose (const juce::String& device)
            {
                auto setup = manager().getAudioDeviceSetup();
                setup.inputDeviceName = device;
                setup.useDefaultInputChannels = true;
                manager().setAudioDeviceSetup (setup, true);
                engine.noteChosenDevice();
            }
        };

        void forgetChosenDevice (AudioEngine& engine)
        {
            updateSettings (engine.getAppProps(), [] (juce::PropertiesFile& props)
            {
                for (const auto* key : { "chosenInputDevice", "chosenOutputDevice", "chosenInputUid",
                                         "chosenOutputUid", "chosenInputCount" })
                    props.removeValue (key);
            });
        }

        // Each value names its position, so audio from the wrong place shows.
        float positionValue (juce::int64 position) noexcept
        {
            return 0.1f + (float) (position % 1000) / 4000.0f;
        }

        struct PositionSource final : public juce::AudioFormatReader
        {
            explicit PositionSource (juce::int64 length) : juce::AudioFormatReader (nullptr, "test")
            {
                sampleRate = kRepairRate; lengthInSamples = length; numChannels = 1;
                bitsPerSample = 32; usesFloatingPointData = true;
            }
            bool readSamples (int* const* dest, int numDest, int offset, juce::int64 start, int num) override
            {
                for (int ch = 0; ch < numDest; ++ch)
                    if (auto* out = reinterpret_cast<float*> (dest[ch]))
                        for (int i = 0; i < num; ++i) out[offset + i] = positionValue (start + i);
                return true;
            }
        };
    }

    class EngineRepairTests final : public juce::UnitTest
    {
    public:
        EngineRepairTests() : UnitTest ("Engine repairs", "zynforge") {}

        static constexpr double kRate = kRepairRate;
        static constexpr int kBlock = kRepairBlock;

        static void feedRecorder (AudioEngine& engine, int blocks, float value)
        {
            auto& rec = engine.getRecorder();
            std::vector<float> samples ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) rec.getNumTracks(), samples.data());
            for (int b = 0; b < blocks; ++b)
            {
                rec.processBlock (inputs.data(), (int) inputs.size(), kBlock);
                rec.drainPendingForTests();
            }
        }

        static void recordOn (AudioEngine& engine, const juce::File& dir, int blocks, float value, bool continuing)
        {
            engine.getRecorder().getTrack (0).armed.store (true);
            if (continuing) engine.armContinue (engine.getCaptureLengthSamples());
            if (! engine.startRecording (dir)) return;
            feedRecorder (engine, blocks, value);
            engine.stopRecording();
        }

        static void republish (AudioEngine& engine)
        {
            engine.syncActiveTake (0);
            engine.getPlayer().setTrackClips (0, engine.clipsFor (0));
        }

        // Runs the engine's callback with every input at `value`.
        struct Callback
        {
            AudioEngine& engine;
            juce::AudioBuffer<float> in, out;
            Callback (AudioEngine& e, int inputs, int outputs) : engine (e), in (inputs, kBlock), out (outputs, kBlock) {}
            void run (int blocks, float value)
            {
                for (int ch = 0; ch < in.getNumChannels(); ++ch)
                    for (int i = 0; i < kBlock; ++i) in.setSample (ch, i, value);
                for (int b = 0; b < blocks; ++b)
                {
                    out.clear();
                    engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                             out.getArrayOfWritePointers(), out.getNumChannels(),
                                                             kBlock, {});
                }
            }
            void runChannels (int blocks, const std::vector<float>& values)
            {
                for (int ch = 0; ch < in.getNumChannels(); ++ch)
                    for (int i = 0; i < kBlock; ++i) in.setSample (ch, i, values[(size_t) ch]);
                for (int b = 0; b < blocks; ++b)
                {
                    out.clear();
                    engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                             out.getArrayOfWritePointers(), out.getNumChannels(),
                                                             kBlock, {});
                }
            }
        };

        static void plainStrips (AudioEngine& engine, int strips)
        {
            engine.getMasterState().muted.store (false);
            engine.getMasterState().gainDb.store (0.0f);
            engine.setMasterStereo (true);
            for (int i = 0; i < strips; ++i)
            {
                auto& t = engine.getRecorder().getTrack (i);
                t.gainDb.store (0.0f); t.pan.store (0.0f); t.muted.store (false); t.soloed.store (false);
                t.armed.store (false); t.monitor.store (false); t.isStereo.store (false);
                engine.setTrackInputRouting (i, i);
                engine.setTrackOutputRouting (i, -1);
            }
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            {
                AudioEngine engine;
                forgetChosenDevice (engine);   // whatever an earlier suite chose
            }
            undoAfterAContinue();
            overlappingNearbyClips();
            deviceComesBackAtItsOwnRate();
            markersKeptWhileAWriteIsOwed();
            loudnessBelongsToTheTake();
            preRollAndPlaybackSurviveALayoutChange();
            stereoMixLinesUpWithTheTracks();
        }

        void undoAfterAContinue()
        {
            const juce::int64 first = 40 * kBlock, trim = 10 * kBlock;

            beginTest ("Undo after a continued take puts the new audio where STOP placed it");
            {
                const auto dir = repairDir ("undo-continue");
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                recordOn (engine, dir, 40, 0.25f, false);
                engine.clipsFor (0)[0].fileLengthSamples -= trim;   // the take ends before its file does
                republish (engine);
                const auto beforeFade = engine.playlistsToJson();
                engine.clipsFor (0)[0].fadeInSamples = 1000;       // the step to undo
                republish (engine);
                recordOn (engine, dir, 30, 0.5f, true);

                auto clips = engine.clipsFor (0);
                expectEquals ((int) clips.size(), 2, "STOP did not add the new span");
                if (clips.size() == 2)
                {
                    const auto placed = clips[1].timelineStartSamples;
                    expectEquals (placed, first - trim, "STOP did not place the take at the end of the arrangement");
                    engine.restorePlaylistSnapshot (beforeFade);
                    clips = engine.clipsFor (0);
                    expectEquals ((int) clips.size(), 2);
                    if (clips.size() == 2)
                    {
                        expectEquals (clips[1].timelineStartSamples, placed, "undo moved the new audio");
                        expectEquals (clips[1].fileStartSamples, first);
                        expectEquals (clips[0].fadeInSamples, (juce::int64) 0, "the edit was not undone");
                    }
                }
                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Undo after a take continued past a duplicate never lays it on top of the duplicate");
            {
                const auto dir = repairDir ("undo-duplicate");
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                recordOn (engine, dir, 40, 0.25f, false);
                auto duplicate = engine.clipsFor (0)[0];
                duplicate.timelineStartSamples = first;
                engine.clipsFor (0).push_back (duplicate);
                republish (engine);
                const auto before = engine.playlistsToJson();
                engine.clipsFor (0)[0].fadeInSamples = 1000;
                republish (engine);
                recordOn (engine, dir, 30, 0.5f, true);
                expectEquals (engine.clipsFor (0).back().timelineStartSamples, 2 * first,
                              "STOP did not place the take after the duplicate");
                engine.restorePlaylistSnapshot (before);
                const auto& clips = engine.clipsFor (0);
                expectEquals ((int) clips.size(), 3);
                if (clips.size() == 3)
                    expectEquals (clips[2].timelineStartSamples, 2 * first, "undo laid the new audio over the duplicate");
                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A continued take that crashed comes back where it was recorded, at the end of the arrangement");
            {
                const auto dir = repairDir ("crash-continue");
                juce::var playlists, lengths;
                {
                    AudioEngine engine;
                    engine.setStripCount (1);
                    engine.prepareForTests (kRate, kBlock);
                    recordOn (engine, dir, 40, 0.25f, false);
                    engine.clipsFor (0)[0].fileLengthSamples -= trim;
                    republish (engine);
                    playlists = engine.playlistsToJson();
                    lengths = engine.mediaLengthsToJson();
                    engine.getRecorder().getTrack (0).armed.store (true);
                    engine.armContinue (engine.getCaptureLengthSamples());
                    expect (engine.startRecording (dir));
                    feedRecorder (engine, 30, 0.5f);
                    // What a crash leaves: the marker STOP would have removed.
                    const auto marker = dir.getChildFile ("recording.session").loadFileAsString();
                    engine.stopRecording();
                    expect (dir.getChildFile ("recording.session").replaceWithText (marker));
                }
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                expectEquals (engine.loadSession (dir), 1);
                engine.loadPlaylistsFromJson (playlists);
                engine.reconcileClipsWithMedia (lengths, true);
                const auto& clips = engine.clipsFor (0);
                expectEquals ((int) clips.size(), 2);
                if (clips.size() == 2)
                    expectEquals (clips[1].timelineStartSamples, first - trim, "the recovered take sits at its file position");
                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void overlappingNearbyClips()
        {
            beginTest ("Two clips of one file less than a block apart both stay in RAM through their overlap");
            juce::TimeSliceThread thread ("repair reader");
            thread.startThread();
            {
                PlaybackReader reader (new PositionSource ((juce::int64) kRate * 60), thread, (int) kRate * 2);
                juce::int64 first = 30000, second = 50000;   // different blocks, under one block apart
                const bool ready = waitFor ([&]
                {
                    // What the player does while it holds a jump: both clips, in list order.
                    reader.prepareAt (first);
                    reader.prepareAt (second);
                    return reader.isBuffered (first, 512) && reader.isBuffered (second, 512);
                }, 2000);
                expect (ready, "the earlier clip's block never came into RAM");

                juce::AudioBuffer<float> out (1, 512);
                int silentFirst = 0, wrong = 0;
                for (int pass = 0; pass < 400; ++pass)
                {
                    for (const auto at : { first, second })
                    {
                        out.clear();
                        const bool read = reader.read (&out, 0, 512, at, true, true);
                        if (! read && at == first) ++silentFirst;
                        if (read)
                            for (int i = 0; i < 512; ++i)
                                if (std::abs (out.getSample (0, i) - positionValue (at + i)) > 1.0e-6f) { ++wrong; break; }
                    }
                    first += 512; second += 512;
                    juce::Thread::sleep (1);
                }
                expectEquals (silentFirst, 0, "the earlier clip dropped out of the crossfade");
                expectEquals (wrong, 0, "a read returned audio from another place");
            }
            thread.stopThread (-1);
        }

        void deviceComesBackAtItsOwnRate()
        {
            beginTest ("A chosen interface that comes back opens at its own rate and buffer, not the stand-in's");
            RepairRig rig;
            rig.inputs = { { "Mic", 2 }, { "Rig", 32 } };
            rig.rates["Mic"] = { 48000.0 };       rig.buffers["Mic"] = { 512 };      rig.defaultBuffer["Mic"] = 512;
            rig.rates["Rig"] = { 44100.0, 48000.0, 96000.0 };
            rig.ownRate["Rig"] = 96000.0;
            rig.buffers["Rig"] = { 128, 256, 512 }; rig.defaultBuffer["Rig"] = 128;
            {
                AudioEngine engine;
                OnRepairRig on (engine, rig);
                on.choose ("Rig");
                rig.inputs.erase ("Rig");
                on.type->listChanged();
                expectEquals (on.input(), juce::String ("Mic"), "no stand-in took over");
                rig.inputs["Rig"] = 32;
                on.type->listChanged();
                juce::String error;
                expect (engine.reopenChosenDevice (error), error);
                expectEquals (on.input(), juce::String ("Rig"));
                expect (rig.openedAt.count ("Rig") > 0);
                expectEquals (rig.openedAt["Rig"].first, 96000.0, "the interface was asked for the stand-in's rate");
                expectEquals (rig.openedAt["Rig"].second, 128, "the interface took the stand-in's buffer");
                forgetChosenDevice (engine);
            }
        }

        void markersKeptWhileAWriteIsOwed()
        {
            beginTest ("Markers dropped during a take survive the take's folder being set again");
            const auto dir = repairDir ("markers");
            const auto other = repairDir ("markers-other");
            auto release = std::make_shared<juce::WaitableEvent> (true);
            {
                MarkersManager markers;
                bool deferred = true;
                markers.deferDiskWhile = [&deferred] { return deferred; };
                markers.setContext (dir, kRate);
                markers.setBeforeDeferredWriteForTests ([release] { release->wait (5000); });
                markers.drop (100, "Encore");                  // its write is held
                markers.setContext (dir, kRate);               // the take's own folder, set again
                expectEquals (markers.getCount(), 1, "the reload dropped the newest marker");
                markers.setContext (other, kRate);
                markers.setContext (dir, kRate);               // away and back while the write is held
                expectEquals (markers.getCount(), 1, "a reload read the file the held write has not reached");
                if (markers.getCount() == 1) expectEquals (markers.getLast().name, juce::String ("Encore"));
                release->signal();
                expect (waitFor ([&dir] { return dir.getChildFile ("markers.json").loadFileAsString().contains ("Encore"); }),
                        "the held write never landed");
                deferred = false;
                markers.drop (200, "Bows");
                markers.setContext (other, kRate);
                markers.setContext (dir, kRate);
                expectEquals (markers.getCount(), 2, "a saved list did not read back");
                markers.setBeforeDeferredWriteForTests ({});
            }
            dir.deleteRecursively();
            other.deleteRecursively();
        }

        void loudnessBelongsToTheTake()
        {
            beginTest ("A take starts the header's integrated loudness and true peak afresh");
            {
                const auto dir = repairDir ("loudness-take");
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                plainStrips (engine, 1);
                engine.getRecorder().getTrack (0).monitor.store (true);
                Callback cb (engine, 1, 2);
                cb.run ((int) (kRate * 0.6) / kBlock, 1.4f);      // a hot soundcheck moment
                expectGreaterThan (engine.getTruePeakDb(), -3.0f, "the fixture never reached the master");
                engine.getRecorder().getTrack (0).armed.store (true);
                expect (engine.startRecording (dir));
                cb.run (2, 0.01f);
                expectLessThan (engine.getTruePeakDb(), -20.0f, "soundcheck's peak is still the take's");
                engine.stopRecording();
                engine.clearAllStripOverrides();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Adding a channel keeps the header's loudness readings");
            {
                RepairRig rig;
                rig.inputs = { { "Mic", 2 } };
                AudioEngine engine;
                OnRepairRig on (engine, rig);
                engine.setStripCount (1);
                plainStrips (engine, 1);
                engine.getRecorder().getTrack (0).monitor.store (true);
                Callback cb (engine, 2, 2);
                cb.run ((int) (kRate * 0.6) / kBlock, 1.4f);
                const auto peak = engine.getTruePeakDb();
                const auto integrated = engine.getIntegratedLufs();
                expectGreaterThan (peak, -3.0f, "the fixture never reached the master");
                engine.setStripCount (2);
                expectEquals (engine.getTruePeakDb(), peak, "a new channel zeroed the true peak");
                expectEquals (engine.getIntegratedLufs(), integrated, "a new channel zeroed the integrated loudness");
                engine.setStripCount (0);
                engine.clearAllStripOverrides();
                forgetChosenDevice (engine);
            }
        }

        void preRollAndPlaybackSurviveALayoutChange()
        {
            beginTest ("Adding a channel keeps every strip's pre-roll");
            {
                const auto dir = repairDir ("preroll-add");
                RepairRig rig;
                rig.inputs = { { "Mic", 2 } };
                {
                    AudioEngine engine;
                    OnRepairRig on (engine, rig);
                    engine.setStripCount (1);
                    plainStrips (engine, 1);
                    engine.getRecorder().setPreRollSeconds (1);
                    Callback cb (engine, 2, 2);
                    cb.run ((int) (kRate * 1.5) / kBlock, 0.25f);
                    engine.setStripCount (2);
                    engine.getRecorder().getTrack (0).armed.store (true);
                    expect (engine.startRecording (dir));
                    cb.run (10, 0.5f);
                    engine.stopRecording();
                    engine.getRecorder().setPreRollSeconds (0);
                    engine.setStripCount (0);
                    engine.clearAllStripOverrides();
                    forgetChosenDevice (engine);
                }
                const auto take = audioOf (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav"));
                expectGreaterThan (take.getNumSamples(), 10 * kBlock + (int) kRate / 2, "the take lost its pre-roll");
                if (take.getNumSamples() > 0)
                    expect (std::abs (take.getSample (0, 0) - 0.25f) < 0.001f, "the pre-roll is not what the strip heard");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A strip move keeps each strip's own pre-roll");
            {
                const auto dir = repairDir ("preroll-move");
                RepairRig rig;
                rig.inputs = { { "Mic", 2 } };
                {
                    AudioEngine engine;
                    OnRepairRig on (engine, rig);
                    engine.setStripCount (2);
                    plainStrips (engine, 2);
                    engine.getRecorder().setPreRollSeconds (1);
                    Callback cb (engine, 2, 2);
                    cb.runChannels ((int) (kRate * 1.5) / kBlock, { 0.25f, 0.5f });
                    expect (engine.reorderTracks ({ 1, 0 }), engine.getReorderRefusal());
                    // Strip 1 is now the one patched to input 2.
                    engine.getRecorder().getTrack (0).armed.store (true);
                    expect (engine.startRecording (dir));
                    cb.runChannels (10, { 0.25f, 0.5f });
                    engine.stopRecording();
                    engine.getRecorder().setPreRollSeconds (0);
                    engine.setStripCount (0);
                    engine.clearAllStripOverrides();
                    forgetChosenDevice (engine);
                }
                const auto take = audioOf (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav"));
                expectGreaterThan (take.getNumSamples(), 10 * kBlock + (int) kRate / 2, "the take lost its pre-roll");
                if (take.getNumSamples() > 0)
                    expect (std::abs (take.getSample (0, 0) - 0.5f) < 0.001f, "the pre-roll came from another strip's input");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Adding a channel while a session plays keeps it playing where it was");
            {
                const auto dir = repairDir ("play-add");
                {
                    AudioEngine engine;
                    engine.setStripCount (1);
                    engine.prepareForTests (kRate, kBlock);
                    recordOn (engine, dir, 400, 0.25f, false);
                    engine.setStripCount (0);
                    engine.clearAllStripOverrides();
                }
                RepairRig rig;
                rig.inputs = { { "Mic", 2 } };
                AudioEngine engine;
                OnRepairRig on (engine, rig);
                engine.setStripCount (1);
                expectEquals (engine.loadSession (dir), 1);
                engine.getPlayer().setPositionSamples (20 * kBlock);
                expect (engine.startPlayback(), "the fixture would not play");
                Callback cb (engine, 2, 2);
                cb.run (4, 0.0f);
                const auto at = engine.getPlayer().getPositionSamples();
                engine.setStripCount (2);
                expect (engine.isPlaying(), "adding a channel stopped playback");
                expectGreaterOrEqual (engine.getPlayer().getPositionSamples(), at, "playback jumped back");
                engine.stopPlayback();
                engine.setStripCount (0);
                engine.clearAllStripOverrides();
                forgetChosenDevice (engine);
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void stereoMixLinesUpWithTheTracks()
        {
            beginTest ("StereoMix.wav starts on the same sample as the track files, pre-roll and all");
            const auto dir = repairDir ("stereomix");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                plainStrips (engine, 1);
                auto& track = engine.getRecorder().getTrack (0);
                track.streamSend.store (true);
                engine.setStreamOutputs (-1, -1);
                engine.setRecordStereoMix (true);
                engine.getRecorder().setPreRollSeconds (1);
                Callback cb (engine, 1, 2);
                cb.run ((int) (kRate * 1.5) / kBlock, 0.25f);
                track.armed.store (true);
                expect (engine.startRecording (dir));
                cb.run (20, 0.5f);
                engine.stopRecording();
                engine.setRecordStereoMix (false);
                engine.getRecorder().setPreRollSeconds (0);
                engine.clearAllStripOverrides();
            }
            const auto take = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
            const auto mix  = dir.getChildFile ("Export Files").getChildFile ("StereoMix.wav");
            const auto report = juce::JSON::parse (dir.getChildFile ("session.report.json"));
            const auto preRoll = (int) (juce::int64) report.getProperty ("passPreRollSamples", 0);
            expectGreaterThan (preRoll, 0, "the fixture wrote no pre-roll");
            expectEquals (lengthOf (mix), lengthOf (take), "the mix and the tracks differ in length");
            const auto audio = audioOf (mix);
            if (audio.getNumSamples() > preRoll)
            {
                expectEquals (audio.getSample (0, preRoll - 1), 0.0f, "the mix has audio where the tracks have pre-roll");
                expectGreaterThan (std::abs (audio.getSample (0, preRoll)), 0.1f, "the mix's first live sample is not there");
            }
            MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
        }
    };

    static EngineRepairTests engineRepairTests;
}
