// What a session operation tells the engineer, and which folders it will touch:
// a handoff that lacks something, a template or map taken during Virtual
// Soundcheck, a session whose drive went away, a folder that is no session.

#include "../UI/MainComponent.h"
#include "../Audio/FastHash.h"
#include "../Audio/SessionBackup.h"
#include "../Audio/SessionRepair.h"
#include "../Audio/ShowHandoff.h"
#include "../Audio/VirtualSoundcheck.h"
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

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        const auto previousTestConstruct = MainComponent::s_testConstruct;
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreConstruct { [previousTestConstruct]
        { MainComponent::s_testConstruct = previousTestConstruct; } };
        testHandoffStatus();
        testHandoffLines();
    }
};

static SessionFixTests sessionFixTests;
}
