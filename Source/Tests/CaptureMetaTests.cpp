// Capture metadata: the BWF time stamp (chunk walker, 8-byte patch, every copy
// and part of a real take through the real recorder), channel names in the
// bext Description, and the device clock source in pre-flight and the report.

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/BwfStamp.h"
#include "../Audio/ClockSource.h"
#include "../Audio/FastHash.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Capture/CaptureDaemon.h"
#include "LtcTestSignal.h"

#include <atomic>
#include <functional>

namespace zynforge
{
    class CaptureMetaTests final : public juce::UnitTest
    {
    public:
        CaptureMetaTests() : juce::UnitTest ("Capture metadata", "zynforge") {}

        void runTest() override
        {
            // Before the first engine exists: it must never open a real device.
            AudioEngine::setTestModeSkipAudioInit (true);
            chunkWalker();
            channelNames();
            stampsOnRealTakes();
            punchKeepsTheTakeStamp();
            otherContainers();
            reportAndHash();
            timeSources();
            clockSource();
        }

    private:
        using StampResult = bwf::StampResult;

        static constexpr double kRate  = 48000.0;
        static constexpr int    kBlock = 256;

        static juce::File tempDir (const juce::String& tag)
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-meta-" + tag + "-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        static juce::MemoryBlock bytesOf (const juce::File& file)
        {
            juce::MemoryBlock block;
            file.loadFileAsData (block);
            return block;
        }

        static juce::String tagText (const char* tag) { return juce::String (tag, 4); }

        // ── Files for the walker ──────────────────────────────────────────
        static void appendChunk (juce::MemoryOutputStream& out, const char* tag,
                                 const juce::MemoryBlock& body, juce::uint32 declaredSize)
        {
            out.write (tag, 4);
            out.writeInt ((int) declaredSize);
            out << body;
            if ((body.getSize() & 1) != 0) out.writeByte (0);
        }

        static juce::MemoryBlock zeros (size_t size)
        {
            juce::MemoryBlock block (size, true);
            return block;
        }

        static juce::MemoryBlock bextBody (juce::uint64 timeReference, size_t size = 604)
        {
            auto body = zeros (size);
            if (size >= (size_t) bwf::kTimeReferenceOffset + 8)
                std::memcpy (static_cast<char*> (body.getData()) + bwf::kTimeReferenceOffset,
                             &timeReference, 8);
            return body;
        }

        // RF64 laid out as JUCE writes it past 4 GiB: ds64 first, bext before
        // data, -1 in the RF64 and data sizes.
        static juce::MemoryBlock rf64File (juce::uint64 stamp, bool bextAfterData)
        {
            const juce::uint64 dataSize = 1000;
            juce::MemoryOutputStream body;
            auto ds64 = zeros (28);
            std::memcpy (static_cast<char*> (ds64.getData()) + 8, &dataSize, 8);
            appendChunk (body, "ds64", ds64, 28);
            appendChunk (body, "fmt ", zeros (16), 16);
            if (! bextAfterData) appendChunk (body, "bext", bextBody (stamp), 604);
            appendChunk (body, "data", zeros ((size_t) dataSize), 0xffffffffu);
            if (bextAfterData) appendChunk (body, "bext", bextBody (stamp), 604);

            juce::MemoryOutputStream file;
            file.write ("RF64", 4);
            file.writeInt (-1);
            file.write ("WAVE", 4);
            file << body.getMemoryBlock();
            return file.getMemoryBlock();
        }

        static juce::MemoryBlock riffFile (const std::function<void (juce::MemoryOutputStream&)>& chunks)
        {
            juce::MemoryOutputStream body;
            chunks (body);
            juce::MemoryOutputStream file;
            file.write ("RIFF", 4);
            file.writeInt ((int) body.getDataSize() + 4);
            file.write ("WAVE", 4);
            file << body.getMemoryBlock();
            return file.getMemoryBlock();
        }

        // A real JUCE WAV holding `stamp`, the same writer the recorder uses.
        static void writeJuceWav (const juce::File& file, juce::int64 stamp, int bits)
        {
            juce::StringPairArray meta;
            meta.set (juce::WavAudioFormat::bwavDescription, "walker test");
            meta.set (juce::WavAudioFormat::bwavTimeReference, juce::String (stamp));
            auto* stream = file.createOutputStream().release();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (stream, kRate, 1, bits, meta, 0));
            juce::AudioBuffer<float> audio (1, 777);
            for (int i = 0; i < 777; ++i) audio.setSample (0, i, 0.25f);
            writer->writeFromAudioSampleBuffer (audio, 0, 777);
        }

        // Stamps `file` and proves nothing outside the 8 bytes moved.
        void expectPatchesOnlyTheStamp (const juce::File& file, juce::uint64 expected, juce::uint64 value)
        {
            const auto before = bytesOf (file);
            juce::FileInputStream in (file);
            const auto bext = bwf::findBext (in);
            expect (bext.result == StampResult::ok);
            const auto offset = bext.dataStart + bwf::kTimeReferenceOffset;

            expect (bwf::stampTimeReference (file, expected, value) == StampResult::ok);

            const auto after = bytesOf (file);
            expectEquals ((juce::int64) after.getSize(), (juce::int64) before.getSize());
            int changedOutside = 0;
            for (size_t i = 0; i < before.getSize() && i < after.getSize(); ++i)
                if (before[i] != after[i] && ((juce::int64) i < offset || (juce::int64) i >= offset + 8))
                    ++changedOutside;
            expectEquals (changedOutside, 0, "bytes outside the time reference changed");

            juce::uint64 stored = 0;
            expect (bwf::readTimeReference (file, stored) == StampResult::ok);
            expect (stored == value);
        }

