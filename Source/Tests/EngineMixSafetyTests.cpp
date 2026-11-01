// What the audio engine puts on outputs and into StereoMix.wav when strips are
// linked, soloed or following the desk. Drives the real audio callback with
// synthetic buffers, no device.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MixKernel.h"

#include <atomic>
#include <functional>
#include <vector>

namespace zynforge
{
    namespace
    {
        struct TempFolder
        {
            juce::File dir { juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zynforge-mixsafety-" + juce::Uuid().toString()) };
            TempFolder()  { dir.createDirectory(); }
            ~TempFolder() { dir.deleteRecursively(); }
        };

        // One engine with neutral strips, all master-only, and callback buffers.
        struct Rig
        {
            Rig (int strips, int inputs, int outputs, int blockSize = 256)
                : block (blockSize), in (inputs, blockSize), out (outputs, blockSize)
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                engine.setStripCount (strips);
                engine.prepareForTests (48000.0, blockSize);
                engine.getMasterState().muted.store (false);
                engine.getMasterState().gainDb.store (0.0f);
                engine.setMasterStereo (true);
                engine.setMasterOutputs (0, 1);
                engine.setStreamOutputs (-1, -1);
                for (int i = 0; i < strips; ++i)
                {
                    auto& t = engine.getRecorder().getTrack (i);
                    t.gainDb.store (0.0f); t.pan.store (0.0f);
                    t.muted.store (false); t.soloed.store (false);
                    t.armed.store (false); t.monitor.store (false);
                    t.isStereo.store (false); t.streamSend.store (false);
                    engine.setTrackInputRouting  (i, juce::jlimit (-1, inputs - 1, i));
                    engine.setTrackOutputRouting (i, -1);
                    for (int s = 0; s < TrackState::kNumSends; ++s)
                        engine.setTrackSend (i, s, -1, 0.0f, true);
                }
                in.clear(); out.clear();
            }

            void input (int ch, float value)
            {
                for (int i = 0; i < block; ++i) in.setSample (ch, i, value);
            }

            void process()
            {
                out.clear();
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                         out.getArrayOfWritePointers(), out.getNumChannels(),
                                                         block, {});
            }

            float peak (int ch) const { return out.getMagnitude (ch, 0, block); }

            AudioEngine engine;
            int block;
            juce::AudioBuffer<float> in, out;
        };

