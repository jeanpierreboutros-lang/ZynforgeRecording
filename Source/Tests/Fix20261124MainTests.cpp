// Main window fixes from the 2026-11-24 audit (team main):
//
// - The SMART check asks diskutil about the drive a session folder is on, not
//   the folder (diskutil said "Could not find disk", so it was always Unknown).
//   A reading belongs to the drive it was read from, a new session's drive is
//   read soon while idle, a failing drive read at idle is on the banner, and
//   nothing is launched once a take or playback has started.
// - Through a device ride-through the take clock and the sample-rate alarm use
//   the take's rate, not the stand-in device's or a 48 kHz fallback.
// - The idle record-time figure is kept current between takes.
//
// They run on the window's real paths, on a silent fake device (or a fake
// device pair for the ride-through). The SMART tests answer in diskutil's
// place through MainComponent::smartQueryForTests, so nothing is launched.

#include "../UI/MainComponent.h"
#include "../Audio/SettingsFile.h"
#include "SilentAudioDevice.h"

#include <CoreFoundation/CoreFoundation.h>

#include <functional>
#include <map>
#include <mutex>

namespace zynforge
{
namespace fix1124main
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 256;
    constexpr int    kVerified = (int) MultitrackRecorder::SmartStatus::Verified;
    constexpr int    kFailing  = (int) MultitrackRecorder::SmartStatus::Failing;

    inline juce::File tempDir (const juce::String& tag)
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("zf-main1124-" + tag + "-" + juce::Uuid().toString());
    }

    inline bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
        return done();
    }

    inline void pumpFor (int ms)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
        while (juce::Time::getMillisecondCounterHiRes() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
    }

    // Separate input and output devices, each at its own rate, as CoreAudio
    // lists them. A device opened with both runs at its input's rate.
    struct Rig
    {
        std::map<juce::String, int> inputs  { { "Interface", 8 }, { "Mac Mic", 1 } };
        std::map<juce::String, int> outputs { { "Headphones", 2 }, { "Mac Speakers", 2 } };
        std::map<juce::String, double> rates { { "Interface", 96000.0 }, { "Headphones", 96000.0 },
                                               { "Mac Mic", 48000.0 }, { "Mac Speakers", 48000.0 } };
        juce::String defaultInput { "Mac Mic" }, defaultOutput { "Mac Speakers" };
        juce::StringArray refusesToOpen;

        static juce::StringArray namesOf (const std::map<juce::String, int>& devices)
        {
            juce::StringArray names;
            for (const auto& device : devices) names.add (device.first);
            return names;
        }
        static int countOf (const std::map<juce::String, int>& devices, const juce::String& name)
        {
            const auto it = devices.find (name);
            return it != devices.end() ? it->second : 0;
        }
        double rateOf (const juce::String& output, const juce::String& input) const
        {
            const auto it = rates.find (input.isNotEmpty() ? input : output);
            return it != rates.end() ? it->second : kRate;
        }
    };

    class PairDevice final : public juce::AudioIODevice
    {
    public:
        PairDevice (const juce::String& out, const juce::String& in, Rig& rigIn)
            : juce::AudioIODevice (out.isNotEmpty() ? out : in, "Pair"),
              inputName (in), outputName (out), rig (rigIn),
              numIns (Rig::countOf (rigIn.inputs, in)), numOuts (Rig::countOf (rigIn.outputs, out)),
              rate (rigIn.rateOf (out, in)) {}
        ~PairDevice() override { close(); }

        juce::StringArray getOutputChannelNames() override { return names (numOuts); }
        juce::StringArray getInputChannelNames() override  { return names (numIns); }
        juce::Array<double> getAvailableSampleRates() override { return { rate }; }
        juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
        int getDefaultBufferSize() override { return kBlock; }
        juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int) override
        {
            if (rig.refusesToOpen.contains (inputName) || rig.refusesToOpen.contains (outputName)) return "refused";
            activeIns.clear(); activeOuts.clear();
            for (int i = 0; i < numIns; ++i)  if (ins[i])  activeIns.setBit (i);
            for (int i = 0; i < numOuts; ++i) if (outs[i]) activeOuts.setBit (i);
            opened = true;
            return {};
        }
        void close() override { stop(); opened = false; }
        bool isOpen() override { return opened; }
        void start (juce::AudioIODeviceCallback* cb) override
        {
            callback = cb;
            if (callback != nullptr) callback->audioDeviceAboutToStart (this);
        }
        void stop() override
        {
            if (auto* cb = std::exchange (callback, nullptr)) cb->audioDeviceStopped();
        }
        bool isPlaying() override { return callback != nullptr; }
        juce::String getLastError() override { return {}; }
        int getCurrentBufferSizeSamples() override { return kBlock; }
        double getCurrentSampleRate() override { return rate; }
        int getCurrentBitDepth() override { return 24; }
        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 0; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
        juce::BigInteger getActiveInputChannels() const override { return activeIns; }

        const juce::String inputName, outputName;

    private:
        static juce::StringArray names (int count)
        {
            juce::StringArray result;
            for (int i = 0; i < count; ++i) result.add (juce::String (i + 1));
            return result;
        }

        Rig& rig;
        const int numIns, numOuts;
        const double rate;
        juce::BigInteger activeIns, activeOuts;
        bool opened { false };
        juce::AudioIODeviceCallback* callback { nullptr };
    };

    class PairType final : public juce::AudioIODeviceType
    {
    public:
        explicit PairType (Rig& rigIn) : juce::AudioIODeviceType ("Pair"), rig (rigIn) {}

        void scanForDevices() override {}
        juce::StringArray getDeviceNames (bool wantInputs) const override
        { return Rig::namesOf (wantInputs ? rig.inputs : rig.outputs); }
        int getDefaultDeviceIndex (bool forInput) const override
        { return juce::jmax (0, getDeviceNames (forInput).indexOf (forInput ? rig.defaultInput : rig.defaultOutput)); }
        int getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const override
        {
            auto* pair = dynamic_cast<PairDevice*> (device);
            return pair == nullptr ? -1 : getDeviceNames (asInput).indexOf (asInput ? pair->inputName : pair->outputName);
        }
        bool hasSeparateInputsAndOutputs() const override { return true; }
        juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
        {
            if (output.isEmpty() && input.isEmpty()) return nullptr;
            return new PairDevice (output, input, rig);
        }
        void listChanged() { callDeviceChangeListeners(); }

    private:
        Rig& rig;
    };
}

