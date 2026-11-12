// Engine and DEVICE-panel fixes from the 2026-11-11 review: a take that
// outlives a device that is not its input, RECORD against a device restart,
// PLAY at the wrong rate, outputs past 64, the kept buffer size, device ids,
// the stereo mix writer, the master meter and the DEVICE panel.
#include <CoreFoundation/CoreFoundation.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/MultiPartReader.h"
#include "../Audio/SettingsFile.h"
#include "../UI/AudioDeviceDialog.h"
#include "SilentAudioDevice.h"

#include <map>
#include <thread>

namespace zynforge
{
namespace enginedevice
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 256;

    // Separate input and output devices, as CoreAudio lists them. A device
    // opened with both is named after its OUTPUT, like JUCE's combiner.
    struct Rig
    {
        std::map<juce::String, int> inputs  { { "Interface", 8 }, { "Mac Mic", 1 } };
        std::map<juce::String, int> outputs { { "Headphones", 2 }, { "Mac Speakers", 2 } };
        juce::String defaultInput { "Mac Mic" }, defaultOutput { "Mac Speakers" };
        juce::StringArray refusesToOpen;
        double rate { kRate };
        juce::Array<int> buffers { 128, 256, 512 };
        int defaultBuffer { kBlock };

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
    };

    class PairDevice final : public juce::AudioIODevice
    {
    public:
        PairDevice (const juce::String& out, const juce::String& in, Rig& rigIn)
            : juce::AudioIODevice (out.isNotEmpty() ? out : in, "Pair"),
              inputName (in), outputName (out), rig (rigIn),
              numIns (Rig::countOf (rigIn.inputs, in)), numOuts (Rig::countOf (rigIn.outputs, out)) {}
        ~PairDevice() override { close(); }

        juce::StringArray getOutputChannelNames() override { return names (numOuts); }
        juce::StringArray getInputChannelNames() override  { return names (numIns); }
        juce::Array<double> getAvailableSampleRates() override { return { rig.rate }; }
        juce::Array<int> getAvailableBufferSizes() override { return rig.buffers; }
        int getDefaultBufferSize() override { return rig.defaultBuffer; }
        juce::String open (const juce::BigInteger& ins, const juce::BigInteger& outs, double, int buffer) override
        {
            if (rig.refusesToOpen.contains (inputName) || rig.refusesToOpen.contains (outputName)) return "refused";
            activeIns.clear(); activeOuts.clear();
            for (int i = 0; i < numIns; ++i)  if (ins[i])  activeIns.setBit (i);
            for (int i = 0; i < numOuts; ++i) if (outs[i]) activeOuts.setBit (i);
            currentBuffer = rig.buffers.contains (buffer) ? buffer : rig.defaultBuffer;
            currentRate = rig.rate;
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
        int getCurrentBufferSizeSamples() override { return currentBuffer; }
        double getCurrentSampleRate() override { return currentRate; }
        int getCurrentBitDepth() override { return 24; }
        int getOutputLatencyInSamples() override { return 0; }
        int getInputLatencyInSamples() override { return 0; }
        juce::BigInteger getActiveOutputChannels() const override { return activeOuts; }
        juce::BigInteger getActiveInputChannels() const override { return activeIns; }

        // What JUCE's combiner does when a HAL call fails: both halves stop,
        // and the callback hears only the error.
        void failWithHalError()
        {
            if (auto* cb = std::exchange (callback, nullptr)) cb->audioDeviceError ("CoreAudio error: 1");
        }
        // A restart CoreAudio asked for: stopped, then started again on the
        // same callback, at whatever rate the device now runs.
        juce::AudioIODeviceCallback* stopForRestart()
        {
            auto* cb = callback;
            stop();
            return cb;
        }
        void startAgain (juce::AudioIODeviceCallback* cb) { currentRate = rig.rate; start (cb); }

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
        juce::BigInteger activeIns, activeOuts;
        int currentBuffer { kBlock };
        double currentRate { kRate };
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

    struct Folder
    {
        juce::File dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("zf-enginedevice-" + juce::Uuid().toString());
        Folder()  { dir.createDirectory(); }
        ~Folder() { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); }
        juce::File track (int n = 1) const
        { return dir.getChildFile ("Audio Files/Track_" + juce::String::formatted ("%02d.wav", n)); }
    };

    inline std::unique_ptr<juce::AudioFormatReader> read (const juce::File& file)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        return ConcatReader::create (fm, findTakeParts (file));
    }

