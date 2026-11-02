// Session files on the way in and out: whose mixer gets written where, what
// happens to damaged files, and which folder an operation may touch.

#include "../UI/MainComponent.h"
#include "../UI/SessionMetadataWriter.h"
#include "../Audio/SessionBackup.h"
#include "../Audio/SessionMixFile.h"
#include "../Audio/SessionRemovedData.h"
#include "../Audio/SessionRepair.h"
#include "../Audio/TrackFileTransaction.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/SettingsFile.h"
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
class SessionSafetyTests final : public juce::UnitTest
{
public:
    SessionSafetyTests() : UnitTest ("Session safety", "zynforge") {}

    struct Folder
    {
        juce::File dir;
        explicit Folder (const juce::File& root = juce::File::getSpecialLocation (juce::File::tempDirectory))
            : dir (root.getChildFile ("zf-session-safety-" + juce::Uuid().toString()))
        { dir.createDirectory(); dir.getChildFile ("Audio Files").createDirectory(); }
        ~Folder() { dir.deleteRecursively(); }
        juce::File project() const { return dir.getChildFile (dir.getFileName() + ".zfproj"); }
        juce::File mix() const { return dir.getChildFile ("session_mix.json"); }
        juce::File settings() const { return dir.getChildFile ("session_settings.json"); }
        juce::File track (int index = 1) const
        { return dir.getChildFile (juce::String::formatted ("Audio Files/Track_%02d.wav", index)); }
    };

