// Capture-path regressions: what a crash leaves on disk, what a punch writes,
// how a lost daemon behaves, and how copies stay out of each other's way.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AiffRepair.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/FloatAiffWriter.h"
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
                rig.daemon.dropLinkForTests();
                expect (pumpUntil ([&]
                {
                    main.serviceCaptureDaemonLink();
                    return main.statusLabel.getText().contains ("REATTACHED");
                }, 8000), "the window did not come back to the rolling take");
                expect (rig.daemon.getRecorder().isRecording(), "the take must not have stopped");
                expectEquals (main.recordButton.getButtonText(), juce::String ("STOP"));

                bool done = false, ok = false;
                main.stopActiveCaptureAsync (false, [&] (bool saved) { done = true; ok = saved; });
                expect (pumpUntil ([&] { return done; }, 8000), "STOP never finished");
                expect (ok, main.lastCaptureFinalizationError);
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
}