    inline void forgetChosenDevice (AudioEngine& engine)
    {
        updateSettings (engine.getAppProps(), [] (juce::PropertiesFile& props)
        {
            for (const auto* key : { "chosenInputDevice", "chosenOutputDevice", "chosenInputUid",
                                     "chosenOutputUid", "chosenInputCount", "chosenBufferSize" })
                props.removeValue (key);
        });
    }

    inline bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 10000)
    {
        const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, true);
        return done();
    }

    // An engine on the rig, the way its constructor would be outside a test.
    struct OnRig
    {
        AudioEngine& engine;
        PairType* type { nullptr };

        OnRig (AudioEngine& engineIn, Rig& rig, const juce::String& input, const juce::String& output,
               int bufferSize = 0)
            : engine (engineIn)
        {
            auto owned = std::make_unique<PairType> (rig);
            type = owned.get();
            manager().addAudioDeviceType (std::move (owned));
            manager().setCurrentAudioDeviceType ("Pair", true);
            devicechoice::Chosen chosen;
            chosen.saved = true;
            chosen.input = input;
            chosen.output = output;
            chosen.bufferSize = bufferSize;
            engine.setChosenDeviceForTests (chosen);
            engine.openChosenDevice();
            manager().addAudioCallback (&engine);
        }
        ~OnRig()
        {
            manager().removeAudioCallback (&engine);
            manager().closeAudioDevice();
            forgetChosenDevice (engine);
        }
        juce::AudioDeviceManager& manager() { return engine.getDeviceManager(); }
        PairDevice* device() { return dynamic_cast<PairDevice*> (manager().getCurrentAudioDevice()); }
    };

    // Feeds the callback as the device would, one block of `value` on every input.
    struct Feeder
    {
        AudioEngine& engine;
        int inputs;
        juce::AudioBuffer<float> in { 8, kBlock }, out { 2, kBlock };
        juce::int64 lastBlockTicks { 0 };

        void blocks (int count, float value, int outputs = 2)
        {
            for (int c = 0; c < inputs; ++c)
                juce::FloatVectorOperations::fill (in.getWritePointer (c), value, kBlock);
            for (int b = 0; b < count; ++b)
            {
                out.clear();
                lastBlockTicks = juce::Time::getHighResolutionTicks();
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), inputs,
                                                         outputs > 0 ? out.getArrayOfWritePointers() : nullptr,
                                                         outputs, kBlock, {});
            }
        }
    };

    // Two armed strips on inputs 1 and 2, no pre-roll, ready to record.
    inline void readyTwoStrips (AudioEngine& engine)
    {
        engine.clearAllStripOverrides();
        engine.setStripCount (2);
        auto& recorder = engine.getRecorder();
        recorder.setPreRollSeconds (0);
        for (int i = 0; i < 2; ++i)
        {
            engine.setTrackInputRouting (i, i);
            recorder.getTrack (i).armed.store (true);
        }
        engine.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
    }

    // A file of `pre` frames at 0.25, then silence, then 0.5 to the end.
    inline void expectGapInFile (juce::UnitTest& test, const juce::File& file, juce::int64 pre, juce::int64 gap,
                                 juce::int64 post)
    {
        auto reader = read (file);
        test.expect (reader != nullptr, "no take file " + file.getFullPathName());
        if (reader == nullptr) return;
        test.expectEquals (reader->lengthInSamples, pre + gap + post, "the take is not pre + gap + post long");
        juce::AudioBuffer<float> data ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&data, 0, (int) reader->lengthInSamples, 0, true, false);
        test.expectWithinAbsoluteError (data.getSample (0, (int) pre - 1), 0.25f, 0.001f);
        if (gap > 0)
            test.expectWithinAbsoluteError (data.getMagnitude (0, (int) pre, (int) gap), 0.0f, 0.00001f);
        test.expectWithinAbsoluteError (data.getSample (0, (int) (pre + gap)), 0.5f, 0.001f);
    }

    // The gap the engine should have written between the last block before a
    // stop and the first one after it: the time between their starts, less
    // one block.
    inline juce::int64 expectedGap (juce::int64 lastTicks, juce::int64 firstTicks)
    {
        const double seconds = juce::Time::highResolutionTicksToSeconds (firstTicks - lastTicks);
        return juce::jmax ((juce::int64) 0, (juce::int64) std::llround (seconds * kRate) - kBlock);
    }

    // Writes a 48 kHz session with one track of constant audio.
    inline void writeSession (const juce::File& dir, int samples = 48000)
    {
        auto file = dir.getChildFile ("Audio Files/Track_01.wav");
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (file.createOutputStream().release(), kRate, 1, 24, {}, 0));
        juce::AudioBuffer<float> audio (1, samples);
        juce::FloatVectorOperations::fill (audio.getWritePointer (0), 0.25f, samples);
        if (writer != nullptr) writer->writeFromAudioSampleBuffer (audio, 0, samples);
    }

    // A file writer whose every write fails, as a full disk's would.
    class FailingWriter final : public juce::AudioFormatWriter
    {
    public:
        explicit FailingWriter (juce::AudioFormatWriter* innerIn)
            : juce::AudioFormatWriter (nullptr, "Failing", innerIn->getSampleRate(),
                                       (unsigned int) innerIn->getNumChannels(),
                                       (unsigned int) innerIn->getBitsPerSample()),
              inner (innerIn)
        {
            usesFloatingPointData = inner->isFloatingPoint();
        }
        bool write (const int**, int) override { return false; }

    private:
        std::unique_ptr<juce::AudioFormatWriter> inner;
    };
}

