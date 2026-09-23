// Validates the full "backup session" snapshot: it copies the session-defining
// files (not the audio) into Session File Backups/<Name>_<stamp>/, and prunes
// to the newest N folders.

#include <juce_core/juce_core.h>
#include "../Audio/SessionBackup.h"

namespace zynforge
{
    class SessionBackupTests final : public juce::UnitTest
    {
    public:
        SessionBackupTests() : UnitTest ("Session backup", "zynforge") {}

        static juce::File makeSession (const juce::String& name)
        {
            auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("zynforge_bk_" + juce::String (juce::Random::getSystemRandom().nextInt (1'000'000)))
                            .getChildFile (name);
            root.createDirectory();
            root.getChildFile (name + ".zfproj")        .replaceWithText ("{\"setlist\":[]}");
            root.getChildFile ("session_mix.json")       .replaceWithText ("{\"tracks\":[]}");
            root.getChildFile ("session_settings.json")  .replaceWithText ("{\"captureFormat\":0}");
            root.getChildFile ("markers.json")           .replaceWithText ("{\"markers\":[]}");
            // Audio that must NOT be copied into a backup.
            root.getChildFile ("Audio Files").createDirectory();
            root.getChildFile ("Audio Files").getChildFile ("Track_01.wav").replaceWithText ("PRETEND-WAV");
            return root;
        }

        void runTest() override
        {
            beginTest ("Snapshot copies the session definition but not the audio");
            {
                auto session = makeSession ("Gig");
                const auto snap = sessionbackup::writeSnapshot (session);

                expect (snap.isDirectory(), "no snapshot folder created");
                expect (snap.getParentDirectory().getFileName() == "Session File Backups",
                        "snapshot not under Session File Backups/");
                expect (snap.getFileName().startsWith ("Gig_"), "snapshot folder misnamed");

                for (auto* f : { "Gig.zfproj", "session_mix.json", "session_settings.json", "markers.json" })
                    expect (snap.getChildFile (f).existsAsFile(), juce::String (f) + " missing from backup");

                // Content survives, and the (large, immutable) audio is excluded.
                expectEquals (snap.getChildFile ("session_mix.json").loadFileAsString(), juce::String ("{\"tracks\":[]}"));
                expect (! snap.getChildFile ("Audio Files").exists(), "backup wrongly copied the audio");
                expect (! snap.getChildFile ("Track_01.wav").exists(), "backup wrongly copied a WAV");

                session.getParentDirectory().deleteRecursively();
            }

            beginTest ("Pruning keeps only the newest N backup folders");
            {
                auto session = makeSession ("Show");
                const auto backupsDir = session.getChildFile ("Session File Backups");
                const auto journal = backupsDir.getChildFile ("reorder_" + juce::Uuid().toString());
                expect (journal.createDirectory().wasOk());
                expect (journal.getChildFile ("recovery.json").replaceWithText ("keep me"));
                const auto unrelated = backupsDir.getChildFile ("Show_notes");
                expect (unrelated.createDirectory().wasOk());

                for (int i = 0; i < 14; ++i)
                    sessionbackup::writeSnapshot (session, 10);   // keep 10

                int snapshotCount = 0;
                for (const auto& folder : backupsDir.findChildFiles (juce::File::findDirectories, false, "Show_2*"))
                    ++snapshotCount;
                expectEquals (snapshotCount, 10, "pruning did not cap snapshots at 10");
                expectEquals (journal.getChildFile ("recovery.json").loadFileAsString(),
                              juce::String ("keep me"), "backup pruning deleted a reorder journal");
                expect (unrelated.isDirectory(), "backup pruning deleted a non-snapshot folder");

                session.getParentDirectory().deleteRecursively();
            }

            beginTest ("A non-existent session dir is a safe no-op");
            {
                expect (! sessionbackup::writeSnapshot (juce::File ("/no/such/zynforge_session")).exists());
            }
        }
    };

    static SessionBackupTests sessionBackupTests;
}
