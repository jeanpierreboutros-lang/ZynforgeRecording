// Session files on the way in and out: whose mixer gets written where, what
// happens to damaged files, and which folder an operation may touch.

#include "../UI/MainComponent.h"
#include "../UI/SessionMetadataWriter.h"
#include "../Audio/SessionBackup.h"
#include "../Audio/SessionMixFile.h"
#include "../Audio/TrackFileTransaction.h"
#include "../Audio/AtomicFile.h"
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
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-session-safety-" + juce::Uuid().toString());
        Folder() { dir.createDirectory(); dir.getChildFile ("Audio Files").createDirectory(); }
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

        beginTest ("A generated click never replaces audio in a session that is not loaded");
        {
            Folder f; writeSavedShow (f, 2);
            expect (writeAudio (f.track (1)));
            const auto original = f.track (1).loadFileAsString();
            MainComponent host; pinUnloaded (host, f.dir);
            bool complete = false, success = true;
            host.generateOrRefreshClickTrack ([&] (bool ok) { success = ok; complete = true; });
            expect (pumpUntil ([&] { return complete; }));
            expect (! success, "a click was generated over a take in an unloaded session");
            expect (f.track (1).loadFileAsString() == original, "a recorded take was replaced");
            expectEquals (host.engine.getRecorder().getNumTracks(), 0);
            host.engine.clearAllStripOverrides();
        }

        beginTest ("A generated click never lands on a track number that already holds audio");
        {
            Folder f; f.mix().replaceWithText (mixJson (1));
            expect (writeAudio (f.track (1))); expect (writeAudio (f.track (2)));
            const auto original = f.track (2).loadFileAsString();
            MainComponent host; host.stopTimer(); host.engine.prepareForTests (48000, 256);
            expectEquals (host.openSessionFolder (f.dir), 2);
            expectEquals (host.engine.getRecorder().getNumTracks(), 1);
            bool complete = false, success = true;
            host.generateOrRefreshClickTrack ([&] (bool ok) { success = ok; complete = true; });
            expect (pumpUntil ([&] { return complete; }));
            expect (! success, "a click was generated over track 2's audio");
            expect (f.track (2).loadFileAsString() == original, "a recorded take was replaced");
            expectEquals (host.engine.getRecorder().getNumTracks(), 1, "the refused click left a strip behind");
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

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto previousTestConstruct = MainComponent::s_testConstruct;
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreConstruct { [previousTestConstruct]
        { MainComponent::s_testConstruct = previousTestConstruct; } };
        testMixerOwnership();
    }
};

static SessionSafetyTests sessionSafetyTests;
}
