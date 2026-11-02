// What the app believes about things it reaches over a wire: the soundcheck
// patch of the strips and of the desk, and what a remote may do to the
// transport. Most of it comes down to one rule: the state on screen follows
// what the desk or the engine actually did, not what was asked of it.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/PreflightDialog.h"
#include "../Audio/VirtualSoundcheck.h"

#include <CoreFoundation/CoreFoundation.h>
#include <functional>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        juce::File scratchDir()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-netstate-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        void recordBaseTake (const juce::File& dir, int tracks, int blocks)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, 0.1f);
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }
    }

    class NetworkStateTests final : public juce::UnitTest
    {
    public:
        NetworkStateTests() : juce::UnitTest ("Network state", "zynforge") {}

        struct Bench
        {
            MainComponent& main;
            juce::File session { scratchDir() };

            explicit Bench (MainComponent& m, int tracks = 3) : main (m)
            {
                recordBaseTake (session, tracks, 200);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Bench()
            {
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                pumpUntil ([this] { return ! engine().isRecording() && ! main.captureMetadataPending
                                           && main.pendingMetadataSaves == 0; });
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.lastTrackCount = -1;
                session.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }
            TrackState& track (int i) { return recorder().getTrack (i); }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            MainComponent main;
            main.setSize (1320, 820);

            cuesKeepTheShowPatch (main);
        }

        // ── Cues and Virtual Soundcheck ────────────────────────────────────

        void cuesKeepTheShowPatch (MainComponent& main)
        {
            beginTest ("A cue recalled during VSC keeps the strips on their input channels");
            {
                Bench b (main);
                for (int t = 0; t < 3; ++t)
                {
                    b.engine().setTrackInputRouting (t, t + 3);
                    b.engine().setTrackOutputRouting (t, -1);
                }
                main.currentCueIndex = -1;
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                vsc::repatch (b.engine());
                for (int t = 0; t < 3; ++t) expectEquals (b.track (t).outputRouting.load(), t + 3);

                main.jumpToCue (0);
                for (int t = 0; t < 3; ++t)
                    expectEquals (b.track (t).outputRouting.load(), t + 3,
                                  "a recall threw the strip off its input channel with VSC still lit");
                expect (vsc::hasSavedPatch (b.engine()));

                vsc::restore (b.engine());
                for (int t = 0; t < 3; ++t)
                    expectEquals (b.track (t).outputRouting.load(), -1, "Restore did not give the cue's patch");
            }

            beginTest ("A cue recalled during VSC replaces what Restore puts back");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                b.engine().setTrackOutputRouting (0, 5);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                b.engine().setTrackOutputRouting (0, 6);
                vsc::repatch (b.engine());
                expectEquals (b.track (0).outputBeforeVsc.load(), 6);
                main.jumpToCue (0);
                expectEquals (b.track (0).outputBeforeVsc.load(), 5);
                vsc::restore (b.engine());
                expectEquals (b.track (0).outputRouting.load(), 5);
            }

            beginTest ("A cue taken during VSC stores the show patch, so a later recall is clean");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                b.engine().setTrackOutputRouting (0, 5);
                vsc::repatch (b.engine());

                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();
                expectEquals (main.cues[0].strips[0].outputRouting, 5, "the soundcheck patch was kept as the show's");

                vsc::restore (b.engine());
                b.engine().setTrackOutputRouting (0, 7);
                main.jumpToCue (0);
                expectEquals (b.track (0).outputRouting.load(), 5);
                expect (! vsc::hasSavedPatch (b.engine()));
            }

            beginTest ("During VSC a strip follows a cue's input, so outputs still match inputs");
            {
                Bench b (main);
                b.engine().setTrackInputRouting (0, 3);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                b.engine().setTrackInputRouting (0, 6);
                vsc::repatch (b.engine());
                expectEquals (b.track (0).outputRouting.load(), 6);
                main.jumpToCue (0);
                expectEquals (b.track (0).inputRouting.load(), 3);
                expectEquals (b.track (0).outputRouting.load(), 3, "VSC reads on, but the output left the input");
            }

            beginTest ("A strip added after the soundcheck takes a cue as usual");
            {
                Bench b (main);
                b.engine().setTrackOutputRouting (0, 5);
                b.engine().setTrackOutputRouting (1, 6);
                main.cues.clear();
                main.cues.push_back ({});
                main.currentCueIndex = 0;
                main.updateCueAtTransport();

                vsc::repatch (b.engine());
                b.track (1).outputBeforeVsc.store (TrackState::kNoSavedOutput);
                b.engine().setTrackOutputRouting (1, 2);
                main.jumpToCue (0);
                expectEquals (b.track (1).outputRouting.load(), 6);
            }
        }
    };

    static NetworkStateTests networkStateTests;
}
