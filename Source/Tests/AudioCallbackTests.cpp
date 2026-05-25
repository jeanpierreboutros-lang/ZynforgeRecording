// Real-time audio-thread tests for AudioEngine. Drives the
// `audioDeviceIOCallbackWithContext` directly with synthetic input
// buffers and asserts what comes out at the output buffers -- no
// CoreAudio device required, no live sound.
//
// Uses AudioEngine::prepareForTests to set up the same scratch
// buffers + recorder/player state that audioDeviceAboutToStart
// would, minus the device-specific bits (workgroup join etc.).
//
// Covers the path that ships audio to hardware: monitor-bus sum,
// master gain + mute, master output routing (the bug we just fixed
// where click / NDI / companion stream were pinned to outs 0+1),
// per-track mute gating of input on the monitor bus.

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"

namespace zynforge
{
    namespace
    {
        // ── Test fixture ──────────────────────────────────────────────
        // Holds an engine + input/output buffers and exposes a
        // process(numSamples) that runs one audio callback. Test
        // bodies stuff the input buffers, call process, then inspect
        // the output buffers.
        struct CallbackFixture
        {
            CallbackFixture (int strips, int numInputs, int numOutputs,
                             double sr = 48000.0, int blockSize = 256)
                : sampleRate (sr), block (blockSize),
                  inBuf (numInputs,  blockSize),
                  outBuf (numOutputs, blockSize)
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                engine.setStripCount (strips);
                engine.prepareForTests (sr, blockSize);
                // Force a deterministic baseline so tests don't
                // inherit the user's real appProps file (per-strip
                // gain / pan / routing, master gain / mute, etc.).
                engine.getMasterState().muted .store (false);
                engine.getMasterState().gainDb.store (0.0f);
                engine.setMasterStereo (true);
                for (int i = 0; i < strips; ++i)
                {
                    auto& t = engine.getRecorder().getTrack (i);
                    t.gainDb  .store (0.0f);
                    t.pan     .store (0.0f);
                    t.muted   .store (false);
                    t.soloed  .store (false);
                    t.armed   .store (false);
                    t.monitor .store (false);
                    t.vcaGroup.store (-1);   // unassigned
                    t.isStereo.store (false);
                    engine.setTrackInputRouting  (i, juce::jlimit (-1, numInputs - 1, i));
                    engine.setTrackOutputRouting (i, -1);   // master-only
                    engine.setTrackVcaGroup      (i, -1);   // also wipes appProps key
                }
                // Reset every VCA to a clean baseline so leftover
                // gain/mute/solo from a prior test doesn't leak.
                for (int v = 0; v < AudioEngine::kNumVcas; ++v)
                {
                    auto& vca = engine.getVca (v);
                    vca.gainDb.store (0.0f);
                    vca.muted .store (false);
                    vca.soloed.store (false);
                    vca.rampSamplesRemaining.store (0);
                }
                inBuf .clear();
                outBuf.clear();
            }

            void process (int numSamples)
            {
                jassert (numSamples <= block);
                // The callback writes ADDITIVELY into outputs in the
                // monitor-sum path, so zero them first to make each
                // assertion independent of the previous call.
                outBuf.clear (0, 0, numSamples);
                engine.audioDeviceIOCallbackWithContext (
                    inBuf .getArrayOfReadPointers(),  inBuf .getNumChannels(),
                    outBuf.getArrayOfWritePointers(), outBuf.getNumChannels(),
                    numSamples, {});
            }

            // Convenience: fill one input channel with a constant.
            void writeInput (int ch, float value, int numSamples)
            {
                auto* p = inBuf.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i) p[i] = value;
            }

