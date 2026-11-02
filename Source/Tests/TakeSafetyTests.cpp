// What a punch may write and what a read may claim: the two ways an edit or a
// render could destroy or silently lose audio. Each runs against real files.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../Capture/CaptureDaemon.h"
#include "../UI/MainComponent.h"
#include "../UI/RecordPosition.h"
#include "SilentAudioDevice.h"

#include <CoreFoundation/CoreFoundation.h>
#include <functional>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = testaudio::kSilentBlock;

        juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-safety-" + tag + "-" + juce::Uuid().toString());
        }

        float levelOf (int track) { return 0.1f * (float) (track + 1); }

        void recordTake (const juce::File& dir, int tracks, int blocks, bool stereoFirst = false)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            if (stereoFirst) rec.getTrack (0).isStereo.store (true);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, levelOf (t));
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        juce::File trackFile (const juce::File& dir, int track)
        {
            return dir.getChildFile ("Audio Files")
                      .getChildFile (juce::String::formatted ("Track_%02d.wav", track + 1));
        }

        void feedAll (MultitrackRecorder& rec, float value, int blocks, juce::int64 playhead = -1)
        {
            const int tracks = rec.getNumTracks();
            std::vector<float> audio ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) tracks, audio.data());
            for (int b = 0; b < blocks; ++b)
                rec.processBlock (inputs.data(), tracks, kBlock,
                                  playhead >= 0 ? playhead + (juce::int64) b * kBlock : -1);
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        juce::String fingerprint (const juce::File& dir)
        {
            juce::String all;
            auto files = dir.getChildFile ("Audio Files").findChildFiles (juce::File::findFiles, false);
            files.sort();
            for (const auto& f : files)
                all << f.getFileName() << ":" << juce::MD5 (f).toHexString() << "\n";
            return all;
        }

        // Every file `before` listed is still there, byte for byte. New parts may have joined them.
        bool keepsEveryFile (const juce::String& before, const juce::String& after)
        {
            for (const auto& line : juce::StringArray::fromLines (before))
                if (! after.contains (line)) return false;
            return true;
        }
    }

    class PunchRuleTests final : public juce::UnitTest
    {
    public:
        PunchRuleTests() : juce::UnitTest ("Punch only where the timeline is the file", "zynforge") {}

        struct Session
        {
            MainComponent& main;
            juce::File dir = tempDir ("punch");

            Session (MainComponent& window, int tracks = 2, int blocks = 200, bool stereoFirst = false)
                : main (window)
            {
                recordTake (dir, tracks, blocks, stereoFirst);
                main.openSessionFolder (dir, true);
                main.engine.prepareForTests (kRate, kBlock);
                arm ({ 0 });
            }

            ~Session()
            {
                if (main.engine.isRecording()) main.stopActiveCaptureAsync (false);
                settled();
                main.punchSessionActive = false;
                main.manualPunchActive = false;
                main.punchWindowTakeActive = false;
                main.stopArmedAtMs = 0;
                main.engine.stopPlayback();
                main.engine.setPunchModeOn (false);
                main.engine.clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                main.engine.setStripCount (0);
                main.engine.clearAllStripOverrides();
                main.lastTrackCount = -1;
                dir.deleteRecursively();
            }

            bool settled()
            {
                return pumpUntil ([this]
                {
                    return ! main.engine.isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
            }

            void arm (std::initializer_list<int> tracks)
            {
                auto& rec = main.engine.getRecorder();
                for (int t = 0; t < rec.getNumTracks(); ++t) main.engine.setTrackArmed (t, false);
                for (int t : tracks) main.engine.setTrackArmed (t, true);
            }

            AudioEngine& engine() { return main.engine; }
            SessionPlayer& player() { return main.engine.getPlayer(); }
        };

        static constexpr juce::int64 kTake = 200 * kBlock;
        static constexpr juce::int64 kCutStart = 40 * kBlock, kCutEnd = 80 * kBlock;

        void expectRefusedNaming (const juce::String& text, const juce::String& strip)
        {
            expect (text.contains ("Punch refused"), "no refusal: " + text);
            expect (text.contains (strip), "the refusal names no strip: " + text);
        }

        void theRule (MainComponent& main)
        {
            beginTest ("An unedited strip punches anywhere, bounded or open-ended");
            {
                Session s (main);
                expect (s.engine().punchRefusal (1000, 2000).isEmpty());
                expect (s.engine().punchRefusal (1000, -1).isEmpty());
                expect (s.engine().punchRefusal (kTake + 4000, -1).isEmpty(), "past the end needs nothing");
            }

            beginTest ("A split that moves nothing does not stop a punch");
            {
                Session s (main);
                expect (s.engine().splitTrackAtSample (0, 12000));
                expect (s.engine().punchRefusal (1000, -1).isEmpty(), s.engine().punchRefusal (1000, -1));
                expect (s.engine().punchRefusal (10000, 14000).isEmpty());
            }

            beginTest ("A trim that keeps its position still punches inside the clip");
            {
                Session s (main);
                expect (s.engine().editClip (0, 0, AudioEngine::ClipEdit::TrimLeft, 4000));
                expect (s.engine().punchRefusal (6000, 8000).isEmpty(), s.engine().punchRefusal (6000, 8000));
                expectRefusedNaming (s.engine().punchRefusal (1000, 2000),
                                     s.engine().getRecorder().getTrack (0).getNameThreadSafe());
            }

            beginTest ("Ripple: what moved refuses, what stayed still punches when bounded");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().rippleDeleteRange (0, kCutStart, kCutEnd));
                expectRefusedNaming (s.engine().punchRefusal (kCutStart + 100, kCutStart + 200), name);
                expect (s.engine().punchRefusal (100, 200).isEmpty(), "audio before the cut did not move");
                expectRefusedNaming (s.engine().punchRefusal (100, -1), name);
            }

            beginTest ("A moved clip refuses; a strip that is not armed is no concern");
            {
                Session s (main);
                expect (s.engine().editClip (1, 0, AudioEngine::ClipEdit::Move, 4800));
                expect (s.engine().punchRefusal (10000, 11000).isEmpty(), "an unarmed strip stopped a punch");
                s.arm ({ 0, 1 });
                expectRefusedNaming (s.engine().punchRefusal (10000, 11000),
                                     s.engine().getRecorder().getTrack (1).getNameThreadSafe());
            }

            beginTest ("Crop refuses inside the kept range and past it");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().cropToRange (kCutStart, kCutEnd) > 0);
                expectRefusedNaming (s.engine().punchRefusal (100, 200), name);
                expectRefusedNaming (s.engine().punchRefusal (kCutEnd + 100, kCutEnd + 200), name);
            }

            beginTest ("A cleared range refuses; either side of it does not");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().clearTrackRange (0, kCutStart, kCutEnd));
                expectRefusedNaming (s.engine().punchRefusal (kCutStart + 10, kCutStart + 20), name);
                expectRefusedNaming (s.engine().punchRefusal (kCutStart - 10, kCutStart + 10), name);
                expect (s.engine().punchRefusal (100, 200).isEmpty());
                expect (s.engine().punchRefusal (kCutEnd + 10, kCutEnd + 20).isEmpty());
            }

            beginTest ("A strip emptied by hand refuses, though nothing plays there");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().clearTrackRange (0, 0, kTake));
                expectRefusedNaming (s.engine().punchRefusal (100, 200), name);
            }

            beginTest ("An end trim leaves the take's audio past it, so that refuses");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().editClip (0, 0, AudioEngine::ClipEdit::TrimRight, -24000));
                expectRefusedNaming (s.engine().punchRefusal (kTake - 1000, kTake - 500), name);
            }

            beginTest ("An edited strip with no clip past its end refuses; a plain early end does not");
            {
                Session s (main, 2);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().punchRefusal (kTake + 1000, kTake + 2000).isEmpty(), "plain strip ended early");
                expect (s.engine().splitTrackAtSample (0, 12000));
                expectRefusedNaming (s.engine().punchRefusal (kTake + 1000, kTake + 2000), name);
                expect (s.engine().punchRefusal (kTake - 1000, -1).isEmpty(),
                        "an open punch that begins on a clip may run off the end");
            }

            beginTest ("A strip with no take yet records as it always did");
            {
                Session s (main);
                s.engine().setStripCount (3);
                s.arm ({ 2 });
                expect (s.engine().punchRefusal (1000, 2000).isEmpty(), s.engine().punchRefusal (1000, 2000));
            }

            beginTest ("A consolidated clip plays another file, so it refuses");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().consolidateRange (0, kCutStart, kCutEnd));
                expectRefusedNaming (s.engine().punchRefusal (kCutStart + 10, kCutStart + 20), name);
                expect (s.engine().punchRefusal (100, 200).isEmpty());
            }

            beginTest ("A duplicate elsewhere shares the file audio, so punching its source refuses");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().pasteClip (0, kTake + 4000, kCutStart, 4000, 0, 0, 0.0f, "copy") >= 0);
                expectRefusedNaming (s.engine().punchRefusal (kCutStart + 10, kCutStart + 20), name);
                expect (s.engine().punchRefusal (kCutEnd, kCutEnd + 100).isEmpty());
            }

            beginTest ("A clip pasted to another strip shares the file audio too");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().pasteClip (1, 0, kCutStart, 4000, 0, 0, 0.0f, "pasted",
                                              s.engine().getTrackAudioFile (0), 0) >= 0);
                expectRefusedNaming (s.engine().punchRefusal (kCutStart + 10, kCutStart + 20), name);
                expect (s.engine().punchRefusal (kCutEnd, kCutEnd + 100).isEmpty());
            }

            beginTest ("A clip on the armed strip that plays another strip's file refuses");
            {
                Session s (main);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().pasteClip (0, 1000, 1000, 4000, 0, 0, 0.0f, "other",
                                              s.engine().getTrackAudioFile (1), 0) >= 0);
                expectRefusedNaming (s.engine().punchRefusal (1500, 1600), name);
            }

            beginTest ("A clip reading the other channel of its own file is not the file's position");
            {
                Session s (main, 2, 200, true);
                const auto take = s.engine().getTrackAudioFile (0);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                expect (s.engine().pasteClip (0, kTake + 100, kTake + 100, 100, 0, 0, 0.0f, "swapped", take, 1) >= 0);
                expectRefusedNaming (s.engine().punchRefusal (kTake + 150, kTake + 160), name);
            }

            beginTest ("The right half of a stereo strip answers for the one file");
            {
                Session s (main, 2, 200, true);
                s.engine().setTrackStereo (0, true);
                s.arm ({ 0 });
                expect (s.engine().editClip (1, 0, AudioEngine::ClipEdit::Move, 4800));
                expectRefusedNaming (s.engine().punchRefusal (1000, 2000),
                                     s.engine().getRecorder().getTrack (1).getNameThreadSafe());
            }
        }

        void edited (Session& s)
        {
            expect (s.engine().rippleDeleteRange (0, kCutStart, kCutEnd));
            s.player().rewind();
        }

        void everyStart (MainComponent& main)
        {
            beginTest ("A stopped cursor punch offers only the safe answers, and they work");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.engine().setEditCursorSample (kCutStart + 100);
                main.onRecordClicked();
                expect (main.cursorPunchQuestion != nullptr, "RECORD gave no way out");
                expect (! s.engine().isRecording(), "a refused cursor started a take");
                if (auto* question = main.cursorPunchQuestion.getComponent())
                {
                    expectEquals (question->getNumButtons(), 2, "Replace was offered where a punch is refused");
                    question->exitModalState (0);
                    pumpUntil ([&main] { return main.cursorPunchQuestion == nullptr; });
                }
                expect (fingerprint (s.dir) == before);

                main.answerCursorPunch (1);
                expect (s.engine().isRecording(), main.statusLabel.getText());
                expect (! main.manualPunchActive, "the safe answer punched");
                feedAll (s.engine().getRecorder(), -0.3f, 4);
                main.stopActiveCaptureAsync (false);
                expect (s.settled());
                expect (keepsEveryFile (before, fingerprint (s.dir)), "appending touched the take");
            }

            beginTest ("An answered cursor press cannot get past the rule");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.engine().setEditCursorSample (kCutStart + 100);
                const juce::ScopedValueSetter<bool> answered (main.cursorPunchAnswered, true);
                main.onRecordClicked();
                expect (! s.engine().isRecording(), "a stale answer punched");
                expect (! s.engine().getRecorder().isPunchInArmed(), "the arm survived");
                expect (main.recordRefusal.contains ("Punch refused"), main.recordRefusal);
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("A rolling punch is refused and playback carries on");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.player().setPositionSamples (kCutStart + 100);
                s.engine().startPlayback();
                main.onRecordClicked();
                expect (! s.engine().isRecording(), "a rolling RECORD punched");
                expect (s.player().isPlaying(), "a refusal stopped playback");
                expect (main.recordRefusal.contains ("Punch refused"), main.recordRefusal);
                s.engine().stopPlayback();
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("A selection punch is refused before anything moves");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.player().selectRange (kCutStart + 100, kCutStart + 4000);
                s.player().setPositionSamples (1234);
                main.onRecordClicked();
                expect (! s.engine().isRecording(), "a selection punched");
                expect (! main.punchSessionActive);
                expect (! s.engine().isPunchModeOn(), "a refusal left PUNCH mode on");
                expect (s.player().isRangeArmed(), "a refusal used up the selection");
                expect (! s.engine().isCaptureWindowActive());
                expectEquals (s.player().getPositionSamples(), (juce::int64) 1234, "a refusal moved the playhead");
                expect (main.recordRefusal.contains ("Punch refused"), main.recordRefusal);
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("PUNCH mode is refused at punch-in, says why, and tries once");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                const auto in = kCutStart + 100, out = kCutStart + 8000;
                main.togglePunchMode();
                s.player().selectRange (in, out);
                s.player().setPositionSamples (in + 8);
                s.engine().startPlayback();
                main.servicePunch();
                expect (! s.engine().isRecording(), "PUNCH mode punched");
                expect (! s.engine().isCaptureWindowActive(), "a refused punch left its window");
                expect (! s.engine().getRecorder().isPunchInArmed(), "a refused punch left its arm");
                expect (main.recordRefusal.contains ("Punch refused")
                        || main.statusLabel.getText().contains ("Punch refused"),
                        main.statusLabel.getText());
                s.engine().stopPlayback();
                main.togglePunchMode();
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("The engine's own door refuses whoever arms the punch, and names the strip");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                const auto name = s.engine().getRecorder().getTrack (0).getNameThreadSafe();
                s.engine().setCaptureWindow (kCutStart + 100, kCutStart + 200);
                s.engine().armPunchIn (kCutStart + 100);
                expect (! s.engine().startRecording (s.dir));
                expect (! s.engine().isRecording());
                expect (! s.engine().getRecorder().isPunchInArmed(), "the arm survived a refusal");
                expectEquals (s.engine().getRecorder().getRecordBaseSamples(), (juce::int64) 0);
                expectRefusedNaming (s.engine().getRecorder().getStartError(), name);
                s.engine().clearCaptureWindow();

                s.engine().armPunchIn (kCutStart + 100);
                expect (! s.engine().startRecording (s.dir), "an open-ended punch got through");
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("A remote RECORD that would punch an edited strip is refused");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.engine().setEditCursorSample (kCutStart + 100);
                juce::String error;
                expect (! s.engine().performRemoteTransport (AudioEngine::RemoteTransportAction::StartRecord, error));
                expect (! s.engine().isRecording());
                expect (error.containsIgnoreCase ("punch"), error);
                expect (main.cursorPunchQuestion == nullptr, "a remote press asked a question on the host");
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("A twin RECORD that would punch an edited strip is refused");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                s.player().selectRange (kCutStart + 100, kCutStart + 4000);
                juce::String refusal;
                expect (! main.twinStartRecord (refusal));
                expect (! s.engine().isRecording());
                expect (refusal.containsIgnoreCase ("punch"), refusal);
                expect (fingerprint (s.dir) == before);
            }

            beginTest ("An append needs no check: an edited strip continues as before");
            {
                Session s (main);
                edited (s);
                const auto before = fingerprint (s.dir);
                main.onRecordClicked();
                expect (s.engine().isRecording(), main.statusLabel.getText());
                expect (! main.manualPunchActive);
                feedAll (s.engine().getRecorder(), -0.3f, 4);
                main.stopActiveCaptureAsync (false);
                expect (s.settled());
                expect (keepsEveryFile (before, fingerprint (s.dir)), "appending touched the take");
            }
        }

        void theDaemon()
        {
            beginTest ("The daemon never punches: RECORD can only append, and PUNCH mode says no");

            capture::CaptureDaemon daemon;
            daemon.setTestModeNoDevice (true);
            int port = 0;
            juce::Random random;
            for (int attempt = 0; attempt < 20 && port == 0; ++attempt)
            {
                const int candidate = 50000 + random.nextInt (9000);
                if (daemon.start (candidate, 1)) port = candidate;
            }
            expect (port != 0);
            if (port == 0) return;
            daemon.prepareForTests (kRate, kBlock, 1);

            auto session = tempDir ("daemon");
            recordTake (session, 1, 200);
            const auto before = fingerprint (session);
            {
                MainComponent main;
                testaudio::useSilentDevice (main.engine);
                main.setSize (1320, 820);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
                main.engine.setTrackArmed (0, true);
                expect (main.engine.rippleDeleteRange (0, kCutStart, kCutEnd));
                main.useCaptureDaemon = true;
                main.captureDaemonPort = port;
                main.connectCaptureDaemon (false);
                expect (main.daemonModeActive());

                main.engine.setEditCursorSample (kCutStart + 100);
                main.engine.getPlayer().selectRange (kCutStart + 100, kCutStart + 4000);
                MainComponent::RecordStartPlan plan;
                expect (main.prepareRecordStart (plan), main.recordRefusal);
                expect (! main.recordPlanPunches (plan), "a daemon plan would punch");
                expectEquals (plan.punchAt, (juce::int64) -1);

                main.togglePunchMode();
                expect (! main.engine.isPunchModeOn(), "PUNCH mode went on under the daemon");

                // A mode already on when the daemon attached still cannot open a punch.
                main.engine.setPunchModeOn (true);
                main.engine.getPlayer().setPositionSamples (kCutStart + 200);
                main.engine.startPlayback();
                main.servicePunch();
                expect (! main.engine.getRecorder().isPunchInArmed());
                expect (! main.engine.isRecording(), "the daemon path opened a local punch");
                main.engine.stopPlayback();
                main.engine.setPunchModeOn (false);

                // Straight into the daemon, into the very folder: its own
                // start can only continue, so the take is left as it was.
                juce::BigInteger arms;
                arms.setBit (0);
                expect (main.captureSupervisor.startRecording (session, 1, (int) CaptureFormat::Wav24, arms));
                std::vector<float> samples ((size_t) kBlock, -0.3f);
                const float* inputs[] { samples.data() };
                for (int b = 0; b < 20; ++b) daemon.processTestBlock (inputs, 1, kBlock);
                expect (! daemon.getRecorder().isPunchInArmed(), "a daemon start armed a punch");
                daemon.getRecorder().stopRecording();

                main.useCaptureDaemon = false;
                main.captureSupervisor.disconnect();
                main.engine.clearSessionState();
                main.engine.clearAllStripOverrides();
            }
            daemon.stop();

            expect (keepsEveryFile (before, fingerprint (session)), "the daemon changed the take");
            expect (session.getChildFile ("Audio Files/Track_01_part02.wav").existsAsFile(),
                    "the daemon's start did not continue the take");
            session.deleteRecursively();
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            {
                MainComponent main;
                testaudio::useSilentDevice (main.engine);
                main.setSize (1320, 820);
                theRule (main);
                everyStart (main);
            }
            theDaemon();
        }
    };

    static PunchRuleTests punchRuleTests;
}
