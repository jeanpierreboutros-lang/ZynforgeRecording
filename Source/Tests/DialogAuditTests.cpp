// Regressions from the dialogs + Theme audit (2026-08-14).
//
// The two that matter are both "a dialog quietly disagreed with the engine":
// a mirror root that resolved onto the take's own files, and a mirror change
// the recorder refused but the settings file recorded anyway.

#include <juce_audio_devices/juce_audio_devices.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    class DialogAuditTests final : public juce::UnitTest
    {
    public:
        DialogAuditTests() : juce::UnitTest ("Dialog audit fixes", "zynforge") {}

        struct TempDir
        {
            juce::File dir { juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("zf-dlg-" + juce::Uuid().toString()) };
            TempDir()  { dir.createDirectory(); }
            ~TempDir() { dir.deleteRecursively(); }
        };

        void runTest() override
        {
            beginTest ("A mirror root that holds the session is REFUSED");
            {
                TempDir tmp;
                auto sessionsRoot = tmp.dir.getChildFile ("Zynforge Sessions");
                auto sessionDir   = sessionsRoot.getChildFile ("Gig");
                sessionDir.createDirectory();

                // THE BUG: a mirror writes root/<sessionName>/Audio Files/, so a
                // root that is the session's PARENT resolves onto the primary
                // take's own files -- two writers, one path, whole show. The
                // picker opens at ~/Music and sessions live in ~/Music/Zynforge
                // Sessions, so this was one click away.
                const auto reason = MultitrackRecorder::mirrorRootRejection (
                    sessionsRoot, sessionDir, juce::File(), {});
                expect (reason.isNotEmpty(),
                        "the sessions root must be refused as a mirror destination");

                // A genuinely separate drive root is fine.
                auto other = tmp.dir.getChildFile ("RECORD");
                other.createDirectory();
                expect (MultitrackRecorder::mirrorRootRejection (
                            other, sessionDir, juce::File(), {}).isEmpty(),
                        "an unrelated root must be accepted");
            }

            beginTest ("Mirror roots are refused inside the session, on the backup, and duplicated");
            {
                TempDir tmp;
                auto sessionDir = tmp.dir.getChildFile ("Sessions").getChildFile ("Gig");
                auto backupRoot = tmp.dir.getChildFile ("Backup");
                auto driveA     = tmp.dir.getChildFile ("A");
                sessionDir.createDirectory(); backupRoot.createDirectory(); driveA.createDirectory();

                expect (MultitrackRecorder::mirrorRootRejection (
                            sessionDir, sessionDir, backupRoot, {}).isNotEmpty(),
                        "the session folder itself is not a second copy");
                expect (MultitrackRecorder::mirrorRootRejection (
                            sessionDir.getChildFile ("Audio Files"), sessionDir, backupRoot, {}).isNotEmpty(),
                        "a folder inside the session is not a second copy");
                expect (MultitrackRecorder::mirrorRootRejection (
                            backupRoot, sessionDir, backupRoot, {}).isNotEmpty(),
                        "the backup root already writes that layout");
                expect (MultitrackRecorder::mirrorRootRejection (
                            driveA, sessionDir, backupRoot, { driveA }).isNotEmpty(),
                        "two mirrors on one folder would open two writers per file");
                expect (MultitrackRecorder::mirrorRootRejection (
                            driveA, sessionDir, backupRoot, {}).isEmpty());
            }

            beginTest ("A colliding mirror never opens a writer, and is COUNTED as skipped");
            {
                TempDir tmp;
                AudioEngine::setTestModeSkipAudioInit (true);
                auto sessionsRoot = tmp.dir.getChildFile ("Sessions");
                auto sessionDir   = sessionsRoot.getChildFile ("Gig");
                sessionDir.createDirectory();
                auto goodDrive    = tmp.dir.getChildFile ("GOOD");
                goodDrive.createDirectory();

                MultitrackRecorder rec;
                rec.prepare (48000.0, 512, 2);
                rec.setTrackCount (1);
                rec.getTrack (0).armed.store (true);

                // One poisonous root (the session's parent) + one good one.
                expect (rec.setMirrors ({ { sessionsRoot, CaptureFormat::Wav24 },
                                          { goodDrive,    CaptureFormat::Wav24 } }));
                expect (rec.startRecording (sessionDir));
                rec.stopRecording();

                expectEquals (rec.getMirrorsSkippedAtStart(), 1,
                              "the colliding mirror must be dropped AND counted -- a mirror that "
                              "never opens creates no entry, so anyMirrorFailed() cannot see it");
                expect (goodDrive.getChildFile ("Gig").getChildFile ("Audio Files")
                                 .getChildFile ("Track_01.wav").existsAsFile(),
                        "the usable mirror must still have written");
                rec.release();
            }

            beginTest ("setMirrors REFUSES mid-take and reports it (nothing may be persisted)");
            {
                TempDir tmp;
                AudioEngine::setTestModeSkipAudioInit (true);
                auto sessionDir = tmp.dir.getChildFile ("Gig");
                sessionDir.createDirectory();
                auto driveA = tmp.dir.getChildFile ("A"); driveA.createDirectory();

                MultitrackRecorder rec;
                rec.prepare (48000.0, 512, 2);
                rec.setTrackCount (1);
                rec.getTrack (0).armed.store (true);

                expect (rec.setMirrors ({}), "editable while stopped");
                expect (rec.startRecording (sessionDir));
                // THE BUG: this returned void, so AudioEngine::setMirrors wrote
                // the new list to appProps regardless -- the settings file then
                // claimed a redundancy the live recorder wasn't providing.
                expect (! rec.setMirrors ({ { driveA, CaptureFormat::Wav24 } }),
                        "a mirror change during a take must be REFUSED, not silently dropped");
                expect (rec.getMirrors().empty(), "and must not have taken effect");
                rec.stopRecording();
                expect (rec.setMirrors ({ { driveA, CaptureFormat::Wav24 } }),
                        "editable again once the take ends");
                rec.release();
            }

            beginTest ("onSignal keeps every accent's label legible (no hardcoded white)");
            {
                // THE BUG: drawToggleButton painted the ON letter white whatever
                // the fill was. Correct only for record red; solo yellow gave a
                // white S at ~1.2:1 on the chip you scan mid-show.
                expect (brand::onSignal (brand::accentSolo)   == juce::Colours::black,
                        "solo yellow needs a BLACK letter");
                expect (brand::onSignal (brand::accentPlay)   == juce::Colours::black,
                        "monitor green needs a BLACK letter");
                expect (brand::onSignal (brand::brandOrange)  == juce::Colours::black,
                        "mute orange needs a BLACK letter");
                expect (brand::onSignal (brand::accentRecord) != juce::Colours::black,
                        "record red is dark enough for a light letter");
            }
        }
    };

    static DialogAuditTests dialogAuditTests;
}
