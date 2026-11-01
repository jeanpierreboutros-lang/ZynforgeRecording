// What the audio engine puts on outputs and into StereoMix.wav when strips are
// linked, soloed or following the desk. Drives the real audio callback with
// synthetic buffers, no device.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MixKernel.h"
#include "../Audio/PlaybackReader.h"
#include "../Audio/SessionPlayer.h"

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

            beginTest ("Solo during a take leaves StereoMix.wav and the stream bus alone");
            {
                TempFolder f;
                Rig r (2, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setStreamOutputs (2, 3);
                e.setTrackStream (0, true);
                e.setTrackStream (1, true);
                e.setTrackArmed (0, true); e.setTrackArmed (1, true);
                e.setRecordStereoMix (true);
                expect (e.startRecording (f.dir));
                r.input (0, 0.5f); r.input (1, 0.5f);
                for (int b = 0; b < 16; ++b) r.process();
                const float bothStrips = r.peak (2);
                expect (bothStrips > 0.5f);
                e.setTrackSoloed (0, true);          // the engineer checks a mic
                r.process();
                expectWithinAbsoluteError (r.peak (2), bothStrips, 0.001f, "solo reached the stream bus");
                expect (r.peak (0) < bothStrips, "solo stopped working on the monitor");
                for (int b = 0; b < 16; ++b) r.process();
                e.setTrackMuted (1, true);           // a mute is part of the mix
                r.process();
                expect (r.peak (2) < bothStrips * 0.75f, "a mute no longer reaches the stream bus");
                e.stopRecording();
                e.setRecordStereoMix (false);
                e.setTrackSoloed (0, false); e.setTrackMuted (1, false);
                rec.getTrack (0).streamSend.store (false);
                rec.getTrack (1).streamSend.store (false);

                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> mix (
                    fm.createReaderFor (f.dir.getChildFile ("Export Files/StereoMix.wav")));
                expect (mix != nullptr, "StereoMix.wav missing");
                if (mix != nullptr)
                {
                    // Block 20 of the take: the solo was on, the mute not yet.
                    juce::AudioBuffer<float> data (2, 256);
                    mix->read (&data, 0, 256, 256 * 20, true, true);
                    expectWithinAbsoluteError (data.getMagnitude (0, 0, 256), bothStrips, 0.001f,
                                               "solo reached the recorded mix");
                }
            }

            beginTest ("1:1 strips keep the mix off console channels 1-2");
            {
                Rig r (4, 4, 8);
                auto& e = r.engine; auto& rec = e.getRecorder();
                for (int i = 0; i < 4; ++i)
                {
                    e.setTrackOutputRouting (i, i);    // the virtual soundcheck patch
                    rec.getTrack (i).monitor.store (true);
                    r.input (i, 0.1f);
                }
                r.process();
                expect (e.isMasterHeldOff(), "nothing says the mix was held off");
                expect (r.peak (0) < 0.02f, "the whole mix landed on the first console channel");
                expect (e.getBusRoutingNotice().contains ("Out 1-2"));

                e.setMixMayShareStripOutputs (true);
                r.process();
                expect (! e.isMasterHeldOff());
                expect (r.peak (0) > 0.05f, "the engineer's override did nothing");
                e.setMixMayShareStripOutputs (false);
            }

            beginTest ("A strip muted for a moment does not let the mix onto its channel");
            {
                Rig r (2, 2, 8);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackOutputRouting (0, 0);
                rec.getTrack (1).monitor.store (true);
                r.input (1, 0.25f);
                e.setTrackMuted (0, true);
                r.process();
                expect (r.peak (0) < 0.001f, "the mix reached a patched output behind a mute");
                rec.getTrack (0).outputMuted.store (true);
                r.process();
                expect (r.peak (0) < 0.001f);
                rec.getTrack (0).outputMuted.store (false);
                e.setTrackMuted (0, false);
            }

            beginTest ("With the mix held off, the click stays off and the master still meters");
            {
                Rig r (2, 2, 8);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackOutputRouting (0, 0);
                rec.getTrack (1).monitor.store (true);
                r.input (1, 0.25f);
                e.getMasterState().peak.store (0.0f);
                e.getClickEngine().prepare (48000.0);
                e.getClickEngine().setEnabled (true);
                float loudest = 0.0f;
                for (int b = 0; b < 200; ++b)
                {
                    r.process();
                    loudest = juce::jmax (loudest, r.peak (1));
                }
                e.getClickEngine().setEnabled (false);
                expectEquals (loudest, 0.0f, "the click stayed on a held pair");
                expect (e.getMasterState().peak.load() > 0.01f, "the held mix stopped metering");
            }

            beginTest ("Master output Off takes nothing to any output");
            {
                Rig r (2, 2, 4);
                auto& e = r.engine; auto& rec = e.getRecorder();
                rec.getTrack (0).monitor.store (true);
                r.input (0, 0.5f);
                e.setMasterOutputs (-1, -1);
                r.process();
                for (int ch = 0; ch < 4; ++ch) expectEquals (r.peak (ch), 0.0f);
                expect (! e.isMasterHeldOff(), "an off master is a choice, not a conflict");
                expect (e.getMasterState().peak.load() > 0.1f);
                e.setMasterOutputs (0, 1);
            }

            beginTest ("A two-output device keeps its mix with strips patched there");
            {
                Rig r (3, 3, 2);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setTrackOutputRouting (0, 0);
                e.setTrackOutputRouting (1, 1);
                rec.getTrack (2).monitor.store (true);
                r.input (2, 0.5f);
                r.process();
                expect (! e.isMasterHeldOff());
                expect (r.peak (0) > 0.1f && r.peak (1) > 0.1f, "laptop monitoring lost the mix");
            }

            beginTest ("A stream pair on a patched output is held off the same way");
            {
                Rig r (2, 2, 8);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setStreamOutputs (2, 3);
                e.setTrackOutputRouting (0, 2);
                e.setTrackStream (1, true);
                rec.getTrack (1).monitor.store (true);
                r.input (1, 0.5f);
                r.process();
                expect (e.isStreamHeldOff());
                expect (r.peak (3) < 0.001f, "the stream bus landed beside a patched strip");
                expect (e.getBusRoutingNotice().contains ("Stream"));
                e.setStreamOutputs (-1, -1);
            }

            beginTest ("A saved pair past a smaller device keeps both channels");
            {
                Rig r (2, 2, 2);
                auto& e = r.engine; auto& rec = e.getRecorder();
                e.setMasterOutputs (8, 9);
                rec.getTrack (0).pan.store (-1.0f);
                rec.getTrack (1).pan.store ( 1.0f);
                rec.getTrack (0).monitor.store (true);
                rec.getTrack (1).monitor.store (true);
                r.input (0, 0.5f); r.input (1, 0.25f);
                r.process();
                expectWithinAbsoluteError (r.peak (0), 0.5f, 0.01f);
                expectWithinAbsoluteError (r.peak (1), 0.25f, 0.01f, "the right channel was dropped");
                e.setMasterOutputs (0, 1);
            }

            beginTest ("A one-output device gets both sides of the mix");
            {
                Rig r (2, 2, 1);
                auto& e = r.engine; auto& rec = e.getRecorder();
                rec.getTrack (0).pan.store (-1.0f);
                rec.getTrack (1).pan.store ( 1.0f);
                rec.getTrack (1).monitor.store (true);
                r.input (1, 0.5f);
                r.process();
                expect (r.peak (0) > 0.2f, "hard-right strips vanished on a single output");
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

            beginTest ("A muted strip that is soloed is heard on the master, rolling and in a bounce");
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
                Rig r (1, 1, 2);
                auto& e = r.engine;
                expectEquals (e.loadSession (f.dir), 1);
                e.setTrackMuted (0, true);
                e.setTrackSoloed (0, true);
                e.startPlayback();
                float heard = 0.0f;
                for (int attempt = 0; attempt < 80 && heard < 0.1f; ++attempt)
                {
                    e.getPlayer().setPositionSamples (0);
                    r.process();
                    heard = r.peak (0);
                    if (heard < 0.1f) juce::Thread::sleep (15);
                }
                e.stopPlayback();
                expect (heard > 0.1f, "solo stopped working on a muted strip once the transport rolled");

                juce::AudioBuffer<float> bounce;
                expect (e.renderStereoMix (bounce, 4800));
                expect (bounce.getMagnitude (0, 0, 4800) > 0.1f, "the bounce dropped what the monitor plays");
                e.setTrackMuted (0, false);
                e.setTrackSoloed (0, false);
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

    namespace
    {
        // A source whose sample at position n is a value only that n could
        // hold, so a torn or misplaced block shows up in the audio.
        float positionValue (juce::int64 n) { return 0.1f + 0.8f * (float) (n % 1021) / 1021.0f; }

        struct CountingSource final : public juce::AudioFormatReader
        {
            explicit CountingSource (juce::int64 length, bool fail = false)
                : juce::AudioFormatReader (nullptr, "test"), failing (fail)
            {
                sampleRate = 48000.0; lengthInSamples = length; numChannels = 1;
                bitsPerSample = 32; usesFloatingPointData = true;
            }

            bool readSamples (int* const* dest, int numDest, int offset, juce::int64 start, int num) override
            {
                while (gate.load()) juce::Thread::sleep (1);
                for (int ch = 0; ch < numDest; ++ch)
                    if (auto* out = reinterpret_cast<float*> (dest[ch]))
                        for (int i = 0; i < num; ++i)
                            out[offset + i] = positionValue (start + i);
                return ! failing;
            }

            std::atomic<bool> gate { false };
            bool failing;
        };

        void writeTrack (const juce::File& file, float level, int length)
        {
            file.getParentDirectory().createDirectory();
            juce::AudioBuffer<float> data (1, 32768);
            for (int i = 0; i < data.getNumSamples(); ++i) data.setSample (0, i, level);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
            for (int left = length; left > 0; left -= data.getNumSamples())
                writer->writeFromAudioSampleBuffer (data, 0, juce::jmin (left, data.getNumSamples()));
        }

        // Plays a session a block at a time, a few times faster than real
        // time, and holds it to one rule: every sample the playhead moved past
        // reached every track. Silence is only allowed while it stands still.
        struct JumpChecker
        {
            JumpChecker (juce::UnitTest& t, SessionPlayer& p, int tracks)
                : test (t), player (p), buffers (tracks, kBlock)
            {
                player.prepare (48000.0, kBlock);
            }

            juce::int64 advanced (juce::int64 before, juce::int64 after) const
            {
                if (after >= before) return after - before;
                return (player.getLoopEnd() - before) + (after - player.getLoopStart());
            }

            void run (int blocks)
            {
                for (int b = 0; b < blocks; ++b)
                {
                    const auto before = player.getPositionSamples();
                    player.processBlock (buffers.getArrayOfWritePointers(), buffers.getNumChannels(), kBlock);
                    const auto moved = advanced (before, player.getPositionSamples());
                    for (int ch = 0; ch < buffers.getNumChannels(); ++ch)
                    {
                        int heard = 0;
                        for (int i = 0; i < kBlock; ++i)
                            heard += buffers.getSample (ch, i) != 0.0f ? 1 : 0;
                        if (heard != (int) moved) ++faults;
                    }
                    if (moved == 0) ++heldBlocks; else playedSamples += moved;
                    juce::Thread::sleep (2);
                }
            }

            static constexpr int kBlock = 512;
            juce::UnitTest& test;
            SessionPlayer& player;
            juce::AudioBuffer<float> buffers;
            int faults { 0 }, heldBlocks { 0 };
            juce::int64 playedSamples { 0 };
        };
    }

    class PlaybackJumpTests final : public juce::UnitTest
    {
    public:
        PlaybackJumpTests() : juce::UnitTest ("Playback jumps", "zynforge") {}

        void runTest() override
        {
            constexpr int kSecond = 48000;

            beginTest ("PlaybackReader: a miss is silence at once and a cue fills beside the read window");
            {
                juce::TimeSliceThread thread ("test reader");
                thread.startThread();
                auto* source = new CountingSource (kSecond * 60);
                PlaybackReader reader (source, thread, kSecond * 2);
                juce::AudioBuffer<float> out (1, 512);

                source->gate = true;
                out.clear();
                expect (! reader.read (&out, 0, 512, kSecond * 30, true, true), "a miss reported success");
                expect (out.getMagnitude (0, 512) == 0.0f, "a miss was not silent");
                source->gate = false;

                const auto wait = [&] (juce::int64 at)
                {
                    for (int i = 0; i < 2000 && ! reader.isBuffered (at, 512); ++i) juce::Thread::sleep (1);
                    return reader.isBuffered (at, 512);
                };
                expect (wait (kSecond * 30), "a read position was not filled");
                reader.cue (kSecond * 50);
                expect (wait (kSecond * 50), "a cue was not filled");
                expect (reader.isBuffered (kSecond * 30, 512), "the cue took the read window's block");

                out.clear();
                expect (reader.read (&out, 0, 512, kSecond * 50, true, true));
                bool right = true;
                for (int i = 0; i < 512; ++i)
                    right = right && out.getSample (0, i) == positionValue (kSecond * 50 + i);
                expect (right, "cued audio is not the audio at that position");
            }

            beginTest ("PlaybackReader: past the end is nothing to wait for, and a failed source says so");
            {
                juce::TimeSliceThread thread ("test reader");
                thread.startThread();
                PlaybackReader reader (new CountingSource (1000), thread, kSecond);
                expect (reader.isBuffered (5000, 512));

                PlaybackReader failed (new CountingSource (kSecond, true), thread, kSecond);
                for (int i = 0; i < 2000 && ! failed.isBuffered (0, 512); ++i) juce::Thread::sleep (1);
                juce::AudioBuffer<float> out (1, 512);
                expect (! failed.read (&out, 0, 512, 0, true, true));
            }

            beginTest ("PlaybackReader: a block is never replaced under a reader");
            {
                juce::TimeSliceThread thread ("test reader");
                thread.startThread();
                PlaybackReader reader (new CountingSource (kSecond * 120), thread, kSecond);
                juce::Random random (0x5eed);
                juce::AudioBuffer<float> out (1, 2048);
                int torn = 0;
                const auto until = juce::Time::getMillisecondCounter() + 700;
                while (juce::Time::getMillisecondCounter() < until)
                {
                    // Jump about so the pool churns under every read.
                    const auto at = (juce::int64) random.nextInt (kSecond * 118);
                    out.clear();
                    reader.cue ((juce::int64) random.nextInt (kSecond * 118));
                    reader.read (&out, 0, 2048, at, true, true);
                    for (int i = 0; i < 2048; ++i)
                    {
                        const float v = out.getSample (0, i);
                        if (v != 0.0f && v != positionValue (at + i)) ++torn;
                    }
                }
                expectEquals (torn, 0, "a read returned audio from another position");
            }

            beginTest ("A loop pass loses no sample on any track, and all of them wrap together");
            {
                TempFolder f;
                for (int t = 1; t <= 3; ++t)
                    writeTrack (f.dir.getChildFile ("Audio Files/Track_0" + juce::String (t) + ".wav"),
                                0.25f * (float) t, kSecond * 8);
                SessionPlayer player;
                JumpChecker check (*this, player, 3);
                expectEquals (player.loadSession (f.dir), 3);
                player.setLoopRegion (kSecond / 2, kSecond * 4);
                player.setLoopEnabled (true);
                player.setPositionSamples (kSecond * 3);
                player.start();
                check.run (500);    // 5.3 s of audio: at least one full pass and two wraps
                player.stop();
                expect (check.playedSamples > (juce::int64) kSecond * 4, "the session did not play");
                expectEquals (check.faults, 0, "a sample passed without audio on some track");
            }

            beginTest ("A locate brings every track in on the same sample");
            {
                TempFolder f;
                for (int t = 1; t <= 3; ++t)
                    writeTrack (f.dir.getChildFile ("Audio Files/Track_0" + juce::String (t) + ".wav"),
                                0.25f * (float) t, kSecond * 30);
                SessionPlayer player;
                JumpChecker check (*this, player, 3);
                expectEquals (player.loadSession (f.dir), 3);
                player.start();
                for (const auto target : { kSecond * 20, kSecond * 5, kSecond * 28, kSecond * 12 })
                {
                    player.setPositionSamples (target);
                    check.run (30);
                    expect (player.getPositionSamples() > target, "the jump never played");
                }
                player.stop();
                expectEquals (check.faults, 0, "a track came in late after a locate");
            }

            beginTest ("PLAY from a far position brings every track in together");
            {
                TempFolder f;
                for (int t = 1; t <= 3; ++t)
                    writeTrack (f.dir.getChildFile ("Audio Files/Track_0" + juce::String (t) + ".wav"),
                                0.25f * (float) t, kSecond * 30);
                SessionPlayer player;
                JumpChecker check (*this, player, 3);
                expectEquals (player.loadSession (f.dir), 3);
                player.setPositionSamples (kSecond * 25);
                player.start();
                check.run (40);
                player.stop();
                expect (check.playedSamples > 0);
                expectEquals (check.faults, 0, "a track came in late on PLAY");
            }

            beginTest ("The next clip's far part of a file is in RAM before the playhead reaches it");
            {
                TempFolder f;
                writeTrack (f.dir.getChildFile ("Audio Files/Track_01.wav"), 0.5f, kSecond * 20);
                SessionPlayer player;
                JumpChecker check (*this, player, 1);
                expectEquals (player.loadSession (f.dir), 1);
                std::vector<Clip> clips (2);
                clips[0].timelineStartSamples = 0;
                clips[0].fileStartSamples     = 0;
                clips[0].fileLengthSamples    = kSecond * 3;
                clips[1].timelineStartSamples = kSecond * 3;
                clips[1].fileStartSamples     = kSecond * 15;
                clips[1].fileLengthSamples    = kSecond * 3;
                player.setTrackClips (0, clips);
                player.setPositionSamples (kSecond);
                player.start();
                check.run (280);    // past the edit at 3 s
                player.stop();
                expect (player.getPositionSamples() > kSecond * 3, "the edit was not reached");
                expectEquals (check.faults, 0, "audio was lost at an edit");
            }

            beginTest ("Overlapping clips from far-apart parts of one file both sound through the overlap");
            {
                TempFolder f;
                writeTrack (f.dir.getChildFile ("Audio Files/Track_01.wav"), 0.25f, kSecond * 20);
                SessionPlayer player;
                player.prepare (48000.0, 512);
                expectEquals (player.loadSession (f.dir), 1);
                std::vector<Clip> clips (2);
                clips[0].timelineStartSamples = 0;
                clips[0].fileStartSamples     = 0;
                clips[0].fileLengthSamples    = kSecond * 3;
                clips[1].timelineStartSamples = kSecond;
                clips[1].fileStartSamples     = kSecond * 15;
                clips[1].fileLengthSamples    = kSecond;
                player.setTrackClips (0, clips);
                player.setPositionSamples (kSecond / 2);
                player.start();
                juce::AudioBuffer<float> out (1, 512);
                int wrong = 0, inOverlap = 0;
                while (player.getPositionSamples() < kSecond * 2 - 512 && player.isPlaying())
                {
                    const auto before = player.getPositionSamples();
                    player.processBlock (out.getArrayOfWritePointers(), 1, 512);
                    if (before >= kSecond && before + 512 <= kSecond * 2 && player.getPositionSamples() > before)
                    {
                        ++inOverlap;
                        if (std::abs (out.getSample (0, 256) - 0.5f) > 0.01f) ++wrong;
                    }
                    juce::Thread::sleep (2);
                }
                player.stop();
                expect (inOverlap > 40, "the overlap was not played");
                expectEquals (wrong, 0, "one clip dropped out of the overlap");
            }

            beginTest ("A jump gives up waiting for a source that never delivers");
            {
                TempFolder f;
                writeTrack (f.dir.getChildFile ("Audio Files/Track_01.wav"), 0.5f, kSecond * 30);
                SessionPlayer player;
                player.prepare (1.0, 512);   // the ceiling is two seconds of device samples
                expectEquals (player.loadSession (f.dir), 1);
                player.setPositionSamples (kSecond * 20);
                player.start();
                juce::AudioBuffer<float> out (1, 512);
                for (int b = 0; b < 4; ++b) player.processBlock (out.getArrayOfWritePointers(), 1, 512);
                player.stop();
                expect (player.getPositionSamples() > kSecond * 20, "the transport hung on the jump");
            }

            beginTest ("A punch never holds the playhead: the capture clock is already running");
            {
                TempFolder f;
                writeTrack (f.dir.getChildFile ("Audio Files/Track_01.wav"), 0.5f, kSecond * 30);
                SessionPlayer player;
                player.prepare (48000.0, 512);
                expectEquals (player.loadSession (f.dir), 1);
                player.setPositionSamples (kSecond * 20);
                player.setSeekLocked (true);
                player.start();
                juce::AudioBuffer<float> out (1, 512);
                player.processBlock (out.getArrayOfWritePointers(), 1, 512);
                expectEquals (player.getPositionSamples(), (juce::int64) (kSecond * 20 + 512));
                player.stop();
                player.setSeekLocked (false);
            }
        }
    };

    static PlaybackJumpTests playbackJumpTests;
}