class EngineDeviceTests final : public juce::UnitTest
{
public:
    EngineDeviceTests() : UnitTest ("Engine device fixes 2026-11-11", "zynforge") {}

    void runTest() override
    {
        AudioEngine::setTestModeSkipAudioInit (true);
        outputsPastSixtyFour();   // C1
        bufferSizeIsKept();   // C1
        deviceIdsAreRetried();   // C1
        playRefusesAWrongRate();   // C2
        devicePanelMasterRevert();   // C3
        devicePanelInputMeter();   // C3
        masterMeterIsPostFader();   // C3
        stereoMixWriteErrors();   // C4
        recordAgainstADeviceStop();   // C5
        takeOutlivesItsMonitorOutput();   // C5
        inputLossStillEndsTheTake();   // C5
        noReturnWithinGraceEndsTheTake();   // C5
        halErrorRidesThrough();   // C5
        restartInPlaceKeepsMonitoring();   // C5
    }

private:
    using Rig = enginedevice::Rig;

    // ── ENG-05 ─────────────────────────────────────────────────────────
    void outputsPastSixtyFour()
    {
        beginTest ("ENG-05: every output a wide device has is opened, at launch and on a reopen");
        AudioEngine engine;
        Rig rig;
        rig.inputs = { { "Desk", 128 }, { "Mac Mic", 1 } };
        rig.outputs = { { "Desk", 128 }, { "Mac Speakers", 2 } };
        enginedevice::OnRig on (engine, rig, "Desk", "Desk");
        expectEquals (engine.getCurrentDeviceOutputCount(), 128, "strips patched past output 64 play nothing");
        expectEquals (engine.getCurrentDeviceInputCount(), 128);

        // The stand-in, then the chosen device back.
        juce::AudioDeviceManager::AudioDeviceSetup standIn;
        on.manager().getAudioDeviceSetup (standIn);
        standIn.inputDeviceName = "Mac Mic";
        standIn.outputDeviceName = "Mac Speakers";
        expect (on.manager().setAudioDeviceSetup (standIn, true).isEmpty());
        juce::String error;
        expect (engine.reopenChosenDevice (error), error);
        expectEquals (engine.getCurrentDeviceOutputCount(), 128, "the reopen fell back to 64 outputs");
    }

    // ── ENG-10 / MIX-14 ────────────────────────────────────────────────
    void bufferSizeIsKept()
    {
        beginTest ("ENG-10: the buffer chosen in DEVICE is saved and used at launch and on a reopen");
        {
            AudioEngine engine;
            Rig rig;
            rig.inputs["Interface"] = 2;
            rig.defaultBuffer = 512;
            enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
            auto setup = on.manager().getAudioDeviceSetup();
            setup.bufferSize = 128;
            expect (on.manager().setAudioDeviceSetup (setup, true).isEmpty());
            engine.noteChosenDevice();
            expectEquals (devicechoice::load (*engine.getAppProps()).bufferSize, 128, "Apply did not keep the buffer");
            expectEquals (engine.getChosenDevice().bufferSize, 128);
        }
        {
            AudioEngine engine;
            Rig rig;
            rig.defaultBuffer = 512;
            enginedevice::OnRig on (engine, rig, "Interface", "Headphones", 128);
            auto* device = on.manager().getCurrentAudioDevice();
            expect (device != nullptr);
            if (device != nullptr)
                expectEquals (device->getCurrentBufferSizeSamples(), 128, "launch opened at the device's default buffer");

            juce::AudioDeviceManager::AudioDeviceSetup standIn;
            on.manager().getAudioDeviceSetup (standIn);
            standIn.inputDeviceName = "Mac Mic";
            standIn.outputDeviceName = "Mac Speakers";
            standIn.bufferSize = 512;
            expect (on.manager().setAudioDeviceSetup (standIn, true).isEmpty());
            juce::String error;
            expect (engine.reopenChosenDevice (error), error);
            device = on.manager().getCurrentAudioDevice();
            expect (device != nullptr);
            if (device != nullptr)
            {
                expectEquals (device->getCurrentBufferSizeSamples(), 128, "the watch reopened at the default buffer");
                expectEquals (device->getCurrentSampleRate(), enginedevice::kRate);
            }
        }
    }

