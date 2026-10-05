#include "../UI/MainComponent.h"
#include "../Audio/NoiseAnalyzer.h"
#include "../UI/SessionMetadataWriter.h"
#include "../UI/SessionPropertiesDialog.h"
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

        beginTest ("Noise completion summary identifies failed analysis instead of reporting clean metrics");
        {
            NoiseFinding failed;
            failed.error = "Audio read failed";
            const auto summary = MainComponent::noiseAnalysisSummary ({ failed });
            expect (summary.contains ("0 of 1"), summary);
            expect (summary.contains ("1 analysis failed"), summary);
        }
        beginTest ("Noise completion counts only completed findings and reports partial failures");
        {
            NoiseFinding complete, failed;
            complete.humFundamentalHz = 50; complete.bumpCount = 2;
            failed.error = "Audio read failed";
            failed.humFundamentalHz = 60; failed.bumpCount = 3;
            const auto summary = MainComponent::noiseAnalysisSummary ({ complete, failed });
            expect (summary.contains ("1 of 2"), summary);
            expect (summary.contains ("1 analysis failed"), summary);
            expect (summary.contains ("1 with hum"), summary);
            expect (summary.contains ("1 with mic bumps"), summary);
            const auto healthy = MainComponent::noiseAnalysisSummary ({ complete });
            expect (healthy.contains ("1 track analysed") && ! healthy.contains ("failed"), healthy);
        }

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
            snapshot.projectPatchJson = R"({"playlists":["new"]})";
            snapshot.mixJson = R"({"revision":"new"})";
            snapshot.backup = false;
            expect (! writeSessionMetadata (snapshot).ok);
            expectEquals (mix.loadFileAsString(), juce::String (R"({"revision":"original"})"));
        }

        beginTest ("A missing project can still be created and valid project fields survive layout patches");
        {
            Folder f;
            SessionMetadataSnapshot snapshot;
            snapshot.directory = f.dir;
            snapshot.projectPatchJson = R"({"playlists":["keep"],"artist":"Artist"})";
            snapshot.backup = false;
            expect (writeSessionMetadata (snapshot).ok);
            snapshot.projectPatchJson = R"({"ui":{"view":"edit"}})";
            expect (writeSessionMetadata (snapshot).ok);
            const auto saved = juce::JSON::parse (f.project());
            expectEquals (saved["artist"].toString(), juce::String ("Artist"));
            expectEquals (saved["playlists"][0].toString(), juce::String ("keep"));
        }

        beginTest ("Session Properties preserves a newer full save completed while its dialog was open");
        {
            Folder f;
            MainComponent host; prepare (host, f);
            expect (f.project().replaceWithText (R"({"name":"Original","playlists":["old"],"automation":{"revision":"old"}})"));
            SessionPropertiesDialog::SaveCallback save;
            SessionPropertiesDialog::Fields fields;
            SessionPropertiesDialog::launchForTests = [&] (auto initial, auto callback)
            { fields = std::move (initial); save = std::move (callback); };
            const juce::ScopeGuard resetHook { [] { SessionPropertiesDialog::launchForTests = {}; } };
            host.showSessionProperties();
            expect ((bool) save, "actual properties callback was not captured");
            SessionMetadataSnapshot newer;
            newer.directory = f.dir;
            newer.projectPatchJson = R"({"playlists":["new saved edit"],"automation":{"revision":"new"},"unknownFutureField":17})";
            newer.backup = false;
            // Same persistence path as an autosave; complete it before the deferred dialog callback.
            expect (writeSessionMetadata (newer).ok);
            fields.artist = "Edited Artist";
            host.lastSavedUndoUnits = -1; // unrelated edits are still dirty
            if (save) save (fields);
            expect (pumpUntil ([&] { return host.pendingMetadataSaves == 0; }));
            const auto saved = juce::JSON::parse (f.project());
            expectEquals (saved["artist"].toString(), fields.artist);
            expectEquals (saved["playlists"][0].toString(), juce::String ("new saved edit"));
            expectEquals (saved["automation"]["revision"].toString(), juce::String ("new"));
            expectEquals ((int) saved["unknownFutureField"], 17);
            expectEquals (host.lastSavedUndoUnits, -1,
                          "descriptive-only Properties save marked unrelated edits clean");
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Reopening a recorded Click channel with input None never adopts or overwrites its take");
        {
            Folder f;
            expect (writeAudio (f.track()));
            const auto original = bytes (f.track());
            MainComponent host; prepare (host, f);
            host.engine.setTrackName (0, "Click");
            host.engine.setTrackInputRouting (0, -1);
            host.engine.getRecorder().getTrack (0).referenceMedia.store (false);
            expect (host.saveSessionStateTo (f.dir));
            expect (host.openSessionFolder (f.dir) > 0);
            expectEquals (host.clickTrackIndex, -1, "ordinary recording was adopted as generated media");
            // Tempo changes refresh only an adopted click slot, exactly as the UI callback does.
            host.engine.setSessionTempoBpm (132.0f);
            if (host.clickTrackIndex >= 0) host.generateOrRefreshClickTrack();
            expect (pumpUntil ([&] { return ! host.sessionIoBusy.load(); }));
            expect (bytes (f.track()) == original, "tempo refresh replaced recorded audio");
            expect (! host.engine.getRecorder().getTrack (0).referenceMedia.load(), "recording ownership was relabelled");
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Click regeneration independently refuses a stale index pointing to ordinary recorded media");
        {
            Folder f;
            expect (writeAudio (f.track()));
            const auto original = bytes (f.track());
            MainComponent host; prepare (host, f);
            host.engine.setTrackName (0, "Click");
            host.engine.setTrackInputRouting (0, -1);
            host.engine.getRecorder().getTrack (0).referenceMedia.store (false);
            host.clickTrackIndex = 0;
            bool complete = false, success = false;
            host.generateOrRefreshClickTrack ([&] (bool ok) { success = ok; complete = true; });
            expect (pumpUntil ([&] { return complete; }));
            expect (! success, "ordinary media was accepted for regeneration");
            expect (bytes (f.track()) == original);
            expect (! host.engine.getRecorder().getTrack (0).referenceMedia.load());
            host.engine.clearAllStripOverrides();
        }

        beginTest ("Explicit reference click media remains regeneratable after reopen");
        {
            Folder f;
            expect (writeAudio (f.track()));
            const auto original = bytes (f.track());
            MainComponent host; prepare (host, f);
            host.engine.setTrackName (0, "Click");
            host.engine.setTrackInputRouting (0, -1);
            host.engine.getRecorder().getTrack (0).referenceMedia.store (true);
            expect (host.saveSessionStateTo (f.dir));
            expect (host.openSessionFolder (f.dir) > 0);
            expectEquals (host.clickTrackIndex, 0);
            bool complete = false, success = false;
            host.generateOrRefreshClickTrack ([&] (bool ok) { success = ok; complete = true; });
            expect (pumpUntil ([&] { return complete; }));
            expect (success);
            expect (bytes (f.track()) != original);
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
            else if (operation == 1) host.completeBounceStems (output.dir);
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
