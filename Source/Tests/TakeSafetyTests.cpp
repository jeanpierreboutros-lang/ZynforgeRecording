// What a punch may write and what a read may claim: the two ways an edit or a
// render could destroy or silently lose audio. Each runs against real files.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/AudioImport.h"
#include "../Audio/CheckedAudioReader.h"
#include "../Audio/FlacRepair.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/PunchSplice.h"
#include "../Audio/QcAnalyzer.h"
#include "../Audio/SongDetector.h"
#include "../Audio/SongExport.h"
#include "../Audio/TrackExporter.h"
#include "../Capture/CaptureDaemon.h"
#include "../UI/MainComponent.h"
#include "../UI/MixerSnapshot.h"
#include "../UI/RecordPosition.h"
#include "SilentAudioDevice.h"

#include <CoreFoundation/CoreFoundation.h>
#include <functional>
#include <unistd.h>

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

            beginTest ("A clip on another strip playing the audio right there is no reason to refuse");
            {
                Session s (main);
                expect (s.engine().pasteClip (1, kCutStart, kCutStart, 4000, 0, 0, 0.0f, "in place",
                                              s.engine().getTrackAudioFile (0), 0) >= 0);
                expect (s.engine().punchRefusal (kCutStart + 10, kCutStart + 20).isEmpty(),
                        s.engine().punchRefusal (kCutStart + 10, kCutStart + 20));
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

    // Every test here fails through a real read(2) on a real file: the drive is
    // played by cutting the file short with truncate(2), so the bytes genuinely
    // are not there. A reader that merely returned false would prove nothing,
    // because the readers JUCE ships never do.
    class FailedReadTests final : public juce::UnitTest
    {
    public:
        FailedReadTests() : juce::UnitTest ("A failed read is a failure", "zynforge") {}

        static constexpr int kSamples = 1 << 18;   // about 5.5 s

        struct Folder
        {
            juce::File dir = tempDir ("read");
            Folder() { dir.getChildFile ("Audio Files").createDirectory(); }
            ~Folder() { dir.deleteRecursively(); }
            juce::File audio (const juce::String& name) const
            { return dir.getChildFile ("Audio Files").getChildFile (name); }
        };

        // Noise, so FLAC cannot shrink it to nothing and silence is obvious.
        static void writeAudio (const juce::File& file, int samples = kSamples, int channels = 1, int seed = 1)
        {
            file.getParentDirectory().createDirectory();
            std::unique_ptr<juce::AudioFormat> format;
            const auto extension = file.getFileExtension().toLowerCase();
            if      (extension == ".flac") format = std::make_unique<juce::FlacAudioFormat>();
            else if (extension == ".aif")  format = std::make_unique<juce::AiffAudioFormat>();
            else                           format = std::make_unique<juce::WavAudioFormat>();
            file.deleteFile();
            std::unique_ptr<juce::AudioFormatWriter> writer (format->createWriterFor (
                file.createOutputStream().release(), kRate, (unsigned int) channels, 24, {}, 0));
            juce::Random random (seed);
            juce::AudioBuffer<float> audio (channels, samples);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < samples; ++i)
                    audio.setSample (c, i, 0.1f + 0.4f * random.nextFloat());
            writer->writeFromAudioSampleBuffer (audio, 0, samples);
        }

        // Keeps `fraction` of the file: a drive that stopped answering part way.
        void cutShort (const juce::File& file, double fraction = 0.5)
        {
            const auto keep = (off_t) ((double) file.getSize() * fraction);
            expectEquals (::truncate (file.getFullPathName().toRawUTF8(), keep), 0, "the fixture did not cut the file");
            expectEquals (file.getSize(), (juce::int64) keep);
        }

        static juce::AudioFormatManager& formats()
        {
            static juce::AudioFormatManager manager;
            if (manager.getNumKnownFormats() == 0) manager.registerBasicFormats();
            return manager;
        }

        static int partials (const juce::File& dir)
        {
            return dir.findChildFiles (juce::File::findFiles, true, "*.partial*").size();
        }

        void readers (Folder& f)
        {
            for (const juce::String extension : { ".wav", ".aif", ".flac" })
            {
                beginTest ("A healthy " + extension + " reads the same through the checked reader, to its last sample");
                {
                    const auto file = f.audio ("healthy" + extension);
                    writeAudio (file);
                    auto checked = ConcatReader::create (formats(), { file });
                    std::unique_ptr<juce::AudioFormatReader> plain (formats().createReaderFor (file));
                    expect (checked != nullptr && plain != nullptr);
                    if (checked == nullptr || plain == nullptr) continue;
                    expectEquals (checked->lengthInSamples, plain->lengthInSamples);
                    expectEquals ((int) checked->bitsPerSample, (int) plain->bitsPerSample);
                    juce::AudioBuffer<float> a (1, kSamples), b (1, kSamples);
                    expect (checked->read (&a, 0, kSamples, 0, true, false));
                    expect (plain->read (&b, 0, kSamples, 0, true, false));
                    bool same = true;
                    for (int i = 0; i < kSamples; ++i) same = same && a.getSample (0, i) == b.getSample (0, i);
                    expect (same, "the checked reader changed the audio");
                    expect (checked->read (&a, 0, 4096, 2048, true, false), "a read back into the file failed");
                }

                beginTest ("A " + extension + " cut short while it is open fails, and stays failed");
                {
                    const auto file = f.audio ("cut" + extension);
                    writeAudio (file);
                    auto reader = ConcatReader::create (formats(), { file });
                    expect (reader != nullptr);
                    if (reader == nullptr) continue;
                    juce::AudioBuffer<float> audio (1, 4096);
                    expect (reader->read (&audio, 0, 4096, 0, true, false), "the head should read");
                    cutShort (file);
                    bool failed = false;
                    for (juce::int64 at = 4096; at + 4096 <= reader->lengthInSamples && ! failed; at += 4096)
                        failed = ! reader->read (&audio, 0, 4096, at, true, false);
                    expect (failed, "reading past a missing part of the file reported success");
                    expect (audio.getMagnitude (0, 4096) == 0.0f, "a failed read handed back something");
                    expect (! reader->read (&audio, 0, 4096, 0, true, false), "a failed reader recovered");
                }
            }

            for (const juce::String extension : { ".wav", ".aif" })
            {
                beginTest ("A " + extension + " whose header promises more than the file holds fails at that point");
                {
                    const auto file = f.audio ("promise" + extension);
                    writeAudio (file);
                    cutShort (file);
                    auto reader = ConcatReader::create (formats(), { file });
                    expect (reader != nullptr);
                    if (reader == nullptr) continue;
                    expectEquals (reader->lengthInSamples, (juce::int64) kSamples);
                    juce::AudioBuffer<float> audio (1, 4096);
                    expect (reader->read (&audio, 0, 4096, 0, true, false), "the part that exists should read");
                    expect (! reader->read (&audio, 0, 4096, kSamples - 8192, true, false),
                            "bytes that are not there read as silence");
                }
            }

            beginTest ("A .flac cut short before it was opened fails where its frames run out");
            {
                const auto file = f.audio ("promise.flac");
                writeAudio (file);
                cutShort (file);
                const auto frames = flacrepair::wholeFramesEnd (file);
                expect (frames > 0 && frames < kSamples, "the frames were not counted");
                auto reader = ConcatReader::create (formats(), { file });
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> audio (1, 4096);
                    expect (reader->read (&audio, 0, 4096, 0, true, false), "the frames that exist should read");
                    expect (! reader->read (&audio, 0, 4096, kSamples - 8192, true, false),
                            "a decoder that ran dry read as silence");
                }
            }

            beginTest ("A second part cut short fails a multi-part take");
            {
                const auto main = f.audio ("Track_09.wav"), part = f.audio ("Track_09_part02.wav");
                writeAudio (main, 8192);
                writeAudio (part);
                cutShort (part);
                auto reader = ConcatReader::create (formats(), findTakeParts (main));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> audio (1, 4096);
                    expect (reader->read (&audio, 0, 4096, 0, true, false));
                    expect (! reader->read (&audio, 0, 4096, reader->lengthInSamples - 4096, true, false));
                }
            }

            beginTest ("An error from the operating system fails the read too");
            {
                // read(2) on a directory fails with EISDIR, the nearest thing to
                // EIO that needs no drive to be pulled.
                auto stream = CheckedInputStream::open (f.dir);
                expect (stream != nullptr);
                if (stream != nullptr)
                {
                    stream->watchAudio (false);
                    char bytes[16] {};
                    expectEquals (stream->read (bytes, sizeof (bytes)), 0);
                    expect (stream->hasFailed(), "a read error went unnoticed");
                    expect (stream->isExhausted(), "a stream that failed must end, or a decoder spins on it");
                }
            }

            beginTest ("A seek past 2 GiB arrives through an int, and still lands");
            {
                // A sparse file: nothing near this size is really written.
                const auto file = f.audio ("sparse.bin");
                constexpr juce::int64 kPlace = 2500000000LL;
                {
                    juce::FileOutputStream out (file);
                    expect (out.openedOk() && out.setPosition (kPlace) && out.writeByte (42));
                }
                auto stream = CheckedInputStream::open (file);
                expect (stream != nullptr);
                if (stream != nullptr)
                {
                    stream->watchAudio (false);
                    expect (stream->setPosition ((int) kPlace), "a seek past 2 GiB failed");
                    expectEquals (stream->getPosition(), kPlace);
                    char byte = 0;
                    expectEquals (stream->read (&byte, 1), 1);
                    expectEquals ((int) byte, 42);
                    expect (! stream->hasFailed());
                }
            }

            beginTest ("Before the audio starts a short read is the format reader's own business");
            {
                const auto file = f.audio ("header.wav");
                expect (file.replaceWithText ("xx"));
                expect (openCheckedReader (formats(), file) == nullptr);
            }

            beginTest ("Playback and drawing still get silence rather than a stop");
            {
                const auto file = f.audio ("playback.wav");
                writeAudio (file);
                cutShort (file);
                auto reader = ConcatReader::createUnchecked (formats(), { file });
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    juce::AudioBuffer<float> audio (1, 4096);
                    expect (reader->read (&audio, 0, 4096, kSamples - 8192, true, false));
                }
            }
        }

        void exports (Folder& f)
        {
            for (bool pair : { false, true })
                for (double rate : { 48000.0, 44100.0 })
                {
                    beginTest (juce::String (pair ? "Stereo pair" : "Track") + " export to " + juce::String (rate, 0)
                               + " fails on a cut source and leaves nothing behind");
                    const auto left = f.audio ("exportL.wav"), right = f.audio ("exportR.wav");
                    writeAudio (left);
                    writeAudio (right, kSamples, 1, 2);
                    cutShort (pair ? right : left);
                    const auto out = f.dir.getChildFile ("Exports");
                    out.createDirectory();
                    const auto stem = out.getChildFile ("result");
                    TrackExporter exporter;
                    ExportOptions options;
                    options.sampleRate = rate;
                    juce::String error;
                    const bool ok = pair ? exporter.exportStereoPair (left, right, stem, options, error)
                                         : exporter.exportTrack (left, stem, options, error);
                    expect (! ok, "an export of a damaged take reported success");
                    expect (error.isNotEmpty());
                    expect (! stem.withFileExtension (".wav").existsAsFile(), "a damaged export was installed");
                    expectEquals (partials (out), 0);
                }

            beginTest ("Export of a healthy take still succeeds and is the whole take");
            {
                const auto source = f.audio ("exportOk.wav");
                writeAudio (source);
                const auto stem = f.dir.getChildFile ("Exports").getChildFile ("ok");
                TrackExporter exporter;
                ExportOptions options;
                juce::String error;
                expect (exporter.exportTrack (source, stem, options, error), error);
                std::unique_ptr<juce::AudioFormatReader> reader (formats().createReaderFor (stem.withFileExtension (".wav")));
                expect (reader != nullptr && reader->lengthInSamples == kSamples);
            }
        }

        void renders()
        {
            const auto session = [this] (AudioEngine& engine, Folder& f, int tracks)
            {
                for (int t = 0; t < tracks; ++t)
                    writeAudio (f.audio (juce::String::formatted ("Track_%02d.wav", t + 1)), kSamples, 1, t + 1);
                engine.clearAllStripOverrides();
                engine.setStripCount (tracks);
                engine.prepareForTests (kRate, kBlock);
                engine.loadSession (f.dir);
            };

            beginTest ("Bounce, stereo mix and pair bounce fail on a cut take and install nothing");
            {
                Folder f;
                AudioEngine engine;
                session (engine, f, 2);
                const auto stem = f.dir.getChildFile ("stem.wav");
                const auto mix  = f.dir.getChildFile ("mix.wav");
                const auto pair = f.dir.getChildFile ("pair.wav");
                expect (engine.bounceStereoMixToWav (mix, kSamples, kRate), "the healthy mix should bounce");
                mix.deleteFile();
                cutShort (f.audio ("Track_01.wav"));
                expect (! engine.bounceTrackArrangementToWav (0, stem, kSamples, kRate, nullptr), "stem");
                expect (! engine.bounceStereoMixToWav (mix, kSamples, kRate), "stereo mix");
                expect (! engine.bounceStereoPairToWav (0, pair, kSamples, kRate, nullptr), "pair");
                expect (! stem.existsAsFile() && ! mix.existsAsFile() && ! pair.existsAsFile());
                expectEquals (partials (f.dir), 0);
            }

            beginTest ("Song export counts a cut take as failed, and no song file claims to be complete");
            {
                Folder f;
                AudioEngine engine;
                session (engine, f, 1);
                cutShort (f.audio ("Track_01.wav"));
                songexport::Choice choice;
                choice.songs = { { 0, kSamples, "Song" } };
                choice.numbers = { 1 };
                choice.format.sampleRate = kRate;
                const std::atomic<bool> cancel { false };
                const auto exports = f.dir.getChildFile ("Export Files");
                const auto outcome = songexport::run (engine, songexport::buildPlan (engine, choice, exports), cancel, nullptr);
                expectEquals (outcome.written, 0, "a damaged song was written");
                expect (outcome.firstError.isNotEmpty());
                expect (! outcome.complete());
                expectEquals (exports.findChildFiles (juce::File::findFiles, true, "*.wav").size(), 0);
            }

            beginTest ("Consolidate of a cut take changes nothing and leaves no render");
            {
                Folder f;
                AudioEngine engine;
                session (engine, f, 1);
                const auto before = juce::JSON::toString (engine.playlistsToJson());
                cutShort (f.audio ("Track_01.wav"));
                expect (! engine.consolidateRange (0, 1024, kSamples - 1024), "a hole was consolidated");
                expect (juce::JSON::toString (engine.playlistsToJson()) == before, "the clips moved anyway");
                expectEquals (f.dir.findChildFiles (juce::File::findFiles, true, "*consolidat*").size(), 0);
            }
        }

        void analysis()
        {
            beginTest ("QC says it could not read a cut take, and never calls it clean");
            {
                Folder f;
                const auto take = f.audio ("Track_01.wav");
                writeAudio (take);
                cutShort (take);
                const auto result = qc::analyzeFile (take);
                expect (result.error.isNotEmpty(), "QC read a hole as quiet audio");
                expect (qc::statusSummary ({ result }, 0).contains ("COULD NOT BE READ"));
                expect (qc::reportText ("Show", { result }).contains ("QC INCOMPLETE"));
            }

            beginTest ("Detect Songs reports a cut take instead of finding no songs");
            {
                Folder f;
                const auto take = f.audio ("Track_01.wav");
                writeAudio (take);
                cutShort (take);
                songs::Voice voice;
                voice.ownFile = take;
                Clip whole;
                whole.fileLengthSamples = kSamples;
                voice.clips.push_back (whole);
                double rate = kRate;
                bool failed = false;
                const songs::Params params;
                const auto flags = songs::scanVoices ({ voice }, params, rate, nullptr, &failed);
                expect (failed, "a failed read looked like a quiet take");
                expect (flags.empty());
            }

            beginTest ("Import of a cut file fails and leaves no track");
            {
                Folder f;
                const auto source = f.dir.getChildFile ("source.wav");
                writeAudio (source);
                cutShort (source);
                const auto dir = f.dir.getChildFile ("Audio Files");
                const auto result = audioimport::importFiles ({ source }, dir, 0, kRate, nullptr);
                expectEquals (result.failed, 1, "a damaged import was accepted");
                expect (result.tracks.empty());
                expect (! dir.getChildFile ("Track_01.wav").existsAsFile());
                expectEquals (partials (dir), 0);

                const auto converted = audioimport::importFiles ({ source }, dir, 0, 44100.0, nullptr);
                expectEquals (converted.failed, 1, "a converted import hid the failure");
                expectEquals (partials (dir), 0);
            }
        }

        void punches()
        {
            beginTest ("A splice refuses a base or an insert it cannot read in full");
            {
                Folder f;
                const auto base = f.audio ("base.wav"), insert = f.audio ("insert.wav"), out = f.audio ("out.wav");
                writeAudio (base);
                writeAudio (insert, 8192);
                expect (splicePunchFile (formats(), base, insert, 4096, out), "the healthy splice should work");
                out.deleteFile();

                cutShort (base);
                expect (! splicePunchFile (formats(), base, insert, 4096, out), "a splice wrote a hole into a take");
                expect (! out.existsAsFile(), "a failed splice left its output");

                writeAudio (base);
                cutShort (insert);
                expect (! splicePunchFile (formats(), base, insert, 4096, out), "a splice wrote a hole from the punch");
                expect (! out.existsAsFile());
            }

            beginTest ("A punch keeps its original when the base cannot be read in full");
            {
                const auto dir = tempDir ("punchbase");
                recordTake (dir, 1, 200);
                const auto take = trackFile (dir, 0);

                MultitrackRecorder rec;
                rec.prepare (kRate, kBlock, 1);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (4 * kBlock);
                expect (rec.startRecording (dir), rec.getStartError());

                // The drive drops under the sidecar while the punch is rolling.
                const auto sidecar = take.getSiblingFile ("Track_01.punchbase.wav");
                expect (sidecar.existsAsFile(), "the base was not stashed");
                cutShort (sidecar);
                const auto damaged = juce::MD5 (sidecar).toHexString();

                feedAll (rec, -0.3f, 10, 4 * kBlock);
                rec.stopRecording();

                expect (rec.hasPunchSpliceFailed(), "a splice over an unreadable base reported success");
                expect (take.existsAsFile());
                expect (juce::MD5 (take).toHexString() == damaged, "the original was not put back as it was");
                expect (! sidecar.existsAsFile(), "the sidecar was left behind");
                expectEquals (dir.getChildFile ("Session File Backups")
                                 .findChildFiles (juce::File::findFiles, true, "Track_01*.wav").size(), 1,
                              "the punch audio was not kept");
                dir.deleteRecursively();
            }
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const juce::ScopeGuard restoreTestMode { [] { AudioEngine::setTestModeSkipAudioInit (false); } };
            {
                Folder f;
                readers (f);
                exports (f);
            }
            renders();
            analysis();
            punches();
        }
    };

    static FailedReadTests failedReadTests;

    class EditDetailTests final : public juce::UnitTest
    {
    public:
        EditDetailTests() : juce::UnitTest ("Edits and exports keep their word", "zynforge") {}

        void stripPaste()
        {
            const auto fresh = [] (AudioEngine& engine)
            {
                engine.clearAllStripOverrides();
                engine.setStripCount (6);
                engine.prepareForTests (kRate, kBlock);
                engine.setTrackStereo (0, true);
                engine.setTrackStereo (2, true);
            };
            const auto stripOf = [] (AudioEngine& engine, int index) -> TrackState&
            { return engine.getRecorder().getTrack (index); };

            beginTest ("A mono paste onto a stereo strip reaches both halves and leaves its image");
            {
                AudioEngine engine;
                fresh (engine);
                engine.setTrackPan (0, -1.0f);
                engine.setTrackPan (1, 1.0f);
                engine.setTrackGainDb (4, -6.0f);
                engine.setTrackInputRouting (4, 5);
                engine.setTrackOutputRouting (4, 7);
                stripOf (engine, 4).muted.store (true);
                stripOf (engine, 4).streamSend.store (true);
                stripOf (engine, 4).monitor.store (true);
                stripOf (engine, 0).armed.store (true);
                stripOf (engine, 1).armed.store (true);

                mixersnapshot::pasteStrip (engine, 0, mixersnapshot::copyStrip (engine, 4));
                for (int half : { 0, 1 })
                {
                    auto& t = stripOf (engine, half);
                    expectWithinAbsoluteError (t.gainDb.load(), -6.0f, 1.0e-3f);
                    expect (t.muted.load(), "a muted strip still played a half");
                    expect (t.streamSend.load());
                    expect (t.monitor.load());
                    expect (! t.armed.load(), "a disarmed strip left a half armed");
                }
                expectEquals (stripOf (engine, 0).inputRouting.load(), 5);
                expectEquals (stripOf (engine, 1).inputRouting.load(), 6, "R stayed on its old input");
                expectEquals (stripOf (engine, 0).outputRouting.load(), 7);
                expectEquals (stripOf (engine, 1).outputRouting.load(), 8);
                expectWithinAbsoluteError (stripOf (engine, 0).pan.load(), -1.0f, 1.0e-3f, "the stereo image moved");
                expectWithinAbsoluteError (stripOf (engine, 1).pan.load(),  1.0f, 1.0e-3f, "the stereo image moved");
            }

            beginTest ("A stereo paste goes half for half, and a mono target takes the left");
            {
                AudioEngine engine;
                fresh (engine);
                engine.setTrackGainDb (2, -3.0f);
                engine.setTrackGainDb (3, -9.0f);
                engine.setTrackPan (2, -0.5f);
                engine.setTrackPan (3, 0.5f);
                engine.setTrackInputRouting (2, 2);
                engine.setTrackInputRouting (3, 7);
                const auto copied = mixersnapshot::copyStrip (engine, 2);

                mixersnapshot::pasteStrip (engine, 0, copied);
                expectWithinAbsoluteError (stripOf (engine, 0).gainDb.load(), -3.0f, 1.0e-3f);
                expectWithinAbsoluteError (stripOf (engine, 1).gainDb.load(), -9.0f, 1.0e-3f);
                expectWithinAbsoluteError (stripOf (engine, 1).pan.load(), 0.5f, 1.0e-3f);
                expectEquals (stripOf (engine, 1).inputRouting.load(), 7);

                const auto before = stripOf (engine, 5).pan.load();
                mixersnapshot::pasteStrip (engine, 4, copied);
                expectWithinAbsoluteError (stripOf (engine, 4).gainDb.load(), -3.0f, 1.0e-3f);
                expectWithinAbsoluteError (stripOf (engine, 4).pan.load(), 0.0f, 1.0e-3f, "half a stereo image landed on a mono strip");
                expectWithinAbsoluteError (stripOf (engine, 5).pan.load(), before, 1.0e-3f, "a mono paste touched the next strip");
            }

            beginTest ("A strip copied during soundcheck carries the show's output and no soundcheck");
            {
                AudioEngine engine;
                fresh (engine);
                engine.setTrackInputRouting (4, 3);
                engine.setTrackOutputRouting (4, 6);
                expect (vsc::repatch (engine) > 0);
                expectEquals (stripOf (engine, 4).outputRouting.load(), 3);
                const auto copied = mixersnapshot::copyStrip (engine, 4);
                vsc::restore (engine);

                mixersnapshot::pasteStrip (engine, 5, copied);
                expectEquals (stripOf (engine, 5).outputRouting.load(), 6, "the soundcheck patch was pasted as the show's");
                expect (! vsc::hasSavedPatch (engine), "a pasted strip lit VSC");
            }

            beginTest ("A paste onto a strip in soundcheck keeps its way back and its soundcheck");
            {
                AudioEngine engine;
                fresh (engine);
                engine.setTrackInputRouting (5, 1);
                engine.setTrackOutputRouting (5, 2);
                engine.setTrackInputRouting (4, 3);
                engine.setTrackOutputRouting (4, 6);
                const auto copied = mixersnapshot::copyStrip (engine, 4);
                expect (vsc::repatch (engine) > 0);

                mixersnapshot::pasteStrip (engine, 5, copied);
                expectEquals (stripOf (engine, 5).outputBeforeVsc.load(), 6, "the way back was lost");
                expectEquals (stripOf (engine, 5).outputRouting.load(), 3, "the soundcheck output left the input");
                vsc::restore (engine);
                expectEquals (stripOf (engine, 5).outputRouting.load(), 6);
            }
        }

        void analysis()
        {
            beginTest ("Detect Songs finds a song in a session with one voice");
            {
                const int windows = 900;   // 90 s of 100 ms windows
                const std::vector<std::vector<char>> one { std::vector<char> ((size_t) windows, 1) };
                const songs::Params params;
                expectEquals ((int) songs::detect (one, kRate, params).size(), 1, "one voice could never be a quorum");

                const std::vector<std::vector<char>> two { std::vector<char> ((size_t) windows, 1),
                                                           std::vector<char> ((size_t) windows, 0) };
                expect (songs::detect (two, kRate, params).empty(), "one ambient mic of two became a song");
            }

            beginTest ("QC reads older takes in the session root beside imported ones");
            {
                const auto dir = tempDir ("qc");
                FailedReadTests::writeAudio (dir.getChildFile ("Track_01.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Track_02.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Track_02_part02.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Audio Files/Track_02.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Audio Files/Track_03.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Audio Files/Track_03_consolidated_1.wav"), 4800);
                FailedReadTests::writeAudio (dir.getChildFile ("Audio Files/Track_04.punchbase.wav"), 4800);

                const auto takes = qc::sessionTakes (dir);
                expectEquals (takes.size(), 3);
                expect (takes[0] == dir.getChildFile ("Track_01.wav"), "the root take was left out");
                expect (takes[1] == dir.getChildFile ("Audio Files/Track_02.wav"), "the modern folder lost its precedence");
                expect (takes[2] == dir.getChildFile ("Audio Files/Track_03.wav"));
                dir.deleteRecursively();
            }
        }

        void timelineCsv()
        {
            beginTest ("The timeline CSV says where each clip plays, so markers line up with the audio");
            MainComponent main;
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            PunchRuleTests::Session s (main);
            expect (s.engine().rippleDeleteRange (0, 4096, 8192));
            const auto csv = main.buildTimelineCsv();
            expect (csv.contains ("[Clips]"), "no clip list");
            expect (csv.contains (",4096,8192," + juce::String (200 * kBlock - 8192) + ",no"),
                    "the clip that moved does not say where it plays and what it reads");
            expect (csv.contains (",0,0,4096,no"), "the clip before the cut is missing");
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
            stripPaste();
            analysis();
            timelineCsv();
        }
    };

    static EditDetailTests editDetailTests;
}
