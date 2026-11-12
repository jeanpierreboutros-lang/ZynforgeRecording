// What a session operation tells the engineer, and which folders it will touch:
// a handoff that lacks something, a template or map taken during Virtual
// Soundcheck, a session whose drive went away, a folder that is no session.

#include "../UI/MainComponent.h"
#include "../Audio/FastHash.h"
#include "../Audio/SessionBackup.h"
#include "../Audio/SessionRepair.h"
#include "../Audio/ShowHandoff.h"
#include "../Audio/VirtualSoundcheck.h"
#include "../UI/ExportDialog.h"
#include "../UI/ExportSongsDialog.h"
#include "../UI/FloatingLaunch.h"
#include "../UI/NewSessionDialog.h"
#include "../UI/SessionInfoDialog.h"
#include "../UI/SessionSettingsDialog.h"
#include "../UI/WelcomeDialog.h"
#include "SilentAudioDevice.h"
#include <csignal>
#include <sys/resource.h>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
class SessionFixTests final : public juce::UnitTest
{
public:
    SessionFixTests() : UnitTest ("Session fixes", "zynforge") {}

    struct Folder
    {
        juce::File dir;
        Folder (const juce::String& name = {}, bool audioFolder = true)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("zf-session-fix-" + juce::Uuid().toString())
                       .getChildFile (name.isNotEmpty() ? name : "Show"))
        {
            dir.createDirectory();
            if (audioFolder) dir.getChildFile ("Audio Files").createDirectory();
        }
        ~Folder() { dir.getParentDirectory().deleteRecursively(); }
        juce::File project() const { return dir.getChildFile (dir.getFileName() + ".zfproj"); }
        juce::File track (int index = 1) const
        { return dir.getChildFile (juce::String::formatted ("Audio Files/Track_%02d.wav", index)); }
    };

    static void writeSavedShow (const Folder& f, int strips = 2)
    {
        juce::Array<juce::var> list;
        for (int i = 0; i < strips; ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", i);
            o->setProperty ("uid", "strip-" + juce::String (i));
            o->setProperty ("name", "Saved " + juce::String (i + 1));
            list.add (juce::var (o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("formatVersion", 2);
        root->setProperty ("trackCount", strips);
        root->setProperty ("strips", list);
        f.dir.getChildFile ("session_mix.json").replaceWithText (juce::JSON::toString (juce::var (root), true));
        f.project().replaceWithText (R"({"zynforgeSession":true,"setlist":[],"playlists":[]})");
        f.dir.getChildFile ("session_settings.json").replaceWithText (R"({"captureFormat":6,"preRollSeconds":0,"sampleRate":48000.0})");
    }

    static bool writeAudio (const juce::File& file)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (1, 4800);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), 0.25f, 4800);
        return writer->writeFromAudioSampleBuffer (audio, 0, 4800);
    }

    static bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 120000)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
           #if JUCE_MAC
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
           #else
            juce::Thread::sleep (10);
           #endif
        }
        return ready();
    }

    static void forget (MainComponent& host)
    {
        host.engine.clearSessionState();
        host.engine.setStripCount (0);
        host.engine.clearAllStripOverrides();
    }

    // A session opened from disk, ready for the test to act on.
    struct Host
    {
        MainComponent main;
        explicit Host (const Folder& f) : Host (f.dir) {}
        explicit Host (const juce::File& dir)
        {
            main.stopTimer();
            main.engine.prepareForTests (48000, 256);
            main.openSessionFolder (dir);
        }
        ~Host() { forget (main); }
        juce::String status() { return main.statusLabel.getText(); }
    };

    // The drive goes: the folder is no longer at its path, and it comes back
    // when the test ends so the fixture can clean up.
    struct PulledDrive
    {
        juce::File dir, away;
        explicit PulledDrive (const Folder& f)
            : dir (f.dir), away (f.dir.getSiblingFile ("unplugged"))
        { dir.moveFileTo (away); }
        void reconnect() { if (away.isDirectory()) away.moveFileTo (dir); }
        ~PulledDrive() { reconnect(); }
    };

    struct MenuRow { int id; juce::String text; bool enabled; };

    static std::vector<MenuRow> walk (const juce::PopupMenu& menu)
    {
        std::vector<MenuRow> rows;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            const auto& item = it.getItem();
            rows.push_back ({ item.itemID, item.text, item.isEnabled });
            if (item.subMenu != nullptr)
                for (const auto& inner : walk (*item.subMenu)) rows.push_back (inner);
        }
        return rows;
    }

    // ---------------------------------------------------------------------

    void testHandoffStatus()
    {
        beginTest ("A handoff without its DAW projects says so on screen and in the manifest");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            // A clean capture report, so nothing else can take the verdict.
            f.dir.getChildFile ("session.report.json").replaceWithText (
                "{\"sha256Pending\":false,\"missedSamples\":0,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\""
                + hashing::fileSha256 (f.track (1)) + "\"]}]}");
            const auto handoff = f.dir.getSiblingFile ("Show Handoff");
            Host host (f);
            auto& clips = host.main.engine.clipsFor (0);
            expect (! clips.empty());
            clips[0].fileLengthSamples += 480000;   // past the end of the media, so export refuses

            expect (host.main.sessionJobs.reserve());
            host.main.completeShowHandoff (f.dir, handoff);
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));

            expect (! host.status().contains ("Verified handoff ready"), host.status());
            expect (host.status().contains ("not written"), host.status());
            expect (host.status().contains ("Show.RPP"), host.status());
            expect (! handoff.getChildFile ("Show.RPP").exists());
            expect (handoff.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "the copy itself must survive");
            const auto manifest = juce::JSON::parse (handoff.getChildFile ("Handoff Manifest.json"));
            expect (manifest.getProperty ("generatedFilesWarning", {}).toString().contains ("not written"),
                    "the manifest has no record of the missing projects");
            expect (! handoff.getChildFile ("HANDOFF INCOMPLETE.txt").exists(), "the copy is complete");
        }

        beginTest ("A complete handoff still reads as verified");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            f.dir.getChildFile ("session.report.json").replaceWithText (
                "{\"sha256Pending\":false,\"missedSamples\":0,\"tracks\":[{\"files\":[\"Track_01.wav\"],\"sha256\":[\""
                + hashing::fileSha256 (f.track (1)) + "\"]}]}");
            const auto handoff = f.dir.getSiblingFile ("Show Handoff");
            Host host (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeShowHandoff (f.dir, handoff);
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            expect (host.status().startsWith ("Verified handoff ready"), host.status());
            expect (handoff.getChildFile ("Show.RPP").existsAsFile());
            expect (! juce::JSON::parse (handoff.getChildFile ("Handoff Manifest.json"))
                          .hasProperty ("generatedFilesWarning"));
        }
    }

    void testHandoffLines()
    {
        beginTest ("The handoff's line names everything the handoff lacks");
        const juce::File dest ("/Volumes/Out/Show Handoff");
        const auto line = [&] (bool ok, bool capture, const juce::String& generated)
        {
            showhandoff::Result result;
            result.ok = ok;
            result.message = "the reason";
            result.captureReportWarning = capture;
            result.generatedWarning = generated;
            return showhandoff::statusLine (result, dest);
        };
        expectEquals (line (true, false, {}), juce::String ("Verified handoff ready: /Volumes/Out/Show Handoff"));
        expectEquals (line (true, true, {}), juce::String ("! Handoff copy verified; CAPTURE REPORT NEEDS REVIEW: /Volumes/Out/Show Handoff"));
        expectEquals (line (true, false, "Show.RPP not written: x"), juce::String ("! Handoff copy verified; Show.RPP not written: x: /Volumes/Out/Show Handoff"));
        const auto both = line (true, true, "Show.RPP not written: x");
        expect (both.contains ("NEEDS REVIEW") && both.contains ("not written"), both);
        expectEquals (line (false, true, "y"), juce::String ("Handoff incomplete at /Volumes/Out/Show Handoff -- the reason"));
    }

    void testSoundcheckPatchStaysOut()
    {
        beginTest ("A template taken during Virtual Soundcheck holds the show patch");
        {
            Folder f; writeSavedShow (f, 2);
            Host host (f);
            auto& engine = host.main.engine;
            engine.setTrackInputRouting (0, 3);  engine.setTrackOutputRouting (0, 7);
            engine.setTrackInputRouting (1, 4);  engine.setTrackOutputRouting (1, 8);
            vsc::repatch (engine);
            expectEquals (engine.getRecorder().getTrack (0).outputRouting.load(), 3);

            const auto text = host.main.sessionTemplateJson ("Festival");
            const auto parsed = juce::JSON::parse (text);
            auto* strips = parsed.getProperty ("strips", {}).getArray();
            expect (strips != nullptr && strips->size() == 2);
            if (strips != nullptr && strips->size() == 2)
            {
                expectEquals ((int) (*strips)[0].getProperty ("outRoute", -9), 7, "the template kept the soundcheck patch");
                expectEquals ((int) (*strips)[1].getProperty ("outRoute", -9), 8);
            }

            const auto file = f.dir.getSiblingFile ("Festival.zftemplate");
            expect (file.replaceWithText (text));
            host.main.applySessionTemplate (file);
            expectEquals (engine.getRecorder().getTrack (0).outputRouting.load(), 7);
            expect (! vsc::hasSavedPatch (engine), "a new session must start with VSC off");
        }

        beginTest ("A template with VSC off holds the live patch");
        {
            Folder f; writeSavedShow (f, 1);
            Host host (f);
            host.main.engine.setTrackOutputRouting (0, 5);
            const auto parsed = juce::JSON::parse (host.main.sessionTemplateJson ("Plain"));
            expectEquals ((int) (*parsed.getProperty ("strips", {}).getArray())[0].getProperty ("outRoute", -9), 5);
        }

        beginTest ("A handoff's channel map shows the show patch from a session saved with VSC on");
        {
            Folder f; writeSavedShow (f, 2);
            Host host (f);
            auto& engine = host.main.engine;
            engine.setTrackInputRouting (0, 3);  engine.setTrackOutputRouting (0, 7);
            engine.setTrackInputRouting (1, 4);  engine.setTrackOutputRouting (1, -1);
            vsc::repatch (engine);
            expect (engine.saveSessionMixTo (f.dir));

            const auto rows = juce::StringArray::fromLines (showhandoff::channelMapCsv (f.dir));
            expect (rows[1].endsWith (",4,8,no"), rows[1]);
            expect (rows[2].endsWith (",5,,no"), "a strip with no output of its own stays blank: " + rows[2]);

            vsc::restore (engine);
            expect (engine.saveSessionMixTo (f.dir));
            expect (juce::StringArray::fromLines (showhandoff::channelMapCsv (f.dir))[1].endsWith (",4,8,no"));
        }
    }

    void testMissingDrive()
    {
        beginTest ("With the drive gone, replacing the session asks and says how to keep the work");
        {
            Folder f, other ("Other"); writeSavedShow (f); writeSavedShow (other);
            Host host (f);
            PulledDrive drive (f);
            int prompts = 0;
            juce::String text;
            bool answer = false;
            host.main.leaveMissingSessionPromptForTests = [&] (const juce::String& t)
            { ++prompts; text = t; return answer; };

            bool replaced = false;
            host.main.confirmSessionReplacement ([&] { replaced = true; });
            expectEquals (prompts, 1, "unsaved work was replaced without a question");
            expect (! replaced, "cancelling must keep the session");
            expect (text.contains (f.dir.getFullPathName()), text);
            expect (text.containsIgnoreCase ("reconnect the drive"), text);

            answer = true;
            host.main.confirmSessionReplacement ([&] { replaced = true; });
            expect (replaced, "the engineer chose to go on");
        }

        beginTest ("A Finder open of another session asks too, and Cancel leaves everything in place");
        {
            Folder f, other ("Other"); writeSavedShow (f); writeSavedShow (other);
            Host host (f);
            PulledDrive drive (f);
            int prompts = 0;
            host.main.leaveMissingSessionPromptForTests = [&] (const juce::String&) { ++prompts; return false; };

            expect (host.main.openSessionDocument (other.project(), true));
            expectEquals (prompts, 1);
            expect (host.main.engine.getActiveSessionDir() == f.dir, "the session was replaced");
            expect (host.main.engine.mixerBelongsTo (f.dir));
            expectEquals (host.main.engine.getRecorder().getNumTracks(), 2);
        }

        beginTest ("Quit with the drive gone asks about the session, not about no session");
        {
            Folder f; writeSavedShow (f);
            Host host (f);
            PulledDrive drive (f);
            int prompts = 0;
            host.main.leaveMissingSessionPromptForTests = [&] (const juce::String&) { ++prompts; return false; };
            host.main.confirmAndQuit();
            expectEquals (prompts, 1, "Quit took the open session for no session at all");
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        }

        beginTest ("Close Session stays available with the drive gone, and asks");
        {
            Folder f; writeSavedShow (f);
            Host host (f);
            PulledDrive drive (f);
            bool found = false;
            for (const auto& row : walk (host.main.getMenuForIndex (0, {})))
                if (row.id == 6) { found = true; expect (row.enabled, "Close Session greyed out"); }
            expect (found);

            int prompts = 0;
            host.main.leaveMissingSessionPromptForTests = [&] (const juce::String&) { ++prompts; return false; };
            host.main.closeSession();
            expectEquals (prompts, 1);
            expect (host.main.engine.getActiveSessionDir() == f.dir);
            juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        }

        beginTest ("Export, Handoff and Move name the missing drive, not a missing session");
        {
            Folder f, out ("Out"); writeSavedShow (f);
            Host host (f);
            PulledDrive drive (f);
            const auto expectNamed = [&] (const juce::String& what)
            {
                expect (host.status().contains ("reconnect its drive"), what + ": " + host.status());
                expect (host.status().contains (f.dir.getFullPathName()), what + ": " + host.status());
                host.main.statusLabel.setText ({}, juce::dontSendNotification);
            };
            host.main.startExportTracksTo (out.dir, { 0 }, {});        expectNamed ("raw export");
            host.main.onExportAudio();                                  expectNamed ("export dialog");
            host.main.exportTimelineCsv();                              expectNamed ("timeline");
            host.main.exportDawProject (projectexport::Format::Reaper); expectNamed ("project");
            host.main.exportShowHandoff();                              expectNamed ("handoff");
            host.main.relocateActiveSession();                          expectNamed ("move");
            host.main.onBounceStereoMix();                              expectNamed ("mix");
            expect (! host.main.sessionIoBusy.load(), "a refused job kept its reservation");
        }

        beginTest ("The ordinary question comes back with the drive");
        {
            Folder f; writeSavedShow (f);
            Host host (f);
            PulledDrive drive (f);
            expect (host.main.sessionFolderIsMissing());
            drive.reconnect();
            expect (! host.main.sessionFolderIsMissing());
        }

        beginTest ("A session pinned but never loaded has nothing to lose, so nothing asks");
        {
            Folder f; writeSavedShow (f);
            const juce::File gone = f.dir.getSiblingFile ("never mounted");
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setActiveSessionDir (gone);
            int prompts = 0;
            host.leaveMissingSessionPromptForTests = [&] (const juce::String&) { ++prompts; return false; };
            bool replaced = false;
            host.confirmSessionReplacement ([&] { replaced = true; });
            expectEquals (prompts, 0);
            expect (replaced);
            forget (host);
        }
    }

    void testCopiesThatDoNotOpen()
    {
        // A journal that cannot be replayed makes the copy refuse to open, and
        // only the copy: the source is already open by then.
        const auto breakJournal = [] (const juce::File& session)
        {
            const auto journal = session.getChildFile ("Session File Backups/reorder_broken");
            journal.createDirectory();
            journal.getChildFile ("journal.json").replaceWithText ("not json");
        };

        beginTest ("Save As says so when the copy would not open");
        {
            Folder f; writeSavedShow (f); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);
            breakJournal (f.dir);
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            expect (host.status().contains ("did not open"), host.status());
            expect (host.status().contains ("incomplete file move"), "the reason was lost: " + host.status());
            expect (host.main.engine.getActiveSessionDir() == f.dir, "the app stays on the session it had");
        }

        beginTest ("Move says so when the session would not open at its new home");
        {
            Folder f, parent ("Parent"); writeSavedShow (f); expect (writeAudio (f.track (1)));
            Host host (f);
            breakJournal (f.dir);
            expect (host.main.sessionJobs.reserve());
            host.main.completeRelocate (f.dir, parent.dir);
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            expect (host.status().contains ("did not open"), host.status());
            expect (host.status().contains (parent.dir.getChildFile ("Show").getFullPathName()), host.status());
        }

        beginTest ("A carried edit stays this session's when the copy does not open");
        {
            Folder f, copy ("Copy"); writeSavedShow (f); writeSavedShow (copy);
            Host host (f);
            breakJournal (copy.dir);
            host.main.carryMixerInto (copy.dir);
            expect (host.main.openCopiedSession (f.dir, copy.dir, true).contains ("incomplete file move"));
            expect (host.main.engine.mixerBelongsTo (f.dir), "memory still holds the old session's edits");
        }

        beginTest ("Move's line carries everything the engineer has to know");
        {
            const juce::File from ("/Volumes/Old/Show"), to ("/Volumes/New/Show");
            const auto line = [&] (bool carried, bool oldRemains, const juce::String& refused)
            { return MainComponent::moveStatusLine (from, to, carried, oldRemains, refused); };
            expectEquals (line (true, false, {}), juce::String ("Session moved to /Volumes/New/Show"));
            expect (line (false, false, {}).contains ("without its unsaved changes"));
            expect (line (true, true, {}).contains ("could not be fully removed: /Volumes/Old/Show"));
            const auto both = line (false, true, {});
            expect (both.contains ("without its unsaved changes") && both.contains ("could not be fully removed"),
                    "the old folder's message hid the lost edits: " + both);
            const auto refused = line (true, false, "Session has an incomplete file move");
            expect (refused.startsWith ("! ") && refused.contains ("did not open: Session has an incomplete file move"), refused);
        }
    }

    void testCopyChecksTheFile()
    {
        beginTest ("A session copy whose tail never reached the disk fails");
        {
            Folder f; writeSavedShow (f); expect (writeAudio (f.track (1)));
            // Smaller than the stream's buffer, so all of it goes out in the flush
            // whose result the stream ignores.
            expect (f.dir.getChildFile ("Notes.txt").replaceWithText (juce::String::repeatedString ("x", 20000)));
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);

            rlimit before {};
            expect (getrlimit (RLIMIT_FSIZE, &before) == 0);
            const auto previous = signal (SIGXFSZ, SIG_IGN);
            rlimit small = before; small.rlim_cur = 12000;
            expect (setrlimit (RLIMIT_FSIZE, &small) == 0);

            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            const bool finished = pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); });

            setrlimit (RLIMIT_FSIZE, &before);
            signal (SIGXFSZ, previous);

            expect (finished);
            expect (host.status().contains ("failed"), "a short copy reported success: " + host.status());
            expect (host.main.engine.getActiveSessionDir() == f.dir, "the app moved onto a short copy");
            expect (! dest.getChildFile ("Notes.txt").exists(), "a short copy was left behind");
        }
    }

    void testUnloadedSession()
    {
        beginTest ("A session pinned but not loaded makes no handoff or project from empty memory");
        {
            Folder f; writeSavedShow (f); expect (writeAudio (f.track (1)));
            const auto handoff = f.dir.getSiblingFile ("Show Handoff");
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setActiveSessionDir (f.dir);

            expect (host.sessionJobs.reserve());
            host.completeShowHandoff (f.dir, handoff);
            expect (! host.sessionIoBusy.load(), "a refused handoff kept its reservation");
            expect (host.statusLabel.getText().contains ("not loaded"), host.statusLabel.getText());
            expect (! handoff.exists(), "a handoff folder was made");

            host.exportShowHandoff();
            expect (host.chooser == nullptr, "the folder chooser opened for a handoff that cannot run");
            host.statusLabel.setText ({}, juce::dontSendNotification);
            host.exportDawProject (projectexport::Format::Reaper);
            expect (host.statusLabel.getText().contains ("not loaded"), host.statusLabel.getText());
            expect (! host.sessionIoBusy.load());
            expect (! f.dir.getChildFile ("Export Files").exists(), "an empty project was written");
            host.statusLabel.setText ({}, juce::dontSendNotification);
            host.exportTimelineCsv();
            expect (host.statusLabel.getText().contains ("not loaded"), host.statusLabel.getText());
            expect (host.chooser == nullptr);
            forget (host);
        }
    }

    void testBackupsAreFound()
    {
        const auto snapshot = [] (const juce::File& session, const juce::String& name, int strips = 2)
        {
            const auto folder = session.getChildFile ("Session File Backups").getChildFile (name);
            folder.createDirectory();
            juce::Array<juce::var> list;
            for (int i = 0; i < strips; ++i) list.add (juce::var (new juce::DynamicObject()));
            auto* root = new juce::DynamicObject();
            root->setProperty ("trackCount", strips);
            root->setProperty ("strips", list);
            folder.getChildFile ("session_mix.json").replaceWithText (juce::JSON::toString (juce::var (root)));
            return folder;
        };
        constexpr auto seq = "_0000000000000000000";

        beginTest ("A backup made under the session's earlier name is offered");
        {
            Folder f ("Renamed");
            const auto older = snapshot (f.dir, juce::String ("Old Name_2026-10-01_10-00-00") + seq);
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == older);
        }

        beginTest ("The newest backup wins by its time, not by the name it carries");
        {
            Folder f ("Renamed");
            snapshot (f.dir, juce::String ("Zebra_2026-09-01_10-00-00") + seq);
            const auto newest = snapshot (f.dir, juce::String ("Alpha_2026-10-01_10-00-00") + seq);
            snapshot (f.dir, juce::String ("Renamed_2026-08-01_10-00-00") + seq);
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == newest);
        }

        beginTest ("A comma or semicolon in the folder name does not hide its backups");
        for (const auto* name : { "Late, Show", "Late; Show" })
        {
            Folder f (name);
            const auto good = snapshot (f.dir, juce::String (name) + "_2026-10-01_10-00-00" + seq);
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == good, name);
        }

        beginTest ("A timed session name and the older backup names still sort by the snapshot's own time");
        {
            Folder f ("Session_2026-01-01_00-00-00");
            snapshot (f.dir, juce::String ("Session_2026-12-31_23-59-59_2026-02-01_10-00-00") + seq);
            const auto newest = snapshot (f.dir, "Session_2026-01-01_00-00-00_2026-03-01_10-00-00(2)");
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == newest);
        }

        beginTest ("Same second, higher sequence is newer");
        {
            Folder f ("Show");
            snapshot (f.dir, "Show_2026-10-01_10-00-00_0000000000000000001");
            const auto later = snapshot (f.dir, "Show_2026-10-01_10-00-00_0000000000000000002");
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == later);
        }

        beginTest ("Only real snapshots count: a journal or the kept originals are never offered");
        {
            Folder f ("Show");
            snapshot (f.dir, "reorder_" + juce::Uuid().toString());
            snapshot (f.dir, "Before Feature Removal");
            snapshot (f.dir, "Interrupted Punch");
            snapshot (f.dir, "Unspliced Punch 2026-10-01 10-00-00");
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == juce::File());
        }

        beginTest ("A backup with no strips is passed over for the one before it");
        {
            Folder f ("Show");
            const auto good = snapshot (f.dir, juce::String ("Other_2026-09-01_10-00-00") + seq);
            snapshot (f.dir, juce::String ("Show_2026-10-01_10-00-00") + seq, 0);
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") == good);
        }

        beginTest ("A damaged mix after a rename is restored from a backup under the old name");
        {
            Folder f ("Renamed"); writeSavedShow (f, 2);
            const auto good = snapshot (f.dir, juce::String ("Old Name_2026-10-01_10-00-00") + seq);
            const auto mix = f.dir.getChildFile ("session_mix.json");
            expect (mix.replaceWithText ("{ damaged"));
            const auto damaged = sessionrepair::quarantineDamagedFiles (f.dir);
            expectEquals ((int) damaged.size(), 1);
            expect (damaged[0].backup == good);
            expect (sessionrepair::restoreFromBackups (f.dir, damaged).isEmpty());
            expectEquals (mix.loadFileAsString(), good.getChildFile ("session_mix.json").loadFileAsString());
        }
    }

    void testOpeningAFolderThatIsNoSession()
    {
        beginTest ("File > Open refuses a folder that is no session and touches nothing");
        {
            Folder show, plain ("Desktop", false);
            writeSavedShow (show);
            const auto notes = plain.dir.getChildFile ("notes.txt");
            expect (notes.replaceWithText ("mine"));
            Host host (show);

            host.main.completeOpenFolder (plain.dir);
            expect (host.status().contains ("not a session folder"), host.status());
            expect (host.main.engine.getActiveSessionDir() == show.dir, "the open session was replaced");
            expect (host.main.engine.mixerBelongsTo (show.dir));
            expectEquals (host.main.engine.getRecorder().getNumTracks(), 2);
            expectEquals (plain.dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*"), 1,
                          "something was written into the folder");
        }

        beginTest ("The Welcome dialog keeps the reason a folder did not open");
        {
            Folder plain ("Desktop", false);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.openSessionFromWelcome (plain.dir);
            expect (host.statusLabel.getText().contains ("not a session folder"), host.statusLabel.getText());
            forget (host);
        }

        beginTest ("Everything a real session can look like still opens");
        {
            {
                Folder justProject ("New", false);
                expect (justProject.project().replaceWithText ("{}"));
                Host host (justProject);
                expect (host.main.engine.getActiveSessionDir() == justProject.dir, "a new session with no audio");
            }
            {
                Folder legacy ("Legacy", false);
                expect (writeAudio (legacy.dir.getChildFile ("Track_01.wav")));
                Host host (legacy);
                expect (host.main.engine.getActiveSessionDir() == legacy.dir, "takes in the session root");
            }
            {
                Folder take ("Session_2026", true);
                expect (writeAudio (take.track (1)));
                Host host (take);
                expect (host.main.engine.getActiveSessionDir() == take.dir, "a take that was never saved");
            }
            {
                Folder damaged ("Damaged", false);
                expect (damaged.dir.getChildFile ("session_mix.json").replaceWithText ("{ damaged"));
                MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
                host.damagedFilesPromptForTests = [] (const auto&) {};
                host.openSessionFolder (damaged.dir);
                expect (host.engine.getActiveSessionDir() == damaged.dir, "a damaged document is still a session");
                forget (host);
            }
        }
    }

    void testFirstSaveIsReported()
    {
        beginTest ("A CSV or console session whose first save failed says so");
        {
            Folder root ("Sessions", false), input ("Input", false);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            auto* props = host.engine.getAppProps();
            props->setValue ("sessionsRoot", root.dir.getFullPathName());
            props->saveIfNeeded();
            const juce::ScopeGuard restoreRoot { [&]
            {
                props->removeValue ("sessionsRoot");
                props->saveIfNeeded();
                forget (host);
            } };
            const auto csv = input.dir.getChildFile ("Channels.csv");
            expect (csv.replaceWithText ("Kick\nSnare\n"));

            host.completeSessionFromCsv (csv);
            expect (! host.statusLabel.getText().contains ("WARNING"), host.statusLabel.getText());
            expect (host.statusLabel.getText().contains ("from Channels.csv"), host.statusLabel.getText());

            // An autosave still pending from before the chooser closed.
            host.pendingMetadataSaves = 1;
            host.completeSessionFromCsv (csv);
            host.pendingMetadataSaves = 0;
            expect (host.statusLabel.getText().contains ("was not saved"), host.statusLabel.getText());
            expect (host.statusLabel.getText().contains ("another save is still finishing"), host.statusLabel.getText());

            host.pendingMetadataSaves = 1;
            host.buildSessionFromConsole ({ { 1, "Kick" } }, {}, 0, "Console ");
            host.pendingMetadataSaves = 0;
            expect (host.statusLabel.getText().contains ("was not saved"), host.statusLabel.getText());

            host.buildSessionFromConsole ({ { 1, "Kick" } }, {}, 0, "Second ");
            expect (! host.statusLabel.getText().contains ("WARNING"), host.statusLabel.getText());
        }

        beginTest ("Work that could not be saved before a rolling take took over is reported");
        {
            Folder held, take ("Take"); writeSavedShow (held); writeSavedShow (take);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (held.dir), 0);
            EngineStatus status;
            status.source = "daemon";
            status.recording = true;
            status.sessionPath = take.dir.getFullPathName();
            status.sampleRate = 48000.0;

            host.pendingMetadataSaves = 1;
            host.adoptDaemonTakeSession (status);
            host.pendingMetadataSaves = 0;
            expect (host.statusLabel.getText().contains ("could not be saved"), host.statusLabel.getText());
            expect (host.statusLabel.getText().contains (held.dir.getFileName()), host.statusLabel.getText());
            forget (host);
        }
    }

    void testManyTemplates()
    {
        beginTest ("More templates than the menu holds never land on another command");
        {
            Folder dir ("Templates", false);
            for (int i = 0; i < 60; ++i)
                expect (dir.dir.getChildFile (juce::String::formatted ("Template %02d.zftemplate", i))
                               .replaceWithText ("{}"));
            MainComponent host; host.stopTimer();
            host.templatesDirForTests = dir.dir;

            int offered = 0, asDefault = 0;
            juce::String note;
            for (const auto& row : walk (host.getMenuForIndex (0, {})))
            {
                if (row.text.contains ("more not shown")) note = row.text;
                if (! row.text.startsWith ("Template ")) continue;
                if (row.id >= 2000) { ++asDefault; expect (row.id <= 2099); }
                else { ++offered; expect (row.id >= 200 && row.id <= 249, row.text); }
            }
            expectEquals (offered, 50);
            expectEquals (asDefault, 50);
            expect (note.startsWith ("10 more"), note);
            expect (host.getMenuCommands().conflicts().isEmpty());
        }

        beginTest ("The template list keeps one order from one menu to the next");
        {
            Folder dir ("Templates", false);
            for (const auto* name : { "Zebra", "Alpha", "Mid" })
                expect (dir.dir.getChildFile (juce::String (name) + ".zftemplate").replaceWithText ("{}"));
            MainComponent host; host.stopTimer();
            host.templatesDirForTests = dir.dir;
            const auto list = host.listSessionTemplates();
            expectEquals (list[0].getFileNameWithoutExtension(), juce::String ("Alpha"));
            expectEquals (list[2].getFileNameWithoutExtension(), juce::String ("Zebra"));
        }
    }

    static bool writeStereoAudio (const juce::File& file, float left, float right)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, 2, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (2, 4800);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), left, 4800);
        juce::FloatVectorOperations::fill (audio.getWritePointer (1), right, 4800);
        return writer->writeFromAudioSampleBuffer (audio, 0, 4800);
    }

    static juce::String savedName (const juce::File& session, int strip)
    {
        const auto mix = juce::JSON::parse (session.getChildFile ("session_mix.json"));
        return mix["strips"][strip]["name"].toString();
    }

    void testCopiesKeepWhatChangedDuringThem()
    {
        beginTest ("Save As keeps the mixer edits made while the copy runs");
        {
            Folder f; writeSavedShow (f); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            host.main.engine.setTrackName (0, "Renamed during the copy");   // MIXER stays live
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            expect (host.main.engine.getActiveSessionDir() == dest, "setup: the copy opened: " + host.status());
            expectEquals (host.main.engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("Renamed during the copy"),
                          "the copy opened without the rename made while it ran");
            expectEquals (savedName (dest, 0), juce::String ("Renamed during the copy"), "the copy's saved mix lacks the rename");
        }

        beginTest ("Move keeps the mixer edits made while the move runs");
        {
            Folder f, parent ("Parent"); writeSavedShow (f); expect (writeAudio (f.track (1)));
            Host host (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeRelocate (f.dir, parent.dir);
            host.main.engine.setTrackName (1, "Renamed during the move");
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            const auto moved = parent.dir.getChildFile (f.dir.getFileName());
            expect (host.main.engine.getActiveSessionDir() == moved, "setup: the session moved: " + host.status());
            expectEquals (host.main.engine.getRecorder().getTrack (1).getNameThreadSafe(), juce::String ("Renamed during the move"),
                          "the moved session opened without the rename made while it moved");
            expectEquals (savedName (moved, 1), juce::String ("Renamed during the move"));
        }

        beginTest ("Save As says it waits for the take's hashes rather than copying");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            f.dir.getChildFile ("session.report.json").replaceWithText (
                R"({"sha256Pending":true,"stoppedAt":"test","tracks":[{"files":["Track_01.wav"]}]})");
            juce::WaitableEvent held;
            MultitrackRecorder::resumeReportHashes (f.dir, {}, [&held] { held.wait (20000); });
            Host host (f);
            const auto dest = f.dir.getSiblingFile ("Copy");
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            expect (pumpUntil ([&] { return host.status().contains ("hash"); }, 5000),
                    "the status never said what the copy waits for: " + host.status());
            expect (! host.status().contains ("(copying session)"), host.status());
            held.signal();
            expect (pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }));
            expect (host.status().contains ("Saved As"), host.status());
        }
    }

    void testQuestionsAnsweredAfterASwitch()
    {
        const auto answerExport = [] (Host& host, const std::function<void()>& meanwhile,
                                      ExportAudioContent::Source source)
        {
            ExportAudioContent::ResultCallback answer;
            ExportAudioDialog::launchForTests = [&answer] (ExportAudioContent::ResultCallback callback) { answer = std::move (callback); };
            host.main.onExportAudio();
            ExportAudioDialog::launchForTests = nullptr;
            if (answer == nullptr) return false;
            meanwhile();
            // A wrong export then stops at its own guard, before any folder picker.
            if (! host.main.sessionJobs.reserve()) return false;
            ExportAudioContent::Result choice;
            choice.source = source;
            choice.rows = { 0 };
            answer (choice);
            host.main.sessionJobs.release();
            return true;
        };

        beginTest ("Export Audio confirmed after another session opened exports nothing");
        {
            Folder a ("A"), b ("B");
            writeSavedShow (a); writeSavedShow (b);
            expect (writeAudio (a.track (1)) && writeAudio (b.track (1)));
            Host host (a);
            expect (answerExport (host, [&] { host.main.openSessionFolder (b.dir); }, ExportAudioContent::Source::editedTracks),
                    "setup: the dialog opened: " + host.status());
            expect (host.status().contains ("session changed"), "the export went on in the other session: " + host.status());
        }

        beginTest ("Export Songs confirmed after another session opened exports nothing");
        {
            Folder a ("A"), b ("B");
            writeSavedShow (a, 1); writeSavedShow (b, 1);
            expect (writeAudio (a.track (1)) && writeAudio (b.track (1)));
            Host host (a);
            host.main.engine.getMarkers().drop (0, "Opener");
            ExportSongsContent::ResultCallback answer;
            ExportSongsDialog::launchForTests = [&answer] (ExportSongsContent::ResultCallback callback) { answer = std::move (callback); };
            host.main.exportSongs();
            ExportSongsDialog::launchForTests = nullptr;
            expect (answer != nullptr, "setup: the dialog opened: " + host.status());
            host.main.openSessionFolder (b.dir);
            host.main.engine.getMarkers().drop (0, "Other");
            songexport::Choice choice;
            choice.songs = { { 0, 4800, "Opener" } };
            choice.numbers = { 1 };
            if (answer != nullptr) answer (choice);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); });
            expect (host.status().contains ("changed"), host.status());
            expect (! b.dir.getChildFile ("Export Files/Songs").exists(), "the first session's songs were cut from the second's audio");
        }

        beginTest ("Export Songs confirmed after the songs moved exports nothing");
        {
            Folder a; writeSavedShow (a, 1); expect (writeAudio (a.track (1)));
            Host host (a);
            host.main.engine.getMarkers().drop (0, "Opener");
            ExportSongsContent::ResultCallback answer;
            ExportSongsDialog::launchForTests = [&answer] (ExportSongsContent::ResultCallback callback) { answer = std::move (callback); };
            host.main.exportSongs();
            ExportSongsDialog::launchForTests = nullptr;
            expect (answer != nullptr, "setup: the dialog opened: " + host.status());
            host.main.engine.getMarkers().drop (2400, "Second");   // a desk or a take behind the dialog
            songexport::Choice choice;
            choice.songs = { { 0, 4800, "Opener" } };
            choice.numbers = { 1 };
            if (answer != nullptr) answer (choice);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); });
            expect (host.status().contains ("changed"), host.status());
            expect (! a.dir.getChildFile ("Export Files/Songs").exists(), "songs were cut at ranges that no longer exist");
        }

        beginTest ("New Session confirmed after another session opened leaves that session alone");
        {
            Folder a ("A"), b ("B");
            writeSavedShow (a); writeSavedShow (b);
            Host host (a);
            NewSessionDialog::ResultCallback answer;
            NewSessionDialog::launchForTests = [&answer] (NewSessionDialog::ResultCallback callback) { answer = std::move (callback); };
            host.main.launchNewSessionDialog();
            NewSessionDialog::launchForTests = nullptr;
            expect (answer != nullptr, "setup: the dialog opened");
            host.main.openSessionFolder (b.dir);
            NewSessionDialog::Result r;
            r.name = "Fresh";
            r.location = a.dir.getParentDirectory();
            if (answer != nullptr) answer (r);
            expect (host.main.engine.getActiveSessionDir() == b.dir, "the new session replaced the one opened behind its dialog");
            expect (! r.location.getChildFile ("Fresh").exists(), "a session was created anyway");
            expect (host.status().contains ("changed"), host.status());
        }
    }

    void testRestoreFindsTheProject()
    {
        beginTest ("Restore Backup brings a project back under another name, after a save landed");
        {
            Folder f ("Gig B");
            writeSavedShow (f);
            f.project().deleteFile();
            const auto project = f.dir.getChildFile ("Gig A.zfproj");   // a Save As copy keeps the source's name
            expect (project.replaceWithText (R"({"zynforgeSession":true,"setlist":[{"name":"Opener","samplePos":0}],"playlists":[]})"));
            expect (sessionbackup::writeSnapshot (f.dir).isDirectory(), "setup: a good backup");
            expect (project.replaceWithText ("{\"setlist\": [ {\"name\": "));

            Host host (f.dir.getSiblingFile ("none"));
            std::vector<sessionrepair::DamagedFile> offered;
            host.main.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            host.main.openSessionFolder (f.dir);
            expectEquals ((int) offered.size(), 1, "setup: the damage was reported");
            expect (host.main.saveSessionStateTo (f.dir), "setup: a save behind the prompt");
            expect (host.main.restoreDamagedFiles (f.dir, offered), host.status());
            expectEquals ((int) host.main.cues.size(), 1, "the backup's cues did not come back");
            expect (findSessionProj (f.dir).loadFileAsString().contains ("Opener"),
                    "the project the session opens is still the defaults: " + findSessionProj (f.dir).getFileName());
        }
    }

    void testRecordNeedsLocalStorage()
    {
        beginTest ("RECORD with the Local Storage drive gone names it instead of using the internal disk");
        {
            MainComponent host; host.stopTimer();
            testaudio::useSilentDevice (host.engine);
            host.engine.prepareForTests (48000, 256);
            auto* props = host.engine.getAppProps();
            expect (props != nullptr);
            if (props != nullptr)
            {
                const auto previousRoot = props->getValue ("sessionsRoot");
                const auto previousActive = props->getValue ("activeSessionDir");
                const juce::String absent ("/Volumes/zf-absent-" + juce::Uuid().toString() + "/Zynforge Sessions");
                props->setValue ("sessionsRoot", absent);
                props->removeValue ("activeSessionDir");
                props->saveIfNeeded();   // strip settings reload the file
                host.engine.setStripCount (1);
                host.engine.setTrackArmed (0, true);
                MainComponent::RecordStartPlan plan;
                expect (! host.prepareRecordStart (plan), "RECORD went ahead into " + plan.dir.getFullPathName());
                expect (host.recordRefusal.contains (absent), host.recordRefusal);
                props->setValue ("sessionsRoot", previousRoot);
                if (previousActive.isNotEmpty()) props->setValue ("activeSessionDir", previousActive);
                props->saveIfNeeded();
            }
            forget (host);
        }
    }

    void testRawExportOfAnUnlinkedPair()
    {
        beginTest ("Raw export of an unlinked stereo take writes each strip's own channel");
        {
            Folder f; writeSavedShow (f, 2);
            expect (writeStereoAudio (f.track (1), 0.25f, -0.5f));
            Host host (f);
            expectEquals (host.main.engine.getRecorder().getNumTracks(), 2, "setup: two mono strips");
            Folder out ("Out");
            const auto exportBoth = [&] (std::vector<int> rows)
            {
                for (const auto& old : out.dir.findChildFiles (juce::File::findFiles, false)) old.deleteFile();
                host.main.startExportTracksTo (out.dir, rows, {});
                pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); });
                juce::StringArray written;
                juce::AudioFormatManager formats; formats.registerBasicFormats();
                for (const auto& file : out.dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
                {
                    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                    float lo[1] {}, hi[1] {};
                    if (reader != nullptr) reader->readMaxLevels (0, 4800, lo[0], hi[0], lo[0], hi[0]);
                    written.add (file.getFileNameWithoutExtension().upToFirstOccurrenceOf (" ", false, false)
                                 + ":" + juce::String (reader != nullptr ? (int) reader->numChannels : 0)
                                 + ":" + juce::String (juce::roundToInt ((lo[0] + hi[0]) * 100.0f)));
                }
                written.sort (false);
                return written.joinIntoString (" ");
            };
            expectEquals (exportBoth ({ 0, 1 }), juce::String ("Track_01:1:50 Track_02:1:-100"),
                          "ticking both strips did not give each its own channel");
            expectEquals (exportBoth ({ 0 }), juce::String ("Track_01:1:50"), "the left strip carried the right one's audio");
            expectEquals (exportBoth ({ 1 }), juce::String ("Track_02:1:-100"));
        }
    }

    // ── 2026-11-11 audit ──────────────────────────────────────────────────

    static void dropModals()
    {
        juce::ModalComponentManager::getInstance()->cancelAllModalComponents();
        pumpUntil ([] { return juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0; }, 3000);
    }

    // What a take starting behind the open modals would close.
    static int closedByATake()
    {
        const int closed = closeModalsForTake();
        dropModals();
        return closed;
    }

    static juce::TextButton* findButton (juce::Component* in, const juce::String& text)
    {
        if (in == nullptr) return nullptr;
        if (auto* button = dynamic_cast<juce::TextButton*> (in); button != nullptr && button->getButtonText() == text)
            return button;
        for (auto* child : in->getChildren())
            if (auto* found = findButton (child, text)) return found;
        return nullptr;
    }

    // Points the sessions root at a test folder, and puts it back.
    struct SessionsRoot
    {
        juce::PropertiesFile* props;
        juce::String previous;
        SessionsRoot (MainComponent& host, const juce::File& root)
            : props (host.engine.getAppProps()), previous (props != nullptr ? props->getValue ("sessionsRoot") : juce::String())
        {
            if (props != nullptr) { props->setValue ("sessionsRoot", root.getFullPathName()); props->saveIfNeeded(); }
        }
        ~SessionsRoot()
        {
            if (props == nullptr) return;
            if (previous.isEmpty()) props->removeValue ("sessionsRoot"); else props->setValue ("sessionsRoot", previous);
            props->saveIfNeeded();
        }
    };

    // Stands in for the native folder panel.
    struct Picker
    {
        MainComponent& host;
        juce::String asked;
        std::function<void (const juce::File&)> answer;
        explicit Picker (MainComponent& m) : host (m)
        {
            host.sessionPickerForTests = [this] (const juce::String& what, std::function<void (const juce::File&)> a)
            { asked = what; answer = std::move (a); };
        }
        ~Picker() { host.sessionPickerForTests = nullptr; }
        void reply (const juce::File& f) { auto a = std::exchange (answer, nullptr); if (a) a (f); }
    };

    void testBackupSnapshotsAreNotSessions()
    {
        beginTest ("A backup snapshot is refused as a session, from File > Open and from Finder");
        {
            Folder f ("Gig"); writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto snapshot = sessionbackup::writeSnapshot (f.dir);
            expect (snapshot.isDirectory(), "setup: a snapshot");
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (snapshot) < 0, "the snapshot opened as a session");
            expect (host.statusLabel.getText().contains ("backup of Gig"), host.statusLabel.getText());
            expect (! host.openSessionDocument (findSessionProj (snapshot)), "a double-click on its project opened it");
            host.completeOpenFolder (snapshot);
            expect (host.engine.getActiveSessionDir() != snapshot, "File > Open made the snapshot the session");
            expect (! snapshot.getChildFile ("Session File Backups").exists(), "something was written into the snapshot");
            expect (! snapshot.getChildFile ("Audio Files").exists());
            forget (host);
        }
    }

    void testStripsAddedSinceTheLastSave()
    {
        beginTest ("Reopening after a crash brings back the strips recorded since the last save");
        {
            Folder f; writeSavedShow (f, 2);
            for (int t = 1; t <= 3; ++t) expect (writeAudio (f.track (t)));
            expect (writeStereoAudio (f.track (4), 0.25f, -0.5f));
            Host host (f);
            auto& recorder = host.main.engine.getRecorder();
            expectEquals (recorder.getNumTracks(), 5, "strips whose audio is on disk were left out");
            expectEquals (recorder.getTrack (0).getNameThreadSafe(), juce::String ("Saved 1"));
            expectEquals (recorder.getTrack (1).getNameThreadSafe(), juce::String ("Saved 2"));
            if (recorder.getNumTracks() == 5)
                expect (recorder.getTrack (3).isStereo.load(), "a stereo take came back as two loose strips");
            expect (host.status().contains ("after the last save"), "the engineer was not told: " + host.status());
        }

        beginTest ("RECORD writes the mixer into the session before the take rolls");
        {
            Folder f; writeSavedShow (f, 1);
            Host host (f);
            auto& engine = host.main.engine;
            engine.setStripCount (2);
            engine.setTrackName (1, "Audience");
            expect (engine.setTrackArmed (1, true));
            MainComponent::RecordStartPlan plan;
            plan.dir = f.dir; plan.numTracks = 2; plan.armed = 1;
            host.main.startLocalTake (plan);
            expect (engine.isRecording(), host.status());
            const auto mix = juce::JSON::parse (f.dir.getChildFile ("session_mix.json"));
            expectEquals ((int) mix.getProperty ("trackCount", 0), 2, "a crash now brings back one strip");
            if (auto* strips = mix.getProperty ("strips", {}).getArray(); strips != nullptr && strips->size() > 1)
                expectEquals (savedName (f.dir, 1), juce::String ("Audience"));
            engine.stopRecording();
        }

        beginTest ("A RECORD whose mixer write fails still rolls");
        {
            Folder f; writeSavedShow (f, 1);
            Host host (f);
            auto& engine = host.main.engine;
            expect (engine.setTrackArmed (0, true));
            expect (f.project().deleteFile());   // nothing to write the mixer into
            MainComponent::RecordStartPlan plan;
            plan.dir = f.dir; plan.numTracks = 1; plan.armed = 1;
            host.main.startLocalTake (plan);
            expect (engine.isRecording(), "the take did not start: " + host.status());
            engine.stopRecording();
        }
    }

    void testSessionModalsCloseForATake()
    {
        beginTest ("Session dialogs and prompts close when a take starts behind them");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            Host host (f);
            auto& main = host.main;
            main.engine.getMarkers().drop (0, "Opener");
            const auto expectClosed = [&] (const juce::String& what, std::function<void()> open)
            {
                dropModals();
                open();
                expect (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0,
                        what + ": setup, nothing opened: " + host.status());
                expectEquals (closedByATake(), 1, what + " stays in front of STOP");
            };
            expectClosed ("New Session", [&] { main.launchNewSessionDialog(); });
            expectClosed ("Export Audio", [&] { main.onExportAudio(); });
            expectClosed ("Export Songs", [&] { main.exportSongs(); });
            expectClosed ("Session Info", [&] { main.showSessionInfo(); });
            expectClosed ("Session Settings", [&] { SessionSettingsDialog::launch (main.engine, {}, {}); });
            expectClosed ("Welcome", [&] { WelcomeDialog::launch (f.dir.getParentDirectory(), 48000.0,
                                                                  CaptureFormat::Wav24, {}, {}, {}); });
            expectClosed ("Replace the session?", [&] { main.confirmSessionReplacement ([] {}); });
            expectClosed ("Close the session?", [&] { main.closeSession(); });
            expectClosed ("Folder missing", [&] { main.offerLeaveMissingSession ("Close Without Saving", [] {}); });
            expectClosed ("Auto-Save", [&] { main.showAutosaveSettings(); });
            expectClosed ("Template name", [&] { main.promptSaveSessionTemplate(); });
            expectClosed ("Continue from disk?", [&] { main.offerContinueFromDisk ("Save As", "x", [] {}, [] {}); });
            expectClosed ("Damaged files", [&] { main.reportDamagedFiles (f.dir, { sessionrepair::DamagedFile() }); });
            dropModals();
        }

        beginTest ("Session Settings does not float over other applications");
        {
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            SessionSettingsDialog::launch (host.engine, {}, {});
            auto* window = juce::ModalComponentManager::getInstance()->getModalComponent (0);
            expect (window != nullptr, "setup: the dialog opened");
            if (window != nullptr) expect (! window->isAlwaysOnTop(), "Session Settings stays above every window");
            dropModals();
            forget (host);
        }

        beginTest ("Session Format & Recording waits for the take like the other dialogs");
        {
            Folder f; writeSavedShow (f, 1);
            Host host (f);
            expect (host.main.engine.setTrackArmed (0, true));
            expect (host.main.engine.startRecording (f.dir));
            dropModals();
            host.main.menuItemSelected (255, 3);
            expectEquals (juce::ModalComponentManager::getInstance()->getNumModalComponents(), 0,
                          "a modal opened over the rolling take");
            expect (host.status().contains ("waits until the take has stopped"), host.status());
            dropModals();
            host.main.engine.stopRecording();
        }
    }

    void testFolderPanelsHoldNothing()
    {
        beginTest ("An open Save As, Move or Handoff panel leaves RECORD free and is named");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            Host host (f);
            Picker picker (host.main);
            for (int which = 0; which < 3; ++which)
            {
                picker.answer = nullptr;
                if (which == 0) host.main.onSaveSessionAs();
                if (which == 1) host.main.relocateActiveSession();
                if (which == 2) host.main.exportShowHandoff();
                expect (picker.answer != nullptr, "setup: the panel opened: " + host.status());
                expect (! host.main.sessionIoBusy.load(),
                        picker.asked + "'s open panel holds the session slot, so a desk or twin RECORD is refused");
                host.main.statusLabel.setText ({}, juce::dontSendNotification);
                host.main.onSaveSessionAs();
                expect (host.status().contains (picker.asked), "the refusal does not name the open panel: " + host.status());
                picker.reply ({});
                expect (! host.main.sessionIoBusy.load(), "a cancelled panel kept the slot");
                expect (host.main.openSessionPicker().isEmpty());
            }
        }

        beginTest ("Save As answered after a take started behind its panel copies nothing");
        {
            Folder f; writeSavedShow (f, 1);
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);
            Picker picker (host.main);
            host.main.onSaveSessionAs();
            expect (picker.answer != nullptr, "setup: the panel opened");
            expect (host.main.engine.setTrackArmed (0, true));
            if (host.main.engine.startRecording (f.dir))
            {
                picker.reply (dest);
                pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 5000);
                expect (! dest.exists(), "Save As copied a session under a rolling take");
                expect (host.status().contains ("take"), host.status());
                host.main.engine.stopRecording();
            }
            else expect (false, "setup: the take did not start");
        }

        beginTest ("Save As answered after another session opened behind its panel copies nothing");
        {
            Folder a ("A"), b ("B"); writeSavedShow (a, 1); writeSavedShow (b, 1);
            const auto dest = a.dir.getSiblingFile ("Copy");
            Host host (a);
            Picker picker (host.main);
            host.main.onSaveSessionAs();
            expect (picker.answer != nullptr, "setup: the panel opened");
            host.main.openSessionFolder (b.dir);
            picker.reply (dest);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 5000);
            expect (! dest.exists(), "Save As copied the first session and carried the second one's mixer into it");
            expect (host.main.engine.getActiveSessionDir() == b.dir);
            expect (host.status().contains ("changed"), host.status());
        }

        beginTest ("A mix bounce answered after another session opened bounces nothing");
        {
            Folder a ("A"), b ("B"); writeSavedShow (a, 1); writeSavedShow (b, 1);
            expect (writeAudio (a.track (1)) && writeAudio (b.track (1)));
            Host host (a);
            Picker picker (host.main);
            host.main.onBounceStereoMix();
            expect (picker.answer != nullptr, "setup: the panel opened");
            host.main.openSessionFolder (b.dir);
            const auto target = a.dir.getChildFile ("Export Files/A - Mix.wav");
            picker.reply (target);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (! target.exists(), "the second session's mix went into the file named for the first");
            expect (host.status().contains ("changed"), host.status());
        }

        beginTest ("New Session from CSV answered after another session opened leaves that session alone");
        {
            Folder a ("A"), b ("B"), root ("Sessions", false), input ("Input", false);
            writeSavedShow (a); writeSavedShow (b);
            const auto csv = input.dir.getChildFile ("Channels.csv");
            expect (csv.replaceWithText ("Kick\nSnare\n"));
            Host host (a);
            SessionsRoot sessions (host.main, root.dir);
            Picker picker (host.main);
            host.main.createSessionFromCsv();
            expect (picker.answer != nullptr, "setup: the panel opened");
            host.main.openSessionFolder (b.dir);
            picker.reply (csv);
            expect (host.main.engine.getActiveSessionDir() == b.dir, "the CSV session replaced the one opened behind its panel");
            expectEquals (root.dir.getNumberOfChildFiles (juce::File::findDirectories), 0, "a session was created anyway");
            expect (host.status().contains ("changed"), host.status());
        }
    }

    void testLongSessionNames()
    {
        beginTest ("A session name too long for the files derived from it is refused in plain words");
        {
            Folder root ("Sessions", false);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            NewSessionDialog::Result r;
            r.name = juce::String::repeatedString (juce::String::fromUTF8 ("\xd8\xb9"), 120);   // 240 bytes
            r.location = root.dir; r.sampleRate = 48000.0;
            const auto made = host.createSessionFolderStructure (r);
            expect (! made.isDirectory(), "a session was made that can never be saved");
            expect (host.sessionCreateFailureMessage().contains ("too long"), host.sessionCreateFailureMessage());
            expectEquals (root.dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories), 0);
            forget (host);
        }

        beginTest ("Saving into a folder renamed too long says why");
        {
            Folder f (juce::String::repeatedString ("x", 212));
            writeSavedShow (f, 1);
            Host host (f);
            expect (! host.main.saveSessionStateTo (f.dir), "setup: the save went through");
            expect (host.main.lastMetadataSaveError.contains ("too long"), host.main.lastMetadataSaveError);
        }

        beginTest ("Save As refuses a folder name too long to save into");
        {
            Folder f; writeSavedShow (f, 1);
            Host host (f);
            const auto dest = f.dir.getSiblingFile (juce::String::repeatedString ("y", 205));
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (! dest.exists(), "Save As made a copy whose folder name is too long");
            expect (host.status().contains ("too long"), host.status());
        }
    }

    void testReadOnlySessionAutosave()
    {
        beginTest ("A session on read-only media with nothing changed auto-saves quietly; a change still warns");
        {
            Folder f; writeSavedShow (f, 2);
            Host host (f);
            expect (host.main.saveSessionStateTo (f.dir), "setup: " + host.main.lastMetadataSaveError);
            expect (f.dir.setReadOnly (true, true));
            const juce::ScopeGuard writable { [&] { f.dir.setReadOnly (false, true); } };
            const auto autosave = [&]
            {
                if (auto* props = host.main.engine.getAppProps()) props->setValue ("autosaveMinutes", 1);
                host.main.lastAutosaveMs = juce::Time::getMillisecondCounter() - 120000;
                host.main.statusLabel.setText ({}, juce::dontSendNotification);
                host.main.serviceAutosave();
                pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 5000);
            };
            autosave();
            expect (! host.status().contains ("FAILED"), "an unchanged session failed to auto-save: " + host.status());
            host.main.engine.setTrackName (0, "Changed");
            autosave();
            expect (host.status().contains ("AUTO-SAVE FAILED"), "a change that cannot be saved went unreported: " + host.status());
            if (auto* props = host.main.engine.getAppProps()) props->setValue ("autosaveMinutes", 5);
        }
    }

    void testRemoveLastCaptureQuestion()
    {
        beginTest ("Remove last capture? holds nothing while it waits, and a take closes it");
        {
            Folder take;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setStripCount (1);
            expect (host.engine.setTrackArmed (0, true));
            expect (host.engine.startRecording (take.dir));
            host.engine.stopRecording();
            expect (host.engine.getLastCapture() == take.dir, "setup: no last capture");
            dropModals();
            host.removeLastCapture();
            expect (! host.sessionIoBusy.load(), "the question holds the session slot: RECORD and Quit are refused meanwhile");
            expectEquals (closedByATake(), 1, "the question stays in front of STOP");
            expect (take.dir.isDirectory());
            host.engine.forgetLastCapture();
            forget (host);
        }
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto previousTestConstruct = MainComponent::s_testConstruct;
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreConstruct { [previousTestConstruct]
        { MainComponent::s_testConstruct = previousTestConstruct; } };
        testHandoffStatus();
        testHandoffLines();
        testSoundcheckPatchStaysOut();
        testMissingDrive();
        testCopiesThatDoNotOpen();
        testCopyChecksTheFile();
        testUnloadedSession();
        testBackupsAreFound();
        testOpeningAFolderThatIsNoSession();
        testFirstSaveIsReported();
        testManyTemplates();
        testCopiesKeepWhatChangedDuringThem();
        testQuestionsAnsweredAfterASwitch();
        testRestoreFindsTheProject();
        testRecordNeedsLocalStorage();
        testRawExportOfAnUnlinkedPair();
        testBackupSnapshotsAreNotSessions();
        testStripsAddedSinceTheLastSave();
        testReadOnlySessionAutosave();
        testLongSessionNames();
        testSessionModalsCloseForATake();
        testFolderPanelsHoldNothing();
        testRemoveLastCaptureQuestion();
    }
};

static SessionFixTests sessionFixTests;
}
