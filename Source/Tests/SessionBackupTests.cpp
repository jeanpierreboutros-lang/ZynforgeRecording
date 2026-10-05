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

            beginTest ("Same-second snapshots retain the newest ten metadata contents");
            {
                const auto session = makeSession ("Chronology");
                const juce::ScopeGuard cleanup { [&] { session.getParentDirectory().deleteRecursively(); } };
                const juce::Time fixedTime (2026, 9, 5, 12, 0, 0, 0, false);
                const auto backups = session.getChildFile ("Session File Backups");
                const auto journal = backups.getChildFile ("reorder_preserve");
                expect (journal.createDirectory().wasOk());
                expect (journal.getChildFile ("recovery.json").replaceWithText ("journal"));
                const auto notes = backups.getChildFile ("Chronology_notes");
                expect (notes.createDirectory().wasOk());
                expect (notes.getChildFile ("keep.txt").replaceWithText ("notes"));

                for (int sequence = 0; sequence < 14; ++sequence)
                {
                    expect (session.getChildFile ("session_mix.json").replaceWithText (
                        juce::String (sequence)));
                    const auto snapshot = sessionbackup::writeSnapshot (session, 10, fixedTime);
                    expect (snapshot.isDirectory(), "the snapshot just returned was pruned");
                    expectEquals (snapshot.getChildFile ("session_mix.json").loadFileAsString(),
                                  juce::String (sequence), "returned snapshot is not the latest save");
                }

                juce::Array<int> sequences;
                for (const auto& folder : backups.findChildFiles (juce::File::findDirectories, false))
                    if (folder.getChildFile ("session_mix.json").existsAsFile())
                        sequences.add (folder.getChildFile ("session_mix.json").loadFileAsString().getIntValue());
                sequences.sort();
                expectEquals (sequences.size(), 10);
                for (int i = 0; i < sequences.size(); ++i)
                    expectEquals (sequences[i], i + 4, "retention preserved an older save instead of a newer one");
                expectEquals (journal.getChildFile ("recovery.json").loadFileAsString(), juce::String ("journal"));
                expectEquals (notes.getChildFile ("keep.txt").loadFileAsString(), juce::String ("notes"));
            }

            beginTest ("Clock rollback preserves chronological retention and recognizes legacy collisions");
            {
                const auto session = makeSession ("Rollback");
                const juce::ScopeGuard cleanup { [&] { session.getParentDirectory().deleteRecursively(); } };
                const auto backups = session.getChildFile ("Session File Backups");
                for (const int collision : { 2, 10 })
                {
                    const auto legacy = backups.getChildFile ("Rollback_2026-10-05_12-00-00 ("
                                                               + juce::String (collision) + ")");
                    expect (legacy.createDirectory().wasOk());
                    expect (legacy.getChildFile ("session_mix.json").replaceWithText (juce::String (collision)));
                }
                // A backward wall clock cannot put a newly accepted save before
                // older snapshots or reuse a suffix removed by prior pruning.
                const juce::Time backward (2026, 9, 4, 12, 0, 0, 0, false);
                for (int revision = 11; revision <= 14; ++revision)
                {
                    expect (session.getChildFile ("session_mix.json").replaceWithText (juce::String (revision)));
                    const auto latest = sessionbackup::writeSnapshot (session, 2, backward);
                    expect (latest.isDirectory(), "clock rollback pruned the just-created snapshot");
                    expectEquals (latest.getChildFile ("session_mix.json").loadFileAsString(), juce::String (revision));
                }
                juce::Array<int> revisions;
                for (const auto& folder : backups.findChildFiles (juce::File::findDirectories, false))
                    revisions.add (folder.getChildFile ("session_mix.json").loadFileAsString().getIntValue());
                revisions.sort();
                expectEquals (revisions.size(), 2);
                if (revisions.size() == 2)
                {
                    expectEquals (revisions[0], 13);
                    expectEquals (revisions[1], 14);
                }
            }

            beginTest ("A linked backup root refuses writes and preserves external snapshots");
            {
                const auto session = makeSession ("LinkedRoot");
                const auto external = session.getParentDirectory().getChildFile ("external");
                const auto link = session.getChildFile ("Session File Backups");
                const juce::ScopeGuard cleanup { [&]
                {
                    link.deleteFile(); // unlink before cleaning the disposable target
                    session.getParentDirectory().deleteRecursively();
                } };
                const auto existing = external.getChildFile ("LinkedRoot_2000-01-01_00-00-00");
                expect (existing.createDirectory().wasOk());
                const auto sentinel = existing.getChildFile ("keep.txt");
                expect (sentinel.replaceWithText ("external snapshot must survive"));
                expect (external.createSymbolicLink (link, false));
                if (link.isSymbolicLink())
                {
                    const auto result = sessionbackup::writeSnapshot (session, 1);
                    expect (result == juce::File(), "snapshot followed an external backup-root link");
                    expectEquals (sentinel.loadFileAsString(), juce::String ("external snapshot must survive"));
                    expectEquals (external.findChildFiles (juce::File::findDirectories, false).size(), 1,
                                  "snapshot wrote or pruned folders outside the session");
                }
            }

            beginTest ("Retention excludes linked snapshot candidates and preserves their target");
            {
                const auto session = makeSession ("LinkedCandidate");
                const auto backups = session.getChildFile ("Session File Backups");
                const auto external = session.getParentDirectory().getChildFile ("external");
                const auto link = backups.getChildFile ("LinkedCandidate_2000-01-01_00-00-00");
                const juce::ScopeGuard cleanup { [&]
                {
                    link.deleteFile();
                    session.getParentDirectory().deleteRecursively();
                } };
                expect (backups.createDirectory().wasOk());
                expect (external.createDirectory().wasOk());
                expect (external.getChildFile ("keep.txt").replaceWithText ("external target"));
                expect (external.createSymbolicLink (link, false));
                if (link.isSymbolicLink())
                {
                    const auto result = sessionbackup::writeSnapshot (session, 1);
                    expect (result.isDirectory(), "a non-snapshot link prevented a valid local backup");
                    expect (link.isSymbolicLink(), "retention pruned a linked candidate it does not own");
                    expectEquals (external.getChildFile ("keep.txt").loadFileAsString(),
                                  juce::String ("external target"));
                }
            }

            beginTest ("Nonpositive snapshot retention refuses before changing existing backups");
            for (const int keepNewest : { 0, -1 })
            {
                const auto session = makeSession ("InvalidRetention");
                const juce::ScopeGuard cleanup { [&] { session.getParentDirectory().deleteRecursively(); } };
                const auto existing = session.getChildFile ("Session File Backups")
                    .getChildFile ("InvalidRetention_2000-01-01_00-00-00");
                expect (existing.createDirectory().wasOk());
                const auto sentinel = existing.getChildFile ("keep.txt");
                expect (sentinel.replaceWithText ("previous recovery state"));
                const auto result = sessionbackup::writeSnapshot (session, keepNewest);
                expect (result == juce::File(), "invalid retention returned a snapshot path");
                expectEquals (sentinel.loadFileAsString(), juce::String ("previous recovery state"));
                expectEquals (existing.getParentDirectory().findChildFiles (
                                  juce::File::findDirectories, false).size(), 1,
                              "invalid retention changed the backup directory");
            }
        }
    };

    static SessionBackupTests sessionBackupTests;
}
