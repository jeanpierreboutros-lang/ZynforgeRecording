// Session fixes from the 2026-11-24 audit: Save As when the drive goes while
// its panel is up, folders a refused copy leaves behind, session and template
// names JUCE reads as a home folder or a hidden one, the autosave check while
// a take closes, a timeline CSV answered after a take started, and a take that
// does not open when its session does.

#include "../UI/MainComponent.h"
#include "../UI/NewSessionDialog.h"
#include "../Audio/PathSafety.h"
#include <juce_audio_formats/juce_audio_formats.h>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
class Fix20261124SessionTests final : public juce::UnitTest
{
public:
    Fix20261124SessionTests() : UnitTest ("Fixes 2026-11-24 session", "zynforge") {}

    struct Folder
    {
        juce::File dir;
        explicit Folder (const juce::String& name = "Show", bool audioFolder = true)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("zf-fix1124-session-" + juce::Uuid().toString())
                       .getChildFile (name))
        {
            dir.createDirectory();
            if (audioFolder) dir.getChildFile ("Audio Files").createDirectory();
        }
        ~Folder() { dir.getParentDirectory().deleteRecursively(); }
        juce::File project() const { return dir.getChildFile (dir.getFileName() + ".zfproj"); }
        juce::File audio (const juce::String& name) const { return dir.getChildFile ("Audio Files").getChildFile (name); }
        juce::File track (int index = 1) const
        { return audio (juce::String::formatted ("Track_%02d.wav", index)); }
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