    // ── ENG-11 ─────────────────────────────────────────────────────────
    void deviceIdsAreRetried()
    {
        beginTest ("ENG-11: a device id read that timed out is read again on Apply");
        {
            AudioEngine engine;
            int inputReads = 0;
            engine.deviceIdsForTests = [&inputReads] (bool inputs) -> std::vector<devicechoice::Listed>
            {
                if (inputs && inputReads++ == 0) return {};   // the HAL did not answer in time
                return { { "Interface", inputs ? "UID-IN" : "UID-OUT" }, { "Headphones", "UID-HP" },
                         { "Mac Mic", "UID-MIC" }, { "Mac Speakers", "UID-SPK" } };
            };
            Rig rig;
            enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
            engine.getDeviceState();
            engine.noteChosenDevice();
            expectEquals (engine.getChosenDevice().inputUid, juce::String ("UID-IN"),
                          "the id was never learned this run");
        }
        beginTest ("ENG-11: Apply never replaces a saved device id with none");
        {
            AudioEngine engine;
            engine.deviceIdsForTests = [] (bool) { return std::vector<devicechoice::Listed> {}; };
            Rig rig;
            enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
            devicechoice::Chosen chosen = engine.getChosenDevice();
            chosen.inputUid = "UID-IN";
            chosen.outputUid = "UID-HP";
            engine.setChosenDeviceForTests (chosen);
            engine.noteChosenDevice();
            expectEquals (engine.getChosenDevice().inputUid, juce::String ("UID-IN"), "the saved id was erased");
            expectEquals (engine.getChosenDevice().outputUid, juce::String ("UID-HP"));
            expectEquals (devicechoice::load (*engine.getAppProps()).inputUid, juce::String ("UID-IN"));
        }
    }

    // ── ENG-02 ─────────────────────────────────────────────────────────
    void playRefusesAWrongRate()
    {
        beginTest ("ENG-02: PLAY, a remote PLAY and chase refuse while the device runs at another rate");
        enginedevice::Folder session;
        enginedevice::writeSession (session.dir);
        {
            AudioEngine engine;
            testaudio::useSilentDevice (engine, 96000.0, 2);
            engine.prepareForTests (96000.0, testaudio::kSilentBlock);
            expect (engine.loadSession (session.dir) > 0);
            engine.getPlayer().setPositionSamples (0);
            expect (! engine.startPlayback(), "the 48 kHz session played to the desk at 96 kHz");
            expect (! engine.getPlayer().isPlaying());
            expect (engine.getPlaybackRateRefusal().contains ("96"), engine.getPlaybackRateRefusal());
            juce::String error;
            expect (! engine.performRemoteTransport (AudioEngine::RemoteTransportAction::StartPlay, error));
            expect (error.contains ("96"), "a remote PLAY was refused without the reason: " + error);
            expect (engine.getChaseParkReason().contains ("96"), "chase gave no reason: " + engine.getChaseParkReason());
            engine.getDeviceManager().closeAudioDevice();
            enginedevice::forgetChosenDevice (engine);
        }
        {
            AudioEngine engine;
            testaudio::useSilentDevice (engine, 48000.0, 2);
            engine.prepareForTests (48000.0, testaudio::kSilentBlock);
            expect (engine.loadSession (session.dir) > 0);
            engine.getPlayer().setPositionSamples (0);
            expect (engine.getPlaybackRateRefusal().isEmpty(), engine.getPlaybackRateRefusal());
            expect (engine.startPlayback(), "a matching rate was refused");
            engine.stopPlayback();
            engine.getDeviceManager().closeAudioDevice();
            enginedevice::forgetChosenDevice (engine);
        }
    }

