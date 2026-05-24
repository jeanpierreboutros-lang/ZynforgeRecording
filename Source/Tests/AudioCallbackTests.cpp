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
                    t.gainDb .store (0.0f);
                    t.pan    .store (0.0f);
                    t.muted  .store (false);
                    t.soloed .store (false);
                    t.armed  .store (false);
                    t.monitor.store (false);
                    engine.setTrackInputRouting  (i, juce::jlimit (-1, numInputs - 1, i));
                    engine.setTrackOutputRouting (i, -1);   // master-only
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
        }
    };

    static AudioCallbackTests audioCallbackTestsInstance;
}