        // Rewrite one strip field in a saved session_mix.json.
        void editMix (const juce::File& dir, const std::function<void (juce::Array<juce::var>&)>& edit)
        {
            const auto file = dir.getChildFile ("session_mix.json");
            auto root = juce::JSON::parse (file);
            edit (*root.getProperty ("strips", {}).getArray());
            file.replaceWithText (juce::JSON::toString (root));
        }
    }

    class EngineMixSafetyTests final : public juce::UnitTest
    {
    public:
        EngineMixSafetyTests() : juce::UnitTest ("Engine mix safety", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("A stereo pair's STREAM send is one control on both halves");
            {
                Rig r (2, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackStereo (0, true);
                e.setTrackStream (0, true);
                expect (rec.getTrack (0).streamSend.load() && rec.getTrack (1).streamSend.load(),
                        "R stayed out of the stream bus");
                e.setTrackStream (0, false);
                expect (! rec.getTrack (0).streamSend.load() && ! rec.getTrack (1).streamSend.load(),
                        "R stayed alone in the stream bus");
                e.setTrackStereo (0, false);
            }

            beginTest ("A stereo strip's bus send reaches the bus from both halves");
            {
                Rig r (3, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackStereo (0, true);
                e.setTrackIsBus (2, true);
                e.setTrackOutputRouting (2, 3);
                e.setTrackSend (0, 0, 2, -6.0f, false);
                expectEquals (rec.getTrack (1).sends[0].targetBus.load(), 2, "R has no send");
                expectWithinAbsoluteError (rec.getTrack (1).sends[0].levelDb.load(), -6.0f, 0.001f);
                expect (! rec.getTrack (1).sends[0].postFader.load());
                e.setTrackSend (0, 0, -1, 0.0f, true);
                expectEquals (rec.getTrack (1).sends[0].targetBus.load(), -1, "R kept a cleared send");
                e.setTrackStereo (0, false);
            }

            beginTest ("A stereo strip feeds a stereo bus half for half");
            {
                Rig r (4, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackIsBus (2, true); e.setTrackIsBus (3, true);
                e.setTrackStereo (2, true);
                e.setTrackStereo (0, true);
                e.setTrackSend (0, 1, 2, 0.0f, true);
                expectEquals (rec.getTrack (0).sends[1].targetBus.load(), 2);
                expectEquals (rec.getTrack (1).sends[1].targetBus.load(), 3);
                e.setTrackStereo (0, false); e.setTrackStereo (2, false);
            }

            beginTest ("Linking a pair brings R's sends onto L's");
            {
                Rig r (3, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackIsBus (2, true);
                e.setTrackSend (0, 0, 2, -3.0f, false);
                e.setTrackStereo (0, true);
                expectEquals (rec.getTrack (1).sends[0].targetBus.load(), 2);
                expectWithinAbsoluteError (rec.getTrack (1).sends[0].levelDb.load(), -3.0f, 0.001f);
                e.setTrackStereo (0, false);
            }

            beginTest ("A session saved with a half-set pair opens with both halves as the strip shows");
            {
                TempFolder f;
                {
                    Rig r (3, 2, 4);
                    auto& e = r.engine;
                    e.setTrackStereo (0, true);
                    e.setTrackIsBus (2, true);
                    expect (e.saveSessionMixTo (f.dir));
                    e.setTrackStereo (0, false);
                }
                // As an older build wrote it: L's STREAM on with R's off, and
                // L's send present with R's slot empty.
                editMix (f.dir, [] (juce::Array<juce::var>& strips)
                {
                    strips.getReference (0).getDynamicObject()->setProperty ("streamSend", true);
                    strips.getReference (1).getDynamicObject()->setProperty ("streamSend", false);
                    auto* sends = strips.getReference (0).getProperty ("sends", {}).getArray();
                    sends->getReference (0).getDynamicObject()->setProperty ("bus", 2);
                    sends->getReference (0).getDynamicObject()->setProperty ("dB", -9.0);
                });
                {
                    Rig r (3, 2, 4);
                    auto& e = r.engine; auto& rec = e.getRecorder();
                    expect (e.loadSessionMixFrom (f.dir));
                    expect (rec.getTrack (1).streamSend.load(), "R missing from the stream after reopen");
                    expectEquals (rec.getTrack (1).sends[0].targetBus.load(), 2);
                    expectWithinAbsoluteError (rec.getTrack (1).sends[0].levelDb.load(), -9.0f, 0.001f);
                    e.setTrackStereo (0, false);
                }
                // The reverse: the strip shows STREAM off while R alone stayed on.
                editMix (f.dir, [] (juce::Array<juce::var>& strips)
                {
                    strips.getReference (0).getDynamicObject()->setProperty ("streamSend", false);
                    strips.getReference (1).getDynamicObject()->setProperty ("streamSend", true);
                });
                {
                    Rig r (3, 2, 4);
                    auto& e = r.engine; auto& rec = e.getRecorder();
                    expect (e.loadSessionMixFrom (f.dir));
                    expect (! rec.getTrack (1).streamSend.load(), "R stayed alone in the stream after reopen");
                    e.setTrackStereo (0, false);
                }
            }

            beginTest ("StereoMix.wav holds both channels of a linked pair");
            {
                TempFolder f;
                {
                    Rig r (2, 2, 2);
                    auto& e = r.engine; auto& rec = e.getRecorder();
                    e.setTrackStereo (0, true);
                    rec.getTrack (0).pan.store (-1.0f);
                    rec.getTrack (1).pan.store ( 1.0f);
                    e.setTrackArmed (0, true);
                    e.setTrackStream (0, true);
                    e.setRecordStereoMix (true);
                    expect (e.startRecording (f.dir));
                    r.input (0, 0.5f); r.input (1, 0.25f);
                    for (int b = 0; b < 32; ++b) r.process();
                    e.stopRecording();
                    e.setRecordStereoMix (false);
                    e.setTrackStereo (0, false);
                }
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> mix (
                    fm.createReaderFor (f.dir.getChildFile ("Export Files/StereoMix.wav")));
                expect (mix != nullptr, "StereoMix.wav missing");
                if (mix != nullptr)
                {
                    juce::AudioBuffer<float> data (2, (int) mix->lengthInSamples);
                    mix->read (&data, 0, data.getNumSamples(), 0, true, true);
                    expectWithinAbsoluteError (data.getMagnitude (0, 0, data.getNumSamples()), 0.5f, 0.01f);
                    expectWithinAbsoluteError (data.getMagnitude (1, 0, data.getNumSamples()), 0.25f, 0.01f,
                                               "the right channel of the pair is missing");
                }
            }

            beginTest ("Trim-Follow with no capture reference holds the channel at 0 dB");
            {
                Rig r (1, 1, 2);
                auto& e = r.engine; auto& t = e.getRecorder().getTrack (0);
                t.monitor.store (true);
                e.setTrimFollowEnabled (true);
                // The desk reports its absolute preamp. No take ever saw a
                // reading, so there is no move to follow.
                e.setTrackLiveInputGain (0, 36.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), 0.0f, 0.001f,
                                           "the absolute preamp value became an offset");
                e.setTrackLiveInputGain (0, 37.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), 0.0f, 0.001f,
                                           "a 1 dB nudge moved a channel with no reference");
                expect (e.getTrimFollowUnreferencedCount() == 1);
                expect (e.getTrimFollowNotice().contains ("no capture reference"),
                        "nothing says the channel is held");
            }

            beginTest ("Trim-Follow with no reference leaves recorded playback untouched on every route");
            {
                TempFolder f;
                {
                    Rig r (1, 1, 2);
                    r.engine.setTrackArmed (0, true);
                    expect (r.engine.startRecording (f.dir));
                    r.input (0, 0.25f);
                    for (int b = 0; b < 64; ++b) r.process();
                    r.engine.stopRecording();
                }
                Rig r (1, 1, 4);
                auto& e = r.engine;
                e.setTrackOutputRouting (0, 2);
                e.setStreamOutputs (-1, -1);
                e.getPlayer().loadSession (f.dir);
                e.startPlayback();
                float reference = 0.0f;
                for (int attempt = 0; attempt < 80 && reference < 0.1f; ++attempt)
                {
                    e.getPlayer().setPositionSamples (0);
                    r.process();
                    reference = r.peak (2);
                    if (reference < 0.1f) juce::Thread::sleep (15);
                }
                expect (reference > 0.1f, "no playback reached the direct output");
                e.setTrimFollowEnabled (true);
                e.setTrackLiveInputGain (0, 36.0f);
                e.getPlayer().setPositionSamples (0);
                r.process();
                expectWithinAbsoluteError (r.peak (2), reference, reference * 0.02f,
                                           "the preamp's absolute value reached the direct output");
                e.stopPlayback();
            }

            beginTest ("Trim-Follow follows a real move and clamps at the PA limit");
            {
                Rig r (1, 1, 2);
                auto& e = r.engine; auto& t = e.getRecorder().getTrack (0);
                e.setTrackLiveInputGain (0, 30.0f);
                t.armed.store (true);
                e.snapshotCaptureInputGains();
                t.armed.store (false);
                expect (t.captureInputGainKnown.load());
                e.setTrackLiveInputGain (0, 31.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), 1.0f, 0.001f);
                e.setTrackLiveInputGain (0, 72.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), mix::kTrimFollowMaxUpDb, 0.001f);
                e.setTrimFollowEnabled (true);
                expect (e.getTrimFollowNotice().contains ("limit"), "a held-back move is not reported");
                e.setTrackLiveInputGain (0, -96.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), mix::kTrimFollowMaxDownDb, 0.001f);
            }

            beginTest ("A take started with no desk reading records no reference, and that survives a reopen");
            {
                TempFolder f;
                {
                    Rig r (1, 1, 2);
                    auto& e = r.engine; auto& t = e.getRecorder().getTrack (0);
                    e.setTrackArmed (0, true);
                    expect (e.startRecording (f.dir));
                    r.input (0, 0.25f);
                    for (int b = 0; b < 8; ++b) r.process();
                    e.stopRecording();
                    expect (! t.captureInputGainKnown.load());
                    expect (e.saveSessionMixTo (f.dir));
                }
                Rig r (1, 1, 2);
                auto& e = r.engine;
                expect (e.loadSessionMixFrom (f.dir));
                e.setTrackLiveInputGain (0, 36.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), 0.0f, 0.001f);
            }

            beginTest ("A session from before the reference flag treats a stored 0 dB as no reference");
            {
                TempFolder f;
                {
                    Rig r (2, 2, 2);
                    expect (r.engine.saveSessionMixTo (f.dir));
                }
                editMix (f.dir, [] (juce::Array<juce::var>& strips)
                {
                    for (auto& s : strips) s.getDynamicObject()->removeProperty ("captureInputGainKnown");
                    strips.getReference (0).getDynamicObject()->setProperty ("captureInputGainDb", 0.0);
                    strips.getReference (1).getDynamicObject()->setProperty ("captureInputGainDb", 30.0);
                });
                Rig r (2, 2, 2);
                auto& e = r.engine;
                expect (e.loadSessionMixFrom (f.dir));
                e.setTrackLiveInputGain (0, 36.0f);
                e.setTrackLiveInputGain (1, 33.0f);
                expectWithinAbsoluteError (e.getTrackTrimDelta (0), 0.0f, 0.001f,
                                           "an unknown old reference produced an offset");
                expectWithinAbsoluteError (e.getTrackTrimDelta (1), 3.0f, 0.001f,
                                           "a real old reference stopped following");
            }

            beginTest ("A saved send level outside the fader range is clamped on load");
            {
                TempFolder f;
                {
                    Rig r (2, 2, 2);
                    r.engine.setTrackIsBus (1, true);
                    r.engine.setTrackSend (0, 0, 1, 0.0f, true);
                    expect (r.engine.saveSessionMixTo (f.dir));
                }
                editMix (f.dir, [] (juce::Array<juce::var>& strips)
                {
                    auto* sends = strips.getReference (0).getProperty ("sends", {}).getArray();
                    sends->getReference (0).getDynamicObject()->setProperty ("dB", 400.0);
                });
                Rig r (2, 2, 2);
                expect (r.engine.loadSessionMixFrom (f.dir));
                expectWithinAbsoluteError (r.engine.getRecorder().getTrack (0).sends[0].levelDb.load(), 12.0f, 0.001f,
                                           "a damaged level reached the bus");
            }

            beginTest ("Trim-Follow stays off live input on the stream bus and StereoMix");
            {
                TempFolder f;
                Rig r (2, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setStreamOutputs (2, 3);
                e.setTrackStream (0, true);
                e.setTrackStream (1, true);
                e.setTrackLiveInputGain (0, 30.0f);
                e.setTrackLiveInputGain (1, 30.0f);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (1).armed.store (true);
                e.snapshotCaptureInputGains();
                rec.getTrack (1).armed.store (false);      // keeps a reference from an earlier take
                expect (e.startRecording (f.dir));
                r.input (0, 0.25f); r.input (1, 0.25f);
                r.process();
                const float level = r.peak (2);
                expect (level > 0.1f);
                e.setTrimFollowEnabled (true);
                e.setTrackLiveInputGain (0, 36.0f);
                e.setTrackLiveInputGain (1, 36.0f);
                r.process();
                expectWithinAbsoluteError (r.peak (2), level, 0.001f, "a preamp move was applied twice to live input");
                e.stopRecording();
            }

            beginTest ("A new session clears Trim-Follow references");
            {
                Rig r (1, 1, 2);
                auto& e = r.engine; auto& t = e.getRecorder().getTrack (0);
                e.setTrackLiveInputGain (0, 30.0f);
                t.armed.store (true);
                e.snapshotCaptureInputGains();
                t.armed.store (false);
                e.clearSessionState();
                expect (! e.getRecorder().getTrack (0).captureInputGainKnown.load());
                expect (! e.getRecorder().getTrack (0).liveInputGainKnown.load());
            }
        }
    };

    static EngineMixSafetyTests engineMixSafetyTests;
}
