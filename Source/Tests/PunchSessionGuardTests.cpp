// Regression tests for the October bug hunt's punch and session-lifecycle
// findings: a refused start must not leave its punch/continue arm, a punch
// take locks the playhead and loop, a rolling punch splices where capture
// actually began, and New Session never runs under a live take or inherits
// the previous session's undo steps.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/TransportBar.h"
#include "../UI/SessionProjPath.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"

namespace zynforge
{
    class PunchSessionGuardTests final : public juce::UnitTest
    {
    public:
        PunchSessionGuardTests() : juce::UnitTest ("Punch and session guards", "zynforge") {}

        static juce::File tempDir (const juce::String& tag)
        {
            auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("zf-guard-" + tag + "-" + juce::Uuid().toString());
            d.deleteRecursively();
            return d;
        }

        // Feed `blocks` blocks of DC `value` to every channel. playhead >= 0
        // tells the recorder where on the timeline the first block sits.
        static void feed (MultitrackRecorder& rec, int channels, float value, int blocks,
                          int block, juce::int64 playhead = -1)
        {
            std::vector<float> buf ((size_t) block, value);
            std::vector<const float*> ptrs ((size_t) channels, buf.data());
            for (int b = 0; b < blocks; ++b)
                rec.processBlock (ptrs.data(), channels, block,
                                  playhead >= 0 ? playhead + (juce::int64) b * block : -1);
        }

        // Reads a track's whole take (Track_NN plus its parts), as playback does.
        static float mean (const juce::File& f, juce::int64 start, juce::int64 len)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            auto r = ConcatReader::create (fm, findTakeParts (f));
            if (r == nullptr || len <= 0 || start + len > r->lengthInSamples) return 999.0f;
            juce::AudioBuffer<float> buf (1, (int) len);
            r->read (&buf, 0, (int) len, start, true, false);
            double sum = 0.0;
            for (int i = 0; i < (int) len; ++i) sum += buf.getSample (0, i);
            return (float) (sum / (double) len);
        }

        static juce::int64 lengthOf (const juce::File& f)
        {
            juce::AudioFormatManager fm; fm.registerBasicFormats();
            auto r = ConcatReader::create (fm, findTakeParts (f));
            return r != nullptr ? r->lengthInSamples : -1;
        }

        void runTest() override
        {
            constexpr double sr = 48000.0;
            constexpr int block = 512;

            beginTest ("punch and continue arms are exclusive");
            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.armPunchIn (4096);
                rec.armContinue (8192);
                expect (rec.isContinueArmed() && ! rec.isPunchInArmed(),
                        "continue left a punch arm set");
                rec.armPunchIn (4096);
                expect (rec.isPunchInArmed() && ! rec.isContinueArmed(),
                        "punch left a continue arm set");
            }