        void expectRefused (const juce::String& what, const juce::MemoryBlock& contents, StampResult result)
        {
            const auto dir = tempDir ("refuse");
            const auto file = dir.getChildFile ("Track_01.wav");
            file.replaceWithData (contents.getData(), contents.getSize());
            expect (bwf::stampTimeReference (file, 7, 9) == result, what);
            expect (bytesOf (file) == contents, what + " was changed");
            dir.deleteRecursively();
        }

        void chunkWalker()
        {
            beginTest ("Chunk walker stamps a real WAV and touches 8 bytes only");
            {
                const auto dir = tempDir ("walker");
                for (const int bits : { 16, 24, 32 })
                {
                    const auto file = dir.getChildFile ("w" + juce::String (bits) + ".wav");
                    writeJuceWav (file, 12345, bits);
                    // Above 2^32 on purpose: a day at 192 kHz needs the high word.
                    const juce::uint64 value = 16000000000ull + (juce::uint64) bits;
                    expectPatchesOnlyTheStamp (file, 12345, value);

                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatReader> reader (
                        wav.createReaderFor (file.createInputStream().release(), true));
                    expect (reader != nullptr);
                    if (reader != nullptr)
                    {
                        expectEquals (reader->metadataValues[juce::WavAudioFormat::bwavTimeReference],
                                      juce::String ((juce::int64) value), "JUCE reads a different stamp");
                        expectEquals (reader->lengthInSamples, (juce::int64) 777);
                    }
                }
                dir.deleteRecursively();
            }

            beginTest ("Chunk walker handles RF64, with bext before or after data");
            {
                const auto dir = tempDir ("rf64");
                for (const bool after : { false, true })
                {
                    const auto file = dir.getChildFile (after ? "after.wav" : "before.wav");
                    const auto contents = rf64File (5555, after);
                    file.replaceWithData (contents.getData(), contents.getSize());
                    expectPatchesOnlyTheStamp (file, 5555, 6666);
                }
                dir.deleteRecursively();
            }

            beginTest ("Chunk walker honours the pad byte of an odd-sized chunk");
            {
                const auto dir = tempDir ("pad");
                const auto file = dir.getChildFile ("pad.wav");
                const auto contents = riffFile ([] (juce::MemoryOutputStream& out)
                {
                    appendChunk (out, "LIST", zeros (5), 5);
                    appendChunk (out, "bext", bextBody (11), 604);
                    appendChunk (out, "data", zeros (10), 10);
                });
                file.replaceWithData (contents.getData(), contents.getSize());
                expectPatchesOnlyTheStamp (file, 11, 22);
                dir.deleteRecursively();
            }

            beginTest ("Corrupt or absent bext refuses and leaves the file alone");
            {
                const auto noBext = riffFile ([] (juce::MemoryOutputStream& out)
                { appendChunk (out, "data", zeros (100), 100); });
                expectRefused ("no bext", noBext, StampResult::noBext);

                const auto tooShort = riffFile ([] (juce::MemoryOutputStream& out)
                { appendChunk (out, "bext", bextBody (7, 300), 300); });
                expectRefused ("short bext", tooShort, StampResult::badBext);

                auto past = riffFile ([] (juce::MemoryOutputStream& out)
                { appendChunk (out, "bext", bextBody (7), 604); });
                past.setSize (past.getSize() - 100);
                expectRefused ("bext past the end of the file", past, StampResult::badBext);

                juce::MemoryBlock text ("this is not a wav file at all", 29);
                expectRefused ("not a wav", text, StampResult::notWav);

                auto riffNotWave = riffFile ([] (juce::MemoryOutputStream&) {});
                std::memcpy (static_cast<char*> (riffNotWave.getData()) + 8, "AVI ", 4);
                expectRefused ("RIFF that is not WAVE", riffNotWave, StampResult::notWav);

                const auto unexpected = riffFile ([] (juce::MemoryOutputStream& out)
                { appendChunk (out, "bext", bextBody (8), 604); });
                expectRefused ("stamp differs from what we wrote", unexpected, StampResult::unexpectedValue);

                expect (bwf::stampTimeReference (juce::File ("/nonexistent/zf.wav"), 1, 2)
                        == StampResult::missingFile);
            }

            beginTest ("wrapToDay wraps both ends of the day");
            {
                const auto day = (juce::int64) 48000 * 86400;
                expectEquals (bwf::wrapToDay (0, kRate), (juce::int64) 0);
                expectEquals (bwf::wrapToDay (day - 1, kRate), day - 1);
                expectEquals (bwf::wrapToDay (day, kRate), (juce::int64) 0);
                expectEquals (bwf::wrapToDay (day + 5, kRate), (juce::int64) 5);
                expectEquals (bwf::wrapToDay (-1, kRate), day - 1);
                expectEquals (bwf::wrapToDay (5, 0.0), (juce::int64) 0);
            }
        }

