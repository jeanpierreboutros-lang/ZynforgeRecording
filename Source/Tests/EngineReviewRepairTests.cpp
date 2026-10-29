// Regressions for the real-time engine and playback review (2026-10-29):
// timecode chase wiring, input routing on a smaller device, persistence of
// pans set by cues and control surfaces, stereo-link state, MIDI clock
// transport messages and the session player's locate / loop behaviour.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MidiControlSurface.h"

#include <thread>

namespace zynforge
{
    class EngineReviewRepairTests final : public juce::UnitTest
    {
    public:
        EngineReviewRepairTests() : juce::UnitTest ("Engine review repairs", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Chase liveness follows the selected source, not whichever one spoke last");
            {
                AudioEngine e;
                e.setChaseMode (AudioEngine::ChaseMode::Ltc);
                // An MTC port left open while LTC is the master.
                e.handleIncomingMidiMessage (nullptr, juce::MidiMessage::fullFrame (
                    1, 2, 3, 4, juce::MidiMessage::fps25));
                expect (! e.isChaseLive(), "MTC made an LTC chase read as live");
                e.setChaseMode (AudioEngine::ChaseMode::Mtc);
                e.handleIncomingMidiMessage (nullptr, juce::MidiMessage::fullFrame (
                    1, 2, 3, 4, juce::MidiMessage::fps25));
                expect (e.isChaseLive(), "MTC chase did not go live on a full frame");
                e.setChaseMode (AudioEngine::ChaseMode::Off);
            }

            beginTest ("The LTC source strip follows a reorder; deleting it clears it");
            {
                AudioEngine e;
                e.setStripCount (3);
                e.setLtcSourceStrip (2);          // 1-based: strip index 1
                expect (e.swapTracks (0, 1));
                expectEquals (e.getLtcSourceStrip(), 1, "LTC source stayed on the old index");
                expect (e.removeStripAt (0));
                expectEquals (e.getLtcSourceStrip(), 0, "a deleted LTC strip left a stale source");
                e.clearAllStripOverrides();
                e.setStripCount (0);
            }

            beginTest ("A smaller device never rewrites the saved input patch");
            {
                AudioEngine e;
                e.setStripCount (4);
                e.prepareForTests (48000.0, 256);
                for (int i = 0; i < 4; ++i) e.setTrackInputRouting (i, i);

                // Laptop prep, or a fallback to the built-in device after an
                // interface dropout: two inputs, and the device (re)start
                // re-applies every strip's state.
                e.setDeviceInputCountForTests (2);
                e.prepareForTests (48000.0, 256);
                auto saved = inRoutes (e);
                expectEquals (saved[2], 2, "session_mix.json saved the wrapped input");
                expectEquals (saved[3], 3, "session_mix.json saved the wrapped input");

                // A reorder re-persists every strip's routing.
                expect (e.swapTracks (0, 1));
                saved = inRoutes (e);
                expectEquals (saved[0], 1); expectEquals (saved[1], 0);
                expectEquals (saved[2], 2, "a reorder persisted the wrapped input");
                expectEquals (saved[3], 3, "a reorder persisted the wrapped input");

                // Live on the small device the strip still hears something:
                // strip 4 wraps onto input 2.
                expect (monitorHears (e, 3, 1, 2), "strip 4 lost its live input on a 2-input device");

                // Back on the full rig, strip 4 records input 4 again.
                e.setDeviceInputCountForTests (4);
                e.prepareForTests (48000.0, 256);
                expect (monitorHears (e, 3, 3, 4), "strip 4 is not on input 4 back on the rig");
                expect (! monitorHears (e, 3, 1, 4), "strip 4 still records the wrapped input");
                e.setDeviceInputCountForTests (0);
                e.clearAllStripOverrides();
                e.setStripCount (0);
            }

            beginTest ("Pans set by a cue ramp or an MCU V-pot survive a device restart");
            {
                AudioEngine e;
                e.setStripCount (2);
                e.prepareForTests (48000.0, 256);
                e.setTrackPan (0, -0.3f);
                e.setTrackPan (1, 0.0f);
                e.setTrackPanRamped (0, 0.6f, 0.25);   // cue recall
                e.tickRamps (48000);
                expectWithinAbsoluteError (e.getRecorder().getTrack (0).pan.load(), 0.6f, 0.001f);

                MidiControlSurface mcu (e);   // V-pot 2, four ticks clockwise
                mcu.handleIncomingMidiMessage (nullptr, juce::MidiMessage::controllerEvent (1, 0x11, 4));
                expectWithinAbsoluteError (e.getRecorder().getTrack (1).pan.load(), 0.2f, 0.001f);

                e.prepareForTests (48000.0, 256);   // device restart: strip state re-applied
                expectWithinAbsoluteError (e.getRecorder().getTrack (0).pan.load(), 0.6f, 0.001f,
                                           "a cue-recalled pan snapped back after a restart");
                expectWithinAbsoluteError (e.getRecorder().getTrack (1).pan.load(), 0.2f, 0.001f,
                                           "a V-pot pan snapped back after a restart");
                e.clearAllStripOverrides();
                e.setStripCount (0);
            }

            beginTest ("Linking a stereo pair brings the R half onto the L half's state");
            {
                AudioEngine e;
                e.setStripCount (2);
                auto& l = e.getRecorder().getTrack (0);
                auto& r = e.getRecorder().getTrack (1);
                e.setTrackMuted (0, true);
                e.setTrackGainDb (0, -10.0f);
                e.setTrackVcaGroup (0, 2);
                e.setTrackEditGroup (0, 1);
                e.setTrackArmed (0, true);
                e.setTrackInputRouting (0, 4);
                e.setTrackOutputRouting (0, 6);
                l.monitor.store (true); l.outputMuted.store (true); l.streamSend.store (true);
                e.setTrackInputRouting (1, 9);
                e.setTrackOutputRouting (1, 9);
                e.setTrackPan (1, 0.4f);

                e.setTrackStereo (0, true);
                expect (r.muted.load(), "R stayed audible under a muted pair");
                expectWithinAbsoluteError (r.gainDb.load(), -10.0f, 0.001f, "R kept its own fader");
                expectEquals (r.vcaGroup.load(), 2, "R does not follow the pair's VCA");
                expectEquals (r.editGroup.load(), 1);
                expect (r.armed.load(), "R is unarmed in an armed pair");
                expect (r.monitor.load() && r.outputMuted.load() && r.streamSend.load());
                expectEquals (r.inputRouting.load(), 5, "R records its own old input");
                expectEquals (r.outputRouting.load(), 7);
                expectWithinAbsoluteError (r.pan.load(), 0.4f, 0.001f, "pan stays per channel");

                e.prepareForTests (48000.0, 256);   // the synced fader is persisted too
                expectWithinAbsoluteError (r.gainDb.load(), -10.0f, 0.001f);
                e.clearAllStripOverrides();
                e.setStripCount (0);
            }

            beginTest ("A locate that lands while a block renders is not overwritten");
            {
                const auto dir = writeSession (4.0);
                AudioEngine e;
                e.setStripCount (1);
                expect (e.loadSession (dir) == 1);
                auto& p = e.getPlayer();
                p.prepare (kRate, 256);

                std::atomic<bool> quit { false }, pause { true }, paused { true };
                std::atomic<int>  blocks { 0 };
                std::thread audio ([&]
                {
                    std::vector<float> buf (256);
                    float* out[1] = { buf.data() };
                    while (! quit.load())
                    {
                        if (pause.load()) { paused.store (true); std::this_thread::yield(); continue; }
                        paused.store (false);
                        p.processBlock (out, 1, 256);
                        blocks.fetch_add (1);
                    }
                });
                auto waitBlocks = [&] (int n)
                {
                    const int start = blocks.load();
                    while (blocks.load() < start + n) std::this_thread::yield();
                };

                int lost = 0;
                for (int k = 0; k < 300; ++k)
                {
                    const juce::int64 from   = (k % 2) != 0 ? 100000 : 10000;
                    const juce::int64 target = (k % 2) != 0 ? 10000 : 100000;
                    p.setPositionSamples (from);
                    p.start();
                    pause.store (false);
                    waitBlocks (2);
                    p.setPositionSamples (target);   // lands mid-render most of the time
                    waitBlocks (2);
                    pause.store (true);
                    while (! paused.load()) std::this_thread::yield();
                    const auto pos = p.getPositionSamples();
                    if (pos < target || pos > target + 48000) ++lost;
                }
                quit.store (true);
                audio.join();
                expectEquals (lost, 0, "locates lost to a block that was rendering");
                e.setStripCount (0);
                dir.deleteRecursively();
            }

            beginTest ("Loop-play wraps when the loop region runs past the session end");
            {
                const auto dir = writeSession (2.0);
                AudioEngine e;
                e.setStripCount (1);
                expect (e.loadSession (dir) == 1);
                auto& p = e.getPlayer();
                p.prepare (kRate, 512);
                const auto total = p.getTotalLengthSamples();
                p.setLoopRegion (total - 24000, total + 72000);
                p.setLoopEnabled (true);
                p.setPositionSamples (total - 1000);
                p.start();
                std::vector<float> buf (512);
                float* out[1] = { buf.data() };
                for (int b = 0; b < 10; ++b) p.processBlock (out, 1, 512);
                expect (p.isPlaying(), "loop-play stopped at the session end");
                const auto pos = p.getPositionSamples();
                expect (pos >= total - 24000 && pos < total, "playhead did not wrap to the loop start");
                p.stop();
                p.setLoopEnabled (false);
                p.clearLoopRegion();
                e.setStripCount (0);
                dir.deleteRecursively();
            }
        }