class Fix20261124MainTests final : public juce::UnitTest
{
public:
    Fix20261124MainTests() : juce::UnitTest ("Fixes 2026-11-24 main", "zynforge") {}

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        MainComponent::s_testConstruct = true;
        const juce::ScopeGuard restoreTestMode { []
        {
            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        } };

        smartReadsTheVolumeNotTheFolder();   // MAIN-01
        {
            MainComponent main;
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            smartVerdictBelongsToItsDrive (main);   // MAIN-01
            smartReadsANewSessionSoon (main);       // MAIN-01
            smartReadsNothingOnceATakeStarts (main);   // MAIN-01, MAIN-03
            smartWorkerStopsWhenATakeStarts (main);    // MAIN-03
            smartFailingAtIdleIsShown (main);           // MAIN-01
            idleRecordTimeIsKeptCurrent (main);         // MAIN-06
        }
        rideThroughKeepsTheTakeRate();   // MAIN-02
    }

private:
    // Stands in for diskutil: answers `answer` and remembers every folder it
    // was asked about. `onQuery` runs on the SMART worker, before the answer.
    struct FakeSmart
    {
        MainComponent& main;
        std::mutex lock;
        juce::Array<juce::File> asked;
        std::atomic<int> answer { fix1124main::kVerified };
        std::function<void (const juce::File&)> onQuery;

        explicit FakeSmart (MainComponent& m) : main (m)
        {
            std::atomic_store (&MainComponent::smartQueryForTests,
                std::make_shared<const std::function<int (const juce::File&)>> ([this] (const juce::File& folder)
                {
                    {
                        const std::lock_guard<std::mutex> hold (lock);
                        asked.add (folder);
                    }
                    if (onQuery) onQuery (folder);
                    return answer.load();
                }));
        }

        ~FakeSmart()
        {
            settle();
            std::atomic_store (&MainComponent::smartQueryForTests,
                               std::shared_ptr<const std::function<int (const juce::File&)>>());
        }

        // No probe in flight, and its answer delivered.
        bool settle (int timeoutMs = 6000)
        {
            const bool done = fix1124main::pumpUntil ([this] { return ! main.smartQueryInFlight.load(); }, timeoutMs);
            fix1124main::pumpFor (20);
            return done;
        }

        juce::Array<juce::File> calls()
        {
            const std::lock_guard<std::mutex> hold (lock);
            return asked;
        }

        bool wasAsked (const juce::File& folder) { return calls().contains (folder); }

        void forget()
        {
            const std::lock_guard<std::mutex> hold (lock);
            asked.clear();
        }
    };

    // A session made but not recorded yet, at `rate`, with `tracks` armed.
    struct Session
    {
        MainComponent& main;
        juce::File dir;

        Session (MainComponent& m, const juce::String& tag, double rate = fix1124main::kRate, int tracks = 2, bool prepare = true)
            : main (m), dir (fix1124main::tempDir (tag))
        {
            for (auto* name : { "Audio Files", "Export Files", "Session File Backups" })
                dir.getChildFile (name).createDirectory();
            auto& engine = main.engine;
            engine.clearSessionState();
            main.forgetSessionUiState();
            main.condemnAllStrips();
            engine.setActiveSessionDir (dir);
            engine.claimMixerFor (dir);
            engine.setStripCount (tracks);
            for (int t = 0; t < tracks; ++t) engine.setTrackArmed (t, true);
            main.pendingSampleRate = rate;
            engine.setSessionSampleRate (rate);
            main.lastTrackCount = -1;
            if (prepare) engine.prepareForTests (rate, fix1124main::kBlock);
        }

        ~Session()
        {
            auto& engine = main.engine;
            if (engine.isRecording()) engine.stopRecording();
            fix1124main::pumpUntil ([&engine] { return ! engine.isCaptureFinalizing(); });
            engine.stopPlayback();
            engine.clearSessionState();
            main.forgetSessionUiState();
            main.condemnAllStrips();
            engine.setStripCount (0);
            engine.clearAllStripOverrides();
            main.sampleRateWarning.clear();
            main.srMismatchWarned = 0.0;
            main.pendingSampleRate = fix1124main::kRate;
            engine.setSessionSampleRate (fix1124main::kRate);
            main.updateCaptureWarnings (EngineStatus());
            main.lastTrackCount = -1;
            MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            dir.deleteRecursively();
        }
    };


    static bool bannerSays (MainComponent& main, const juce::String& text)
    {
        for (const auto& alarm : main.alarmBanner.getAlarms())
            if (alarm.text.contains (text)) return true;
        return false;
    }

    static void idleTicks (MainComponent& main, int count)
    {
        for (int i = 0; i < count; ++i)
            main.updateClockAndDiskCadence (fix1124main::kRate);
    }

    // ── MAIN-01: diskutil is asked about the volume ───────────────────────
    void smartReadsTheVolumeNotTheFolder()
    {
        beginTest ("MAIN-01: SMART is read for the drive a session folder is on, not the folder");
        const auto root = MultitrackRecorder::querySmartStatus (juce::File ("/"));
        if (root == MultitrackRecorder::SmartStatus::Unknown)
        {
            // A CI VM's disk reports "Not Supported"; there is nothing to compare.
            logMessage ("  skipped: this Mac's boot volume reports no SMART status");
            expect (true);
            return;
        }
        // A folder below a mount point: diskutil says "Could not find disk"
        // for it, and the drive's real status was never read.
        auto folder = fix1124main::tempDir ("smart");
        expect (folder.getChildFile ("Audio Files").createDirectory().wasOk());
        const auto status = MultitrackRecorder::querySmartStatus (folder.getChildFile ("Audio Files"));
        expect (status != MultitrackRecorder::SmartStatus::Unknown,
                "a session folder on a drive that reports SMART read as Unknown");
        folder.deleteRecursively();

        // A folder that is not there is not read as the boot disk's.
        expect (MultitrackRecorder::querySmartStatus (folder) == MultitrackRecorder::SmartStatus::Unknown);
    }

    // ── MAIN-01: a reading is for one drive ───────────────────────────────
    void smartVerdictBelongsToItsDrive (MainComponent& main)
    {
        beginTest ("MAIN-01: a SMART reading does not carry over to another session's drive");
        FakeSmart smart (main);
        {
            Session first (main, "smart-a");
            smart.answer = fix1124main::kFailing;
            main.pollSmartStatusAsync();
            expect (smart.settle(), "the SMART probe never finished");
            expect (smart.wasAsked (first.dir), "the session's drive was not read");
            EngineStatus rolling;
            rolling.recording = true;
            main.updateCaptureWarnings (rolling);
            expect (main.activeCaptureWarning.contains ("SMART FAILING"),
                    "a failing reading for this session's drive is not shown during a take");
            main.updateCaptureWarnings (EngineStatus());
        }
        smart.answer = fix1124main::kVerified;
        Session second (main, "smart-b");
        idleTicks (main, 1);
        EngineStatus rolling;
        rolling.recording = true;
        main.updateCaptureWarnings (rolling);
        expect (! main.activeCaptureWarning.contains ("SMART FAILING"),
                "another drive's failing reading is shown for this session: " + main.activeCaptureWarning);
        main.updateCaptureWarnings (EngineStatus());
        expect (! bannerSays (main, "SMART FAILING"), "the banner still blames the last drive");
    }

    // ── MAIN-01: a new session's drive is read while idle, soon ───────────
    void smartReadsANewSessionSoon (MainComponent& main)
    {
        beginTest ("MAIN-01: a session's drive is read within seconds of the session opening, while idle");
        FakeSmart smart (main);
        smart.settle();
        idleTicks (main, 1);
        smart.settle();
        main.smartIdleTick = 100;   // far from the regular poll
        smart.forget();
        Session session (main, "smart-new");
        for (int i = 0; i < 30 && ! smart.wasAsked (session.dir); ++i)
        {
            idleTicks (main, 1);
            fix1124main::pumpFor (5);
        }
        smart.settle();
        expect (smart.wasAsked (session.dir), "the new session's drive was not read within 3 s of idle");
    }

    // ── MAIN-01 / MAIN-03: nothing during a take, the drive read after it ─
    void smartReadsNothingOnceATakeStarts (MainComponent& main)
    {
        beginTest ("MAIN-01: no SMART read while a take rolls; the take's drive is read once it is over");
        FakeSmart smart (main);
        main.smartIdleTick = 100;
        Session session (main, "smart-take");
        expect (main.engine.startRecording (session.dir), main.engine.getRecorder().getStartError());
        smart.forget();
        for (int i = 0; i < 40; ++i)
        {
            main.updateClockAndDiskCadence (fix1124main::kRate);
            fix1124main::pumpFor (2);
        }
        smart.settle();
        expectEquals (smart.calls().size(), 0, "SMART was read during a take");

        main.engine.stopRecording();
        expect (fix1124main::pumpUntil ([&main] { return ! main.engine.isCaptureFinalizing(); }));
        for (int i = 0; i < 30 && ! smart.wasAsked (session.dir); ++i)
        {
            idleTicks (main, 1);
            fix1124main::pumpFor (5);
        }
        smart.settle();
        expect (smart.wasAsked (session.dir), "the take's drive was never read after the take");
    }

    // ── MAIN-03: the worker checks again before each drive ────────────────
    void smartWorkerStopsWhenATakeStarts (MainComponent& main)
    {
        beginTest ("MAIN-03: the SMART worker launches nothing more once a take has started");
        auto backup = fix1124main::tempDir ("smart-backup");
        expect (backup.createDirectory().wasOk());
        {
            Session session (main, "smart-worker");
            expect (main.engine.getRecorder().setBackupDirectory (backup), "the backup folder was refused");
            FakeSmart smart (main);
            smart.settle();
            smart.forget();
            juce::WaitableEvent entered, release;
            std::atomic<bool> first { true };
            smart.onQuery = [&entered, &release, &first] (const juce::File&)
            {
                if (! first.exchange (false)) return;
                entered.signal();
                release.wait (5000);
            };
            main.pollSmartStatusAsync();
            const bool asked = entered.wait (5000);
            expect (asked, "the SMART worker never asked about the first drive");
            if (asked)
                expect (main.engine.startRecording (session.dir), main.engine.getRecorder().getStartError());
            release.signal();
            expect (smart.settle(), "the SMART probe never finished");
            expectEquals (smart.calls().size(), 1, "a second drive was read after the take started");
            smart.onQuery = nullptr;
            if (main.engine.isRecording()) main.engine.stopRecording();
            fix1124main::pumpUntil ([&main] { return ! main.engine.isCaptureFinalizing(); });
            main.engine.getRecorder().setBackupDirectory ({});
        }
        backup.deleteRecursively();
    }

    // ── MAIN-01: a failing drive read at idle is said before RECORD ───────
    void smartFailingAtIdleIsShown (MainComponent& main)
    {
        beginTest ("MAIN-01: a failing SMART reading taken while idle is on the banner before any take");
        FakeSmart smart (main);
        Session session (main, "smart-idle");
        idleTicks (main, 1);
        smart.settle();
        smart.answer = fix1124main::kFailing;
        main.pollSmartStatusAsync();
        expect (smart.settle(), "the SMART probe never finished");
        expect (smart.wasAsked (session.dir));
        main.updateCaptureWarnings (EngineStatus());
        expect (bannerSays (main, "SMART FAILING"), "a failing drive read at idle is not on the banner");
        smart.answer = fix1124main::kVerified;
        main.pollSmartStatusAsync();
        smart.settle();
        main.updateCaptureWarnings (EngineStatus());
        expect (! bannerSays (main, "SMART FAILING"), "the banner kept a reading the drive no longer gives");
    }

    // ── MAIN-06: TIME LEFT between takes ───────────────────────────────────
    void idleRecordTimeIsKeptCurrent (MainComponent& main)
    {
        beginTest ("MAIN-06: between takes the record-time figure is worked out again every few seconds");
        Session session (main, "time-left");
        auto& engine = main.engine;
        engine.refreshDiskMinutesRemaining();
        const int at48 = engine.getEstimatedMinutesRemaining();
        expect (at48 > 0, "no record-time figure for the session's drive");

        // Twice the rate drains the drive twice as fast; nothing pre-flight's
        // figure was worked out for changed, so the clock keeps showing it.
        engine.prepareForTests (2.0 * fix1124main::kRate, fix1124main::kBlock);
        for (int t = 0; t < 2; ++t) engine.setTrackArmed (t, true);
        main.idleEstimateFor = main.idleEstimateKey();
        expectEquals (engine.getEstimatedMinutesRemaining(), at48);

        for (int i = 0; i < 60; ++i)
        {
            main.updateClockAndDiskCadence (fix1124main::kRate);
            fix1124main::pumpFor (2);
        }
        fix1124main::pumpUntil ([&engine, at48] { return engine.getEstimatedMinutesRemaining() != at48; }, 3000);
        const int now = engine.getEstimatedMinutesRemaining();
        expect (std::abs (now - at48 / 2) <= juce::jmax (2, at48 / 20),
                "the idle figure was not refreshed: " + juce::String (now) + " min, expected about "
                    + juce::String (at48 / 2));

        beginTest ("MAIN-06: during a take the idle refresh reads no drive");
        engine.prepareForTests (fix1124main::kRate, fix1124main::kBlock);
        for (int t = 0; t < 2; ++t) engine.setTrackArmed (t, true);
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        const auto reads = main.idleRecordTimeReads;
        for (int i = 0; i < 100; ++i) main.updateClockAndDiskCadence (fix1124main::kRate);
        fix1124main::pumpFor (50);
        expectEquals (main.idleRecordTimeReads, reads, "the drives were read for the idle figure during a take");
        engine.stopRecording();
        main.engine.prepareForTests (fix1124main::kRate, fix1124main::kBlock);
    }

    // ── MAIN-02: the take's rate through a ride-through ───────────────────
    void rideThroughKeepsTheTakeRate()
    {
        beginTest ("MAIN-02: during a ride-through the clock and the rate alarm use the take's rate");
        fix1124main::Rig rig;
        MainComponent main;
        main.setSize (1320, 820);
        auto& engine = main.engine;
        auto& manager = engine.getDeviceManager();
        auto owned = std::make_unique<fix1124main::PairType> (rig);
        auto* type = owned.get();
        manager.addAudioDeviceType (std::move (owned));
        manager.setCurrentAudioDeviceType ("Pair", true);
        devicechoice::Chosen chosen;
        chosen.saved = true;
        chosen.input = "Interface";
        chosen.output = "Headphones";
        engine.setChosenDeviceForTests (chosen);
        engine.openChosenDevice();
        manager.addAudioCallback (&engine);
        const juce::ScopeGuard unhook { [&manager, &engine]
        {
            manager.removeAudioCallback (&engine);
            manager.closeAudioDevice();
            updateSettings (engine.getAppProps(), [] (juce::PropertiesFile& props)
            {
                for (const auto* key : { "chosenInputDevice", "chosenOutputDevice", "chosenInputUid",
                                         "chosenOutputUid", "chosenInputCount", "chosenBufferSize" })
                    props.removeValue (key);
            });
        } };
        auto* device = manager.getCurrentAudioDevice();
        expect (device != nullptr && device->getCurrentSampleRate() == 96000.0, "the interface did not open at 96 kHz");

        {
            Session session (main, "ride", 96000.0, 2, false);
            expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
            main.timerCallback();
            expectEquals (main.shownTakeRate, 96000.0);
            expect (main.sampleRateWarning.isEmpty(), main.sampleRateWarning);

            // Headphones out: JUCE closes the pair and opens the Mac's own at 48 kHz.
            rig.outputs.erase ("Headphones");
            type->listChanged();
            expect (engine.isRecording() && engine.isRidingThroughDeviceStop(), "the take is not held open");
            auto* standIn = manager.getCurrentAudioDevice();
            expect (standIn != nullptr && standIn->getCurrentSampleRate() == 48000.0, "no 48 kHz stand-in");
            main.timerCallback();
            expectEquals (main.shownTakeRate, 96000.0, "the take clock runs at the stand-in's rate");
            expect (! main.sampleRateWarning.contains ("MISMATCH"),
                    "a held take raised a rate alarm for the stand-in: " + main.sampleRateWarning);

            // The interface will not open again: nothing is open at all.
            rig.refusesToOpen.add ("Interface");
            engine.serviceDeviceWatch (juce::Time::getMillisecondCounter());
            if (engine.isRidingThroughDeviceStop() && manager.getCurrentAudioDevice() == nullptr)
            {
                main.timerCallback();
                expectEquals (main.shownTakeRate, 96000.0, "the take clock runs at the 48 kHz fallback");
                expect (! main.sampleRateWarning.contains ("MISMATCH"),
                        "a held take raised a rate alarm with no device open: " + main.sampleRateWarning);
            }
            else
            {
                logMessage ("  (no device-less hold on this rig; that half is not checked)");
            }
            engine.stopRecording();
            fix1124main::pumpUntil ([&engine] { return ! engine.isCaptureFinalizing(); });
        }
    }
};

static Fix20261124MainTests fix20261124MainTests;
}
