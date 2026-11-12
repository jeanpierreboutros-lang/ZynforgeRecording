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

};

static EngineDeviceTests engineDeviceTests;
}
