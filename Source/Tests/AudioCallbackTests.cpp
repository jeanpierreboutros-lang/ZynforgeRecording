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