    // ── MIX-01 ─────────────────────────────────────────────────────────
    static juce::ComboBox* monitorCombo (juce::Component& root)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* combo = dynamic_cast<juce::ComboBox*> (child))
                if (combo->indexOfItemId (100000) >= 0) return combo;
            if (auto* found = monitorCombo (*child)) return found;
        }
        return nullptr;
    }

    void devicePanelMasterRevert()
    {
        beginTest ("MIX-01: closing DEVICE never undoes a master pair set elsewhere");
        AudioEngine engine;
        testaudio::useSilentDevice (engine, 48000.0, 2);
        engine.setMasterOutputs (0, 1);
        {
            std::unique_ptr<juce::DialogWindow> window (AudioDeviceDialog::launch (engine));
            expect (window != nullptr);
            engine.setMasterOutputs (-1, -1);   // the MASTER strip takes the mix off its pair
        }
        expectEquals (engine.getMasterOutputL(), -1, "closing DEVICE put the mix back on its pair");
        expectEquals (engine.getMasterOutputR(), -1);

        beginTest ("MIX-01: closing DEVICE without Apply reverts the pair the panel itself changed");
        engine.setMasterOutputs (0, 1);
        {
            std::unique_ptr<juce::DialogWindow> window (AudioDeviceDialog::launch (engine));
            auto* combo = window != nullptr ? monitorCombo (*window) : nullptr;
            expect (combo != nullptr, "no monitor pair picker");
            if (combo != nullptr) combo->setSelectedId (100000, juce::sendNotificationSync);
            expectEquals (engine.getMasterOutputL(), -1, "the picker did not move the mix");
        }
        expectEquals (engine.getMasterOutputL(), 0, "Cancel kept the panel's untried change");
        expectEquals (engine.getMasterOutputR(), 1);
        engine.getDeviceManager().closeAudioDevice();
        enginedevice::forgetChosenDevice (engine);
    }

    // ── MIX-03 ─────────────────────────────────────────────────────────
    void devicePanelInputMeter()
    {
        beginTest ("MIX-03: the DEVICE input meter shows the device's inputs, with no strip and not playback");
        AudioEngine engine;
        engine.clearAllStripOverrides();
        engine.setStripCount (0);
        engine.prepareForTests (enginedevice::kRate, enginedevice::kBlock);
        engine.setDeviceInputMeterOn (true);
        enginedevice::Feeder feed { engine, 2 };
        feed.blocks (4, 0.5f);
        expectWithinAbsoluteError (AudioDeviceDialog::inputMeterLevel (engine), 0.5f, 0.01f,
                                   "the meter stayed dead on a live input");

        engine.setStripCount (1);
        feed.blocks (60, 0.0f);
        engine.getRecorder().getTrack (0).peak.store (0.9f);   // what VSC playback writes
        expectLessThan (AudioDeviceDialog::inputMeterLevel (engine), 0.01f,
                        "the meter showed a strip's playback as input");
        engine.setDeviceInputMeterOn (false);
    }

    // ── MIX-12 ─────────────────────────────────────────────────────────
    void masterMeterIsPostFader()
    {
        beginTest ("MIX-12: the MASTER meter follows the master fader and MUTE");
        AudioEngine engine;
        engine.clearAllStripOverrides();
        engine.setStripCount (1);
        engine.prepareForTests (enginedevice::kRate, enginedevice::kBlock);
        auto& t = engine.getRecorder().getTrack (0);
        t.monitor.store (true); t.armed.store (false); t.gainDb.store (0.0f); t.pan.store (0.0f);
        t.muted.store (false); t.soloed.store (false);
        engine.setTrackInputRouting (0, 0);
        engine.setTrackOutputRouting (0, -1);
        engine.setMasterStereo (true);
        engine.setMasterOutputs (0, 1);
        engine.getMasterState().muted.store (false);
        engine.getMasterState().gainDb.store (0.0f);
        enginedevice::Feeder feed { engine, 2 };
        auto& master = engine.getMasterState();

        master.peak.store (0.0f);
        feed.blocks (1, 0.5f);
        const float unity = master.peak.load();
        expect (unity > 0.2f, "no master level at unity");

        master.gainDb.store (-20.0f);
        master.peak.store (0.0f);
        feed.blocks (1, 0.5f);
        expectWithinAbsoluteError (master.peak.load(), unity * 0.1f, 0.01f, "the meter ignored the fader");

        master.gainDb.store (0.0f);
        master.muted.store (true);
        master.peak.store (0.0f);
        feed.blocks (1, 0.5f);
        expectLessThan (master.peak.load(), 0.0001f, "the meter moved with MUTE on");
        master.muted.store (false);
    }

    // ── ENG-09 ─────────────────────────────────────────────────────────
    void stereoMixWriteErrors()
    {
        beginTest ("ENG-09: a StereoMix.wav write that fails is reported");
        enginedevice::Folder session;
        AudioEngine engine;
        engine.clearAllStripOverrides();
        engine.setStripCount (1);
        engine.prepareForTests (enginedevice::kRate, enginedevice::kBlock);
        engine.getRecorder().setPreRollSeconds (0);
        engine.setTrackInputRouting (0, 0);
        engine.getRecorder().getTrack (0).armed.store (true);
        engine.getRecorder().getTrack (0).streamSend.store (true);
        engine.setRecordStereoMix (true);
        engine.wrapStereoMixWriterForTests = [] (juce::AudioFormatWriter* inner) -> juce::AudioFormatWriter*
        { return new enginedevice::FailingWriter (inner); };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 1 };
        feed.blocks (40, 0.25f);
        expect (enginedevice::pumpUntil ([&] { return engine.hasStereoMixWriteFailed(); }, 3000),
                "the mix lost its audio and nothing said so");
        expect (engine.captureStatus().stereoMixFailed);
        engine.stopRecording();
        expect (engine.hasStereoMixWriteFailed(), "STOP forgot the failure");
        engine.setRecordStereoMix (false);
        engine.wrapStereoMixWriterForTests = nullptr;
    }

    // ── ENG-04 ─────────────────────────────────────────────────────────
    void recordAgainstADeviceStop()
    {
        beginTest ("ENG-04: a device stop landing on a RECORD press never closes the new take unannounced");
        enginedevice::Folder session;
        AudioEngine engine;
        engine.clearAllStripOverrides();
        engine.setStripCount (1);
        engine.prepareForTests (enginedevice::kRate, enginedevice::kBlock);
        engine.getRecorder().setPreRollSeconds (0);
        engine.getRecorder().getTrack (0).armed.store (true);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };

        std::thread coreAudio;
        auto& structure = engine.getRecorder().getStructureLock();
        engine.beforeRecorderStartForTests = [&]
        {
            // The recorder opens its files under its structure lock; the
            // device restart lands on CoreAudio's thread meanwhile.
            structure.enter();
            coreAudio = std::thread ([&engine] { engine.audioDeviceStopped(); });
            juce::Thread::sleep (200);
        };
        engine.afterRecorderStartForTests = [&] { structure.exit(); };
        const bool started = engine.startRecording (session.dir);
        coreAudio.join();
        engine.beforeRecorderStartForTests = nullptr;
        engine.afterRecorderStartForTests = nullptr;
        expect (started, engine.getRecorder().getStartError());

        enginedevice::pumpUntil ([&] { return ! engine.isCaptureFinalizing(); });
        const bool rolling = engine.isRecording();
        expect (rolling || interrupted, "the take was closed with no alarm and no interruption");
        if (! rolling) expect (engine.captureStatus().captureDeviceLost, "a closed take did not say why");
        if (rolling) engine.stopRecording();
    }

    // ── ENG-01 ─────────────────────────────────────────────────────────
    void takeOutlivesItsMonitorOutput()
    {
        beginTest ("ENG-01: unplugging the monitor output of a device pair keeps the take rolling, aligned");
        enginedevice::Folder session;
        AudioEngine engine;
        Rig rig;
        enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
        enginedevice::readyTwoStrips (engine);
        engine.getRecorder().getTrack (0).streamSend.store (true);
        engine.setRecordStereoMix (true);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 8 };
        feed.blocks (40, 0.25f);
        const auto lastBefore = feed.lastBlockTicks;

        // Headphones out: JUCE closes the combined device, named after them,
        // and opens the Mac's default pair in its place.
        rig.outputs.erase ("Headphones");
        on.type->listChanged();
        expect (engine.isRecording(), "losing the headphones ended the take");
        expect (! engine.isCaptureFinalizing());
        expect (engine.isRidingThroughDeviceStop(), "the take is not held open");
        expect (engine.captureStatus().deviceNotice.isNotEmpty(), "nothing says the device stopped");

        // The watch opens the interface again on its own.
        engine.serviceDeviceWatch (juce::Time::getMillisecondCounter());
        expect (! engine.isRidingThroughDeviceStop(), "the interface was not opened again");
        const auto setup = on.manager().getAudioDeviceSetup();
        expectEquals (setup.inputDeviceName, juce::String ("Interface"));
        expect (setup.outputDeviceName.isEmpty(), "the take came back on " + setup.outputDeviceName);

        juce::Thread::sleep (150);
        feed.blocks (1, 0.5f, 0);
        const auto gap = enginedevice::expectedGap (lastBefore, feed.lastBlockTicks);
        feed.blocks (39, 0.5f, 0);
        const auto status = engine.captureStatus();
        expect (status.missedSamples >= 2 * (gap - 64), "the gap was not counted");
        expect (status.deviceNotice.contains ("MONITOR OUTPUT LOST"), status.deviceNotice);
        expect (status.deviceNotice.contains ("continues"), status.deviceNotice);
        expect (! status.captureDeviceLost);

        engine.stopRecording();
        expect (! interrupted);
        auto reader = enginedevice::read (session.track (1));
        expect (reader != nullptr);
        if (reader != nullptr)
        {
            const auto measured = reader->lengthInSamples - 80 * enginedevice::kBlock;
            expectWithinAbsoluteError ((double) measured, (double) gap, 64.0,
                                       "the silence written is not the time the device was away");
            enginedevice::expectGapInFile (*this, session.track (1), 40 * enginedevice::kBlock, measured,
                                           40 * enginedevice::kBlock);
            enginedevice::expectGapInFile (*this, session.track (2), 40 * enginedevice::kBlock, measured,
                                           40 * enginedevice::kBlock);
            auto mix = enginedevice::read (session.dir.getChildFile ("Export Files/StereoMix.wav"));
            expect (mix != nullptr);
            if (mix != nullptr)
                expectEquals (mix->lengthInSamples, reader->lengthInSamples, "StereoMix.wav lost its place");
        }
        engine.setRecordStereoMix (false);

        beginTest ("ENG-01: after the take, the monitor output comes back once it is listed again");
        rig.outputs["Headphones"] = 2;
        on.type->listChanged();
        const auto state = engine.getDeviceState();
        expect (devicechoice::shouldReopen (state, true), "nothing will put the headphones back");
        juce::String error;
        expect (engine.reopenChosenDevice (error), error);
        expectEquals (on.manager().getAudioDeviceSetup().outputDeviceName, juce::String ("Headphones"));
        expect (! devicechoice::shouldReopen (engine.getDeviceState(), true));
    }

    void inputLossStillEndsTheTake()
    {
        beginTest ("ENG-01: losing the input device still ends the take as a device loss");
        enginedevice::Folder session;
        AudioEngine engine;
        Rig rig;
        enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
        enginedevice::readyTwoStrips (engine);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 8 };
        feed.blocks (20, 0.25f);

        rig.inputs.erase ("Interface");
        if (auto* device = on.device()) device->stop();   // the combiner stops with its input half
        expect (! engine.isRecording(), "a take whose input is gone kept rolling");
        expect (! engine.isRidingThroughDeviceStop());
        expect (enginedevice::pumpUntil ([&] { return interrupted && ! engine.isCaptureFinalizing(); }),
                "the take never finished closing");
        expect (engine.captureStatus().captureDeviceLost, "the device loss was not latched");
    }

    void noReturnWithinGraceEndsTheTake()
    {
        beginTest ("ENG-01: an input that does not come back within the grace period ends the take");
        enginedevice::Folder session;
        AudioEngine engine;
        Rig rig;
        enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
        enginedevice::readyTwoStrips (engine);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 8 };
        feed.blocks (20, 0.25f);

        rig.refusesToOpen.add ("Interface");
        rig.outputs.erase ("Headphones");
        on.type->listChanged();
        const auto start = juce::Time::getMillisecondCounter();
        engine.serviceDeviceWatch (start + 1000);
        expect (engine.isRecording(), "the take ended inside the grace period");
        expect (engine.isRidingThroughDeviceStop());
        engine.serviceDeviceWatch (start + AudioEngine::kRideThroughGraceMs + 100);
        expect (! engine.isRecording(), "the take is still held open past the grace period");
        expect (! engine.isRidingThroughDeviceStop());
        expect (enginedevice::pumpUntil ([&] { return interrupted && ! engine.isCaptureFinalizing(); }),
                "the take never finished closing");
        expect (engine.captureStatus().captureDeviceLost, "the device loss was not latched");
        auto reader = enginedevice::read (session.track (1));
        expect (reader != nullptr);
        if (reader != nullptr) expectEquals (reader->lengthInSamples, (juce::int64) 20 * enginedevice::kBlock);
    }

    void halErrorRidesThrough()
    {
        beginTest ("ENG-01: a device pair stopped by a HAL error is opened again and the take resumes");
        enginedevice::Folder session;
        AudioEngine engine;
        Rig rig;
        enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
        enginedevice::readyTwoStrips (engine);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 8 };
        feed.blocks (20, 0.25f);
        const auto lastBefore = feed.lastBlockTicks;

        if (auto* device = on.device()) device->failWithHalError();
        const auto now = juce::Time::getMillisecondCounter();
        engine.serviceDeviceWatch (now);
        expect (engine.isRecording());
        expect (on.device() != nullptr && ! on.device()->isPlaying(), "the device should still be stopped");
        engine.serviceDeviceWatch (now + AudioEngine::kSilentStopMs + 100);
        expect (engine.isRecording(), "the HAL error ended the take");
        expect (! engine.isRidingThroughDeviceStop(), "the device was not opened again");
        expect (on.device() != nullptr && on.device()->isPlaying(), "nothing is running");

        juce::Thread::sleep (100);
        feed.blocks (1, 0.5f, 0);
        const auto gap = enginedevice::expectedGap (lastBefore, feed.lastBlockTicks);
        feed.blocks (19, 0.5f, 0);
        engine.stopRecording();
        expect (! interrupted);
        auto reader = enginedevice::read (session.track (1));
        expect (reader != nullptr);
        if (reader != nullptr)
        {
            const auto measured = reader->lengthInSamples - 40 * enginedevice::kBlock;
            expectWithinAbsoluteError ((double) measured, (double) gap, 64.0);
            enginedevice::expectGapInFile (*this, session.track (1), 20 * enginedevice::kBlock, measured,
                                           20 * enginedevice::kBlock);
        }
    }

    void restartInPlaceKeepsMonitoring()
    {
        beginTest ("ENG-01: a restart CoreAudio asks for, from its own thread, resumes on the same pair");
        enginedevice::Folder session;
        AudioEngine engine;
        Rig rig;
        enginedevice::OnRig on (engine, rig, "Interface", "Headphones");
        enginedevice::readyTwoStrips (engine);
        bool interrupted = false;
        engine.onTakeInterrupted = [&interrupted] (bool) { interrupted = true; };
        expect (engine.startRecording (session.dir), engine.getRecorder().getStartError());
        enginedevice::Feeder feed { engine, 8 };
        feed.blocks (20, 0.25f);

        auto* device = on.device();
        expect (device != nullptr);
        if (device == nullptr) return;
        juce::AudioIODeviceCallback* callback = nullptr;
        std::thread coreAudio ([&] { callback = device->stopForRestart(); });
        coreAudio.join();
        expect (engine.isRecording(), "a device restart ended the take");
        expect (engine.isRidingThroughDeviceStop());
        device->startAgain (callback);
        expect (! engine.isRidingThroughDeviceStop(), "the restarted device was not taken back");
        expectEquals (on.manager().getAudioDeviceSetup().outputDeviceName, juce::String ("Headphones"));
        feed.blocks (20, 0.5f);
        const auto notice = engine.captureStatus().deviceNotice;
        expect (notice.contains ("continues") && ! notice.contains ("MONITOR OUTPUT LOST"), notice);
        engine.stopRecording();
        expect (! interrupted);

        beginTest ("ENG-01: a restart at another rate is not taken back, and the take ends after the grace period");
        enginedevice::Folder second;
        expect (engine.startRecording (second.dir), engine.getRecorder().getStartError());
        feed.blocks (20, 0.25f);
        device = on.device();
        if (device == nullptr) return;
        callback = device->stopForRestart();
        rig.rate = 96000.0;
        device->startAgain (callback);
        expect (engine.isRecording());
        expect (engine.isRidingThroughDeviceStop(), "a 96 kHz device was taken into a 48 kHz take");
        engine.serviceDeviceWatch (juce::Time::getMillisecondCounter() + AudioEngine::kRideThroughGraceMs + 100);
        expect (! engine.isRecording(), "the take is still open");
        expect (enginedevice::pumpUntil ([&] { return interrupted && ! engine.isCaptureFinalizing(); }));
    }
};

static EngineDeviceTests engineDeviceTests;
}