        void channelNames()
        {
            const juce::String takeText ("Zynforge Recording -- Show -- Track_01");

            beginTest ("sTRK lines lead the Description and keep the take text");
            {
                expectEquals (bwf::channelDescription (takeText, "Kick In"),
                              "sTRK1=Kick In\r\n" + takeText);
                expectEquals (bwf::channelDescription (takeText, "OH L", "OH R"),
                              "sTRK1=OH L\r\nsTRK2=OH R\r\n" + takeText);
                expectEquals (bwf::channelDescription (takeText, {}), takeText);
            }

            beginTest ("A name cannot break out of its sTRK line");
            {
                expectEquals (bwf::channelDescription ("t", "Kick\r\nsTRK2=Evil"),
                              juce::String ("sTRK1=Kick  sTRK2=Evil\r\nt"));
            }

            beginTest ("sTRK truncation stays within 256 bytes on a character boundary");
            {
                const auto longTake = juce::String::repeatedString ("session ", 60);
                for (const auto& letter : { juce::String ("x"),
                                            juce::String (juce::CharPointer_UTF8 ("\xc3\xa9")),          // 2 bytes
                                            juce::String (juce::CharPointer_UTF8 ("\xd8\xb9")),          // Arabic
                                            juce::String (juce::CharPointer_UTF8 ("\xf0\x9f\x8e\xa4")) }) // 4 bytes
                {
                    const auto name = juce::String::repeatedString (letter, 200);
                    const auto text = bwf::channelDescription (longTake, name, name);
                    expect (text.getNumBytesAsUTF8() <= (size_t) bwf::kDescriptionBytes,
                            "Description overflows the fixed field");
                    expect (juce::CharPointer_UTF8::isValidString (text.toRawUTF8(), bwf::kDescriptionBytes),
                            "a multi-byte character was cut in half");
                    expect (text.startsWith ("sTRK1=" + letter));
                    expect (text.contains ("\r\nsTRK2=" + letter));
                    expect (text.contains ("session"), "long names pushed the take text out");
                    expect (bwf::channelNameField (name).getNumBytesAsUTF8() <= (size_t) bwf::kMaxNameBytes);
                }
            }

            beginTest ("A long Description survives a real writer unchanged");
            {
                const auto dir = tempDir ("names");
                const auto file = dir.getChildFile ("Track_01.wav");
                const auto text = bwf::channelDescription (juce::String::repeatedString ("take ", 80),
                                                           juce::String::repeatedString ("n", 80));
                expectEquals ((int) text.getNumBytesAsUTF8(), bwf::kDescriptionBytes);
                juce::StringPairArray meta;
                meta.set (juce::WavAudioFormat::bwavDescription, text);
                meta.set (juce::WavAudioFormat::bwavOriginator, "Zynforge Recording");
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                        file.createOutputStream().release(), kRate, 1, 24, meta, 0));
                }
                const auto read = metadataOf (file);
                expectEquals (read[juce::WavAudioFormat::bwavDescription], text);
                expectEquals (read[juce::WavAudioFormat::bwavOriginator], juce::String ("Zynforge Recording"));
                dir.deleteRecursively();
            }
        }

        // ── Real takes ────────────────────────────────────────────────────
        static juce::StringPairArray metadataOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            return reader != nullptr ? reader->metadataValues : juce::StringPairArray();
        }

        static juce::int64 lengthOf (const juce::File& file)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
            return reader != nullptr ? reader->lengthInSamples : -1;
        }

        static juce::int64 stampOf (const juce::File& file)
        {
            juce::uint64 stamp = 0;
            return bwf::readTimeReference (file, stamp) == StampResult::ok ? (juce::int64) stamp : -1;
        }

        static void feed (MultitrackRecorder& rec, int blocks, int channels = 1, float level = 0.25f)
        {
            std::vector<float> samples ((size_t) kBlock, level);
            std::vector<const float*> inputs ((size_t) channels, samples.data());
            for (int b = 0; b < blocks; ++b)
            {
                rec.processBlock (inputs.data(), channels, kBlock);
                if ((b % 8) == 0) rec.drainPendingForTests();
            }
        }

        // The stamp the recorder's own clock readings call for. Worked out here
        // so a test does not just repeat passStartSamples.
        static juce::int64 expectedStart (const MultitrackRecorder& rec, const bwf::TimeAnchor& anchor,
                                          juce::int64 preRoll)
        {
            const auto liveSample = (juce::int64) std::llround (
                anchor.secondsAt (rec.firstLiveTicks.load()) * kRate);
            return bwf::wrapToDay (liveSample - rec.firstLiveBlock.load() - preRoll, kRate);
        }

        static bwf::TimeAnchor anchorAt (double seconds, const char* source = "ltc")
        {
            return { seconds, juce::Time::getHighResolutionTicks(), source };
        }

        static juce::var waitForReport (const juce::File& dir)
        {
            const auto file = dir.getChildFile ("session.report.json");
            for (int i = 0; i < 3000; ++i)
            {
                const auto report = juce::JSON::parse (file);
                if (report.isObject() && ! (bool) report.getProperty ("sha256Pending", true))
                    return report;
                juce::Thread::sleep (10);
            }
            return juce::JSON::parse (file);
        }

        void stampsOnRealTakes()
        {
            beginTest ("A take is stamped with its first sample, less the pre-roll actually written");
            {
                const auto dir = tempDir ("preroll");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                feed (rec, 47);   // only 12032 frames of history, far short of the second asked for

                const auto anchor = anchorAt (3600.0);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                const auto before = juce::Time::getHighResolutionTicks();
                feed (rec, 20);
                const auto after = juce::Time::getHighResolutionTicks();
                const auto expected = expectedStart (rec, anchor, 47 * kBlock);
                rec.stopRecording();

                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                expectEquals (lengthOf (wav), (juce::int64) (67 * kBlock));
                expectEquals (stampOf (wav), expected);
                // Independent of the recorder's own reading of the clock.
                const auto low  = (juce::int64) (anchor.secondsAt (before) * kRate) - 67 * kBlock - 2;
                const auto high = (juce::int64) (anchor.secondsAt (after) * kRate) + 2;
                expect (stampOf (wav) >= low && stampOf (wav) <= high, "stamp outside the first block's time");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Full pre-roll steps back over midnight and wraps");
            {
                const auto dir = tempDir ("midnight");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                feed (rec, 400);

                const auto anchor = anchorAt (0.25);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 4);
                const auto expected = expectedStart (rec, anchor, 48000);
                rec.stopRecording();
                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                expectEquals (stampOf (wav), expected);
                expect (stampOf (wav) > (juce::int64) 86399 * 48000, "pre-roll did not wrap to before midnight");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Backup and every mirror carry the primary's stamp; a stereo pair stamps once");
            {
                const auto dir = tempDir ("copies"), backup = tempDir ("copies-backup"),
                           mirror = tempDir ("copies-mirror");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 2);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (0).isStereo.store (true);
                rec.getTrack (1).armed.store (true);
                rec.getTrack (0).setNameThreadSafe ("Overheads L");
                rec.getTrack (1).setNameThreadSafe ("Overheads R");
                rec.setBackupDirectory (backup);
                rec.setBackupCaptureFormat (CaptureFormat::Wav16);
                expect (rec.setMirrors ({ { mirror, CaptureFormat::Wav32Float } }));

                const auto anchor = anchorAt (7200.0);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 16, 2);
                const auto expected = expectedStart (rec, anchor, 0);
                rec.stopRecording();

                const auto session = dir.getFileName();
                const auto primaryAudio = dir.getChildFile ("Audio Files");
                for (const auto& copy : { primaryAudio,
                                          backup.getChildFile (session).getChildFile ("Audio Files"),
                                          mirror.getChildFile (session).getChildFile ("Audio Files") })
                {
                    const auto wav = copy.getChildFile ("Track_01.wav");
                    expectEquals (stampOf (wav), expected, copy.getFullPathName());
                    const auto description = metadataOf (wav)[juce::WavAudioFormat::bwavDescription];
                    expect (description.startsWith ("sTRK1=Overheads L\r\nsTRK2=Overheads R\r\n"), description);
                    expect (description.contains (session), "take text lost");
                    expect (! copy.getChildFile ("Track_02.wav").exists(), "stereo pair wrote a second file");
                }
                for (const auto& d : { backup, mirror }) d.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A continuation part is stamped with its own pass; the first file keeps its stamp");
            {
                const auto dir = tempDir ("continue");
                const auto audio = dir.getChildFile ("Audio Files");
                juce::int64 firstStamp = 0, secondExpected = 0;
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    const auto anchor = anchorAt (100.0);
                    rec.setTimeReferenceHint (anchor);
                    expect (rec.startRecording (dir));
                    feed (rec, 8);
                    firstStamp = expectedStart (rec, anchor, 0);
                    rec.stopRecording();
                }
                expectEquals (stampOf (audio.getChildFile ("Track_01.wav")), firstStamp);
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.armContinue (0);
                    const auto anchor = anchorAt (500.0);
                    rec.setTimeReferenceHint (anchor);
                    expect (rec.startRecording (dir));
                    feed (rec, 8);
                    secondExpected = expectedStart (rec, anchor, 0);
                    rec.stopRecording();
                }
                expectEquals (stampOf (audio.getChildFile ("Track_01.wav")), firstStamp,
                              "a continue changed the earlier part");
                expectEquals (stampOf (audio.getChildFile ("Track_01_part02.wav")), secondExpected);
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Auto-split parts stamp the pass start plus the frames before them, per copy");
            {
                const juce::ScopeGuard restore { [] { MultitrackRecorder::setAutoSplitThresholdBytesForTests (0); } };
                MultitrackRecorder::setAutoSplitThresholdBytesForTests (20000);
                const auto dir = tempDir ("split"), backup = tempDir ("split-backup"),
                           mirror = tempDir ("split-mirror");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.setPreRollSeconds (1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (backup);
                rec.setBackupCaptureFormat (CaptureFormat::Wav16);   // rolls at a different frame
                expect (rec.setMirrors ({ { mirror, CaptureFormat::Wav32Float } }));
                feed (rec, 400);

                const auto anchor = anchorAt (9000.0);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 100);
                const auto expected = expectedStart (rec, anchor, 48000);
                rec.stopRecording();

                const auto session = dir.getFileName();
                for (const auto& copy : { dir.getChildFile ("Audio Files"),
                                          backup.getChildFile (session).getChildFile ("Audio Files"),
                                          mirror.getChildFile (session).getChildFile ("Audio Files") })
                {
                    const auto parts = findTakeParts (copy.getChildFile ("Track_01.wav"));
                    expect (parts.size() > 2, "the take did not split");
                    juce::int64 framesBefore = 0;
                    for (const auto& part : parts)
                    {
                        expectEquals (stampOf (part), bwf::wrapToDay (expected + framesBefore, kRate),
                                      part.getFullPathName());
                        framesBefore += lengthOf (part);
                    }
                }
                for (const auto& d : { backup, mirror }) d.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A crash survivor keeps a usable estimate from the moment the writer opened");
            {
                const auto dir = tempDir ("crash");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                const auto anchor = anchorAt (3600.0);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 16);
                rec.drainPendingForTests();
                rec.flushOpenWritersForTests();

                // What a power cut would leave: the file as it stands mid-take.
                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                const auto survivor = dir.getChildFile ("survivor.wav");
                expect (wav.copyFileTo (survivor));
                const auto estimate = stampOf (survivor);
                const auto exact = expectedStart (rec, anchor, 0);
                expect (estimate > 0, "survivor has no usable time");
                expect (std::llabs (estimate - exact) < (juce::int64) kRate * 2,
                        "estimate is more than two seconds out");
                expect (lengthOf (survivor) > 0, "survivor lost its audio");
                expectEquals (rec.getTrack (0).clipCount.load(), 0);
                rec.stopRecording();
                expectEquals (stampOf (wav), exact, "the clean stop did not replace the estimate");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void punchKeepsTheTakeStamp()
        {
            const auto record = [this] (const juce::File& dir, const juce::File& backup, juce::int64 punchAt,
                                        bool continuing, double seconds, juce::int64* stamp)
            {
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (0).setNameThreadSafe (punchAt >= 0 ? "Renamed" : "Lead Vox");
                if (backup != juce::File()) rec.setBackupDirectory (backup);
                if (punchAt >= 0) rec.armPunchIn (punchAt);
                if (continuing) rec.armContinue (0);
                const auto anchor = anchorAt (seconds);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 8, 1, punchAt >= 0 ? 0.75f : 0.25f);
                if (stamp != nullptr) *stamp = expectedStart (rec, anchor, 0);
                rec.stopRecording();
            };

            beginTest ("A punched take keeps the original stamp and bext on every copy");
            {
                const auto dir = tempDir ("punch"), backup = tempDir ("punch-backup");
                const auto session = dir.getFileName();
                juce::int64 original = 0;
                record (dir, backup, -1, false, 1000.0, &original);
                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                const auto backupWav = backup.getChildFile (session).getChildFile ("Audio Files")
                                             .getChildFile ("Track_01.wav");
                const auto before = metadataOf (wav);

                record (dir, backup, 512, false, 2000.0, nullptr);

                for (const auto& file : { wav, backupWav })
                {
                    expectEquals (stampOf (file), original, file.getFullPathName());
                    const auto after = metadataOf (file);
                    for (const auto* key : { juce::WavAudioFormat::bwavDescription,
                                             juce::WavAudioFormat::bwavOriginator,
                                             juce::WavAudioFormat::bwavOriginatorRef,
                                             juce::WavAudioFormat::bwavOriginationDate,
                                             juce::WavAudioFormat::bwavOriginationTime })
                        expectEquals (after[key], before[key], key);
                }
                const auto report = waitForReport (dir);
                expectEquals ((juce::int64) report.getProperty ("timeReference", 0), original,
                              "the report no longer names the take's stamp");
                const auto* failures = report.getProperty ("timeReferenceStampFailures", {}).getArray();
                expect (failures != nullptr && failures->isEmpty(), "the patch was aimed at a spliced file");
                expectEquals (lengthOf (wav), (juce::int64) (512 + 8 * kBlock));
                backup.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A punch into a multi-part take keeps the first part's stamp");
            {
                const auto dir = tempDir ("punch-parts");
                juce::int64 original = 0;
                record (dir, {}, -1, false, 1000.0, &original);
                record (dir, {}, -1, true, 3000.0, nullptr);
                const auto audio = dir.getChildFile ("Audio Files");
                expect (audio.getChildFile ("Track_01_part02.wav").existsAsFile());

                record (dir, {}, 256, false, 4000.0, nullptr);
                expect (! audio.getChildFile ("Track_01_part02.wav").exists(), "the splice did not flatten");
                expectEquals (stampOf (audio.getChildFile ("Track_01.wav")), original);
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A track newly armed in a punch is stamped with the punch's own time");
            {
                const auto dir = tempDir ("punch-new");
                const auto audio = dir.getChildFile ("Audio Files");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 8, 2);
                    rec.stopRecording();
                }
                juce::int64 punchStamp = 0;
                const auto first = stampOf (audio.getChildFile ("Track_01.wav"));
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (0).armed.store (true);
                    rec.getTrack (1).armed.store (true);
                    rec.armPunchIn (512);
                    const auto anchor = anchorAt (70000.0);
                    rec.setTimeReferenceHint (anchor);
                    expect (rec.startRecording (dir));
                    feed (rec, 8, 2);
                    punchStamp = expectedStart (rec, anchor, 0);
                    rec.stopRecording();
                }
                expectEquals (stampOf (audio.getChildFile ("Track_01.wav")), first);
                // Its audio sits after the silent lead-in part; findTakeParts
                // lists the silence (never stamped: no bext) then the capture.
                const auto parts = findTakeParts (audio.getChildFile ("Track_02.wav"));
                expect (parts.size() == 2, "expected a lead-in part and an audio part");
                if (parts.size() == 2)
                {
                    expectEquals (stampOf (parts[1]), punchStamp);
                    expectEquals (stampOf (parts[0]), (juce::int64) -1);
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void otherContainers()
        {
            beginTest ("AIFF and FLAC have no bext: skipped without a failure, beside a stamped WAV");
            {
                const auto dir = tempDir ("formats"), backup = tempDir ("formats-backup");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setCaptureFormat (CaptureFormat::Aiff24);
                rec.setBackupDirectory (backup);
                rec.setBackupCaptureFormat (CaptureFormat::Flac24);
                expect (rec.startRecording (dir));
                feed (rec, 16);
                rec.stopRecording();
                const auto session = dir.getFileName();
                expect (dir.getChildFile ("Audio Files").getChildFile ("Track_01.aif").existsAsFile());
                expect (backup.getChildFile (session).getChildFile ("Audio Files")
                              .getChildFile ("Track_01.flac").existsAsFile());
                const auto report = waitForReport (dir);
                const auto* failures = report.getProperty ("timeReferenceStampFailures", {}).getArray();
                expect (failures != nullptr && failures->isEmpty(), "a container without bext was reported as a failure");
                expect (! (bool) report.getProperty ("primaryFailed", true));
                expect (! (bool) report.getProperty ("backupFailed", true));
                backup.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void reportAndHash()
        {
            beginTest ("The report's hashes describe the stamped bytes, and it records the time");
            {
                const auto dir = tempDir ("hash"), mirror = tempDir ("hash-mirror");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                const auto anchor = anchorAt (43200.0, "mtc");
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 16);
                const auto stamp = expectedStart (rec, anchor, 0);
                rec.stopRecording();

                const auto report = waitForReport (dir);
                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                const auto mirrorWav = mirror.getChildFile (dir.getFileName()).getChildFile ("Audio Files")
                                             .getChildFile ("Track_01.wav");
                expectEquals (stampOf (wav), stamp);
                expectEquals ((juce::int64) report.getProperty ("timeReference", 0), stamp);
                expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("mtc"));

                const auto track = report.getProperty ("tracks", {})[0];
                expectEquals (track.getProperty ("sha256", {})[0].toString(),
                              juce::String (hashing::fileSha256 (wav)), "primary hash is of other bytes");
                expectEquals (track.getProperty ("mirrors", {})[0].getProperty ("sha256", {})[0].toString(),
                              juce::String (hashing::fileSha256 (mirrorWav)), "mirror hash is of other bytes");
                mirror.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A file that refuses the stamp is reported and marks nothing bad");
            {
                const auto dir = tempDir ("refused"), backup = tempDir ("refused-backup");
                const auto backupWav = backup.getChildFile (dir.getFileName()).getChildFile ("Audio Files")
                                             .getChildFile ("Track_01.wav");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setBackupDirectory (backup);
                juce::MemoryBlock damaged;
                rec.beforeStampForTests = [&]
                {
                    // Runs after the writers closed, where a real bit-rot or a
                    // foreign tool would have to strike. Rename the chunk.
                    auto bytes = bytesOf (backupWav);
                    juce::MemoryInputStream in (bytes, false);
                    const auto bext = bwf::findBext (in);
                    expect (bext.result == StampResult::ok);
                    std::memcpy (static_cast<char*> (bytes.getData()) + bext.dataStart - 8, "bexX", 4);
                    backupWav.replaceWithData (bytes.getData(), bytes.getSize());
                    damaged = bytes;
                };
                const auto anchor = anchorAt (5000.0);
                rec.setTimeReferenceHint (anchor);
                expect (rec.startRecording (dir));
                feed (rec, 16);
                const auto stamp = expectedStart (rec, anchor, 0);
                rec.stopRecording();

                expect (bytesOf (backupWav) == damaged, "the refused file was written to");
                expectEquals (stampOf (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav")), stamp,
                              "one copy's refusal stopped the others");
                expect (! rec.hasPrimaryFailed() && ! rec.hasBackupFailed() && ! rec.anyMirrorFailed());

                const auto report = waitForReport (dir);
                const auto* failures = report.getProperty ("timeReferenceStampFailures", {}).getArray();
                expect (failures != nullptr && failures->size() == 1, "the refusal is not in the report");
                if (failures != nullptr && failures->size() == 1)
                {
                    expect ((*failures)[0].toString().contains (backupWav.getFullPathName()));
                    expect ((*failures)[0].toString().contains ("no bext"));
                }
                expect (! (bool) report.getProperty ("backupFailed", true));
                expect (! (bool) report.getProperty ("primaryFailed", true));
                expect (! (bool) report.getProperty ("sha256Failed", true));
                backup.deleteRecursively();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A continue keeps the earlier stamp failures in the report");
            {
                const auto dir = tempDir ("carry");
                const auto wav = dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 1);
                    rec.getTrack (0).armed.store (true);
                    rec.beforeStampForTests = [&] { wav.deleteFile(); };
                    expect (rec.startRecording (dir));
                    feed (rec, 4);
                    rec.stopRecording();
                }
                expect (wav.replaceWithData ("x", 1), "could not restore a stand-in file");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (1).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 4, 2);
                    rec.stopRecording();
                }
                const auto report = waitForReport (dir);
                const auto* failures = report.getProperty ("timeReferenceStampFailures", {}).getArray();
                expect (failures != nullptr && failures->size() == 1,
                        "a clean later pass erased the earlier failure");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("tools/verify_take.sh accepts a stamped take");
            {
                const auto script = juce::File (__FILE__).getParentDirectory().getParentDirectory()
                                        .getParentDirectory().getChildFile ("tools/verify_take.sh");
                const auto dir = tempDir ("verify");
                {
                    MultitrackRecorder rec;
                    rec.prepare (kRate, kBlock, 2);
                    rec.getTrack (0).armed.store (true);
                    rec.getTrack (0).isStereo.store (true);
                    rec.getTrack (1).armed.store (true);
                    rec.getTrack (0).setNameThreadSafe ("Overheads");
                    expect (rec.startRecording (dir));
                    feed (rec, 64, 2);
                    rec.stopRecording();
                }
                waitForReport (dir);
                juce::ChildProcess verify;
                const auto path = juce::String ("PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin");
                expect (verify.start (juce::StringArray { "/usr/bin/env", path, "/bin/bash",
                                                          script.getFullPathName(), dir.getFullPathName() }));
                expect (verify.waitForProcessToFinish (60000));
                const auto exitCode = verify.getExitCode();
                if (exitCode == 2) logMessage ("verify_take.sh needs ffprobe, jq, xxd: skipped");
                else expectEquals ((int) exitCode, 0, verify.readAllProcessOutput());
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void timeSources()
        {
            beginTest ("Without a hint a take stamps the wall clock");
            {
                const auto dir = tempDir ("wallclock");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                const auto now = bwf::TimeAnchor::fromWallClock();
                expect (rec.startRecording (dir));
                feed (rec, 16);
                rec.stopRecording();
                const auto report = waitForReport (dir);
                expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("clock"));
                const auto seconds = (double) stampOf (dir.getChildFile ("Audio Files").getChildFile ("Track_01.wav")) / kRate;
                auto difference = std::abs (seconds - now.secondsSinceMidnight);
                difference = juce::jmin (difference, 86400.0 - difference);   // a run at midnight
                expect (difference < 5.0, "wall-clock stamp is not the time of day");
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Each start consumes the hint");
            {
                const auto dir = tempDir ("hint");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.setTimeReferenceHint (anchorAt (10.0));
                expect (rec.startRecording (dir));
                expectEquals (rec.timeAnchor.source, juce::String ("ltc"));
                feed (rec, 4);
                rec.stopRecording();
                rec.armContinue (0);
                expect (rec.startRecording (dir));
                expectEquals (rec.timeAnchor.source, juce::String ("clock"), "a stale hint stamped the next take");
                feed (rec, 4);
                rec.stopRecording();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A running timecode label is converted with the age of its frame");
            {
                TimecodeChase chase;
                chase.feedMtcFullFrame (1, 2, 3, 12, 25.0f);
                expectWithinAbsoluteError (chase.timeOfDaySeconds (2), 3723.0 + 14.0 / 25.0, 1.0e-9);
                expectWithinAbsoluteError (chase.timeOfDaySeconds (1), 3723.0 + 13.0 / 25.0, 1.0e-9);
            }

            const auto engineTake = [this] (AudioEngine& engine, const juce::File& dir, std::function<void()> feedChase)
            {
                engine.setStripCount (1);
                engine.prepareForTests (kRate, kBlock);
                engine.setTrackInputRouting (0, 0);
                engine.getRecorder().getTrack (0).armed.store (true);
                juce::AudioBuffer<float> in (1, kBlock), out (2, kBlock);
                in.clear(); out.clear();
                feedChase();
                expect (engine.startRecording (dir));
                for (int b = 0; b < 8; ++b)
                    engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1,
                                                             out.getArrayOfWritePointers(), 2, kBlock, {});
                engine.stopRecording();
                return waitForReport (dir);
            };

            beginTest ("A locked MTC source stamps the take through the engine");
            {
                const auto dir = tempDir ("mtc");
                {
                    AudioEngine engine;
                    engine.setChaseMode (AudioEngine::ChaseMode::Mtc);
                    // A running master: one whole quarter-frame run of 10:20:30:00
                    // at 25 fps. A full frame alone is a cue, not a running clock.
                    const auto report = engineTake (engine, dir, [&]
                    {
                        const int nibbles[8] { 0, 0, 30 & 0x0F, 30 >> 4, 20 & 0x0F, 20 >> 4, 10, 1 << 1 };
                        for (int type = 0; type < 8; ++type)
                            engine.getTimecodeChase().feedMtcQuarterFrame ((juce::uint8) ((type << 4) | nibbles[type]));
                    });
                    expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("mtc"));
                    const auto seconds = (double) (juce::int64) report.getProperty ("timeReference", 0) / kRate;
                    expect (seconds > 37230.0 && seconds < 37232.0, juce::String (seconds));
                    engine.setChaseMode (AudioEngine::ChaseMode::Off);
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A locked LTC source stamps the take through the engine");
            {
                const auto dir = tempDir ("ltc");
                {
                    AudioEngine engine;
                    engine.setChaseMode (AudioEngine::ChaseMode::Ltc);
                    const auto report = engineTake (engine, dir, [&]
                    {
                        const auto signal = ltctest::ltcSignal (kRate, 25.0, false, { 5, 0, 0, 0 }, 25);
                        auto& chase = engine.getTimecodeChase();
                        for (size_t at = 0; at < signal.size(); at += (size_t) kBlock)
                            chase.feedLtc (signal.data() + at,
                                           (int) juce::jmin ((size_t) kBlock, signal.size() - at), kRate);
                        expect (chase.isLtcLocked(), "the test signal did not lock");
                    });
                    expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("ltc"));
                    const auto seconds = (double) (juce::int64) report.getProperty ("timeReference", 0) / kRate;
                    expect (seconds > 18000.9 && seconds < 18002.0, juce::String (seconds));
                    engine.setChaseMode (AudioEngine::ChaseMode::Off);
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("Chase that is selected but not live falls back to the wall clock");
            {
                const auto dir = tempDir ("nochase");
                {
                    AudioEngine engine;
                    engine.setChaseMode (AudioEngine::ChaseMode::Ltc);
                    const auto report = engineTake (engine, dir, [] {});
                    expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("clock"));
                    engine.setChaseMode (AudioEngine::ChaseMode::Off);
                }
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("The daemon takes a timecode hint from the Start command");
            {
                using namespace zynforge::capture;
                CaptureDaemon daemon; daemon.setTestModeNoDevice (true);
                bool listening = false;
                for (int port : { 49760, 49761, 49762 })
                    if (daemon.start (port, 1)) { listening = true; break; }
                expect (listening);
                if (listening)
                {
                    daemon.prepareForTests (kRate, kBlock, 1);
                    daemon.getRecorder().getTrack (0).armed.store (true);
                    CaptureClient client;
                    expect (client.connect ("127.0.0.1", daemon.getPort()));
                    expect (client.hello (2000).ok);
                    const auto dir = tempDir ("daemon-hint");
                    const auto anchor = anchorAt (40000.0, "ltc");
                    Command start; start.action = Action::StartRecording;
                    start.sessionDir = dir.getFullPathName();
                    start.configuration = anchor.toJson();
                    expect (client.request (start, 3000).ok);
                    std::vector<float> samples ((size_t) kBlock, 0.25f);
                    const float* inputs[] { samples.data() };
                    daemon.processTestBlock (inputs, 1, kBlock);
                    Command stop; stop.action = Action::StopRecording;
                    expect (client.request (stop, 5000).ok);

                    const auto report = waitForReport (dir);
                    expectEquals (report.getProperty ("timeReferenceSource", "").toString(), juce::String ("ltc"));
                    const auto seconds = (double) (juce::int64) report.getProperty ("timeReference", 0) / kRate;
                    // The first sample precedes the first block's arrival by that block.
                    expect (seconds > 40000.0 - (double) kBlock / kRate && seconds < 40010.0,
                            juce::String (seconds, 6));
                    client.disconnect();
                    daemon.stop();
                    MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
                }
            }

            beginTest ("A malformed time hint is ignored");
            {
                bwf::TimeAnchor anchor;
                expect (! bwf::TimeAnchor::fromJson ({}, anchor));
                expect (! bwf::TimeAnchor::fromJson (juce::JSON::parse (R"({"seconds":90000,"ticks":5,"source":"ltc"})"), anchor));
                expect (! bwf::TimeAnchor::fromJson (juce::JSON::parse (R"({"seconds":10,"ticks":5,"source":"clock"})"), anchor));
                expect (bwf::TimeAnchor::fromJson (juce::JSON::parse (R"({"seconds":10,"ticks":5,"source":"mtc"})"), anchor));
            }
        }

        void clockSource()
        {
            using namespace clocksource;

            beginTest ("Clock source names are classified");
            {
                for (auto* name : { "Internal", "internal", "Internal Clock", "INT", "Int.", "Mac Internal", "Internal (48 kHz)" })
                    expect (classify (name) == Kind::internal, name);
                for (auto* name : { "Word Clock", "MADI", "ADAT 1", "AES", "S/PDIF", "Dante", "Optical In", "Interface Sync", "Printer" })
                    expect (classify (name) == Kind::external, name);
                expect (classify ({}) == Kind::unreported);
                expect (classify ("   ") == Kind::unreported);
            }

            beginTest ("Pre-flight row: names the source, warns on internal, never fails on a missing one");
            {
                expectEquals (preflightRow ({ Kind::external, "Word Clock" }),
                              juce::String ("[OK]  Clock source: Word Clock"));
                const auto internal = preflightRow ({ Kind::internal, "Internal" });
                expect (internal.startsWith ("[!]"));
                expect (internal.contains ("Internal") && internal.contains ("master"));
                const auto unreported = preflightRow ({ Kind::unreported, {} });
                expect (unreported.startsWith ("[i]"), "a device with no clock property is not a failure");
                expect (preflightRow ({}).startsWith ("[i]"));
                expect (! preflightRow ({ Kind::external, "MADI" }).containsIgnoreCase ("locked"),
                        "CoreAudio cannot tell us about sync lock, so the row must not claim it");
            }

            beginTest ("A device that is not there reads as not available");
            {
                expect (readDevice ({}).kind == Kind::unavailable);
                expect (readDevice ("No Such Device 8f3a").kind == Kind::unavailable);
            }

           #if JUCE_MAC
            beginTest ("The HAL reader answers for every device on this machine");
            {
                // Scanning names opens no device. A name shared by two devices
                // reads as unavailable, so only "none answered" is a failure.
                juce::AudioDeviceManager manager;
                juce::OwnedArray<juce::AudioIODeviceType> types;
                manager.createAudioDeviceTypes (types);
                int devices = 0, answered = 0;
                for (auto* type : types)
                {
                    type->scanForDevices();
                    for (const auto& name : type->getDeviceNames (true))
                    {
                        const auto reading = readDevice (name);
                        logMessage (name + ": " + kindName (reading.kind) + " " + reading.name);
                        ++devices;
                        if (reading.kind != Kind::unavailable) ++answered;
                    }
                }
                expect (devices == 0 || answered > 0, "the HAL reader matched no device by name");
            }
           #endif

            beginTest ("A stalled driver costs the caller its timeout and nothing more");
            {
                auto release = std::make_shared<juce::WaitableEvent> (true);
                auto reads   = std::make_shared<std::atomic<int>> (0);
                Probe probe ([release, reads] (const juce::String&) -> Reading
                {
                    ++*reads;
                    release->wait (5000);
                    return { Kind::external, "Word Clock" };
                });

                const auto start = juce::Time::getMillisecondCounterHiRes();
                expect (probe.wait ("Device", 30).kind == Kind::unavailable);
                expect (juce::Time::getMillisecondCounterHiRes() - start < 1000.0, "the wait was not bounded");
                probe.request ("Device");
                expect (probe.latest().kind == Kind::unavailable);
                expectEquals (reads->load(), 1, "a second read piled up behind the stalled one");

                release->signal();
                for (int i = 0; i < 300 && probe.latest().kind != Kind::external; ++i)
                    juce::Thread::sleep (10);
                expectEquals (probe.latest().name, juce::String ("Word Clock"));
                expect (probe.wait ("Device", 1000).kind == Kind::external);
            }

            beginTest ("A stalled read leaves unknown, not the previous device's clock");
            {
                auto stall   = std::make_shared<std::atomic<bool>> (false);
                auto release = std::make_shared<juce::WaitableEvent> (true);
                Probe probe ([stall, release] (const juce::String& device) -> Reading
                {
                    if (stall->load()) release->wait (5000);
                    return { Kind::external, device };
                });

                expectEquals (probe.wait ("First interface", 1000).name, juce::String ("First interface"));
                stall->store (true);
                probe.request ("Second interface");
                expect (probe.latest().kind == Kind::unavailable, "a report would name the wrong clock");
                release->signal();
            }

            beginTest ("A read that outlives its probe is safe");
            {
                auto release = std::make_shared<juce::WaitableEvent> (true);
                auto done    = std::make_shared<juce::WaitableEvent> (true);
                {
                    Probe probe ([release, done] (const juce::String&) -> Reading
                    {
                        release->wait (5000);
                        done->signal();
                        return { Kind::internal, "Internal" };
                    });
                    probe.request ("Device");
                }
                release->signal();
                expect (done->wait (2000));
            }

            beginTest ("The clock source goes in the report");
            {
                const auto dir = tempDir ("clock");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.clockProbe = Probe ([] (const juce::String&) -> Reading { return { Kind::internal, "Internal" }; });
                rec.setClockDevice ("Device");
                expect (rec.startRecording (dir));
                feed (rec, 4);
                for (int i = 0; i < 300 && rec.clockProbe.latest().kind == Kind::unavailable; ++i)
                    juce::Thread::sleep (10);
                rec.stopRecording();
                const auto report = waitForReport (dir);
                expectEquals (report.getProperty ("clockSource", "").toString(), juce::String ("Internal"));
                expectEquals (report.getProperty ("clockSourceKind", "").toString(), juce::String ("internal"));
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("A report with no device to ask says so");
            {
                const auto dir = tempDir ("noclock");
                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                expect (rec.startRecording (dir));
                feed (rec, 4);
                rec.stopRecording();
                const auto report = waitForReport (dir);
                expectEquals (report.getProperty ("clockSourceKind", "").toString(), juce::String ("unavailable"));
                expect (report.hasProperty ("clockSource"));
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }
    };

    static CaptureMetaTests captureMetaTests;
}