    static juce::String mixJson (int strips)
    {
        juce::Array<juce::var> arr;
        for (int i = 0; i < strips; ++i)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("index", i);
            o->setProperty ("uid", "strip-" + juce::String (i));
            o->setProperty ("name", "Saved " + juce::String (i + 1));
            arr.add (juce::var (o));
        }
        auto* root = new juce::DynamicObject();
        root->setProperty ("formatVersion", 2);
        root->setProperty ("trackCount", strips);
        root->setProperty ("strips", arr);
        return juce::JSON::toString (juce::var (root), true);
    }

    // A show saved by an earlier run: the mixer, its cue and its settings.
    static void writeSavedShow (const Folder& f, int strips = 3)
    {
        f.mix().replaceWithText (mixJson (strips));
        f.project().replaceWithText (R"({"zynforgeSession":true,"setlist":[{"name":"Opener","samplePos":0}],"playlists":[],"automation":[]})");
        f.settings().replaceWithText (R"({"captureFormat":6,"preRollSeconds":0,"sampleRate":48000.0})");
    }

    static juce::String savedFiles (const Folder& f)
    {
        return f.mix().loadFileAsString() + f.project().loadFileAsString() + f.settings().loadFileAsString();
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

    static bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 10000)
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

    // Empty rather than a crash when the strip is missing, which is the failure
    // several of these tests exist to catch.
    static juce::String stripName (MainComponent& host, int index)
    {
        return index < host.engine.getRecorder().getNumTracks()
             ? host.engine.getRecorder().getTrack (index).getNameThreadSafe() : juce::String();
    }

    // A host that launched with `dir` pinned from settings and nothing loaded.
    static void pinUnloaded (MainComponent& host, const juce::File& dir)
    {
        host.stopTimer();
        host.engine.prepareForTests (48000, 256);
        host.engine.setActiveSessionDir (dir);
    }

    static void autosaveDue (MainComponent& host)
    {
        if (auto* props = host.engine.getAppProps()) props->setValue ("autosaveMinutes", 1);
        host.lastAutosaveMs = juce::Time::getMillisecondCounter() - 120000;
    }

    static zynforge::EngineStatus daemonStatus (const juce::File& dir, int tracks)
    {
        zynforge::EngineStatus status;
        status.source = "daemon";
        status.recording = true;
        status.sessionPath = dir.getFullPathName();
        status.sampleRate = 48000.0;
        for (int i = 0; i < tracks; ++i)
        {
            zynforge::TrackStatus track;
            track.name = "Daemon " + juce::String (i + 1);
            track.armed = true;
            status.tracks.push_back (track);
        }
        status.numTracks = tracks;
        return status;
    }

    void testMixerOwnership()
    {
        beginTest ("A session pinned at launch with no mixer loaded is never written");
        {
            Folder f; writeSavedShow (f);
            const auto before = savedFiles (f);
            MainComponent host; pinUnloaded (host, f.dir);
            expectEquals (host.engine.getRecorder().getNumTracks(), 0);

            expect (! host.saveSessionStateTo (f.dir), "direct save wrote an unloaded session");
            expect (host.lastMetadataSaveError.isNotEmpty(), "a refused save must say why");

            bool completed = false, ok = true;
            host.enqueueMetadataSave (f.dir, {}, [&] (bool saved) { completed = true; ok = saved; });
            expect (pumpUntil ([&] { return completed; }));
            expect (! ok, "queued save wrote an unloaded session");
            expect (! host.saveSetlistToActiveSession (false), "cue save wrote an unloaded session");

            autosaveDue (host);
            host.serviceAutosave();
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            expect (host.statusLabel.getText().contains ("not loaded"), host.statusLabel.getText());

            host.requestUILayoutSave();
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            expectEquals (savedFiles (f), before, "a refused save still touched the session");
            host.engine.clearAllStripOverrides();
        }

        beginTest ("A failed reopen leaves the saved show untouched by autosave");
        {
            Folder f; writeSavedShow (f);
            const auto journal = f.dir.getChildFile ("Session File Backups/reorder_broken");
            expect (journal.createDirectory().wasOk());
            expect (journal.getChildFile ("journal.json").replaceWithText ("not json"));
            const auto before = savedFiles (f);

            MainComponent host; pinUnloaded (host, f.dir);
            expect (host.openSessionFolder (f.dir) < 0);
            expect (! host.engine.mixerBelongsTo (f.dir));
            autosaveDue (host);
            host.serviceAutosave();
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            expectEquals (savedFiles (f), before);
            host.engine.clearAllStripOverrides();
        }

        beginTest ("A reattached daemon take loads the session's mixer, and STOP's save keeps it");
        {
            Folder f; writeSavedShow (f, 4);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.adoptRollingDaemonTake (daemonStatus (f.dir, 4), juce::Time::currentTimeMillis());
            expect (host.engine.isRecording(), "the take must be reported as rolling");
            expectEquals (host.engine.getRecorder().getNumTracks(), 4);
            expectEquals (stripName (host, 2), juce::String ("Saved 3"));
            expect (host.engine.mixerBelongsTo (f.dir));
            expectEquals (host.engine.getActiveSessionDir().getFullPathName(), f.dir.getFullPathName());
            expectEquals ((int) host.cues.size(), 1, "the session's cues were not loaded");

            host.engine.setExternalRecording (false);
            expect (host.saveSessionStateTo (f.dir));
            expectEquals (sessionmix::stripCountIn (f.dir), 4);
            const auto setlist = juce::JSON::parse (f.project())["setlist"];
            expect (setlist.size() == 1 && setlist[0]["name"].toString() == "Opener", "the session's cues were lost");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A reattached take with nothing saved takes its strips from the daemon");
        {
            Folder f;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.adoptDaemonTakeSession (daemonStatus (f.dir, 2));
            expectEquals (host.engine.getRecorder().getNumTracks(), 2);
            expectEquals (stripName (host, 1), juce::String ("Daemon 2"));
            expect (host.saveSessionStateTo (f.dir));
            expectEquals (sessionmix::stripCountIn (f.dir), 2);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A session open when a rolling take is found is saved, then replaced");
        {
            Folder held, take; writeSavedShow (held, 2); writeSavedShow (take, 5);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (held.dir) == 0);
            expect (host.engine.mixerBelongsTo (held.dir));
            host.engine.setTrackName (0, "Edited before the daemon was found");
            host.adoptDaemonTakeSession (daemonStatus (take.dir, 5));
            expect (held.mix().loadFileAsString().contains ("Edited before the daemon was found"));
            expectEquals (host.engine.getRecorder().getNumTracks(), 5);
            expect (host.engine.mixerBelongsTo (take.dir));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A daemon take's folder holds the mixer before it rolls");
        {
            Folder held; const auto takeDir = held.dir.getSiblingFile (held.dir.getFileName() + "-take");
            const juce::ScopeGuard removeTake { [&] { takeDir.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setStripCount (3);
            host.engine.setTrackName (1, "Lead Vox");
            expect (host.saveMixerToTakeFolder (takeDir));
            expectEquals (sessionmix::stripCountIn (takeDir), 3);
            expect (takeDir.getChildFile ("session_mix.json").loadFileAsString().contains ("Lead Vox"));
            expect (! host.engine.mixerBelongsTo (takeDir), "only a started take claims the mixer");
            host.engine.clearAllStripOverrides();
        }

        beginTest ("A take never lands in a session that is pinned but not loaded");
        {
            Folder f; writeSavedShow (f);
            MainComponent host; pinUnloaded (host, f.dir);
            expect (host.makeNewSessionDir() != f.dir);
            host.engine.claimMixerFor (f.dir);
            expect (host.makeNewSessionDir() == f.dir, "a loaded session keeps taking its takes");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("Starting a take makes these strips that folder's mixer");
        {
            Folder f;
            AudioEngine e; e.prepareForTests (48000, 256); e.setStripCount (1); e.setTrackArmed (0, true);
            expect (! e.mixerBelongsTo (f.dir));
            expect (e.startRecording (f.dir));
            expect (e.mixerBelongsTo (f.dir));
            e.stopRecording();
            expect (e.saveSessionMixTo (f.dir));
            e.clearSessionState();
            expect (! e.mixerBelongsTo (f.dir), "closing a session must release its mixer");
            e.setStripCount (0); e.clearAllStripOverrides();
        }

        beginTest ("The mix writer refuses another session's mixer and an empty mixer over saved strips");
        {
            Folder f, other; writeSavedShow (f);
            const auto saved = f.mix().loadFileAsString();
            juce::String why;
            expect (! sessionmix::write (f.dir, other.dir, mixJson (3), &why));
            expect (why.isNotEmpty());
            expect (! sessionmix::write (f.dir, {}, mixJson (3)));
            expect (! sessionmix::write (f.dir, f.dir, mixJson (0), &why));
            expect (why.contains ("saved strips"), why);
            expect (! sessionmix::write (f.dir, f.dir, "{\"truncated\":", &why));
            expectEquals (f.mix().loadFileAsString(), saved);
            expect (sessionmix::write (f.dir, f.dir, mixJson (5)));
            expectEquals (sessionmix::stripCountIn (f.dir), 5);

            Folder empty; // A new session has nothing to lose yet.
            expect (sessionmix::write (empty.dir, empty.dir, mixJson (0)));
            expect (sessionmix::write (empty.dir, empty.dir, mixJson (0)));
        }

        beginTest ("The metadata writer applies the mixer guard before touching any file");
        {
            Folder f, other; writeSavedShow (f);
            const auto before = savedFiles (f);
            SessionMetadataSnapshot snapshot;
            snapshot.directory = f.dir;
            snapshot.settingsJson = "{\"captureFormat\":0}";
            snapshot.mixJson = mixJson (0);
            snapshot.projectPatchJson = R"({"setlist":[]})";
            snapshot.backup = false;

            snapshot.mixerOwner = other.dir;
            expect (! writeSessionMetadata (snapshot).ok);
            expectEquals (savedFiles (f), before);

            snapshot.mixerOwner = f.dir; // right owner, but an empty mixer
            expect (! writeSessionMetadata (snapshot).ok);
            expectEquals (savedFiles (f), before);

            SessionMetadataSnapshot cuesOnly;
            cuesOnly.directory = f.dir;
            cuesOnly.projectPatchJson = R"({"setlist":[]})";
            cuesOnly.backup = false;
            expect (! writeSessionMetadata (cuesOnly).ok, "a cue-only patch still replaces the session's cues");
            expectEquals (savedFiles (f), before);

            SessionMetadataSnapshot layoutOnly;
            layoutOnly.directory = f.dir;
            layoutOnly.projectPatchJson = R"({"ui":{"view":"Edit"}})";
            layoutOnly.backup = false;
            expect (writeSessionMetadata (layoutOnly).ok, "a descriptive patch carries no mixer state");
        }

        beginTest ("A reorder refuses before any take moves when the mixer is not the session's");
        {
            Folder f; writeSavedShow (f, 2);
            expect (writeAudio (f.track (1))); expect (writeAudio (f.track (2)));
            AudioEngine e; e.prepareForTests (48000, 256); e.setStripCount (2);
            e.setActiveSessionDir (f.dir);
            expect (! e.reorderTracks ({ 1, 0 }));
            expect (f.track (1).existsAsFile() && f.track (2).existsAsFile());
            expectEquals (f.mix().loadFileAsString(), mixJson (2));
            e.claimMixerFor (f.dir);
            expect (! e.reorderTracks ({}), "emptying a saved mixer must refuse");
            expect (f.track (1).existsAsFile() && f.track (2).existsAsFile());
            e.clearSessionState(); e.setStripCount (0); e.clearAllStripOverrides();
        }

        beginTest ("A mix already saved with no strips does not hide a session's audio");
        {
            Folder f; f.mix().replaceWithText (mixJson (0));
            expect (writeAudio (f.track (1))); expect (writeAudio (f.track (2)));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 2);
            expectEquals (host.engine.getRecorder().getNumTracks(), 2);
            expect (host.engine.mixerBelongsTo (f.dir));
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("Retention keeps the newest populated mix when empty mixes fill the window");
        {
            Folder f; f.mix().replaceWithText (mixJson (8));
            expect (sessionbackup::writeSnapshot (f.dir, 3).isDirectory());
            for (int i = 0; i < 6; ++i)
            {
                f.mix().replaceWithText (mixJson (0) + juce::String::repeatedString (" ", i));
                expect (sessionbackup::writeSnapshot (f.dir, 3).isDirectory());
            }
            int populated = 0, total = 0;
            for (const auto& snapshot : f.dir.getChildFile ("Session File Backups")
                                             .findChildFiles (juce::File::findDirectories, false))
            {
                ++total;
                if (sessionmix::stripCountIn (snapshot) == 8) ++populated;
            }
            expectEquals (populated, 1, "the last good mix was pruned");
            expectEquals (total, 4, "only that one extra snapshot may outlive retention");

            f.mix().replaceWithText (mixJson (6));
            expect (sessionbackup::writeSnapshot (f.dir, 3).isDirectory());
            f.mix().replaceWithText (mixJson (7));
            expect (sessionbackup::writeSnapshot (f.dir, 3).isDirectory());
            int remaining = 0;
            for (const auto& snapshot : f.dir.getChildFile ("Session File Backups")
                                             .findChildFiles (juce::File::findDirectories, false))
                remaining += sessionmix::stripCountIn (snapshot) == 8 ? 1 : 0;
            expectEquals (remaining, 0, "a newer populated mix makes the old anchor ordinary again");
        }
    }

    void testOpeningTheOpenSession()
    {
        beginTest ("Opening the session that is already open keeps unsaved work");
        {
            Folder f; writeSavedShow (f, 2);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 0);
            host.engine.setTrackName (0, "Unsaved edit");

            expect (host.openSessionDocument (f.project(), true));
            expectEquals (stripName (host, 0), juce::String ("Unsaved edit"));
            expect (host.statusLabel.getText().contains ("already open"), host.statusLabel.getText());

            // File > Open and Welcome reach the funnel directly.
            host.openSessionFolder (f.dir);
            expectEquals (stripName (host, 0), juce::String ("Unsaved edit"));

            // The same folder through a link is still the same session.
            const auto link = f.dir.getSiblingFile (f.dir.getFileName() + "-link");
            const juce::ScopeGuard removeLink { [&] { link.deleteFile(); } };
            expect (f.dir.createSymbolicLink (link, true));
            expect (host.openSessionDocument (link.getChildFile (f.project().getFileName()), true));
            expectEquals (stripName (host, 0), juce::String ("Unsaved edit"));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A session pinned but not loaded still opens from its document");
        {
            Folder f; writeSavedShow (f, 2);
            MainComponent host; pinUnloaded (host, f.dir);
            expect (host.openSessionDocument (f.project(), true));
            expectEquals (host.engine.getRecorder().getNumTracks(), 2);
            expect (host.engine.mixerBelongsTo (f.dir));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A forced reload reads the session again");
        {
            Folder f; writeSavedShow (f, 2);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 0);
            f.mix().replaceWithText (mixJson (5));
            expectEquals (host.openSessionFolder (f.dir, true), 0);
            expectEquals (host.engine.getRecorder().getNumTracks(), 5);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A session saved with several takes opens, plays its active take and saves them all");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            f.project().replaceWithText (R"({"zynforgeSession":true,"setlist":[],"automation":[],
                "playlists":[{"track":0,"activeTake":1,"takes":[
                    {"name":"First", "clips":[{"tlStart":0,"fileStart":0,"fileLen":4800}]},
                    {"name":"Second","clips":[{"tlStart":0,"fileStart":0,"fileLen":2400}]},
                    {"name":"Third", "clips":[{"tlStart":3600,"fileStart":0,"fileLen":1200}]}]}]})");

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            expectEquals (host.engine.getActiveTakeIdx (0), 1);
            expectEquals (host.engine.getTakeName (0, 1), juce::String ("Second"));

            juce::AudioBuffer<float> played;
            expect (host.engine.renderTrackArrangement (0, played, 4800));
            expect (played.getMagnitude (0, 0, 2400) > 0.2f, "the active take does not play");
            expectLessThan (played.getMagnitude (0, 2400, 2400), 0.001f, "another take plays too");

            host.onSaveSessionState();
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            const auto saved = juce::JSON::parse (f.project());
            expectEquals ((int) saved["playlists"][0]["activeTake"], 1);
            const auto* takes = saved["playlists"][0]["takes"].getArray();
            expect (takes != nullptr && takes->size() == 3, "a take was lost on the way back to disk");
            if (takes != nullptr && takes->size() == 3)
            {
                expectEquals ((*takes)[0]["name"].toString(), juce::String ("First"));
                expectEquals ((int) (*takes)[0]["clips"][0]["fileLen"], 4800);
                expectEquals ((*takes)[2]["name"].toString(), juce::String ("Third"));
                expectEquals ((int) (*takes)[2]["clips"][0]["tlStart"], 3600);
            }
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    static juce::Array<juce::File> keptCopies (const juce::File& dir, const juce::String& name)
    { return dir.findChildFiles (juce::File::findFiles, false, name + ".damaged-*"); }

    static juce::String keptCopy (const juce::File& dir, const juce::String& name)
    {
        const auto copies = keptCopies (dir, name);
        return copies.isEmpty() ? juce::String() : copies.getFirst().loadFileAsString();
    }

    // A backup taken while the show was healthy, the way a save writes one.
    static juce::File healthyBackup (const Folder& f, int strips = 4)
    {
        writeSavedShow (f, strips);
        f.dir.getChildFile ("markers.json").replaceWithText (R"({"sampleRate":48000,"markers":[{"sample":100,"name":"Verse"}]})");
        return sessionbackup::writeSnapshot (f.dir);
    }

    void testDamagedFiles()
    {
        beginTest ("A damaged mix is kept under a dated name and its newest good backup is offered");
        {
            Folder f; expect (healthyBackup (f).isDirectory());
            const juce::String damaged ("{\"trackCount\": 4, \"strips\": [{\"name\":");
            expect (f.mix().replaceWithText (damaged));
            expect (writeAudio (f.track (1)));

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            std::vector<sessionrepair::DamagedFile> offered;
            host.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            expectEquals (host.openSessionFolder (f.dir), 1);

            expectEquals ((int) offered.size(), 1, "the engineer was not told");
            if (! offered.empty())
            {
                expectEquals (offered[0].name, juce::String ("session_mix.json"));
                expect (offered[0].keptAs.existsAsFile());
                expectEquals (offered[0].keptAs.loadFileAsString(), damaged, "the kept copy is not the damaged file");
                expect (offered[0].backup.isDirectory(), "no backup was offered");
            }
            expectEquals (keptCopies (f.dir, "session_mix.json").size(), 1);
            expectEquals (host.engine.getRecorder().getNumTracks(), 1, "strips did not fall back to the audio");

            // Nothing later replaces the repairable bytes.
            expect (host.saveSessionStateTo (f.dir));
            expectEquals (keptCopy (f.dir, "session_mix.json"), damaged);

            expect (host.restoreDamagedFiles (f.dir, offered));
            expectEquals (host.engine.getRecorder().getNumTracks(), 4, "the backup's strips did not come back");
            expectEquals (stripName (host, 3), juce::String ("Saved 4"));
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("Damaged markers are kept, not replaced by the next marker");
        {
            Folder f; expect (healthyBackup (f).isDirectory());
            const auto markers = f.dir.getChildFile ("markers.json");
            const juce::String damaged ("{\"markers\": [ {\"sample\": 1");
            expect (markers.replaceWithText (damaged));
            expect (writeAudio (f.track (1))); // markers need a session with audio

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            std::vector<sessionrepair::DamagedFile> offered;
            host.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            host.openSessionFolder (f.dir);
            expectEquals ((int) offered.size(), 1);
            expectEquals (keptCopies (f.dir, "markers.json").size(), 1);
            host.engine.getMarkers().drop (480, "After the damage");
            expect (host.engine.getMarkers().save());
            expectEquals (keptCopy (f.dir, "markers.json"), damaged);
            expect (host.restoreDamagedFiles (f.dir, offered));
            expectEquals (host.engine.getMarkers().getCount(), 1);
            expectEquals (host.engine.getMarkers().getMarker (0).name, juce::String ("Verse"));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A file damaged after opening is kept before a save replaces it");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 1);
            expect (f.mix().replaceWithText ("{ truncated"));
            expect (host.saveSessionStateTo (f.dir));
            expectEquals (keptCopies (f.dir, "session_mix.json").size(), 1);
            expectEquals (keptCopy (f.dir, "session_mix.json"), juce::String ("{ truncated"));
            expect (host.lastMetadataSaveNotice.contains ("session_mix.json"), host.lastMetadataSaveNotice);

            const auto markers = f.dir.getChildFile ("markers.json");
            expect (markers.replaceWithText ("{ also truncated"));
            host.engine.getMarkers().drop (480, "New");
            expect (host.engine.getMarkers().save());
            expectEquals (keptCopies (f.dir, "markers.json").size(), 1);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A damaged project opens with a warning and no longer blocks saving");
        {
            Folder f; expect (healthyBackup (f).isDirectory());
            const juce::String damaged ("{\"setlist\": [ {\"name\": ");
            expect (f.project().replaceWithText (damaged));

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            std::vector<sessionrepair::DamagedFile> offered;
            host.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            host.openSessionFolder (f.dir);
            expectEquals ((int) offered.size(), 1, "the engineer was not told");
            expectEquals (keptCopy (f.dir, f.project().getFileName()), damaged);
            expect (host.saveSessionStateTo (f.dir), "the session could not be saved afterwards");

            expect (host.restoreDamagedFiles (f.dir, offered));
            expectEquals ((int) host.cues.size(), 1, "the backup's cues did not come back");
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("A mix emptied by an older build offers the backup that still has strips");
        {
            Folder f; expect (healthyBackup (f, 6).isDirectory());
            expect (f.mix().replaceWithText (mixJson (0)));
            expect (writeAudio (f.track (1)));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            std::vector<sessionrepair::DamagedFile> offered;
            host.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            expectEquals (host.openSessionFolder (f.dir), 1);
            expectEquals ((int) offered.size(), 1);
            expect (host.restoreDamagedFiles (f.dir, offered));
            expectEquals (host.engine.getRecorder().getNumTracks(), 6);
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("With no good backup the damage is still reported and nothing is offered to restore");
        {
            Folder f; expect (f.mix().replaceWithText ("garbage"));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            std::vector<sessionrepair::DamagedFile> offered;
            host.damagedFilesPromptForTests = [&] (const auto& files) { offered = files; };
            host.openSessionFolder (f.dir);
            expectEquals ((int) offered.size(), 1);
            expect (! offered.empty() && offered[0].backup == juce::File());
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A healthy session reports nothing and moves nothing");
        {
            Folder f; writeSavedShow (f, 2);
            f.dir.getChildFile ("markers.json").replaceWithText (R"({"markers":[]})");
            const auto files = f.dir.getNumberOfChildFiles (juce::File::findFiles);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            bool told = false;
            host.damagedFilesPromptForTests = [&] (const auto&) { told = true; };
            host.openSessionFolder (f.dir);
            expect (! told);
            expectEquals (f.dir.getNumberOfChildFiles (juce::File::findFiles), files);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    void testSettingsWriters()
    {
        beginTest ("A UI preference save keeps the patch another writer just saved");
        {
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setStripCount (8);
            host.engine.setTrackInputRouting (5, 3);
            host.setStripWidthPreset (MainComponent::StripWidth::L);
            host.engine.setTrackInputRouting (6, 2);
            host.setStripWidthPreset (MainComponent::StripWidth::S);
            auto* props = host.engine.getAppProps();
            expect (props != nullptr);
            if (props != nullptr)
            {
                reloadSettingsReplacing (*props);
                expectEquals (props->getIntValue ("strip_in_5", -1), 3, "a preference save rolled the patch back");
                expectEquals (props->getIntValue ("strip_in_6", -1), 2);
            }
            host.setStripWidthPreset (MainComponent::StripWidth::M);
            host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }
    }

    void testMissingSessionFolder()
    {
        beginTest ("Auto-save says so when the session's drive disappears, and keeps saying it");
        {
            Folder f; writeSavedShow (f, 2);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 0);
            const auto gone = f.dir.getSiblingFile (f.dir.getFileName() + "-unplugged");
            const juce::ScopeGuard restore { [&] { gone.moveFileTo (f.dir); } };
            expect (f.dir.moveFileTo (gone));

            host.statusLabel.setText ({}, juce::dontSendNotification);
            autosaveDue (host);
            host.serviceAutosave();
            expect (host.statusLabel.getText().contains ("AUTO-SAVE FAILED"), host.statusLabel.getText());
            expect (host.statusLabel.getText().contains (f.dir.getFileName()), "the missing folder must be named");

            host.onSaveSessionState();
            expect (host.statusLabel.getText().contains ("missing"), host.statusLabel.getText());
            expect (host.chooser == nullptr, "Save fell through to Save As with the session's drive gone");

            host.onSaveSessionAs();
            expect (host.statusLabel.getText().contains ("missing"), host.statusLabel.getText());
            expect (host.chooser == nullptr, "Save As could repin the session to a folder with no audio");
            host.completeAudioImport ({ gone.getChildFile ("anything.wav") });
            expect (host.statusLabel.getText().contains ("missing"), host.statusLabel.getText());
            expect (host.engine.getActiveSessionDir() == f.dir, "the pin moved off the missing session");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("An autosave that keeps failing backs off instead of repeating every 15 seconds");
        {
            Folder f; writeSavedShow (f, 2);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 0);
            const juce::uint32 interval = 5 * 60000;
            expectEquals ((int) host.nextAutosaveRetryMs (interval), 15000);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), 30000);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), 60000);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), 120000);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), 240000);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), (int) interval);
            expectEquals ((int) host.nextAutosaveRetryMs (interval), (int) interval);
            host.autosaveFailures = 0;
            expectEquals ((int) host.nextAutosaveRetryMs (60000), 15000);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    // A session on read-only media: the folder refuses new files, so every save
    // fails. Permissions need a real Mac volume. The private test home can sit on
    // exFAT, which has none, so these sessions live in /tmp.
    static juce::File permissionsRoot() { return juce::File ("/tmp"); }
    void makeReadOnly (const Folder& f)
    {
        f.dir.setReadOnly (true);
        expect (! atomicfile::writeText (f.dir.getChildFile ("probe"), "x"), "the fixture is not really read-only");
    }
    static void makeWritable (const Folder& f)  { f.dir.setReadOnly (false); }

    void testReadOnlySessions()
    {
        beginTest ("Save As and Show Handoff copy an ordinary session end to end");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto saved = f.dir.getSiblingFile (f.dir.getFileName() + "-saved-as");
            const auto handoff = f.dir.getSiblingFile (f.dir.getFileName() + "-handoff");
            const juce::ScopeGuard removeCopies { [&] { saved.deleteRecursively(); handoff.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);

            expect (host.sessionJobs.reserve());
            host.completeShowHandoff (f.dir, handoff);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }, 20000));
            expect (handoff.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "the handoff has no audio");

            expect (host.sessionJobs.reserve());
            host.completeSaveAs (f.dir, saved);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }, 20000));
            expect (saved.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "Save As has no audio");
            expect (host.engine.mixerBelongsTo (saved));
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("Save As copies a read-only session and keeps the unsaved edits");
        {
            Folder f (permissionsRoot()); writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile (f.dir.getFileName() + "-copy");
            const juce::ScopeGuard removeDest { [&] { dest.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            host.engine.setTrackName (0, "Edited on read-only media");
            expect (host.lastSavedUndoUnits >= 0);

            makeReadOnly (f);
            const juce::ScopeGuard writable { [&] { makeWritable (f); } };
            juce::String prompted;
            host.continueFromDiskPromptForTests = [&] (const juce::String& text) { prompted = text; return true; };
            expect (host.sessionJobs.reserve());
            host.completeSaveAs (f.dir, dest);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            expect (prompted.isNotEmpty(), "the engineer was not told the source could not be saved");
            expect (dest.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "the audio was not copied");
            expect (host.engine.getActiveSessionDir() == dest);
            expect (host.engine.mixerBelongsTo (dest));
            expectEquals (stripName (host, 0), juce::String ("Edited on read-only media"), "the copy lost the unsaved edit");
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("Declining the prompt leaves a read-only session where it is");
        {
            Folder f (permissionsRoot()); writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile (f.dir.getFileName() + "-declined");
            const juce::ScopeGuard removeDest { [&] { dest.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            makeReadOnly (f);
            const juce::ScopeGuard writable { [&] { makeWritable (f); } };
            host.continueFromDiskPromptForTests = [] (const juce::String&) { return false; };
            expect (host.sessionJobs.reserve());
            host.completeSaveAs (f.dir, dest);
            expect (! host.sessionIoBusy.load(), "a declined prompt must release the reservation");
            expect (! dest.exists() || dest.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
            expect (host.engine.getActiveSessionDir() == f.dir);
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("Move and Handoff of a read-only session ask, then go on from the disk");
        {
            Folder f (permissionsRoot()); writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto parent = f.dir.getSiblingFile (f.dir.getFileName() + "-new-home");
            const auto handoff = f.dir.getSiblingFile (f.dir.getFileName() + "-handoff");
            expect (parent.createDirectory().wasOk());
            const juce::ScopeGuard removeCopies { [&] { parent.deleteRecursively(); handoff.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            host.engine.setTrackName (0, "Edited on read-only media");
            makeReadOnly (f);
            const juce::ScopeGuard writable { [&] { makeWritable (f); } };

            int prompts = 0;
            host.continueFromDiskPromptForTests = [&] (const juce::String&) { ++prompts; return true; };
            expect (host.sessionJobs.reserve());
            host.completeShowHandoff (f.dir, handoff);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }, 20000));
            expectEquals (prompts, 1, "the handoff did not say it could not save");
            expect (handoff.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "the handoff has no audio");

            // A read-only folder cannot be renamed away, so this takes the copy
            // path a move between drives takes.
            expect (host.sessionJobs.reserve());
            host.completeRelocate (f.dir, parent);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }, 20000));
            expectEquals (prompts, 2, "the move did not say it could not save");
            const auto moved = parent.getChildFile (f.dir.getFileName());
            expect (moved.getChildFile ("Audio Files/Track_01.wav").existsAsFile(), "the move has no audio");
            expect (host.engine.getActiveSessionDir() == moved);
            expectEquals (stripName (host, 0), juce::String ("Edited on read-only media"), "the move lost the unsaved edit");
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }

        beginTest ("A writable session moves without asking");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto parent = f.dir.getSiblingFile (f.dir.getFileName() + "-new-home");
            expect (parent.createDirectory().wasOk());
            const juce::ScopeGuard removeParent { [&] { parent.deleteRecursively(); } };
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            bool prompted = false;
            host.continueFromDiskPromptForTests = [&] (const juce::String&) { prompted = true; return true; };
            expect (host.sessionJobs.reserve());
            host.completeRelocate (f.dir, parent);
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            expect (! prompted, "a session that saved normally must not ask");
            expect (host.engine.getActiveSessionDir().getParentDirectory() == parent);
            host.engine.clearSessionState(); host.engine.setStripCount (0); host.engine.clearAllStripOverrides();
        }
    }

    void testRemoveLastCapture()
    {
        beginTest ("Remove Last Capture targets the recorded take by identity and uses the Trash");
        {
            Folder take, opened;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setStripCount (1); host.engine.setTrackArmed (0, true);
            expect (host.engine.startRecording (take.dir));
            host.engine.stopRecording();
            expect (host.engine.getLastCapture() == take.dir);

            // An older session that was merely opened and saved looks newest.
            writeSavedShow (opened, 1);
            expect (opened.mix().setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::hours (1)));
            expect (host.engine.getLastCapture() == take.dir);

            const auto prompt = MainComponent::lastCapturePrompt (take.dir);
            expect (prompt.contains (take.dir.getFileName()), "the prompt must name it");
            expect (prompt.contains ("Trash"), prompt);

            const auto marker = take.dir.getChildFile ("marker.txt");
            expect (marker.replaceWithText ("still here"));
            const auto before = MainComponent::testTrashFolder().getNumberOfChildFiles (juce::File::findDirectories);
            expect (host.deleteCaptureSession (take.dir));
            expect (! take.dir.exists());
            expectEquals (MainComponent::testTrashFolder().getNumberOfChildFiles (juce::File::findDirectories), before + 1,
                          "the capture was destroyed instead of moved to the Trash");
            expect (host.engine.getLastCapture() == juce::File(), "a second press would find an older session");
            expect (opened.dir.isDirectory());
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A folder at the recorded path with another identity is not the capture");
        {
            Folder take;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setStripCount (1); host.engine.setTrackArmed (0, true);
            expect (host.engine.startRecording (take.dir));
            host.engine.stopRecording();
            expect (take.dir.getChildFile (".zynforge-capture-id").replaceWithText ("a different capture"));
            expect (host.engine.getLastCapture() == juce::File());
            host.removeLastCapture();
            expect (host.statusLabel.getText().contains ("No recorded capture"), host.statusLabel.getText());
            expect (take.dir.isDirectory());
            host.engine.forgetLastCapture();
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    void testOpenMessages()
    {
        beginTest ("File > Open keeps the real reason when a session does not open");
        {
            Folder f; writeSavedShow (f);
            const auto journal = f.dir.getChildFile ("Session File Backups/reorder_broken");
            expect (journal.createDirectory().wasOk());
            expect (journal.getChildFile ("journal.json").replaceWithText ("not json"));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.completeOpenFolder (f.dir);
            expect (host.statusLabel.getText().contains ("incomplete file move"), host.statusLabel.getText());
            host.engine.clearAllStripOverrides();
        }

        beginTest ("File > Open on the open session says so, even when it has no audio");
        {
            Folder f; writeSavedShow (f, 1);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 0);
            host.completeOpenFolder (f.dir);
            expect (host.statusLabel.getText().contains ("already open"), host.statusLabel.getText());
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A new session keeps its capture format when reopened before the first save");
        {
            Folder f;
            expect (f.project().replaceWithText (R"({"zynforgeSession":true,"sampleRate":48000.0,"captureFormat":6})"));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
            expectEquals (host.openSessionFolder (f.dir), 0);
            expectEquals ((int) host.engine.getRecorder().getCaptureFormat(), 6);

            expect (f.settings().replaceWithText (R"({"captureFormat":3})"));
            expectEquals (host.openSessionFolder (f.dir, true), 0);
            expectEquals ((int) host.engine.getRecorder().getCaptureFormat(), 3, "saved settings must win");
            host.engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    void testHeldRecordPress()
    {
        beginTest ("A RECORD press during an idle save is held and runs when the save ends");
        {
            Folder f;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.engine.setActiveSessionDir (f.dir); host.engine.claimMixerFor (f.dir);

            juce::WaitableEvent entered, release;
            std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                std::make_shared<const std::function<void()>> ([&] { entered.signal(); release.wait (5000); }));
            const juce::ScopeGuard clearHook { [&]
            {
                release.signal();
                std::atomic_store (&sessionbackup::beforeSnapshotForTests, std::shared_ptr<const std::function<void()>>());
            } };

            host.enqueueMetadataSave (f.dir, {}, {}, true, true);
            expect (entered.wait (5000));
            host.onRecordClicked();
            expect (host.recordPressHeldAtMs != 0, "the press was refused");
            expect (host.statusLabel.getText().contains ("held"), host.statusLabel.getText());

            release.signal();
            // No strips here, so the replayed press stops at RECORD's first refusal.
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            expectEquals ((int) host.recordPressHeldAtMs, 0);
            expect (host.statusLabel.getText().contains ("+CH"), host.statusLabel.getText());
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A held press expires, and a session job never holds one");
        {
            Folder f;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.recordPressHeldAtMs = juce::Time::getMillisecondCounter() - 60000;
            host.releaseHeldRecordPress();
            expect (host.statusLabel.getText().contains ("press RECORD again"), host.statusLabel.getText());

            expect (host.sessionJobs.reserve());
            host.onRecordClicked();
            expectEquals ((int) host.recordPressHeldAtMs, 0, "a long job must refuse, not hold");
            host.sessionJobs.release();
            host.engine.clearAllStripOverrides();
        }
    }

    void testShortWrites()
    {
        beginTest ("A short write never replaces a good file");
        {
            Folder f;
            const auto target = f.dir.getChildFile ("metadata.json");
            expect (atomicfile::writeText (target, "the good copy"));

            // A real short write: the kernel stops the file at the limit and
            // write() returns less than it was given.
            rlimit before {};
            expect (getrlimit (RLIMIT_FSIZE, &before) == 0);
            const auto previous = signal (SIGXFSZ, SIG_IGN);
            rlimit small = before; small.rlim_cur = 4096;
            expect (setrlimit (RLIMIT_FSIZE, &small) == 0);
            const bool ok = atomicfile::writeText (target, juce::String::repeatedString ("x", 10000));
            setrlimit (RLIMIT_FSIZE, &before);
            signal (SIGXFSZ, previous);

            expect (! ok, "a truncated save reported success");
            expectEquals (target.loadFileAsString(), juce::String ("the good copy"));
            expect (atomicfile::writeText (target, "a full save still works"));
        }
    }

    void testUnchangedBackups()
    {
        beginTest ("An unchanged autosave takes no place in the backups");
        {
            Folder f; writeSavedShow (f, 2);
            const auto backups = f.dir.getChildFile ("Session File Backups");
            const auto count = [&] { return backups.getNumberOfChildFiles (juce::File::findDirectories); };
            const auto first = sessionbackup::writeSnapshot (f.dir);
            expect (first.isDirectory());
            for (int i = 0; i < 12; ++i)
            {
                // A save rewrites the project's time; that alone is not a change.
                auto project = juce::JSON::parse (f.project());
                project.getDynamicObject()->setProperty ("updatedAt", "2026-11-01T12:00:" + juce::String (i));
                expect (f.project().replaceWithText (juce::JSON::toString (project)));
                expect (sessionbackup::writeSnapshot (f.dir) == first, "an unchanged save took a place");
            }
            expectEquals (count(), 1);

            for (const auto& changed : { f.mix(), f.settings() })
            {
                expect (changed.replaceWithText (changed.loadFileAsString() + " "));
                expect (sessionbackup::writeSnapshot (f.dir) != first);
            }
            expectEquals (count(), 3);
            expect (f.dir.getChildFile ("markers.json").replaceWithText ("{\"markers\":[]}"));
            sessionbackup::writeSnapshot (f.dir);
            expectEquals (count(), 4, "a new markers file is a change");
            expect (f.project().replaceWithText ("{\"setlist\":[{\"name\":\"Changed\"}]}"));
            sessionbackup::writeSnapshot (f.dir);
            expectEquals (count(), 5, "a project edit is a change");
        }

        beginTest ("Ten real changes keep ten states, however many idle saves fell between them");
        {
            Folder f;
            for (int change = 0; change < 12; ++change)
            {
                f.mix().replaceWithText (mixJson (change + 1));
                for (int idle = 0; idle < 3; ++idle) expect (sessionbackup::writeSnapshot (f.dir).isDirectory());
            }
            juce::Array<int> strips;
            for (const auto& snapshot : f.dir.getChildFile ("Session File Backups")
                                             .findChildFiles (juce::File::findDirectories, false))
                strips.add (sessionmix::stripCountIn (snapshot));
            strips.sort();
            expectEquals (strips.size(), 10);
            expectEquals (strips.getFirst(), 3);
            expectEquals (strips.getLast(), 12);
        }
    }

    // A session as an earlier build saved it when nobody touched the features
    // that have since gone: every key there, at the value it wrote.
    static juce::var oldMix (int strips)
    {
        auto mix = juce::JSON::parse (mixJson (strips));
        auto* root = mix.getDynamicObject();
        root->setProperty ("tempoBpm", 120.0);
        root->setProperty ("timeSigNum", 4);
        root->setProperty ("timeSigDenom", 4);
        root->setProperty ("tempoMap", juce::var (juce::Array<juce::var>()));
        juce::Array<juce::var> vcas;
        for (int i = 0; i < 8; ++i)
            vcas.add (juce::JSON::parse (R"({"gainDb":0.0,"muted":false,"soloed":false})"));
        root->setProperty ("vcas", juce::var (vcas));
        if (auto* saved = mix["strips"].getArray())
            for (const auto& strip : *saved)
            {
                auto* fields = strip.getDynamicObject();
                fields->setProperty ("isBus", false);
                fields->setProperty ("vcaGroup", -1);
                fields->setProperty ("referenceMedia", false);
                fields->setProperty ("inRoute", 0);
                juce::Array<juce::var> sends;
                for (int s = 0; s < 4; ++s)
                    sends.add (juce::JSON::parse (R"({"bus":-1,"dB":-60.0,"post":true})"));
                fields->setProperty ("sends", juce::var (sends));
            }
        return mix;
    }

    static juce::var oldProject()
    {
        return juce::JSON::parse (R"({"zynforgeSession":true,"formatVersion":3,"playlists":[],
            "automation":[{"track":0,"volume":[],"pan":[],"mute":[],"safe":false,"vTrim":0.0,"pTrim":0.0}],
            "setlist":[{"name":"Opener","samplePos":0,"tempoBpm":120.0,"transition":0,"fadeBeats":0.0,
                        "strips":[],"automation":[]}]})");
    }

    static void writeOldSession (const Folder& f, const juce::var& mix, const juce::var& project)
    {
        f.mix().replaceWithText (juce::JSON::toString (mix, true));
        f.project().replaceWithText (juce::JSON::toString (project));
        f.settings().replaceWithText (R"({"captureFormat":6,"preRollSeconds":0,"sampleRate":48000.0})");
    }

    static juce::MemoryBlock bytesOf (const juce::File& file)
    {
        juce::MemoryBlock data;
        file.loadFileAsData (data);
        return data;
    }

    static void pumpFor (int ms)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + ms;
        pumpUntil ([deadline] { return juce::Time::getMillisecondCounterHiRes() >= deadline; }, ms + 1000);
    }

    struct RemovedData
    {
        juce::String what;
        std::function<void (juce::var& mix, juce::var& project)> set;
    };

    static std::vector<RemovedData> removedDataFixtures()
    {
        const auto strip0 = [] (juce::var& mix) { return mix["strips"][0].getDynamicObject(); };
        const auto lanes0 = [] (juce::var& project) { return project["automation"][0].getDynamicObject(); };
        const auto cue0   = [] (juce::var& project) { return project["setlist"][0].getDynamicObject(); };
        const auto point  = [] { return juce::JSON::parse (R"([{"s":1000,"v":-6.0,"c":1}])"); };
        return {
            { "a Volume lane", [=] (auto&, auto& p) { lanes0 (p)->setProperty ("volume", point()); } },
            { "a Pan lane",    [=] (auto&, auto& p) { lanes0 (p)->setProperty ("pan", point()); } },
            { "a Mute lane",   [=] (auto&, auto& p) { lanes0 (p)->setProperty ("mute", point()); } },
            { "an automation trim", [=] (auto&, auto& p) { lanes0 (p)->setProperty ("vTrim", -3.0); } },
            { "Automation Safe",    [=] (auto&, auto& p) { lanes0 (p)->setProperty ("safe", true); } },
            { "a cue's automation", [=] (auto&, auto& p)
              { cue0 (p)->setProperty ("automation", juce::JSON::parse (R"([{"track":0,"volume":[{"s":1,"v":0.0,"c":1}]}])")); } },
            { "a cue tempo curve", [=] (auto&, auto& p)
              { cue0 (p)->setProperty ("tempoCurve", juce::JSON::parse (R"([{"off":100.0,"bpm":90.0}])")); } },
            { "a Cue Fade",  [=] (auto&, auto& p) { cue0 (p)->setProperty ("transition", 1); cue0 (p)->setProperty ("fadeBeats", 8.0); } },
            { "a cue tempo", [=] (auto&, auto& p) { cue0 (p)->setProperty ("tempoBpm", 92.0); } },
            { "a VCA level", [] (auto& m, auto&) { m["vcas"][2].getDynamicObject()->setProperty ("gainDb", -6.0); } },
            { "a VCA mute",  [] (auto& m, auto&) { m["vcas"][0].getDynamicObject()->setProperty ("muted", true); } },
            { "a VCA solo",  [] (auto& m, auto&) { m["vcas"][7].getDynamicObject()->setProperty ("soloed", true); } },
            { "a VCA assignment", [=] (auto& m, auto&) { strip0 (m)->setProperty ("vcaGroup", 2); } },
            { "a bus strip",  [=] (auto& m, auto&) { strip0 (m)->setProperty ("isBus", true); } },
            { "an aux send",  [=] (auto& m, auto&)
              { strip0 (m)->getProperty ("sends")[0].getDynamicObject()->setProperty ("bus", 1); } },
            { "a session tempo",   [] (auto& m, auto&) { m.getDynamicObject()->setProperty ("tempoBpm", 132.5); } },
            { "a time signature",  [] (auto& m, auto&) { m.getDynamicObject()->setProperty ("timeSigNum", 3); } },
            { "a tempo map", [] (auto& m, auto&)
              { m.getDynamicObject()->setProperty ("tempoMap", juce::JSON::parse (R"([{"pos":4800,"bpm":140.0}])")); } },
            { "a generated click row", [=] (auto& m, auto&) { strip0 (m)->setProperty ("referenceMedia", true); } },
            { "a click row from before the flag", [=] (auto& m, auto&)
              {
                  strip0 (m)->removeProperty ("referenceMedia");
                  strip0 (m)->setProperty ("name", "Click");
                  strip0 (m)->setProperty ("inRoute", -1);
              } },
        };
    }

    void testRemovedFeatureData()
    {
        for (const auto& fixture : removedDataFixtures())
        {
            beginTest ("A session holding " + fixture.what + " opens, plays and saves, and keeps its originals");
            Folder f;
            auto mix = oldMix (3);
            auto project = oldProject();
            fixture.set (mix, project);
            writeOldSession (f, mix, project);
            expect (writeAudio (f.track (1)));
            const auto audio = bytesOf (f.track (1));
            const auto mixBefore = bytesOf (f.mix()), projectBefore = bytesOf (f.project()),
                       settingsBefore = bytesOf (f.settings());

            expect (removeddata::holdsData (f.dir));
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 1);
            expectEquals (host.engine.getRecorder().getNumTracks(), 3);

            const auto kept = removeddata::folderFor (f.dir);
            expect (kept.isDirectory(), "the originals were not copied");
            expect (bytesOf (kept.getChildFile ("session_mix.json")) == mixBefore);
            expect (bytesOf (kept.getChildFile (f.project().getFileName())) == projectBefore);
            expect (bytesOf (kept.getChildFile ("session_settings.json")) == settingsBefore);
            expect (pumpUntil ([&] { return host.statusLabel.getText().contains (removeddata::kFolderName); }),
                    host.statusLabel.getText());

            expectEquals ((int) host.cues.size(), 1);
            host.jumpToCue (0);
            host.engine.getMasterState().muted.store (false);
            host.engine.startPlayback();
            juce::AudioBuffer<float> out (2, 256);
            for (int block = 0; block < 8; ++block)
                host.engine.audioDeviceIOCallbackWithContext (nullptr, 0, out.getArrayOfWritePointers(), 2, 256, {});
            host.engine.stopPlayback();

            expect (host.saveSessionStateTo (f.dir), host.lastMetadataSaveError);
            expect (bytesOf (kept.getChildFile ("session_mix.json")) == mixBefore, "a save reached the kept copy");
            expect (bytesOf (kept.getChildFile (f.project().getFileName())) == projectBefore);
            expect (bytesOf (f.track (1)) == audio, "the audio was touched");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        const auto expectLeftAlone = [this] (const Folder& f)
        {
            expect (! removeddata::holdsData (f.dir), "default values read as data");
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) >= 0);
            host.statusLabel.setText ("untouched", juce::dontSendNotification);
            pumpFor (150);
            expectEquals (host.statusLabel.getText(), juce::String ("untouched"), "a status line for nothing");
            expect (host.saveSessionStateTo (f.dir), host.lastMetadataSaveError);
            expect (! removeddata::folderFor (f.dir).exists(), "a backup folder for nothing");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        };

        beginTest ("A session saved by this build gets no backup folder and no status line");
        {
            Folder f; writeSavedShow (f);
            expectLeftAlone (f);
        }

        beginTest ("A session an earlier build saved without these features gets none either");
        {
            Folder f; writeOldSession (f, oldMix (3), oldProject());
            expectLeftAlone (f);
        }

        beginTest ("A session with no saved mixer at all gets none either");
        {
            Folder f; expect (writeAudio (f.track (1)));
            expectLeftAlone (f);
        }

        beginTest ("A save never makes a session look old: the next open leaves it alone");
        {
            Folder f; auto mix = oldMix (3);
            writeOldSession (f, mix, oldProject());
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) >= 0);
            expect (host.saveSessionStateTo (f.dir), host.lastMetadataSaveError);
            expect (! removeddata::holdsData (f.dir), "this build's own save reads as an old session");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("The originals are copied once, and a later open says nothing");
        {
            Folder f; auto mix = oldMix (3); auto project = oldProject();
            mix["vcas"][0].getDynamicObject()->setProperty ("gainDb", -6.0);
            writeOldSession (f, mix, project);
            const auto mixBefore = bytesOf (f.mix());

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) >= 0);
            expect (pumpUntil ([&] { return host.statusLabel.getText().contains (removeddata::kFolderName); }));
            expect (host.saveSessionStateTo (f.dir), host.lastMetadataSaveError);
            expect (bytesOf (f.mix()) != mixBefore, "setup: the save must replace the mix");

            // The data comes back, as if an earlier build had saved over it.
            f.mix().replaceWithText (juce::JSON::toString (mix, true) + "\n");
            host.statusLabel.setText ("untouched", juce::dontSendNotification);
            expect (host.openSessionFolder (f.dir, true) >= 0);
            pumpFor (150);
            expect (! host.statusLabel.getText().contains (removeddata::kFolderName), "told twice");
            expect (bytesOf (removeddata::folderFor (f.dir).getChildFile ("session_mix.json")) == mixBefore,
                    "the first originals were replaced");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A daemon take adopted at launch keeps its session's originals first");
        {
            Folder f; auto mix = oldMix (3);
            mix.getDynamicObject()->setProperty ("tempoBpm", 90.0);
            writeOldSession (f, mix, oldProject());
            const auto mixBefore = bytesOf (f.mix());
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            host.adoptDaemonTakeSession (daemonStatus (f.dir, 3));
            expect (bytesOf (removeddata::folderFor (f.dir).getChildFile ("session_mix.json")) == mixBefore);
            expect (pumpUntil ([&] { return host.statusLabel.getText().contains (removeddata::kFolderName); }));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("If the originals cannot be copied the session still opens, and no save replaces them");
        {
            Folder f; auto mix = oldMix (3); auto project = oldProject();
            mix["strips"][1].getDynamicObject()->setProperty ("isBus", true);
            project["setlist"][0].getDynamicObject()->setProperty ("fadeBeats", 4.0);
            writeOldSession (f, mix, project);
            const auto inTheWay = f.dir.getChildFile ("Session File Backups");
            expect (inTheWay.replaceWithText ("a file where the folder goes"));
            const auto mixBefore = bytesOf (f.mix()), projectBefore = bytesOf (f.project());

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) >= 0, "a failed copy must not stop the session opening");
            expect (pumpUntil ([&] { return host.statusLabel.getText().startsWith ("!"); }), host.statusLabel.getText());
            expect (! host.saveSessionStateTo (f.dir), "a save dropped data with no copy of it");
            expect (host.lastMetadataSaveError.contains ("could not be copied"), host.lastMetadataSaveError);
            expect (! host.engine.saveSessionMixTo (f.dir));
            expect (! host.saveSetlistToActiveSession (false));
            expect (bytesOf (f.mix()) == mixBefore && bytesOf (f.project()) == projectBefore);

            expect (inTheWay.deleteFile());
            expect (host.saveSessionStateTo (f.dir), host.lastMetadataSaveError);
            expect (bytesOf (removeddata::folderFor (f.dir).getChildFile ("session_mix.json")) == mixBefore,
                    "the first good save must copy them before writing");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A copy that fails part way leaves no folder that looks complete");
        {
            Folder f (permissionsRoot()); auto mix = oldMix (2);
            mix["vcas"][0].getDynamicObject()->setProperty ("muted", true);
            writeOldSession (f, mix, oldProject());
            const auto markers = f.dir.getChildFile ("markers.json");
            expect (markers.replaceWithText ("{}"));
            expectEquals (::chmod (markers.getFullPathName().toRawUTF8(), 0), 0);
            expect (removeddata::keepOriginals (f.dir) == removeddata::Kept::failed);
            ::chmod (markers.getFullPathName().toRawUTF8(), 0644);
            expect (! removeddata::folderFor (f.dir).exists(), "a part of the set took the folder's name");
            expect (f.dir.getChildFile ("Session File Backups").findChildFiles (juce::File::findDirectories, false).isEmpty(),
                    "the staging folder was left behind");
            expect (removeddata::keepOriginals (f.dir) == removeddata::Kept::justCopied);
            expect (removeddata::keepOriginals (f.dir) == removeddata::Kept::nothingToDo);
            expect (removeddata::folderFor (f.dir).getChildFile ("markers.json").existsAsFile());
        }

        beginTest ("Retention and repair leave the kept originals alone");
        {
            Folder f; auto mix = oldMix (2);
            mix["vcas"][0].getDynamicObject()->setProperty ("muted", true);
            writeOldSession (f, mix, oldProject());
            expect (removeddata::keepOriginals (f.dir) == removeddata::Kept::justCopied);
            for (int i = 0; i < 4; ++i)
            {
                f.mix().replaceWithText (mixJson (2 + i));
                expect (sessionbackup::writeSnapshot (f.dir, 2).isDirectory());
            }
            expect (removeddata::folderFor (f.dir).getChildFile ("session_mix.json").existsAsFile(),
                    "retention pruned the only copy of the data");
            expect (sessionrepair::newestBackupOf (f.dir, "session_mix.json") != removeddata::folderFor (f.dir),
                    "repair offered a months-old mix as the newest backup");
        }

        beginTest ("An old bus strip loads as an ordinary strip that records");
        {
            Folder f; auto mix = oldMix (3);
            auto* bus = mix["strips"][2].getDynamicObject();
            bus->setProperty ("isBus", true);
            bus->setProperty ("inRoute", -1);
            writeOldSession (f, mix, oldProject());
            expect (writeAudio (f.track (1)));

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) > 0);
            auto& engine = host.engine;
            engine.setTrackInputRouting (2, 0);
            engine.setTrackArmed (2, true);
            expect (engine.startRecording (f.dir), engine.getRecorder().getStartError());
            juce::AudioBuffer<float> in (1, 256), out (2, 256);
            in.clear();
            for (int block = 0; block < 16; ++block)
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1,
                                                         out.getArrayOfWritePointers(), 2, 256, {});
            engine.stopRecording();
            expect (! f.dir.getChildFile ("Audio Files").findChildFiles (juce::File::findFiles, false, "Track_03.*").isEmpty(),
                    "the strip that was a bus did not record");
            engine.clearSessionState(); engine.clearAllStripOverrides();
        }
    }

    // Earlier builds saved all of these. Nothing reads them now, and none of
    // them may get in the way of what still loads.
    void testKeysFromEarlierBuilds()
    {
        beginTest ("A session saved with the keys of removed features opens and loads the rest");
        {
            Folder f; writeSavedShow (f);
            auto project = juce::JSON::parse (f.project());
            auto* saved = project.getDynamicObject();
            saved->setProperty ("interleaved", true);
            saved->setProperty ("ioPreset", "Last Used");
            for (const auto* key : { "artist", "venue", "fohEngineer", "date", "notes" })
                saved->setProperty (key, "Left by an earlier build");
            expect (f.project().replaceWithText (juce::JSON::toString (project)));

            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) == 0);
            expect (host.engine.mixerBelongsTo (f.dir));
            expectEquals (host.engine.getRecorder().getNumTracks(), 3);
            expectEquals (stripName (host, 2), juce::String ("Saved 3"));
            expectEquals ((int) host.cues.size(), 1, "the session's cues were not loaded");
            expect (host.saveSessionStateTo (f.dir));
            expectEquals (juce::JSON::parse (f.project())["venue"].toString(), juce::String ("Left by an earlier build"),
                          "a save dropped a field it does not own");
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("Settings keys left by removed features are ignored");
        {
            {
                MainComponent writer; writer.stopTimer();
                auto* props = writer.engine.getAppProps();
                expect (props != nullptr);
                if (props != nullptr)
                {
                    props->setValue ("interleavedFlag", true);
                    props->setValue ("ioPreset", "Last Used");
                    props->setValue ("sessionName", "Old");
                    props->setValue ("sessionTempoBpm", 92.0);
                    props->setValue ("timeSigNumerator", 3);
                    props->setValue ("timeSigDenominator", 8);
                    props->setValue ("vca_0_name", "DRUMS");
                    props->setValue ("vca_0_colour", 12345.0);
                    props->setValue ("cloudUploadCommand", "/usr/bin/false");
                    props->setValue ("cs_mackie_type", 2);
                    props->saveIfNeeded();
                }
            }
            Folder f; writeSavedShow (f);
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expect (host.openSessionFolder (f.dir) == 0);
            expectEquals (host.engine.getRecorder().getNumTracks(), 3);
            if (auto* props = host.engine.getAppProps())
                for (const auto* key : { "interleavedFlag", "ioPreset", "sessionName", "cloudUploadCommand", "cs_mackie_type",
                                         "sessionTempoBpm", "timeSigNumerator", "timeSigDenominator",
                                         "vca_0_name", "vca_0_colour" })
                    props->removeValue (key);
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }

        beginTest ("A new session's document holds no I/O preset or interleaved flag");
        {
            Folder f;
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            NewSessionDialog::Result r;
            r.name = "Fresh";
            r.location = f.dir;
            const auto session = host.createSessionFolderStructure (r);
            expect (session.isDirectory());
            const auto document = juce::JSON::parse (session.getChildFile ("Fresh.zfproj"));
            expect (document["zynforgeSession"], "the document was not written");
            expect (! document.hasProperty ("interleaved") && ! document.hasProperty ("ioPreset"));
            host.engine.clearSessionState(); host.engine.clearAllStripOverrides();
        }
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto previousTestConstruct = MainComponent::s_testConstruct;
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreConstruct { [previousTestConstruct]
        { MainComponent::s_testConstruct = previousTestConstruct; } };
        testMixerOwnership();
        testOpeningTheOpenSession();
        testDamagedFiles();
        testSettingsWriters();
        testMissingSessionFolder();
        testReadOnlySessions();
        testRemoveLastCapture();
        testOpenMessages();
        testHeldRecordPress();
        testShortWrites();
        testUnchangedBackups();
        testKeysFromEarlierBuilds();
        testRemovedFeatureData();
    }
};

static SessionSafetyTests sessionSafetyTests;
}