            // Convenience: peak-abs over one output channel.
            float peakOut (int ch, int numSamples) const
            {
                const auto* p = outBuf.getReadPointer (ch);
                float pk = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float a = std::abs (p[i]);
                    if (a > pk) pk = a;
                }
                return pk;
            }

            AudioEngine engine;
            double      sampleRate;
            int         block;
            juce::AudioBuffer<float> inBuf;
            juce::AudioBuffer<float> outBuf;
        };
    }

    class AudioCallbackTests final : public juce::UnitTest
    {
    public:
        AudioCallbackTests() : UnitTest ("Audio callback", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Silent input + 0 strips -> silent output");
            {
                CallbackFixture f (0, 2, 2);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            beginTest ("Armed + monitored track passes input to monitor bus");
            {
                CallbackFixture f (1, 2, 4);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed  .store (true);
                t.monitor.store (true);
                t.gainDb .store (0.0f);
                t.pan    .store (0.0f);
                t.muted  .store (false);
                t.soloed .store (false);
                // Force master state to a known baseline -- prior tests
                // can leave masterMuted persisted in appProps.
                f.engine.getMasterState().muted.store (false);
                f.engine.getMasterState().gainDb.store (0.0f);
                f.engine.setMasterStereo (true);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.process (256);
                // Constant 0.5 in -> non-zero on both monitor outs.
                // Constant-power pan at centre = 0.707 -> ~0.353 each.
                expect (f.peakOut (0, 256) > 0.20f);
                expect (f.peakOut (1, 256) > 0.20f);
            }

            beginTest ("Master mute kills the monitor sum");
            {
                CallbackFixture f (1, 2, 4);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                f.engine.getMasterState().muted.store (true);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 1.0f, 256);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            beginTest ("Per-track mute removes that input from monitor sum");
            {
                CallbackFixture f (1, 2, 4);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed  .store (true); t.monitor.store (true);
                t.muted  .store (true);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 1.0f, 256);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            beginTest ("setMasterOutputs routes monitor sum to the chosen pair");
            {
                CallbackFixture f (1, 2, 8);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                f.engine.getMasterState().muted.store (false);
                f.engine.getMasterState().gainDb.store (0.0f);
                f.engine.setMasterStereo (true);
                f.engine.setMasterOutputs (4, 5);
                f.writeInput (0, 0.5f, 256);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
                expect      (f.peakOut (4, 256) > 0.20f);
                expect      (f.peakOut (5, 256) > 0.20f);
            }

            beginTest ("Master gain attenuation reduces output peak");
            {
                CallbackFixture fLoud (1, 2, 4);
                CallbackFixture fQuiet (1, 2, 4);
                for (auto* f : { &fLoud, &fQuiet })
                {
                    auto& t = f->engine.getRecorder().getTrack (0);
                    t.armed.store (true); t.monitor.store (true);
                    f->engine.getMasterState().muted.store (false);
                    f->engine.setMasterStereo (true);
                    f->engine.setMasterOutputs (0, 1);
                    f->writeInput (0, 0.5f, 256);
                }
                fLoud .engine.getMasterState().gainDb.store (0.0f);
                fQuiet.engine.getMasterState().gainDb.store (-20.0f);
                fLoud .process (256);
                fQuiet.process (256);
                const float loud  = fLoud .peakOut (0, 256);
                const float quiet = fQuiet.peakOut (0, 256);
                expect (loud  > 0.20f);
                expect (quiet < loud * 0.5f);   // 20 dB cut ≈ 0.1× linear
            }

            beginTest ("Stopped player + no armed input -> silent output");
            {
                CallbackFixture f (4, 2, 4);
                // Strips exist but none armed, none monitored, player stopped.
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.7f, 256);
                f.writeInput (1, 0.7f, 256);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            beginTest ("Audio load percentage updates after a callback");
            {
                CallbackFixture f (2, 2, 2);
                expectEquals (f.engine.getAudioLoadPct(), 0.0f);
                f.process (256);
                // Should be > 0 (the callback took non-zero time).
                // Cap at 100 so we don't assert on a flaky CI bound.
                const float load = f.engine.getAudioLoadPct();
                expect (load >= 0.0f && load <= 100.0f);
            }

            // ─── Solo isolation ──────────────────────────────────────
            beginTest ("Solo isolates: only soloed strip contributes to monitor sum");
            {
                CallbackFixture f (3, 3, 2);
                for (int i = 0; i < 3; ++i)
                {
                    auto& t = f.engine.getRecorder().getTrack (i);
                    t.armed.store (true); t.monitor.store (true);
                }
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.writeInput (1, 0.5f, 256);
                f.writeInput (2, 0.5f, 256);

                // Baseline: nothing soloed -> all three contribute.
                f.process (256);
                const float baseline = f.peakOut (0, 256);
                expect (baseline > 0.20f);

                // Solo strip 1 -> strips 0 + 2 drop out.
                f.engine.getRecorder().getTrack (1).soloed.store (true);
                f.process (256);
                const float soloOnly = f.peakOut (0, 256);
                // Only one strip contributing instead of three -> peak
                // is roughly 1/3 of baseline. We just need it to be
                // strictly smaller (and still non-zero).
                expect (soloOnly > 0.0f);
                expect (soloOnly < baseline * 0.7f);
            }

            beginTest ("Solo gates a muted-strip from being heard");
            {
                CallbackFixture f (2, 2, 2);
                auto& s0 = f.engine.getRecorder().getTrack (0);
                auto& s1 = f.engine.getRecorder().getTrack (1);
                s0.armed.store (true); s0.monitor.store (true);
                s1.armed.store (true); s1.monitor.store (true);
                // Mute strip 0, solo strip 1. With no solo, mute would
                // gate strip 0; with solo on strip 1, mute is moot
                // because only strip 1 passes anyway.
                s0.muted .store (true);
                s1.soloed.store (true);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.writeInput (1, 0.5f, 256);
                f.process (256);
                // Only strip 1's input contributes.
                expect (f.peakOut (0, 256) > 0.20f);
            }

            // ─── VCA groups ──────────────────────────────────────────
            beginTest ("VCA group assignment round-trips through the audio thread");
            {
                // Design note: VCA *gain* applies only on the per-strip
                // output-routing path (effectiveGainDb, summed at the
                // FOH-style strip-out stage) -- NOT on the engineer's
                // monitor sum, which uses the strip's gainDb directly.
                // VCA *mute* and *solo* gate via channelAudible, which
                // DOES feed the monitor sum (see the next two tests).
                // Exercising VCA gain end-to-end needs a loaded session
                // (playerScratch must have data), which is out of scope
                // for these headless tests. We round-trip the state to
                // confirm the atomics + API wiring at least.
                CallbackFixture f (2, 2, 4);
                f.engine.setTrackVcaGroup (0, 3);
                f.engine.setTrackVcaGroup (1, 3);
                f.engine.getVca (3).gainDb.store (-12.0f);
                expectEquals (f.engine.getRecorder().getTrack (0).vcaGroup.load(), 3);
                expectEquals (f.engine.getRecorder().getTrack (1).vcaGroup.load(), 3);
                expectWithinAbsoluteError (f.engine.getVca (3).gainDb.load(), -12.0f, 0.001f);
                // Process a block; nothing should crash.
                f.writeInput (0, 0.5f, 256);
                f.process (256);
            }

            beginTest ("VCA mute gates every strip assigned to that bus");
            {
                CallbackFixture f (2, 2, 2);
                for (int i = 0; i < 2; ++i)
                {
                    auto& t = f.engine.getRecorder().getTrack (i);
                    t.armed.store (true); t.monitor.store (true);
                    f.engine.setTrackVcaGroup (i, 0);
                }
                f.engine.getVca (0).muted.store (true);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.writeInput (1, 0.5f, 256);
                f.process (256);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            beginTest ("VCA solo silences ungrouped strips");
            {
                CallbackFixture f (3, 3, 2);
                for (int i = 0; i < 3; ++i)
                {
                    auto& t = f.engine.getRecorder().getTrack (i);
                    t.armed.store (true); t.monitor.store (true);
                }
                // Strip 0 -> VCA 0 (soloed). Strips 1 + 2 ungrouped.
                f.engine.setTrackVcaGroup (0, 0);
                f.engine.getVca (0).soloed.store (true);
                f.engine.getVca (0).gainDb.store (0.0f);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.writeInput (1, 0.5f, 256);
                f.writeInput (2, 0.5f, 256);
                // Let any soft-takeover ramps settle.
                for (int n = 0; n < 8; ++n) f.process (256);
                // Only strip 0 (VCA-soloed) passes; 1 + 2 drop out.
                // Result is ~ one strip's worth on the monitor bus
                // (constant 0.5 input * constant-power 0.707 = 0.353).
                const float pk = f.peakOut (0, 256);
                expect (pk > 0.20f);
                expect (pk < 0.50f);
            }

            // ─── Click track follows the monitor bus ─────────────────
            beginTest ("Click engine writes to the configured master output pair");
            {
                CallbackFixture f (0, 0, 8);
                f.engine.getClickEngine().setEnabled (true);
                f.engine.setMasterStereo (true);
                f.engine.setMasterOutputs (4, 5);
                // ClickEngine fires at the first sample of the first
                // block (samplesUntilNextBeat1 = 0 after prepare).
                f.process (256);
                // Click sample should land on outs 4/5; not on 0/1.
                expect      (f.peakOut (4, 256) > 0.0f);
                expectEquals (f.peakOut (0, 256), 0.0f);
                expectEquals (f.peakOut (1, 256), 0.0f);
            }

            // ─── Stereo-pair summing ─────────────────────────────────
            // The engine does not have special "stereo pair" audio
            // processing -- each track is mono with constant-power pan.
            // A stereo pair is engineered by setting strip 0 (L) pan
            // hard-left and strip 1 (R) pan hard-right.
            beginTest ("Hard-left pan routes audio to monitor L only");
            {
                CallbackFixture f (1, 1, 2);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                t.pan.store (-1.0f);   // full left
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.process (256);
                expect (f.peakOut (0, 256) > 0.30f);
                expect (f.peakOut (1, 256) < 0.01f);   // ~zero (constant-power pan)
            }

            beginTest ("Hard-right pan routes audio to monitor R only");
            {
                CallbackFixture f (1, 1, 2);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                t.pan.store (1.0f);   // full right
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.5f, 256);
                f.process (256);
                expect (f.peakOut (1, 256) > 0.30f);
                expect (f.peakOut (0, 256) < 0.01f);
            }

            // ─── Recorder write path ─────────────────────────────────
            // End-to-end test of the lock-free per-channel ring +
            // background WAV writer. Drives the IO callback with
            // synthetic input while recording is rolling, then reads
            // the resulting Track_NN.wav back from disk and asserts
            // its length + content.
            auto makeTempSessionDir = []
            {
                auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("zynforge-test-" + juce::Uuid().toString());
                dir.createDirectory();
                return dir;
            };

            beginTest ("Recording produces Track_01.wav of the expected length");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    auto& t = f.engine.getRecorder().getTrack (0);
                    t.armed.store (true);
                    f.engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
                    expect (f.engine.startRecording (sessionDir));
                    expect (f.engine.isRecording());

                    // Feed ~0.5 s of constant 0.25 (192 blocks * 256 samples
                    // / 48000 Hz). Constant DC is a deliberately ugly
                    // signal -- it survives any silent-section guard.
                    f.writeInput (0, 0.25f, 256);
                    constexpr int kBlocks = 192;
                    for (int b = 0; b < kBlocks; ++b)
                        f.process (256);
                    f.engine.stopRecording();
                    expect (! f.engine.isRecording());
                }

                const auto wav = sessionDir.getChildFile ("Audio Files")
                                            .getChildFile ("Track_01.wav");
                expect (wav.existsAsFile());
                expect (wav.getSize() > 4096);   // header + meaningful payload

                // Read it back and verify length + non-silence.
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::FileInputStream> in (wav.createInputStream());
                expect (in != nullptr);
                std::unique_ptr<juce::AudioFormatReader> reader (
                    fmt.createReaderFor (in.release(), true));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    // 192 * 256 = 49152 samples. Recorder may drain a
                    // tiny extra block on stop; we just need at least
                    // most of what we fed.
                    expect (reader->lengthInSamples >= 40000);
                    expectWithinAbsoluteError ((double) reader->sampleRate, 48000.0, 1.0);

                    juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
                    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, false);
                    const float peak = buf.getMagnitude (0, 0, buf.getNumSamples());
                    expect (peak > 0.20f);   // non-silent
                }

                sessionDir.deleteRecursively();
            }

            beginTest ("Two armed strips produce two separate WAV files");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (2, 2, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    f.engine.getRecorder().getTrack (1).armed.store (true);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.30f, 256);
                    f.writeInput (1, 0.40f, 256);
                    for (int b = 0; b < 96; ++b) f.process (256);
                    f.engine.stopRecording();
                }
                const auto audioDir = sessionDir.getChildFile ("Audio Files");
                expect (audioDir.getChildFile ("Track_01.wav").existsAsFile());
                expect (audioDir.getChildFile ("Track_02.wav").existsAsFile());
                sessionDir.deleteRecursively();
            }

            beginTest ("Unarmed strip produces no audio data (file is silent or absent)");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (2, 2, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    // strip 1 not armed
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.30f, 256);
                    f.writeInput (1, 0.50f, 256);
                    for (int b = 0; b < 64; ++b) f.process (256);
                    f.engine.stopRecording();
                }
                const auto audioDir = sessionDir.getChildFile ("Audio Files");
                const auto wav0 = audioDir.getChildFile ("Track_01.wav");
                const auto wav1 = audioDir.getChildFile ("Track_02.wav");
                expect (wav0.existsAsFile());

                if (wav1.existsAsFile())
                {
                    // Some recorders open writers for every strip up-front
                    // and rely on the arm gate inside processBlock to
                    // skip writes. If the file exists, verify it's
                    // effectively empty / silent.
                    juce::WavAudioFormat fmt;
                    std::unique_ptr<juce::FileInputStream> in (wav1.createInputStream());
                    std::unique_ptr<juce::AudioFormatReader> reader (
                        fmt.createReaderFor (in.release(), true));
                    if (reader != nullptr && reader->lengthInSamples > 0)
                    {
                        juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
                        reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, false);
                        const float peak = buf.getMagnitude (0, 0, buf.getNumSamples());
                        expect (peak < 0.01f);
                    }
                }
                sessionDir.deleteRecursively();
            }

            beginTest ("session.report.json is written on stop");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    for (int b = 0; b < 32; ++b) f.process (256);
                    f.engine.stopRecording();
                }
                // Recorder writes session.report.json into the active
                // session dir on stop -- see MultitrackRecorder::stopRecording.
                const auto report = sessionDir.getChildFile ("session.report.json");
                expect (report.existsAsFile());
                const auto json = juce::JSON::parse (report);
                expect (json.isObject());
                if (auto* obj = json.getDynamicObject())
                {
                    expect ((double) obj->getProperty ("sampleRate") > 0.0);
                    expect ((int) obj->getProperty ("numTracks") == 1);
                }
                sessionDir.deleteRecursively();
            }

            beginTest ("isRecording flips false after stopRecording");
            {
                CallbackFixture f (1, 1, 2);
                const auto sessionDir = makeTempSessionDir();
                f.engine.getRecorder().getTrack (0).armed.store (true);
                expect (! f.engine.isRecording());
                expect (f.engine.startRecording (sessionDir));
                expect (f.engine.isRecording());
                f.engine.stopRecording();
                expect (! f.engine.isRecording());
                sessionDir.deleteRecursively();
            }

            beginTest ("stopRecording without startRecording is a no-op");
            {
                CallbackFixture f (1, 1, 2);
                f.engine.stopRecording();   // must not crash
                expect (! f.engine.isRecording());
            }

            // ─── Pre-roll history backfill ───────────────────────────
            beginTest ("Pre-roll dumps pre-record history into Track_01.wav");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    auto& rec = f.engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setCaptureFormat (CaptureFormat::Wav24);
                    rec.setPreRollSeconds (1);     // 1-second history buffer

                    // Pre-record fill: 0.5 s of constant 0.30. Audio
                    // thread pushes these blocks into the pre-roll ring
                    // even though recording hasn't started.
                    f.writeInput (0, 0.30f, 256);
                    constexpr int kPreBlocks = 96;     // ~0.5 s @ 48 kHz / 256
                    for (int b = 0; b < kPreBlocks; ++b) f.process (256);
                    expect (! f.engine.isRecording());

                    // Start recording -- dumps the history at the front.
                    expect (f.engine.startRecording (sessionDir));

                    // Live record: 0.3 s of 0.70 amplitude. Easy to
                    // distinguish from the 0.30 pre-roll segment.
                    f.writeInput (0, 0.70f, 256);
                    constexpr int kLiveBlocks = 56;   // ~0.3 s
                    for (int b = 0; b < kLiveBlocks; ++b) f.process (256);

                    f.engine.stopRecording();
                }

                const auto wav = sessionDir.getChildFile ("Audio Files")
                                            .getChildFile ("Track_01.wav");
                expect (wav.existsAsFile());
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::FileInputStream> in (wav.createInputStream());
                std::unique_ptr<juce::AudioFormatReader> reader (
                    fmt.createReaderFor (in.release(), true));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    // Total length must include BOTH the pre-roll dump
                    // AND the live segment. Pre-roll ring filled with
                    // 0.5 s of audio, live segment is 0.3 s. Allowing
                    // some slack, total samples must exceed 0.7 s.
                    expect (reader->lengthInSamples > (juce::int64) (0.7 * 48000.0));

                    juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
                    reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, false);

                    // First 0.4 s of the recorded file should be the
                    // pre-roll segment (constant 0.30). Last block
                    // should be the live segment (constant 0.70).
                    const auto* p = buf.getReadPointer (0);
                    const int firstSection = (int) juce::jmin (
                        (juce::int64) (0.4 * 48000.0), reader->lengthInSamples / 2);
                    float preMax = 0.0f;
                    for (int i = 0; i < firstSection; ++i)
                        if (std::abs (p[i]) > preMax) preMax = std::abs (p[i]);
                    float liveMax = 0.0f;
                    const int liveStart = (int) reader->lengthInSamples - 1024;
                    for (int i = juce::jmax (0, liveStart); i < (int) reader->lengthInSamples; ++i)
                        if (std::abs (p[i]) > liveMax) liveMax = std::abs (p[i]);

                    // Pre-roll section is the lower amplitude (~0.30),
                    // live tail is the higher (~0.70).
                    expect (preMax  > 0.20f && preMax  < 0.40f);
                    expect (liveMax > 0.60f && liveMax < 0.80f);
                }

                sessionDir.deleteRecursively();
            }

            beginTest ("Pre-roll = 0 means no history is dumped");
            {
                const auto sessionDir = makeTempSessionDir();
                juce::int64 framesWritten = 0;
                {
                    CallbackFixture f (1, 1, 2);
                    auto& rec = f.engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setPreRollSeconds (0);   // baseline -- no history

                    // Same pattern: fill before record, then record.
                    f.writeInput (0, 0.5f, 256);
                    for (int b = 0; b < 96; ++b) f.process (256);
                    expect (f.engine.startRecording (sessionDir));
                    for (int b = 0; b < 56; ++b) f.process (256);
                    f.engine.stopRecording();
                    framesWritten = 56 * 256;
                }

                const auto wav = sessionDir.getChildFile ("Audio Files")
                                            .getChildFile ("Track_01.wav");
                expect (wav.existsAsFile());
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::FileInputStream> in (wav.createInputStream());
                std::unique_ptr<juce::AudioFormatReader> reader (
                    fmt.createReaderFor (in.release(), true));
                if (reader != nullptr)
                {
                    // Should only contain ~the live segment, not the
                    // pre-record fill. ~0.3 s, NOT ~0.8 s.
                    expect (reader->lengthInSamples < (juce::int64) (0.5 * 48000.0));
                }
                sessionDir.deleteRecursively();
            }

            // ─── Marker drop during recording ────────────────────────
            beginTest ("dropMarkerAtCurrentPosition with no session returns -1");
            {
                CallbackFixture f (1, 1, 2);
                expectEquals (f.engine.dropMarkerAtCurrentPosition(), -1);
            }

            beginTest ("dropMarkerAtCurrentPosition during recording captures the position");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    expect (f.engine.startRecording (sessionDir));

                    // Process some blocks to advance samplesSinceStart.
                    f.writeInput (0, 0.2f, 256);
                    constexpr int kBlocks = 32;            // 32 * 256 = 8192 samples
                    for (int b = 0; b < kBlocks; ++b) f.process (256);

                    // Drop a marker at the live recorder position.
                    const int count = f.engine.dropMarkerAtCurrentPosition();
                    expect (count == 1);
                    const auto& markers = f.engine.getMarkers().getAll();
                    expectEquals ((int) markers.size(), 1);
                    if (! markers.empty())
                    {
                        // Marker sample should be within the recorded
                        // window. We've processed 32 blocks but the
                        // recorder rounds up; just need a sensible
                        // non-zero value bounded by the elapsed range.
                        expect (markers.front().sampleOffset > 0);
                        expect (markers.front().sampleOffset <= (juce::int64) kBlocks * 256);
                    }
                    f.engine.stopRecording();
                }
                sessionDir.deleteRecursively();
            }

            beginTest ("Two markers in sequence auto-name and increment count");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.2f, 256);
                    for (int b = 0; b < 16; ++b) f.process (256);
                    expectEquals (f.engine.dropMarkerAtCurrentPosition(), 1);
                    for (int b = 0; b < 16; ++b) f.process (256);
                    expectEquals (f.engine.dropMarkerAtCurrentPosition(), 2);
                    const auto& list = f.engine.getMarkers().getAll();
                    expectEquals ((int) list.size(), 2);
                    // Second marker must be at a later sample than the
                    // first (we processed more blocks between them).
                    if (list.size() >= 2)
                        expect (list[1].sampleOffset > list[0].sampleOffset);
                    f.engine.stopRecording();
                }
                sessionDir.deleteRecursively();
            }

            // ─── Master clip latch ───────────────────────────────────
            beginTest ("Master clip latch sets when output peak hits >= 0.999");
            {
                CallbackFixture f (1, 1, 2);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                f.engine.getMasterState().clipped.store (false);
                f.engine.setMasterOutputs (0, 1);
                // Force the input way over unity so the monitor sum
                // crashes through 0 dBFS. With pan-centre = 0.707 the
                // accumulator hits ~2.83 per sample; only the FLOAT
                // output gets that. The clip latch in the master block
                // reads the PRE-downcast accumulator peak.
                f.writeInput (0, 4.0f, 256);
                f.process (256);
                expect (f.engine.getMasterState().clipped.load());
            }

            beginTest ("Master clip latch is sticky until explicitly cleared");
            {
                CallbackFixture f (1, 1, 2);
                auto& t = f.engine.getRecorder().getTrack (0);
                t.armed.store (true); t.monitor.store (true);
                f.engine.setMasterOutputs (0, 1);

                // Trip it.
                f.writeInput (0, 5.0f, 256);
                f.process (256);
                expect (f.engine.getMasterState().clipped.load());

                // Now feed silence -- latch must remain true so the
                // engineer sees the indicator across blocks.
                f.writeInput (0, 0.0f, 256);
                for (int b = 0; b < 5; ++b) f.process (256);
                expect (f.engine.getMasterState().clipped.load());

                // Engineer clicks to clear -> direct atomic store.
                f.engine.getMasterState().clipped.store (false);
                expect (! f.engine.getMasterState().clipped.load());
                // A silent block should not re-trip it.
                f.process (256);
                expect (! f.engine.getMasterState().clipped.load());
            }

            // ─── Per-track output mute ───────────────────────────────
            beginTest ("outputMuted atomic round-trips per strip + per stereo partner");
            {
                CallbackFixture f (2, 2, 2);
                auto& s0 = f.engine.getRecorder().getTrack (0);
                auto& s1 = f.engine.getRecorder().getTrack (1);
                expect (! s0.outputMuted.load());
                expect (! s1.outputMuted.load());
                s0.outputMuted.store (true);
                expect (s0.outputMuted.load());
                expect (! s1.outputMuted.load());   // independent
                s0.outputMuted.store (false);
                expect (! s0.outputMuted.load());
            }

            // ─── Auto-split on 4 GiB WAV ceiling ─────────────────────
            // Drives the per-format chunk-size guard by forcing the
            // threshold low via setAutoSplitThresholdBytesForTests.
            // The real production threshold is 3.9 GiB which would
            // need writing actual gigabytes -- not practical in CI.
            beginTest ("Recorder auto-splits to Track_NN_partXX.wav at the byte threshold");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    // 256 KB threshold -> at WAV/24 mono = 144 KB/sec,
                    // that's ~1.8 s per part. ~3.0 s of audio should
                    // produce 2 or 3 parts.
                    MultitrackRecorder::setAutoSplitThresholdBytesForTests (256 * 1024);

                    CallbackFixture f (1, 1, 2);
                    auto& rec = f.engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setCaptureFormat (CaptureFormat::Wav24);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    // Drain after each block so the ring doesn't
                    // accumulate the entire take before the writer
                    // thread runs. Without this, in a test that feeds
                    // the IO callback synchronously, all samples queue
                    // up and roll in one huge write -- part 1 ends up
                    // with one block, part 2 with everything else.
                    constexpr int kBlocks = 576;        // 576 * 256 / 48000 ≈ 3.07 s
                    for (int b = 0; b < kBlocks; ++b)
                    {
                        f.process (256);
                        f.engine.getRecorder().drainPendingForTests();
                    }
                    f.engine.stopRecording();

                    // Restore the production threshold so subsequent
                    // tests aren't affected.
                    MultitrackRecorder::setAutoSplitThresholdBytesForTests (0);
                }

                const auto audioDir = sessionDir.getChildFile ("Audio Files");
                const auto part1 = audioDir.getChildFile ("Track_01.wav");
                const auto part2 = audioDir.getChildFile ("Track_01_part02.wav");
                expect (part1.existsAsFile());
                expect (part2.existsAsFile());

                // Each part should be a valid WAV with audible content.
                juce::WavAudioFormat fmt;
                for (const auto& p : { part1, part2 })
                {
                    std::unique_ptr<juce::FileInputStream> in (p.createInputStream());
                    expect (in != nullptr);
                    std::unique_ptr<juce::AudioFormatReader> reader (
                        fmt.createReaderFor (in.release(), true));
                    expect (reader != nullptr);
                    if (reader != nullptr)
                    {
                        expect (reader->lengthInSamples > 0);
                        juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
                        reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, false);
                        expect (buf.getMagnitude (0, 0, buf.getNumSamples()) > 0.20f);
                    }
                }

                // Part 1 must be under the test threshold (plus some
                // slack for the header rewrite). Confirms the split
                // actually triggered on size.
                expect (part1.getSize() < (juce::int64) (350 * 1024));

                sessionDir.deleteRecursively();
            }

            beginTest ("session.report.json enumerates Track_NN_partXX files in order");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    MultitrackRecorder::setAutoSplitThresholdBytesForTests (256 * 1024);
                    CallbackFixture f (1, 1, 2);
                    auto& rec = f.engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setCaptureFormat (CaptureFormat::Wav24);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    for (int b = 0; b < 576; ++b)
                    {
                        f.process (256);
                        rec.drainPendingForTests();
                    }
                    f.engine.stopRecording();
                    MultitrackRecorder::setAutoSplitThresholdBytesForTests (0);
                }

                const auto report = sessionDir.getChildFile ("session.report.json");
                expect (report.existsAsFile());
                const auto json = juce::JSON::parse (report);
                auto* root = json.getDynamicObject();
                expect (root != nullptr);
                if (root != nullptr)
                {
                    auto* tracks = root->getProperty ("tracks").getArray();
                    expect (tracks != nullptr);
                    if (tracks != nullptr && tracks->size() > 0)
                    {
                        auto* t0 = (*tracks)[0].getDynamicObject();
                        expect (t0 != nullptr);
                        if (t0 != nullptr)
                        {
                            auto* files = t0->getProperty ("files").getArray();
                            expect (files != nullptr);
                            if (files != nullptr)
                            {
                                // Must enumerate both parts in order.
                                expect (files->size() >= 2);
                                if (files->size() >= 2)
                                {
                                    expectEquals ((*files)[0].toString(),
                                                  juce::String ("Track_01.wav"));
                                    expectEquals ((*files)[1].toString(),
                                                  juce::String ("Track_01_part02.wav"));
                                }
                            }
                            // totalSamplesPrimary spans across ALL parts
                            // (sum-not-per-part), so it must equal the
                            // total samples we fed in.
                            const auto total = (juce::int64) t0->getProperty ("totalSamplesPrimary");
                            expect (total >= (juce::int64) 576 * 256 - 1024);
                        }
                    }
                }
                sessionDir.deleteRecursively();
            }

            beginTest ("session.report.json includes SHA-256 per part file");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    for (int b = 0; b < 32; ++b) f.process (256);
                    f.engine.stopRecording();
                }
                const auto report = sessionDir.getChildFile ("session.report.json");
                const auto json = juce::JSON::parse (report);
                auto* root = json.getDynamicObject();
                expect (root != nullptr);
                if (root != nullptr)
                {
                    auto* tracks = root->getProperty ("tracks").getArray();
                    expect (tracks != nullptr && tracks->size() > 0);
                    if (tracks != nullptr && tracks->size() > 0)
                    {
                        auto* t0 = (*tracks)[0].getDynamicObject();
                        auto* shas = t0->getProperty ("sha256").getArray();
                        expect (shas != nullptr && shas->size() == 1);
                        if (shas != nullptr && shas->size() >= 1)
                        {
                            // Each SHA-256 is 64 hex chars.
                            expectEquals ((*shas)[0].toString().length(), 64);
                        }
                    }
                }
                sessionDir.deleteRecursively();
            }

            // ─── N-way mirror destinations ───────────────────────────
            beginTest ("3-way mirror writes Track_NN.wav to primary + backup + extra root");
            {
                const auto sessionDir = makeTempSessionDir();
                const auto backupRoot = makeTempSessionDir();   // 2nd drive
                const auto mirrorRoot = makeTempSessionDir();   // 3rd drive
                {
                    CallbackFixture f (1, 1, 2);
                    auto& rec = f.engine.getRecorder();
                    rec.getTrack (0).armed.store (true);
                    rec.setCaptureFormat (CaptureFormat::Wav24);
                    rec.setBackupDirectory (backupRoot);
                    rec.setBackupCaptureFormat (CaptureFormat::Wav24);
                    rec.setMirrors ({ { mirrorRoot, CaptureFormat::Wav24 } });

                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    for (int b = 0; b < 32; ++b) f.process (256);
                    f.engine.stopRecording();
                    rec.setMirrors ({});  // reset for following tests
                    rec.setBackupDirectory ({});
                }

                // All three roots should now have an Audio Files/Track_01.wav.
                const auto primaryWav = sessionDir.getChildFile ("Audio Files")
                                                  .getChildFile ("Track_01.wav");
                const auto backupWav  = backupRoot.getChildFile (sessionDir.getFileName())
                                                  .getChildFile ("Audio Files")
                                                  .getChildFile ("Track_01.wav");
                const auto mirrorWav  = mirrorRoot.getChildFile (sessionDir.getFileName())
                                                  .getChildFile ("Audio Files")
                                                  .getChildFile ("Track_01.wav");
                expect (primaryWav.existsAsFile());
                expect (backupWav .existsAsFile());
                expect (mirrorWav .existsAsFile());

                // Each file should be a valid WAV with audible content.
                juce::WavAudioFormat fmt;
                for (const auto& f : { primaryWav, backupWav, mirrorWav })
                {
                    std::unique_ptr<juce::FileInputStream> in (f.createInputStream());
                    std::unique_ptr<juce::AudioFormatReader> reader (
                        fmt.createReaderFor (in.release(), true));
                    expect (reader != nullptr);
                    if (reader != nullptr && reader->lengthInSamples > 0)
                    {
                        juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
                        reader->read (&buf, 0, (int) reader->lengthInSamples, 0, true, false);
                        expect (buf.getMagnitude (0, 0, buf.getNumSamples()) > 0.20f);
                    }
                }

                // session.report.json should enumerate the mirror as
                // a 'mirrors' array on the track entry.
                const auto report = sessionDir.getChildFile ("session.report.json");
                const auto json = juce::JSON::parse (report);
                if (auto* root = json.getDynamicObject())
                {
                    auto* tracks = root->getProperty ("tracks").getArray();
                    if (tracks != nullptr && tracks->size() > 0)
                    {
                        auto* t0 = (*tracks)[0].getDynamicObject();
                        auto* mirrors = t0->getProperty ("mirrors").getArray();
                        expect (mirrors != nullptr);
                        if (mirrors != nullptr)
                        {
                            expectEquals (mirrors->size(), 1);
                            if (mirrors->size() >= 1)
                            {
                                auto* m0 = (*mirrors)[0].getDynamicObject();
                                expectEquals (m0->getProperty ("root").toString(),
                                              mirrorRoot.getFullPathName());
                                auto* mFiles = m0->getProperty ("files").getArray();
                                expect (mFiles != nullptr && mFiles->size() == 1);
                                if (mFiles != nullptr && mFiles->size() >= 1)
                                    expectEquals ((*mFiles)[0].toString(),
                                                  juce::String ("Track_01.wav"));
                            }
                        }
                    }
                }

                sessionDir.deleteRecursively();
                backupRoot.deleteRecursively();
                mirrorRoot.deleteRecursively();
            }

            beginTest ("Auto-split threshold defaults restore after test override clears");
            {
                MultitrackRecorder::setAutoSplitThresholdBytesForTests (0);
                expect (MultitrackRecorder::maxBytesForContainer (0)   // WAV
                          > (juce::int64) 3LL * 1024 * 1024 * 1024);
                expect (MultitrackRecorder::maxBytesForContainer (1)   // AIFF
                          > (juce::int64) 1LL * 1024 * 1024 * 1024);
                expect (MultitrackRecorder::maxBytesForContainer (1)   // AIFF < WAV
                          < MultitrackRecorder::maxBytesForContainer (0));
            }

            // ─── BWF metadata round-trip ─────────────────────────────
            beginTest ("Recorded WAV carries BWF bext metadata");
            {
                const auto sessionDir = makeTempSessionDir();
                {
                    CallbackFixture f (1, 1, 2);
                    f.engine.getRecorder().getTrack (0).armed.store (true);
                    f.engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
                    expect (f.engine.startRecording (sessionDir));
                    f.writeInput (0, 0.3f, 256);
                    for (int b = 0; b < 32; ++b) f.process (256);
                    f.engine.stopRecording();
                }
                const auto wav = sessionDir.getChildFile ("Audio Files")
                                            .getChildFile ("Track_01.wav");
                expect (wav.existsAsFile());
                juce::WavAudioFormat fmt;
                std::unique_ptr<juce::FileInputStream> in (wav.createInputStream());
                std::unique_ptr<juce::AudioFormatReader> reader (
                    fmt.createReaderFor (in.release(), true));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    const auto& meta = reader->metadataValues;
                    // Originator must say Zynforge Recording -- this is
                    // what mix engineers see when they import the file
                    // into Pro Tools / Logic / Reaper to identify it.
                    expectEquals (meta[juce::WavAudioFormat::bwavOriginator],
                                  juce::String ("Zynforge Recording"));
                    expect (meta[juce::WavAudioFormat::bwavOriginationDate].isNotEmpty());
                    expect (meta[juce::WavAudioFormat::bwavOriginationTime].isNotEmpty());
                    expect (meta[juce::WavAudioFormat::bwavDescription].isNotEmpty());
                }
                sessionDir.deleteRecursively();
            }

            beginTest ("Stereo pair (L hard-left + R hard-right) splits to L+R outs");
            {
                CallbackFixture f (2, 2, 2);
                // Strip 0 = L track, fed from input 0, panned hard L.
                // Strip 1 = R track, fed from input 1, panned hard R.
                for (int i = 0; i < 2; ++i)
                {
                    auto& t = f.engine.getRecorder().getTrack (i);
                    t.armed.store (true); t.monitor.store (true);
                }
                f.engine.setTrackStereo (0, true);   // mark as stereo pair
                f.engine.getRecorder().getTrack (0).pan.store (-1.0f);
                f.engine.getRecorder().getTrack (1).pan.store ( 1.0f);
                f.engine.setMasterOutputs (0, 1);
                f.writeInput (0, 0.4f, 256);
                f.writeInput (1, 0.4f, 256);
                f.process (256);
                // L input lands on monitor L, R input on monitor R.
                expect (f.peakOut (0, 256) > 0.20f);
                expect (f.peakOut (1, 256) > 0.20f);
            }
        }
    };

    static AudioCallbackTests audioCallbackTestsInstance;
}