    static bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 20000)
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
        explicit Host (const Folder& f)
        {
            main.stopTimer();
            main.engine.prepareForTests (48000, 256);
            main.openSessionFolder (f.dir);
        }
        Host()
        {
            main.stopTimer();
            main.engine.prepareForTests (48000, 256);
        }
        ~Host()
        {
            main.continueFromDiskPromptForTests = nullptr;
            pumpUntil ([this] { return ! main.engine.isCaptureFinalizing() && ! main.sessionIoBusy.load(); }, 10000);
            forget (main);
        }
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

    static bool hiddenChildIn (const juce::File& dir)
    {
        for (const auto& child : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
            if (child.getFileName().startsWithChar ('.')) return true;
        return false;
    }

    // ── SES-04 ──────────────────────────────────────────────────────────
    void saveAsAfterTheDriveWent()
    {
        beginTest ("Save As answered after the session drive went refuses, and switches to nothing");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);
            Picker picker (host.main);
            host.main.onSaveSessionAs();
            expect (picker.answer != nullptr, "setup: the panel opened: " + host.status());
            PulledDrive drive (f);
            picker.reply (dest);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (! dest.exists(), "Save As made a session with no audio after its drive went");
            expect (host.main.engine.getActiveSessionDir() == f.dir,
                    "the app switched to the copy: " + host.main.engine.getActiveSessionDir().getFullPathName());
            expect (host.main.engine.mixerBelongsTo (f.dir), "the mixer was claimed for the copy");
            expect (! host.status().contains ("Saved As"), host.status());
            expect (host.status().contains ("missing") && host.status().contains ("reconnect"),
                    "the refusal does not say the folder is missing: " + host.status());
            expect (host.status().contains (f.dir.getFullPathName()), host.status());
            expect (! host.main.sessionIoBusy.load(), "the refusal kept the session slot");
        }

        beginTest ("Save As reached with the open session's folder gone copies nothing");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto dest = f.dir.getSiblingFile ("Copy");
            Host host (f);
            PulledDrive drive (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, dest);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (! dest.exists(), "Save As made a session with no audio");
            expect (host.main.engine.getActiveSessionDir() == f.dir);
            expect (host.status().contains ("reconnect"), host.status());
        }

        beginTest ("Save As with no session open still saves the mixer into a new folder");
        {
            Folder parent ("Parent", false);
            const auto dest = parent.dir.getChildFile ("Fresh");
            Host host;
            host.main.engine.setStripCount (1);
            Picker picker (host.main);
            host.main.onSaveSessionAs();
            expect (picker.answer != nullptr, "setup: the panel opened: " + host.status());
            picker.reply (dest);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (dest.isDirectory(), host.status());
            expect (host.status().startsWith ("Saved As"), host.status());
        }
    }

    // ── SES-05 ──────────────────────────────────────────────────────────
    void refusedCopiesLeaveNoFolder()
    {
        beginTest ("A Save As refused for a folder inside the session leaves no folder there");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            const auto inside = f.dir.getChildFile ("Copy Inside");
            Host host (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, inside);
            expect (host.status().contains ("outside"), host.status());
            expect (! inside.exists(), "the refused Save As left an empty folder inside the session");
            expect (! host.main.sessionIoBusy.load());
        }

        beginTest ("An empty folder the engineer made is left where it was");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            const auto inside = f.dir.getChildFile ("Made By Hand");
            expect (inside.createDirectory().wasOk());
            Host host (f);
            expect (host.main.sessionJobs.reserve());
            host.main.completeSaveAs (f.dir, inside);
            expect (host.status().contains ("outside"), host.status());
            expect (inside.isDirectory(), "a folder this Save As did not make was removed");
        }

        beginTest ("A handoff cancelled at the save-failure question leaves no folder");
        {
            Folder f; writeSavedShow (f, 2); expect (writeAudio (f.track (1)));
            const auto handoff = f.dir.getSiblingFile ("Show Handoff");
            Host host (f);
            host.main.engine.setTrackName (0, "Unsaved change");
            expect (f.dir.setReadOnly (true));
            const juce::ScopeGuard writable { [&] { f.dir.setReadOnly (false); } };
            int asked = 0;
            host.main.continueFromDiskPromptForTests = [&] (const juce::String&) { ++asked; return false; };
            expect (host.main.sessionJobs.reserve());
            host.main.completeShowHandoff (f.dir, handoff);
            expectEquals (asked, 1, "setup: the save did not fail: " + host.status());
            expect (host.status().contains ("cancelled"), host.status());
            expect (! handoff.exists(), "the cancelled handoff left an empty folder");
            expect (! host.main.sessionIoBusy.load());
        }

        beginTest ("A handoff refused for a clip outside the session leaves no folder");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            Folder elsewhere ("Elsewhere");
            const auto outside = elsewhere.dir.getChildFile ("Outside.wav");
            expect (writeAudio (outside));
            const auto handoff = f.dir.getSiblingFile ("Show Handoff");
            Host host (f);
            auto& clips = host.main.engine.clipsFor (0);
            expect (! clips.empty(), "setup: the strip has no clip");
            if (! clips.empty()) clips[0].audioFile = outside;
            expect (host.main.sessionJobs.reserve());
            host.main.completeShowHandoff (f.dir, handoff);
            pumpUntil ([&] { return ! host.main.sessionIoBusy.load(); }, 10000);
            expect (host.status().contains ("outside this session"), "setup: " + host.status());
            expect (! handoff.exists(), "the refused handoff left an empty folder");
        }
    }

    // ── SES-06 ──────────────────────────────────────────────────────────
    void namesJuceReadsAsPaths()
    {
        const auto newSession = [this] (const juce::String& name, const juce::File& root)
        {
            Host host;
            NewSessionDialog::Result r;
            r.name = name; r.location = root; r.sampleRate = 48000.0;
            const auto made = host.main.createSessionFolderStructure (r);
            const auto message = made.isDirectory() ? juce::String() : host.main.sessionCreateFailureMessage();
            forget (host.main);
            return std::make_pair (made, message);
        };

        beginTest ("A typed name loses the front JUCE would read as a home folder or a hidden one");
        {
            expectEquals (pathsafety::visibleName ("~Show"), juce::String ("Show"));
            expectEquals (pathsafety::visibleName (".Show"), juce::String ("Show"));
            expectEquals (pathsafety::visibleName (" ~ .. Show"), juce::String ("Show"));
            expectEquals (pathsafety::visibleName ("."), juce::String());
            expectEquals (pathsafety::visibleName (".."), juce::String());
            expectEquals (pathsafety::visibleName ("~"), juce::String());
            expectEquals (pathsafety::visibleName (juce::File::createLegalFileName ("?.")), juce::String());
            expectEquals (pathsafety::visibleName ("Show ~ Night 2.0"), juce::String ("Show ~ Night 2.0"));
            expectEquals (pathsafety::visibleName ("Show."), juce::String ("Show."));
        }

        beginTest ("A session named \"~Show\" is made as a visible folder in the sessions folder");
        {
            Folder root ("Sessions", false);
            const auto [made, message] = newSession ("~Show", root.dir);
            expect (made.isDirectory(), "no session was made: " + message);
            expect (made.getParentDirectory() == root.dir, made.getFullPathName());
            expect (! made.getFileName().startsWithChar ('~') && ! made.getFileName().startsWithChar ('.'),
                    made.getFileName());
            expect (made.getFileName().contains ("Show"), made.getFileName());
            expect (! message.contains ("permissions"), message);
        }

        for (const auto* name : { ".", "..", "~", ".Show", "?." })
        {
            beginTest ("A session named \"" + juce::String (name) + "\" never makes a hidden folder");
            Folder root ("Sessions", false);
            const auto [made, message] = newSession (name, root.dir);
            expect (! hiddenChildIn (root.dir), "a hidden folder was made for \"" + juce::String (name) + "\"");
            expect (! root.dir.getParentDirectory().getChildFile ("..2").exists());
            if (made.isDirectory())
            {
                expect (made.getParentDirectory() == root.dir, made.getFullPathName());
                expect (! made.getFileName().startsWithChar ('.') && ! made.getFileName().startsWithChar ('~'),
                        made.getFileName());
            }
            else
                expect (message.isNotEmpty() && ! message.contains ("permissions"),
                        "a misleading refusal: " + message);
        }

        beginTest ("A template named \"~Live\" is saved where the menu lists it");
        {
            Folder dir ("Templates", false);
            Host host;
            host.main.templatesDirForTests = dir.dir;
            expect (host.main.saveSessionTemplate ("~Live"), host.status());
            expect (! host.status().contains ("permissions"), host.status());
            const auto list = host.main.listSessionTemplates();
            expectEquals (list.size(), 1, "the template is not in the menu's list");
            if (! list.isEmpty())
                expect (list[0].getFileName().contains ("Live") && ! list[0].getFileName().startsWithChar ('~'),
                        list[0].getFileName());
        }

        for (const auto* name : { ".", "..", "/" })
        {
            beginTest ("A template named \"" + juce::String (name) + "\" is refused in plain words");
            Folder dir ("Templates", false);
            Host host;
            host.main.templatesDirForTests = dir.dir;
            expect (! host.main.saveSessionTemplate (name), "a template file was written: " + host.status());
            expect (! hiddenChildIn (dir.dir), "a hidden template file was written");
            expect (! host.status().contains ("permissions"), host.status());
            expect (host.status().containsIgnoreCase ("name"), host.status());
        }
    }

    // ── MAIN-04 ─────────────────────────────────────────────────────────
    void autosaveWhileATakeCloses()
    {
        beginTest ("The autosave check does nothing while a take's files are closing");
        {
            Folder f; writeSavedShow (f, 1);   // no take yet: the one below is its first
            Host host (f);
            auto& main = host.main;
            // Saved, because the take reloads the settings file before it writes.
            const auto autosaveEvery = [&] (int minutes)
            {
                if (auto* props = main.engine.getAppProps())
                { props->setValue ("autosaveMinutes", minutes); props->saveIfNeeded(); }
            };
            const juce::ScopeGuard restore { [&] { autosaveEvery (5); } };
            expect (main.engine.setTrackArmed (0, true));
            if (! main.engine.startRecording (f.dir))
            { expect (false, "setup: the take did not start: " + main.engine.getRecorder().getStartError()); return; }
            // The engine closes the take on its worker, as a device loss does:
            // no take is recording and no session job holds the slot.
            expect (main.engine.stopRecordingAsync ([] (bool) {}), "setup: the take did not start closing");
            expect (! main.engine.isRecording() && main.engine.isCaptureFinalizing(), "setup: not finalizing");
            expect (! main.sessionIoBusy.load(), "setup: a job holds the slot");

            autosaveEvery (1);
            main.lastAutosaveMs = juce::Time::getMillisecondCounter() - 120000;
            main.statusLabel.setText ({}, juce::dontSendNotification);
            main.serviceAutosave();
            expectEquals (main.pendingMetadataSaves, 0, "an autosave went to the take's drive while its files closed");
            expect (! main.sessionIoBusy.load(), "an autosave took the slot while the take's files closed");
            expect (! main.statusLabel.getText().containsIgnoreCase ("auto-save"), main.statusLabel.getText());
            expect (juce::Time::getMillisecondCounter() - main.lastAutosaveMs < 60000,
                    "the interval did not start again from the take");
            expect (pumpUntil ([&] { return ! main.engine.isCaptureFinalizing(); }), "the take never closed");
        }
    }

    // ── MAIN-05 ─────────────────────────────────────────────────────────
    void timelineCsvAfterATakeStarted()
    {
        beginTest ("A timeline CSV answered after a take started behind its panel writes nothing");
        {
            Folder f; writeSavedShow (f, 1);   // no take yet: the one below is its first
            Folder out ("Out", false);
            const auto target = out.dir.getChildFile ("Show_timeline.csv");
            Host host (f);
            Picker picker (host.main);
            host.main.exportTimelineCsv();
            expect (picker.answer != nullptr, "setup: the panel opened: " + host.status());
            expect (host.main.engine.setTrackArmed (0, true));
            if (host.main.engine.startRecording (f.dir))
            {
                picker.reply (target);
                expect (! target.exists(), "the CSV was written while a take rolled");
                expect (host.status().contains ("take"), host.status());
                expect (! host.main.sessionIoBusy.load(), "the refusal kept the session slot");
                host.main.engine.stopRecording();
            }
            else expect (false, "setup: the take did not start: " + host.main.engine.getRecorder().getStartError());
        }

        beginTest ("A timeline CSV answered with no take is written");
        {
            Folder f; writeSavedShow (f, 1); expect (writeAudio (f.track (1)));
            Folder out ("Out", false);
            const auto target = out.dir.getChildFile ("Show_timeline.csv");
            Host host (f);
            Picker picker (host.main);
            host.main.exportTimelineCsv();
            expect (picker.answer != nullptr, "setup: the panel opened: " + host.status());
            picker.reply (target);
            expect (target.existsAsFile(), host.status());
            expect (host.status().startsWith ("Timeline exported"), host.status());
            expect (! host.main.sessionIoBusy.load(), "the export kept the session slot");
        }
    }

    // ── PLAY-03 ─────────────────────────────────────────────────────────
    void takeThatDoesNotOpenIsReported()
    {
        beginTest ("A take that does not open at session open is named, and not counted as loaded");
        {
            Folder f; writeSavedShow (f, 3);
            expect (writeAudio (f.track (1)));
            expect (writeAudio (f.audio ("Track_02_part02.wav")));   // its main file is missing
            expect (writeAudio (f.track (3)));
            Host host (f);
            expect (host.main.engine.getPlayer().trackFailedToOpen (1), "setup: the take opened");
            const auto status = host.status();
            expect (status.contains ("Track 2"), "the status does not name the strip: " + status);
            expect (status.contains ("Saved 2"), "the status does not give the strip's name: " + status);
            expect (! status.contains ("Loaded 3"), "the silent strip was counted as loaded: " + status);
            const auto toast = host.main.toast.getCurrentMessage();
            expect (toast.contains ("Track 2"), "no warning names the strip: " + toast);
            expect (! toast.contains ("Track 1") && ! toast.contains ("Track 3"), toast);
        }

        beginTest ("A session whose takes all open says Loaded, with no warning");
        {
            Folder f; writeSavedShow (f, 2);
            expect (writeAudio (f.track (1)));
            expect (writeAudio (f.track (2)));
            Host host (f);
            expect (host.status().startsWith ("Loaded 2 tracks"), host.status());
            expect (host.main.toast.getCurrentMessage().isEmpty(), host.main.toast.getCurrentMessage());
        }
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto previousTestConstruct = MainComponent::s_testConstruct;
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreConstruct { [previousTestConstruct]
        {
            MainComponent::s_testConstruct = previousTestConstruct;
            AudioEngine::setTestModeSkipAudioInit (false);
        } };
        saveAsAfterTheDriveWent();
        refusedCopiesLeaveNoFolder();
        namesJuceReadsAsPaths();
        autosaveWhileATakeCloses();
        timelineCsvAfterATakeStarted();
        takeThatDoesNotOpenIsReported();
    }
};

static Fix20261124SessionTests fix20261124SessionTests;
}