    private:
        static constexpr double kRate = 48000.0;

        // A one-track session folder with `seconds` of DC in Audio Files.
        static juce::File writeSession (double seconds)
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-engine-review-" + juce::Uuid().toString());
            const auto audio = dir.getChildFile ("Audio Files");
            audio.createDirectory();
            const int len = (int) (seconds * kRate);
            std::vector<float> samples ((size_t) len, 0.25f);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (audio.getChildFile ("Track_01.wav").createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), kRate, 1, 24, {}, 0));
            if (w != nullptr)
            {
                os.release();
                const float* ch[1] = { samples.data() };
                w->writeFromFloatArrays (ch, 1, len);
            }
            return dir;
        }

        static juce::Array<int> inRoutes (AudioEngine& e)
        {
            juce::Array<int> routes;
            const auto mix = juce::JSON::parse (e.captureSessionMixJson());
            if (auto* strips = mix["strips"].getArray())
                for (const auto& s : *strips) routes.add ((int) s["inRoute"]);
            return routes;
        }

        // Runs one callback with DC on device input `input` only and strip
        // `strip` the only one monitored; true when the master hears it.
        static bool monitorHears (AudioEngine& e, int strip, int input, int numInputs)
        {
            constexpr int block = 256;
            e.getMasterState().muted.store (false);
            e.getMasterState().gainDb.store (0.0f);
            e.setMasterStereo (true);
            e.setMasterOutputs (0, 1);
            for (int i = 0; i < e.getRecorder().getNumTracks(); ++i)
            {
                auto& t = e.getRecorder().getTrack (i);
                t.monitor.store (i == strip); t.armed.store (false);
                t.muted.store (false); t.soloed.store (false);
                t.gainDb.store (0.0f); t.pan.store (0.0f); t.vcaGroup.store (-1);
            }
            juce::AudioBuffer<float> in (numInputs, block), out (2, block);
            in.clear(); out.clear();
            for (int i = 0; i < block; ++i) in.setSample (input, i, 0.5f);
            e.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), numInputs,
                                                out.getArrayOfWritePointers(), 2, block, {});
            return out.getMagnitude (0, 0, block) > 0.1f;
        }
    };

    static EngineReviewRepairTests engineReviewRepairTests;
}