            beginTest ("a refused recorder start consumes its punch arm");
            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (0).isStereo.store (true);   // incomplete pair: refused before any file
                rec.armPunchIn (4096);
                expect (! rec.startRecording (tempDir ("refused")));
                expect (! rec.isPunchInArmed(), "a refused punch kept its arm for the next RECORD");
                expectEquals (rec.getRecordBaseSamples(), (juce::int64) 0);
            }

            beginTest ("a refused engine start consumes its punch arm");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                AudioEngine engine;
                engine.setStripCount (1);
                engine.prepareForTests (sr, block);
                engine.setExternalRecording (true);   // a daemon take owns the transport
                engine.armPunchIn (4096);
                expect (! engine.startRecording (tempDir ("engine-refused")));
                expect (! engine.getRecorder().isPunchInArmed(),
                        "an engine refusal kept the punch arm");
                engine.setExternalRecording (false);
                AudioEngine::setTestModeSkipAudioInit (false);
            }

            const juce::int64 baseLen = 90 * block;
            const juce::int64 armedAt = 20 * block;
            const juce::int64 capturedAt = 25 * block;   // start work took 5 blocks
            const juce::int64 punchLen = 15 * block;

            beginTest ("a rolling punch splices where capture began, not where it was armed");
            {
                const auto dir = tempDir ("origin");
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 1, 0.5f, 90, block);
                    rec.stopRecording();
                }
                MultitrackRecorder rec;
                rec.prepare (sr, block, 2);
                rec.getTrack (0).armed.store (true);
                rec.getTrack (1).armed.store (true);   // new track: no base, silent lead-in
                rec.armPunchIn (armedAt);
                expect (rec.startRecording (dir));
                feed (rec, 2, -0.3f, 15, block, capturedAt);
                expectEquals (rec.getRecordTimelineSamples(), capturedAt + punchLen,
                              "the take clock ignores where capture began");
                rec.stopRecording();

                const auto t1 = dir.getChildFile ("Audio Files/Track_01.wav");
                expectEquals (lengthOf (t1), baseLen);
                expectWithinAbsoluteError (mean (t1, armedAt, capturedAt - armedAt), 0.5f, 1.0e-3f,
                                           "original audio before capture began was replaced");
                expectWithinAbsoluteError (mean (t1, capturedAt, punchLen), -0.3f, 1.0e-3f,
                                           "punched audio landed early");
                expectWithinAbsoluteError (mean (t1, capturedAt + punchLen, baseLen - capturedAt - punchLen),
                                           0.5f, 1.0e-3f, "audio after the punch-out was overwritten");

                const auto t2 = dir.getChildFile ("Audio Files/Track_02.wav");
                expectEquals (lengthOf (t2), capturedAt + punchLen,
                              "the new track's lead-in does not reach where capture began");
                expectWithinAbsoluteError (mean (t2, 0, capturedAt), 0.0f, 1.0e-6f);
                expectWithinAbsoluteError (mean (t2, capturedAt, punchLen), -0.3f, 1.0e-3f);
            }

            beginTest ("RECORD on a continue does not wait for a new track's lead-in");
            {
                const auto dir = tempDir ("leadin-deferred");
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 1, 0.5f, 90, block);
                    rec.stopRecording();
                }
                MultitrackRecorder rec;
                rec.prepare (sr, block, 2);
                rec.getTrack (1).armed.store (true);   // a new track with no audio yet
                rec.armContinue (0);
                expect (rec.startRecording (dir));
                const auto audio = dir.getChildFile ("Audio Files");
                expect (! audio.getChildFile ("Track_02.wav").existsAsFile(),
                        "the silent lead-in was written before capture could start");
                expect (audio.getChildFile ("Track_02_part02.wav").existsAsFile(),
                        "the new audio does not open in the part after the lead-in");
                expect (audio.getChildFile ("Pending Lead-ins/Track_02.json").existsAsFile(),
                        "no crash-recovery marker for the deferred lead-in");
                feed (rec, 2, -0.3f, 8, block);
                rec.stopRecording();
                const auto t2 = audio.getChildFile ("Track_02.wav");
                expectEquals (lengthOf (t2), baseLen + 8 * block);
                expectWithinAbsoluteError (mean (t2, 0, baseLen), 0.0f, 1.0e-6f);
                expectWithinAbsoluteError (mean (t2, baseLen, 8 * block), -0.3f, 1.0e-3f);
                expect (! audio.getChildFile ("Pending Lead-ins").exists(), "the marker outlived STOP");
            }

            beginTest ("an interrupted take's lead-in is completed when the session opens");
            {
                const auto audio = tempDir ("leadin-recover").getChildFile ("Audio Files");
                audio.createDirectory();
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (
                        audio.getChildFile ("Track_03_part02.wav").createOutputStream().release(),
                        sr, 1, 24, {}, 0));
                    std::vector<float> dc ((size_t) block, 0.2f);
                    const float* p = dc.data();
                    for (int b = 0; b < 4; ++b) w->writeFromFloatArrays (&p, 1, block);
                }
                juce::DynamicObject::Ptr m (new juce::DynamicObject());
                m->setProperty ("track", "Track_03");   m->setProperty ("ext", ".wav");
                m->setProperty ("firstPart", 1);        m->setProperty ("parts", 1);
                m->setProperty ("container", 0);        m->setProperty ("bitDepth", 24);
                m->setProperty ("channels", 1);         m->setProperty ("sampleRate", sr);
                m->setProperty ("priorLength", 0);      m->setProperty ("timelineStart", 3000);
                audio.getChildFile ("Pending Lead-ins").createDirectory();
                audio.getChildFile ("Pending Lead-ins/Track_03.json")
                     .replaceWithText (juce::JSON::toString (juce::var (m.get())));
                expect (MultitrackRecorder::recoverInterruptedPunches (audio));
                const auto t3 = audio.getChildFile ("Track_03.wav");
                expectEquals (lengthOf (t3), (juce::int64) 3000 + 4 * block);
                expectWithinAbsoluteError (mean (t3, 3000, 4 * block), 0.2f, 1.0e-3f);
                expect (! audio.getChildFile ("Pending Lead-ins").exists(), "the marker survived recovery");
            }

            beginTest ("a lead-in longer than one file fills its reserved parts, none empty");
            {
                const auto dir = tempDir ("leadin-split");
                MultitrackRecorder::setAutoSplitThresholdBytesForTests (20000 * 3);
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.setCaptureFormat (CaptureFormat::Aiff24);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 1, 0.5f, 90, block);
                    rec.stopRecording();
                }
                MultitrackRecorder rec;
                rec.prepare (sr, block, 2);
                rec.setCaptureFormat (CaptureFormat::Aiff24);
                rec.getTrack (1).armed.store (true);
                rec.armContinue (0);
                expect (rec.startRecording (dir), rec.getStartError());
                feed (rec, 2, -0.3f, 8, block);
                rec.stopRecording();
                MultitrackRecorder::setAutoSplitThresholdBytesForTests (0);
                const auto t2 = dir.getChildFile ("Audio Files/Track_02.aif");
                const auto parts = findTakeParts (t2);
                expect (parts.size() >= 4, "a 46080-sample lead-in did not split at a 20000-frame ceiling");
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                for (const auto& part : parts)
                {
                    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (part));
                    expect (r != nullptr && r->lengthInSamples > 0, part.getFileName() + " is empty");
                }
                expectEquals (lengthOf (t2), baseLen + 8 * block);
                expectWithinAbsoluteError (mean (t2, baseLen, 8 * block), -0.3f, 1.0e-3f);
            }

            beginTest ("a punch with playback stopped still splices at the armed point");
            {
                const auto dir = tempDir ("stopped");
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 1, 0.5f, 90, block);
                    rec.stopRecording();
                }
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (armedAt);
                expect (rec.startRecording (dir));
                feed (rec, 1, -0.3f, 15, block);
                rec.stopRecording();
                const auto t1 = dir.getChildFile ("Audio Files/Track_01.wav");
                expectWithinAbsoluteError (mean (t1, armedAt, punchLen), -0.3f, 1.0e-3f);
            }

            beginTest ("a punch take locks the playhead and loop until it stops");
            {
                const auto dir = tempDir ("seeklock");
                {
                    MultitrackRecorder rec;
                    rec.prepare (sr, block, 1);
                    rec.getTrack (0).armed.store (true);
                    expect (rec.startRecording (dir));
                    feed (rec, 1, 0.5f, 90, block);
                    rec.stopRecording();
                }
                AudioEngine::setTestModeSkipAudioInit (true);
                {
                    AudioEngine engine;
                    engine.setStripCount (1);
                    engine.prepareForTests (sr, block);
                    engine.getRecorder().setBackupDirectory ({});
                    engine.getRecorder().setMirrors ({});
                    expectEquals (engine.loadSession (dir), 1);
                    auto& player = engine.getPlayer();
                    player.setPositionSamples (armedAt);
                    engine.setTrackArmed (0, true);
                    engine.armPunchIn (armedAt);
                    expect (engine.startRecording (dir));
                    player.setPositionSamples (0);
                    expectEquals (player.getPositionSamples(), armedAt, "a seek moved a punch take's playhead");
                    player.setLoopRegion (0, baseLen);
                    player.setLoopEnabled (true);
                    expect (! player.isLoopEnabled(), "loop turned on during a punch take");

                    TransportBar bar (engine);
                    engine.stopRecording();
                    player.setPositionSamples (0);
                    expectEquals (player.getPositionSamples(), (juce::int64) 0, "seek stayed locked after STOP");

                    engine.setPunchModeOn (true);
                    for (auto* child : bar.getChildren())
                        if (auto* b = dynamic_cast<juce::Button*> (child))
                            if (b->getTitle() == "Loop region") b->onClick();
                    expect (! player.isLoopEnabled(), "loop turned on in PUNCH mode");
                    engine.setPunchModeOn (false);
                }
                AudioEngine::setTestModeSkipAudioInit (false);
            }

            // Base take of DC 0.5 on primary + backup, then a punch of -0.3
            // at armedAt. `sabotage` runs as finalization starts.
            const auto punchWithBackup = [&] (const juce::String& tag, std::function<void (const juce::File&, const juce::File&)> sabotage,
                                              bool feedAudio, MultitrackRecorder& rec, juce::File& dir, juce::File& backupRoot)
            {
                dir = tempDir (tag);
                backupRoot = tempDir (tag + "-backup");
                backupRoot.createDirectory();
                {
                    MultitrackRecorder base;
                    base.prepare (sr, block, 1);
                    base.setBackupDirectory (backupRoot);
                    base.getTrack (0).armed.store (true);
                    expect (base.startRecording (dir));
                    feed (base, 1, 0.5f, 90, block);
                    base.stopRecording();
                }
                rec.prepare (sr, block, 1);
                rec.setBackupDirectory (backupRoot);
                rec.getTrack (0).armed.store (true);
                rec.armPunchIn (armedAt);
                expect (rec.startRecording (dir), rec.getStartError());
                if (feedAudio) feed (rec, 1, -0.3f, 15, block);
                const auto primary = dir.getChildFile ("Audio Files/Track_01.wav");
                const auto backups = backupRoot.findChildFiles (juce::File::findFiles, true, "Track_01.wav");
                expectEquals (backups.size(), 1);
                const auto backup = backups.isEmpty() ? juce::File() : backups.getFirst();
                rec.beforeFinalizationForTests = [sabotage, primary, backup] { sabotage (primary, backup); };
                rec.stopRecording();
                return std::make_pair (primary, backup);
            };

            beginTest ("a backup that cannot splice keeps its take; the primary punch still lands");
            {
                MultitrackRecorder rec; juce::File dir, backupRoot;
                const auto files = punchWithBackup ("backup-fails",
                    [] (const juce::File&, const juce::File& backup) { backup.deleteFile(); }, true, rec, dir, backupRoot);
                expectWithinAbsoluteError (mean (files.first, armedAt, punchLen), -0.3f, 1.0e-3f,
                                           "a backup failure threw away the primary punch");
                expectWithinAbsoluteError (mean (files.second, armedAt, punchLen), 0.5f, 1.0e-3f,
                                           "the failed backup copy did not keep its original take");
                expect (! rec.hasPunchSpliceFailed());
                expect (rec.hasBackupFailed(), "the backup copy's failure was not reported");
            }

            beginTest ("a primary that cannot splice archives the new audio instead of deleting it");
            {
                MultitrackRecorder rec; juce::File dir, backupRoot;
                const auto files = punchWithBackup ("primary-fails",
                    [] (const juce::File& primary, const juce::File&)
                    {
                        // A punch file the splice cannot read.
                        primary.deleteFile();
                        primary.replaceWithText ("not audio");
                    }, true, rec, dir, backupRoot);
                expect (rec.hasPunchSpliceFailed());
                expectWithinAbsoluteError (mean (files.first, armedAt, punchLen), 0.5f, 1.0e-3f);
                expectWithinAbsoluteError (mean (files.second, armedAt, punchLen), 0.5f, 1.0e-3f,
                                           "copies disagree after a failed primary splice");
                const auto archived = backupRoot.findChildFiles (juce::File::findFiles, true, "Track_01.wav");
                bool kept = false;
                for (const auto& f : archived)
                    if (f.getFullPathName().contains ("Unspliced Punch")
                        && std::abs (mean (f, 0, punchLen) + 0.3f) < 1.0e-3f) kept = true;
                expect (kept, "the new punch audio was deleted rather than archived");
            }

            beginTest ("a punch stopped before any audio is not a failed splice");
            {
                MultitrackRecorder rec; juce::File dir, backupRoot;
                const auto files = punchWithBackup ("empty-punch",
                    [] (const juce::File&, const juce::File&) {}, false, rec, dir, backupRoot);
                expect (! rec.hasPunchSpliceFailed(), "an empty punch was reported as a failed splice");
                expectEquals (lengthOf (files.first), baseLen);
                expectWithinAbsoluteError (mean (files.first, 0, baseLen), 0.5f, 1.0e-3f);
            }

            beginTest ("a refused start always says why");
            {
                MultitrackRecorder rec;
                rec.prepare (sr, block, 1);
                rec.getTrack (0).armed.store (true);
                expect (! rec.startRecording (juce::File ("/System/zf-cannot-write-here")));
                expect (rec.getStartError().isNotEmpty(), "a refusal left startError empty");
            }

            beginTest ("the project lookup skips AppleDouble companions");
            {
                const auto dir = tempDir ("appledouble");
                dir.createDirectory();
                dir.getChildFile ("._Show.zfproj").replaceWithText (juce::String::repeatedString ("\x01", 8));
                dir.getChildFile ("Show.zfproj").replaceWithText ("{}");
                expectEquals (findSessionProj (dir).getFileName(), juce::String ("Show.zfproj"));
                dir.deleteRecursively();
            }

           #if JUCE_MAC
            beginTest ("New Session refuses under a live take and keeps its patch");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                MainComponent::s_testConstruct = true;
                {
                    MainComponent main;
                    auto& recorder = main.engine.getRecorder();
                    recorder.prepare (sr, block, 2);
                    recorder.setBackupDirectory ({});
                    recorder.setMirrors ({});
                    recorder.getTrack (0).inputRouting.store (7);
                    recorder.getTrack (0).isStereo.store (true);
                    recorder.getTrack (0).armed.store (true);
                    const auto live = tempDir ("live");
                    expect (recorder.startRecording (live));

                    NewSessionDialog::Result r;
                    r.name = "Second";
                    r.location = tempDir ("second-root");
                    expect (! main.createSessionFolderStructure (r).isDirectory(),
                            "a new session was created under a live take");
                    expectEquals (recorder.getTrack (0).inputRouting.load(), 7,
                                  "New Session re-patched the live take");
                    expect (recorder.getTrack (0).isStereo.load(), "New Session unlinked a live pair");
                    expect (main.sessionCreateFailureMessage().containsIgnoreCase ("recording"));
                    main.engine.clearSessionState();
                    expectEquals (recorder.getTrack (0).inputRouting.load(), 7,
                                  "clearSessionState re-patched the live take");
                    recorder.stopRecording();

                    beginTest ("New Session starts with a clean undo stack");
                    struct Noop final : juce::UndoableAction
                    {
                        bool perform() override { return true; }
                        bool undo() override    { return true; }
                    };
                    main.undoManager.beginNewTransaction ("Clip edit in A");
                    main.undoManager.perform (new Noop());
                    expect (main.undoManager.canUndo());
                    expect (main.createSessionFolderStructure (r).isDirectory());
                    expect (! main.undoManager.canUndo(), "session A's undo steps carried into B");

                    beginTest ("Undo names the step it undid");
                    main.undoManager.beginNewTransaction ("First");
                    main.undoManager.perform (new Noop());
                    main.undoManager.beginNewTransaction ("Second");
                    main.undoManager.perform (new Noop());
                    main.mixerUndoReady = false;
                    main.editUndo();
                    expectEquals (main.statusLabel.getText(), juce::String ("Undo: Second"));
                    main.editRedo();
                    expectEquals (main.statusLabel.getText(), juce::String ("Redo: Second"));

                    beginTest ("Move up at the top edge leaves the selected strips in order");
                    main.engine.setStripCount (3);
                    for (int t = 0; t < 3; ++t) main.engine.setTrackName (t, juce::String::charToString ((juce::juce_wchar) ('A' + t)));
                    main.selectedLogical = { 0, 1 };
                    main.moveSelectedStrips (-1);
                    expectEquals (recorder.getTrack (0).getNameThreadSafe(), juce::String ("A"));
                    expectEquals (recorder.getTrack (1).getNameThreadSafe(), juce::String ("B"),
                                  "the selected strips swapped with each other");
                    expect (main.selectedLogical == std::set<int> { 0, 1 });

                    beginTest ("LOCK also locks the VCA panel");
                    if (main.vcaPanel != nullptr)
                    {
                        main.onLockToggled();
                        expect (! main.vcaPanel->isEnabled(), "the VCA panel stayed live under LOCK");
                        main.onLockToggled();
                        expect (main.vcaPanel->isEnabled());
                    }

                    beginTest ("Layout changes during a session job wait instead of failing");
                    const auto active = tempDir ("active");
                    active.getChildFile ("Audio Files").createDirectory();
                    main.engine.setActiveSessionDir (active);
                    main.sessionIoBusy.store (true);
                    main.statusLabel.setText ({}, juce::dontSendNotification);
                    main.requestUILayoutSave();
                    expect (! main.statusLabel.getText().containsIgnoreCase ("Save failed"),
                            main.statusLabel.getText());
                    expect (main.uiLayoutSavePending);
                    main.sessionIoBusy.store (false);

                    beginTest ("Export refuses channel indices that went stale");
                    {
                        const auto dest = tempDir ("export-dest");
                        dest.createDirectory();
                        main.startExportTracksTo (dest, { 0, 7 }, {});
                        expect (main.statusLabel.getText().containsIgnoreCase ("channel list changed"),
                                main.statusLabel.getText());
                        dest.deleteRecursively();
                    }

                    beginTest ("Analysis and linked patching wait for the take to end");
                    {
                        const auto take = tempDir ("analysis-take");
                        recorder.getTrack (0).armed.store (true);
                        recorder.getTrack (0).outputRouting.store (3);
                        expect (recorder.startRecording (take));
                        main.runNoiseAnalysis();
                        expect (main.statusLabel.getText().containsIgnoreCase ("after recording stops"),
                                main.statusLabel.getText());
                        main.engine.setTrackLinkedRouting (0, 9);
                        expectEquals (recorder.getTrack (0).outputRouting.load(), 3,
                                      "a linked patch change half-applied during a take");
                        main.clickTrackIndex = 0;
                        main.tempoBar.onBpmChanged (131.0f);
                        expect (main.clickRefreshPending, "a tempo change during a take never re-renders the click");
                        main.clickTrackIndex = -1;
                        main.clickRefreshPending = false;
                        recorder.stopRecording();
                    }
                    r.location.deleteRecursively();
                    active.deleteRecursively();
                }
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            }
           #endif
        }
    };

    static PunchSessionGuardTests punchSessionGuardTests;
}
