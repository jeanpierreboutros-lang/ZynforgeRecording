#include "../UI/MainComponent.h"
#include "../UI/SessionMetadataWriter.h"
#include "../UI/SessionInfoDialog.h"
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace zynforge
{
class ReviewPersistenceRepairTests final : public juce::UnitTest
{
public:
    ReviewPersistenceRepairTests() : UnitTest ("Review persistence repairs", "zynforge") {}

    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-review-persistence-" + juce::Uuid().toString());
        Folder() { dir.createDirectory(); }
        ~Folder() { dir.deleteRecursively(); }
        juce::File project() const { return dir.getChildFile (dir.getFileName() + ".zfproj"); }
        juce::File track (int index = 1) const
        { return dir.getChildFile (juce::String::formatted ("Audio Files/Track_%02d.wav", index)); }
    };

    static bool pumpUntil (const std::function<bool()>& ready, int timeoutMs = 10000)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
           #if JUCE_MAC
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
           #elif JUCE_MODAL_LOOPS_PERMITTED
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
           #else
            juce::Thread::sleep (10);
           #endif
        }
        return ready();
    }

    static bool writeAudio (const juce::File& file, float value = 0.25f)
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (
            file.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
        if (! writer) return false;
        juce::AudioBuffer<float> audio (1, 4800);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), value, 4800);
        return writer->writeFromAudioSampleBuffer (audio, 0, 4800);
    }

    static juce::MemoryBlock bytes (const juce::File& file)
    { juce::MemoryBlock result; file.loadFileAsData (result); return result; }

    static void prepare (MainComponent& host, const Folder& folder, int tracks = 1)
    {
        host.stopTimer(); // tests drive completions; never an unrelated timed autosave
        host.engine.clearAllStripOverrides();
        host.engine.setStripCount (tracks);
        host.engine.prepareForTests (48000, 256);
        host.engine.setActiveSessionDir (folder.dir);
        host.engine.claimMixerFor (folder.dir);
        host.engine.loadSession (folder.dir);
        for (int i = 0; i < tracks; ++i)
        {
            host.engine.setTrackName (i, "Source " + juce::String (i + 1));
            host.engine.setTrackStereo (i, false);
        }
    }

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);

        beginTest ("Malformed existing project is preserved when an automatic layout patch is saved");
        {
            Folder f;
            const juce::String damaged = "{\"playlists\": [\"recoverable edit\"], \"unfinished\":";
            expect (f.project().replaceWithText (damaged));
            SessionMetadataSnapshot snapshot;
            snapshot.directory = f.dir;
            snapshot.projectPatchJson = R"({"ui":{"view":"edit"},"updatedAt":"test"})";
            snapshot.backup = false;
            const auto result = writeSessionMetadata (snapshot);
            expect (! result.ok, "unreadable existing project was reported saved");
            expectEquals (f.project().loadFileAsString(), damaged, "layout write destroyed recoverable project bytes");
            expect (result.error.isNotEmpty(), "save failure must explain the project read error");
        }

        beginTest ("Malformed existing project refuses before updating other session metadata");
        {
            Folder f;
            expect (f.project().replaceWithText ("not valid JSON"));
            const auto mix = f.dir.getChildFile ("session_mix.json");
            expect (mix.replaceWithText (R"({"revision":"original"})"));
            SessionMetadataSnapshot snapshot;
            snapshot.directory = f.dir;
            snapshot.mixerOwner = f.dir;
            snapshot.projectPatchJson = R"({"playlists":["new"]})";
            snapshot.mixJson = R"({"trackCount":1,"revision":"new"})";
            snapshot.backup = false;
            expect (! writeSessionMetadata (snapshot).ok);
            expectEquals (mix.loadFileAsString(), juce::String (R"({"revision":"original"})"));
        }

        beginTest ("A missing project can still be created and valid project fields survive layout patches");
        {
            Folder f;
            SessionMetadataSnapshot snapshot;
            snapshot.directory = f.dir;
            snapshot.mixerOwner = f.dir;
            snapshot.projectPatchJson = R"({"playlists":["keep"],"artist":"Artist"})";
            snapshot.backup = false;
            expect (writeSessionMetadata (snapshot).ok);
            snapshot.projectPatchJson = R"({"ui":{"view":"edit"}})";
            expect (writeSessionMetadata (snapshot).ok);
            const auto saved = juce::JSON::parse (f.project());
            expectEquals (saved["artist"].toString(), juce::String ("Artist"));
            expectEquals (saved["playlists"][0].toString(), juce::String ("keep"));
        }

        beginTest ("Session Info only reads, and a later save keeps what it does not own");
        {
            Folder f;
            MainComponent host; prepare (host, f);
            expect (f.project().replaceWithText (R"({"name":"Original","artist":"Earlier Build","playlists":["old"],"automation":{"revision":"old"}})"));
            SessionInfoDialog::Fields shown;
            SessionInfoDialog::launchForTests = [&] (auto fields) { shown = std::move (fields); };
            const juce::ScopeGuard resetHook { [] { SessionInfoDialog::launchForTests = {}; } };
            const auto before = f.project().loadFileAsString();
            host.showSessionInfo();
            // The session is named by its folder, whatever its project says.
            expectEquals (shown.name, f.dir.getFileName());
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            expectEquals (f.project().loadFileAsString(), before, "showing the session changed its project");

            // An earlier build's fields and anything a newer one wrote stay put.
            SessionMetadataSnapshot newer;
            newer.directory = f.dir;
            newer.mixerOwner = f.dir;
            newer.projectPatchJson = R"({"playlists":["new saved edit"],"automation":{"revision":"new"},"unknownFutureField":17})";
            newer.backup = false;
            expect (writeSessionMetadata (newer).ok);
            const auto saved = juce::JSON::parse (f.project());
            expectEquals (saved["artist"].toString(), juce::String ("Earlier Build"));
            expectEquals (saved["playlists"][0].toString(), juce::String ("new saved edit"));
            expectEquals (saved["automation"]["revision"].toString(), juce::String ("new"));
            expectEquals ((int) saved["unknownFutureField"], 17);
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Raw export resolves legacy and modern media per track with modern precedence");
        {
            Folder f, output;
            expect (writeAudio (f.dir.getChildFile ("Track_01.wav"), 0.25f));
            expect (writeAudio (f.dir.getChildFile ("Track_02.wav"), 0.1f));
            expect (writeAudio (f.track (2), 0.5f));
            MainComponent host; prepare (host, f, 2);
            host.startExportTracksTo (output.dir, { 0, 1 }, ExportOptions());
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            expectEquals (host.lastExportFailures, 0);
            expect (output.dir.getChildFile ("Track_01 - Source 1.wav").existsAsFile());
            const auto second = output.dir.getChildFile ("Track_02 - Source 2.wav");
            juce::AudioFormatManager formats; formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (second));
            expect (reader != nullptr);
            if (reader)
            {
                juce::AudioBuffer<float> sample (1, 1);
                expect (reader->read (&sample, 0, 1, 0, true, false));
                expectWithinAbsoluteError (sample.getSample (0, 0), 0.5f, 0.0001f);
            }
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Edited tracks render the ticked strips, and a stale tick refuses");
        {
            Folder f, output;
            expect (writeAudio (f.track()));
            expect (writeAudio (f.track (2)));
            MainComponent host; prepare (host, f, 2);
            host.completeBounceStems (output.dir, { 1 });
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            const auto stems = output.dir.findChildFiles (juce::File::findFiles, false, "*.wav");
            expectEquals (stems.size(), 1);
            if (stems.size() == 1)
                expect (stems[0].getFileName().startsWith ("Track_02"), stems[0].getFileName());

            host.completeBounceStems (output.dir, { 0, 7 });
            expect (host.statusLabel.getText().containsIgnoreCase ("channel list changed"),
                    host.statusLabel.getText());
            expectEquals (output.dir.findChildFiles (juce::File::findFiles, false, "*.wav").size(), 1,
                          "a stale tick still rendered");
            host.engine.clearAllStripOverrides();
        }

        for (int operation = 0; operation < 3; ++operation)
        {
            beginTest (juce::String ("Deferred picker refuses newly active recording: ")
                       + (operation == 0 ? "import" : operation == 1 ? "stems" : "mix"));
            Folder f, input, output, capture;
            expect (writeAudio (f.track()));
            expect (writeAudio (input.track()));
            MainComponent host; prepare (host, f);
            auto& recorder = host.engine.getRecorder();
            recorder.getTrack (0).armed.store (true);
            expect (recorder.startRecording (capture.dir), "isolated no-device capture did not start");
            if (operation == 0) host.completeAudioImport ({ input.track() });
            else if (operation == 1) host.completeBounceStems (output.dir, { 0 });
            else host.completeBounceStereoMix (output.dir.getChildFile ("mix.wav"));
            expect (! host.sessionIoBusy.load(), "late callback admitted disk work during recording");
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            expect (! f.track (2).existsAsFile(), "late import appended media during recording");
            expect (output.dir.findChildFiles (juce::File::findFiles, false, "*.wav").isEmpty(),
                    "late bounce produced output during recording");
            expect (host.statusLabel.getText().containsIgnoreCase ("Stop recording"));
            recorder.stopRecording();
            host.engine.clearAllStripOverrides();
        }
    }
};
static ReviewPersistenceRepairTests reviewPersistenceRepairTests;
}
