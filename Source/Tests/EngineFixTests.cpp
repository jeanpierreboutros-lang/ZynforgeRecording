// The device a take records from, and the engine's other guards. The device
// rules are pure and tested as data. The rest runs the real AudioDeviceManager
// and RECORD's whole path on a fake rig, so nothing here reaches CoreAudio.
// What only a real interface can prove is listed in FIELD-TEST.md.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/AudioDeviceDialog.h"
#include "../UI/MainComponent.h"
#include "../Audio/CallbackWatchdog.h"
#include "../Audio/DeviceChoice.h"
#include "../Audio/OscRemote.h"
#include "../Capture/CaptureDaemon.h"

#include <functional>
#include <map>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        // What is plugged in. The manager owns the device type, so the test
        // keeps this and changes it, the way a cable would.
        struct Rig
        {
            std::map<juce::String, int> inputs;   // device name -> its input count
            juce::StringArray outputs { "Speakers" };
            juce::String defaultInput { "Mic" };
            juce::StringArray refusesToOpen;
            int opens { 0 };

            juce::StringArray inputNames() const
            {
                juce::StringArray names;
                for (const auto& device : inputs) names.add (device.first);
                return names;
            }
        };

        class FakeDevice final : public juce::AudioIODevice
        {
        public:
            FakeDevice (const juce::String& nameIn, Rig& rigIn)
                : juce::AudioIODevice (nameIn, "Fake"), rig (rigIn),
                  inputs (rig.inputs.count (nameIn) > 0 ? rig.inputs[nameIn] : 0) {}
            ~FakeDevice() override { stop(); }

            juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
            juce::StringArray getInputChannelNames() override
            {
                juce::StringArray names;
                for (int i = 0; i < inputs; ++i) names.add (juce::String (i + 1));
                return names;
            }
            juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
            juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
            int getDefaultBufferSize() override { return kBlock; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override
            {
                if (rig.refusesToOpen.contains (getName())) return "refused";
                ++rig.opens;
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
            double getCurrentSampleRate() override { return kRate; }
            int getCurrentBitDepth() override { return 24; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
            juce::BigInteger getActiveOutputChannels() const override { return 3; }
            juce::BigInteger getActiveInputChannels() const override
            {
                juce::BigInteger bits;
                bits.setRange (0, inputs, true);
                return bits;
            }

        private:
            Rig& rig;
            int inputs;
            bool opened { false };
            juce::AudioIODeviceCallback* callback { nullptr };
        };

        class FakeType final : public juce::AudioIODeviceType
        {
        public:
            explicit FakeType (Rig& rigIn) : juce::AudioIODeviceType ("Fake"), rig (rigIn) {}

            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool wantInputs) const override
            { return wantInputs ? rig.inputNames() : rig.outputs; }
            int getDefaultDeviceIndex (bool forInput) const override
            { return forInput ? juce::jmax (0, rig.inputNames().indexOf (rig.defaultInput)) : 0; }
            int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
            bool hasSeparateInputsAndOutputs() const override { return true; }
            juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
            { return new FakeDevice (input.isNotEmpty() ? input : output, rig); }
            void listChanged() { callDeviceChangeListeners(); }

        private:
            Rig& rig;
        };

        // The engine has no device of its own in a test process, so this does
        // what its constructor does outside one.
        struct OnRig
        {
            AudioEngine& engine;
            FakeType* type { nullptr };

            OnRig (AudioEngine& engineIn, Rig& rig) : engine (engineIn)
            {
                auto owned = std::make_unique<FakeType> (rig);
                type = owned.get();
                manager().addAudioDeviceType (std::move (owned));
                manager().setCurrentAudioDeviceType ("Fake", true);
                engine.openChosenDevice();
                manager().addAudioCallback (&engine);
            }
            ~OnRig()
            {
                manager().removeAudioCallback (&engine);
                manager().closeAudioDevice();
            }
            juce::AudioDeviceManager& manager() { return engine.getDeviceManager(); }
            juce::String input() { return manager().getAudioDeviceSetup().inputDeviceName; }
            void choose (const juce::String& device)
            {
                auto setup = manager().getAudioDeviceSetup();
                setup.inputDeviceName = device;
                setup.useDefaultInputChannels = true;
                manager().setAudioDeviceSetup (setup, true);
                engine.noteChosenDevice();
            }
        };

        void forgetChosenDevice (AudioEngine& engine)
        {
            updateSettings (engine.getAppProps(), [] (juce::PropertiesFile& props)
            {
                for (const auto* key : { "chosenInputDevice", "chosenOutputDevice", "chosenInputUid",
                                         "chosenOutputUid", "chosenInputCount" })
                    props.removeValue (key);
            });
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        void feedCallback (AudioEngine& engine, int channels, int blocks)
        {
            juce::AudioBuffer<float> in (channels, kBlock), out (2, kBlock);
            in.clear();
            juce::AudioIODeviceCallbackContext context;
            for (int b = 0; b < blocks; ++b)
                engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), channels,
                                                         out.getArrayOfWritePointers(), 2, kBlock, context);
        }
    }

    class EngineFixTests final : public juce::UnitTest
    {
    public:
        EngineFixTests() : juce::UnitTest ("Engine fixes", "zynforge") {}

        void runTest() override
        {
            deviceRules();
            callbackWatchdogRules();
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            chosenDeviceComesBack();
            recordAsksAboutTheDevice();
            deviceThatStopsCallingBack();
            trimFollowHearsThePatch();
        }

    private:
        using Verdict = devicechoice::Verdict;

        static devicechoice::Chosen rigChosen()
        {
            devicechoice::Chosen c;
            c.saved = true;
            c.input = "Rig";
            c.output = "Rig";
            c.inputUid = "uid-rig";
            c.outputUid = "uid-rig";
            c.inputs = 32;
            return c;
        }

        void deviceRules()
        {
            using namespace devicechoice;
            const DeviceList macOnly { { "Mic", "uid-mic" } };
            const DeviceList withRig { { "Mic", "uid-mic" }, { "Rig", "uid-rig" } };

            beginTest ("A saved device is found by its id under a new name, and never by a near name");
            {
                expectEquals (nameNow ("Rig", "uid-rig", withRig), juce::String ("Rig"));
                expectEquals (nameNow ("Rig", "uid-rig", { { "Rig Pro", "uid-rig" } }), juce::String ("Rig Pro"));
                expectEquals (nameNow ("Rig", "", { { "Rig", "" } }), juce::String ("Rig"));
                expect (nameNow ("Rig", "uid-rig", macOnly).isEmpty());
                expect (nameNow ("Rig", "", { { "Rig Pro", "" }, { "Rig (2)", "" } }).isEmpty(),
                        "a near name passed for the chosen device");

                // Two identical units: the id follows the unit when numbering swaps.
                const DeviceList twins { { "Unit (1)", "uid-b" }, { "Unit (2)", "uid-a" } };
                expectEquals (nameNow ("Unit (1)", "uid-a", twins), juce::String ("Unit (2)"));
            }

            beginTest ("What opens at launch: each side stands alone");
            {
                auto open = planOpen (rigChosen(), withRig, withRig, "Mic", "Speakers");
                expectEquals (open.input, juce::String ("Rig"));
                expectEquals (open.output, juce::String ("Rig"));

                open = planOpen (rigChosen(), withRig, macOnly, "Mic", "Speakers");
                expectEquals (open.input, juce::String ("Rig"), "a missing output cost the input");
                expectEquals (open.output, juce::String ("Speakers"));

                open = planOpen (rigChosen(), macOnly, macOnly, "Mic", "Speakers");
                expectEquals (open.input, juce::String ("Mic"));

                auto noInput = rigChosen();
                noInput.input.clear();
                noInput.inputUid.clear();
                expect (planOpen (noInput, withRig, withRig, "Mic", "Speakers").input.isEmpty(),
                        "a choice of no input came back as the Mac's default");
            }

            beginTest ("The verdict on the open device");
            {
                expect (assess ({}, "Mic", macOnly, 1).verdict == Verdict::nothingChosen);
                expect (assess (rigChosen(), "Rig", withRig, 32).verdict == Verdict::onChosen);
                expect (assess (rigChosen(), "Mic", macOnly, 1).verdict == Verdict::chosenMissing);
                expect (assess (rigChosen(), "Mic", withRig, 1).verdict == Verdict::chosenNotOpen);
                expect (assess (rigChosen(), "Rig Pro", { { "Rig Pro", "uid-rig" } }, 32).verdict == Verdict::onChosen,
                        "a renamed device did not count as the chosen one");

                auto noInput = rigChosen();
                noInput.input.clear();
                expect (assess (noInput, {}, macOnly, 0).verdict == Verdict::onChosen);

                expect (! isWrongDevice (assess ({}, "Mic", macOnly, 1)),
                        "a first launch lit the wrong-device warning");
                expect (isWrongDevice (assess (rigChosen(), "Mic", macOnly, 1)));
                expect (isWrongDevice (assess (rigChosen(), "Mic", withRig, 1)));
                expect (! isWrongDevice (assess (rigChosen(), "Rig", withRig, 32)));
                expect (labelText (assess (rigChosen(), "Mic", macOnly, 1)).startsWith ("! In: Mic"));
                expect (labelText (assess (rigChosen(), "Rig", withRig, 32)).startsWith ("In: Rig"));
            }

            beginTest ("An aggregate that lost a member is not the device that was chosen");
            {
                const auto state = assess (rigChosen(), "Rig", withRig, 16);
                expect (state.verdict == Verdict::onChosen);
                expect (state.inputsShrank());
                expect (isWrongDevice (state), "half the inputs gone looked fine");
                expect (! assess (rigChosen(), "Rig", withRig, 64).inputsShrank(), "more inputs is no fault");

                const auto question = recordQuestion (state, 0, {}, "show");
                expect (question.needed);
                expect (question.text.contains ("16"));
                expect (question.text.contains ("32"));
            }

            beginTest ("Only strips that lose their patch count as wrapped");
            {
                expectEquals (stripsThroughWrap ({ { 0, 0 }, { 1, 1 } }, 2), 0);
                expectEquals (stripsThroughWrap ({ { 0, 0 }, { 1, 0 } }, 2), 0);   // a split, on purpose
                expectEquals (stripsThroughWrap ({ { 0, 8 }, { 1, 9 } }, 2), 2);
                expectEquals (stripsThroughWrap ({ { 5, -2 } }, 8), 0);            // default routing
                expectEquals (stripsThroughWrap ({ { 5, -2 }, { 6, -1 } }, 2), 2); // past the end
                expectEquals (stripsThroughWrap ({ { 1, -1 } }, 2), 0);            // gets its own input
                expectEquals (stripsThroughWrap ({ { 0, 8 } }, 0), 0);
            }

            beginTest ("RECORD asks, once per session and device");
            {
                const auto missing = assess (rigChosen(), "Mic", macOnly, 1);
                Answers answers;

                auto question = recordQuestion (missing, 0, answers, "show");
                expect (question.needed);
                expect (! question.answered);
                expect (question.text.contains ("Rig"), "the question did not name the chosen device");
                expect (question.text.contains ("Mic"), "the question did not name the open device");
                expect (question.refusal.contains ("Rig"));
                expect (question.refusal.contains ("Mic"));
                expectEquals (question.device, juce::String ("Mic"));

                answers.add ("show", "Mic");
                question = recordQuestion (missing, 0, answers, "show");
                expect (! question.needed, "an answered question came back");
                expect (question.answered);

                expect (recordQuestion (missing, 0, answers, "other").needed, "an answer crossed sessions");
                const auto otherDevice = assess (rigChosen(), "Line In", macOnly, 2);
                expect (recordQuestion (otherDevice, 0, answers, "show").needed, "an answer crossed devices");

                expect (recordQuestion (assess ({}, "Mic", macOnly, 1), 0, {}, "show").needed,
                        "a device nobody chose rolled without asking");

                const auto onRig = assess (rigChosen(), "Rig", withRig, 32);
                expect (! recordQuestion (onRig, 0, {}, "show").needed);
                question = recordQuestion (onRig, 3, {}, "show");
                expect (question.needed, "the wrap rolled on the chosen device without asking");
                expect (question.text.contains ("3 armed strips"));

                answers.move ("show", "take-folder", "Mic");
                expect (answers.contains ("take-folder", "Mic"));
                expect (! answers.contains ("show", "Mic"));
            }

            beginTest ("A reopen waits for quiet, and a failure backs off");
            {
                const auto waiting = assess (rigChosen(), "Mic", withRig, 1);
                expect (shouldReopen (waiting, true));
                expect (! shouldReopen (waiting, false), "a device was reopened under a take");
                expect (! shouldReopen (assess (rigChosen(), "Mic", macOnly, 1), true));
                expect (! shouldReopen (assess ({}, "Mic", macOnly, 1), true));

                ReopenPacing pacing;
                expect (pacing.due (5000));
                pacing.failed (5000);
                expect (! pacing.due (5500));
                expect (pacing.due (7000));
                pacing.failed (7000);
                expect (! pacing.due (10000), "the wait did not grow");
                expect (pacing.due (11000));
                for (int i = 0; i < 12; ++i) pacing.failed (11000);
                expect (pacing.due (11000 + 60000), "the wait grew past its limit");
                pacing.reset();
                expect (pacing.due (0));
            }

            beginTest ("A choice survives the settings file, and no choice stays none");
            {
                juce::PropertySet props;
                expect (! load (props).saved);
                save (props, rigChosen());
                const auto back = load (props);
                expect (back.saved);
                expectEquals (back.input, juce::String ("Rig"));
                expectEquals (back.outputUid, juce::String ("uid-rig"));
                expectEquals (back.inputs, 32);

                auto noInput = rigChosen();
                noInput.input.clear();
                juce::PropertySet none;
                save (none, noInput);
                expect (load (none).saved, "a choice of no input read as never chosen");
            }
        }

        void trimFollowHearsThePatch()
        {
            beginTest ("Trim-Follow moves the strip the desk channel feeds, not the strip at that position");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setDeviceInputCountForTests (32);
                engine.setStripCount (8);
                for (int i = 0; i < 8; ++i) engine.setTrackInputRouting (i, i);
                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);
                const auto gain = [&engine] (int strip) { return engine.getRecorder().getTrack (strip).liveInputGainDb.load(); };
                const auto known = [&engine] (int strip) { return engine.getRecorder().getTrack (strip).liveInputGainKnown.load(); };

                engine.setTrackInputRouting (2, 6);     // strip 3 listens to In 7
                engine.setTrackInputRouting (6, -1);    // strip 7 hears nothing
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/7/gain", -6.0f));
                expectWithinAbsoluteError (gain (2), -6.0f, 0.001f, "the strip on that channel did not follow");
                expect (! known (6), "the strip at that position moved by another channel's preamp");

                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/3/gain", 9.0f));
                expect (known (2) && std::abs (gain (2) + 6.0f) < 0.001f,
                        "a desk channel nothing listens to moved a strip");

                engine.setTrackStereo (3, true);        // a pair on In 4 and In 5
                engine.setTrackInputRouting (3, 3);
                engine.setTrackInputRouting (4, 4);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/4/gain", 3.0f));
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/5/gain", 4.0f));
                expectWithinAbsoluteError (gain (3), 3.0f, 0.001f, "the left half did not follow");
                expectWithinAbsoluteError (gain (4), 4.0f, 0.001f, "the right half did not follow");

                engine.setTrackInputRouting (4, 3);     // both halves on one channel
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/4/gain", 1.0f));
                expectWithinAbsoluteError (gain (3), 1.0f, 0.001f);
                expectWithinAbsoluteError (gain (4), 1.0f, 0.001f, "a second strip on the channel was left out");

                engine.setDeviceInputCountForTests (0);
                engine.clearAllStripOverrides();
            }

            beginTest ("A strip keeps following its channel after it moves");
            {
                AudioEngine engine;
                engine.clearAllStripOverrides();
                engine.setDeviceInputCountForTests (32);
                engine.setStripCount (3);
                for (int i = 0; i < 3; ++i) engine.setTrackInputRouting (i, i);
                engine.setTrackName (2, "Snare");
                expect (engine.reorderTracks ({ 2, 0, 1 }));

                OscRemote osc (engine);
                osc.setDialect (OscRemote::Dialect::DiGiCo);
                osc.dispatchForTest (juce::OSCMessage ("/Console/Channels/3/gain", -3.0f));
                const auto& moved = engine.getRecorder().getTrack (0);
                expectEquals (moved.getNameThreadSafe(), juce::String ("Snare"));
                expectWithinAbsoluteError (moved.liveInputGainDb.load(), -3.0f, 0.001f, "the moved strip stopped following");
                expect (! engine.getRecorder().getTrack (2).liveInputGainKnown.load(),
                        "the strip now at that position took the moved strip's preamp");

                engine.setDeviceInputCountForTests (0);
                engine.clearAllStripOverrides();
            }
        }

        void callbackWatchdogRules()
        {
            beginTest ("The callback watchdog goes by wall time, from outside the callback");
            {
                CallbackWatchdog dog;
                expect (! dog.update (false, 0, 1000, false), "idle is not a stall");
                expect (! dog.update (true, 0, 1000, false));
                expect (! dog.update (true, 0, 2900, false), "1.9 s is not yet a stall");
                expect (dog.update (true, 0, 3000, false), "2 s with no callback went unseen");
                expectEquals ((int) dog.stalledMs (3500), 2500);

                expect (! dog.update (true, 1, 3600, false), "progress did not clear the alarm");
                expectEquals ((int) dog.stalledMs (9000), 2600, "a closed stall was lost or grew");

                expect (! dog.update (true, 2, 10000, true));
                expect (! dog.update (true, 2, 10200, true));
                expect (dog.update (true, 2, 10300, true), "a device error did not bring the check forward");
                expect (! dog.update (true, 3, 10400, true), "a device that ran through its error stayed alarmed");

                expect (! dog.update (false, 3, 20000, false));
                expect (! dog.update (true, 3, 20000, false));
                expect (! dog.update (true, 3, 21000, false));
                expectEquals ((int) dog.stalledMs (21000), 0, "a new take inherited the last take's stalls");
            }
        }

        void deviceThatStopsCallingBack()
        {
            MainComponent main;
            main.setSize (1320, 820);
            forgetChosenDevice (main.engine);
            Rig rig;
            rig.inputs = { { "Rig", 8 } };
            rig.defaultInput = "Rig";

            beginTest ("A device that stops calling back raises the alarm and never stops the take");
            {
                Show show (main, rig, 1);
                show.on.choose ("Rig");
                show.patchStraight();
                show.arm ({ 0 });
                main.onRecordClicked();
                expect (main.engine.isRecording(), main.statusLabel.getText());

                feedCallback (main.engine, 8, 2);
                expect (! main.engine.captureStatus().deviceStalled);
                expect (pumpUntil ([&main] { return main.engine.captureStatus().deviceStalled; }, 6000),
                        "no alarm after the callback stopped");
                const auto stalled = main.engine.captureStatus();
                expect (main.engine.isRecording(), "the watchdog stopped the take");
                expect (stalled.deviceStallMs >= 2000);

                main.updateCaptureWarnings (stalled);
                expect (main.activeCaptureWarning.contains ("NO AUDIO FROM THE DEVICE"), main.activeCaptureWarning);
                expect (main.alarmBanner.getText().contains ("NO AUDIO"), main.alarmBanner.getText());

                feedCallback (main.engine, 8, 1);
                const auto back = main.engine.captureStatus();
                expect (! back.deviceStalled, "the alarm outlived the stall");
                expect (back.deviceStallMs >= 2000, "the stall was forgotten when the device came back");
                main.updateCaptureWarnings (back);
                expect (main.activeCaptureWarning.contains ("not in the take"), main.activeCaptureWarning);

                show.stop();
                main.updateCaptureWarnings (main.engine.captureStatus());
                const auto report = juce::JSON::parse (show.session.getChildFile ("session.report.json"));
                expectGreaterOrEqual ((juce::int64) report.getProperty ("deviceStallMs", 0), (juce::int64) 2000,
                                      "the report does not say the take has a hole");
            }

            beginTest ("A device error brings the alarm forward without waiting two seconds");
            {
                Show show (main, rig, 1);
                show.on.choose ("Rig");
                show.patchStraight();
                show.arm ({ 0 });
                main.onRecordClicked();
                expect (main.engine.isRecording(), main.statusLabel.getText());
                feedCallback (main.engine, 8, 1);
                main.engine.captureStatus();
                const auto start = juce::Time::getMillisecondCounter();
                main.engine.audioDeviceError ("CoreAudio error");
                expect (pumpUntil ([&main] { return main.engine.captureStatus().deviceStalled; }, 1500),
                        "the error was not seen");
                expectLessThan ((int) (juce::Time::getMillisecondCounter() - start), 1800);
                expect (main.engine.isRecording(), "a device error stopped the take");
                show.stop();
            }

            beginTest ("The capture daemon carries the same alarm in its status");
            {
                capture::CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                daemon.prepareForTests (kRate, kBlock, 1);
                auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("zf-engine-" + juce::Uuid().toString());
                auto& recorder = daemon.getRecorder();
                recorder.getTrack (0).armed.store (true);
                expect (recorder.startRecording (folder));

                std::vector<float> audio ((size_t) kBlock, 0.1f);
                const float* inputs[] { audio.data() };
                daemon.processTestBlock (inputs, 1, kBlock);
                expect (! daemon.buildStatus().deviceStalled);
                expect (pumpUntil ([&daemon] { return daemon.buildStatus().deviceStalled; }, 6000),
                        "the daemon did not see its callback stop");
                expect (daemon.isRecording(), "the daemon's watchdog stopped the take");

                const auto wire = EngineStatus::fromJson (daemon.buildStatus().toJson());
                expect (wire.deviceStalled, "the alarm did not cross the wire");
                expectGreaterOrEqual (wire.deviceStallMs, (juce::int64) 2000);

                daemon.processTestBlock (inputs, 1, kBlock);
                const auto back = daemon.buildStatus();
                expect (! back.deviceStalled);
                expectGreaterOrEqual (back.deviceStallMs, (juce::int64) 2000);
                recorder.stopRecording();
                folder.deleteRecursively();
            }

            forgetChosenDevice (main.engine);
        }

        void chosenDeviceComesBack()
        {
            Rig rig;
            rig.inputs = { { "Mic", 1 }, { "Rig", 32 } };

            {
                AudioEngine engine;
                forgetChosenDevice (engine);
            }

            beginTest ("With nothing saved the Mac's default opens and RECORD still has to ask");
            {
                AudioEngine engine;
                OnRig on (engine, rig);
                expectEquals (on.input(), juce::String ("Mic"));
                expect (engine.getDeviceState().verdict == Verdict::nothingChosen);
                expect (! devicechoice::isWrongDevice (engine.getDeviceState()));
            }

            beginTest ("The chosen device comes back at launch, whatever the Mac's default is");
            {
                {
                    AudioEngine engine;
                    OnRig on (engine, rig);
                    on.choose ("Rig");
                }
                AudioEngine engine;
                OnRig on (engine, rig);
                expectEquals (on.input(), juce::String ("Rig"), "the default device opened instead");
                expect (engine.getDeviceState().verdict == Verdict::onChosen);
                expect (engine.getDeviceManager().createStateXml() != nullptr,
                        "a restored device left no state for the capture daemon");
            }

            beginTest ("A chosen device that is absent opens the default and never looks fine");
            {
                auto away = rig;
                away.inputs.erase ("Rig");
                AudioEngine engine;
                OnRig on (engine, away);
                expectEquals (on.input(), juce::String ("Mic"));
                const auto state = engine.getDeviceState();
                expect (state.verdict == Verdict::chosenMissing);
                expect (devicechoice::isWrongDevice (state));
                expect (engine.getChosenDevice().input == "Rig", "being absent lost the choice");

                // A picker only accepted as it stood keeps the choice; a hand on it replaces it.
                engine.noteChosenDevice (false);
                expectEquals (engine.getChosenDevice().input, juce::String ("Rig"),
                              "an accepted default replaced the interface for good");
                engine.noteChosenDevice (true);
                expectEquals (engine.getChosenDevice().input, juce::String ("Mic"));
                on.choose ("Mic");
            }

            beginTest ("A chosen device that returns while idle is reopened, and what was accepted goes");
            {
                auto stage = rig;
                stage.inputs.erase ("Rig");
                AudioEngine engine;
                OnRig on (engine, stage);
                on.choose ("Mic");
                stage.inputs["Rig"] = 32;
                on.choose ("Rig");

                stage.inputs.erase ("Rig");
                on.type->listChanged();
                expectEquals (on.input(), juce::String ("Mic"), "the device manager kept a device that left");
                expect (engine.getDeviceState().verdict == Verdict::chosenMissing);
                engine.getDeviceAnswers().add ("show", "Mic");

                stage.inputs["Rig"] = 32;
                on.type->listChanged();
                expectEquals (on.input(), juce::String ("Mic"), "JUCE moved back by itself: the watch has no job");
                expect (engine.getDeviceState().verdict == Verdict::chosenNotOpen);

                juce::String error;
                expect (engine.reopenChosenDevice (error), error);
                expectEquals (on.input(), juce::String ("Rig"));
                expect (engine.getDeviceState().verdict == Verdict::onChosen);
                expect (! engine.getDeviceAnswers().contains ("show", "Mic"),
                        "a prep answer outlived the rig coming back");
            }

            beginTest ("A reopen that fails leaves the stand-in open");
            {
                auto stage = rig;
                AudioEngine engine;
                OnRig on (engine, stage);
                on.choose ("Rig");
                stage.inputs.erase ("Rig");
                on.type->listChanged();
                stage.inputs["Rig"] = 32;
                stage.refusesToOpen.add ("Rig");
                on.type->listChanged();

                juce::String error;
                expect (! engine.reopenChosenDevice (error));
                expect (error.isNotEmpty());
                expectEquals (on.input(), juce::String ("Mic"), "a failed reopen left no device");
                expect (engine.getDeviceManager().getCurrentAudioDevice() != nullptr);

                stage.refusesToOpen.clear();
                expect (engine.reopenChosenDevice (error), error);
                expectEquals (on.input(), juce::String ("Rig"));
            }

            beginTest ("A device found under a new name opens and keeps that name");
            {
                {
                    AudioEngine engine;
                    engine.deviceIdsForTests = [] (bool)
                    { return devicechoice::DeviceList { { "Mic", "uid-mic" }, { "Rig", "uid-rig" } }; };
                    OnRig on (engine, rig);
                    on.choose ("Rig");
                    expectEquals (engine.getChosenDevice().inputUid, juce::String ("uid-rig"));
                }
                auto renamed = rig;
                renamed.inputs.erase ("Rig");
                renamed.inputs["Rig Pro"] = 32;
                AudioEngine engine;
                engine.deviceIdsForTests = [] (bool)
                { return devicechoice::DeviceList { { "Mic", "uid-mic" }, { "Rig Pro", "uid-rig" } }; };
                OnRig on (engine, renamed);
                expectEquals (on.input(), juce::String ("Rig Pro"));
                expect (engine.getDeviceState().verdict == Verdict::onChosen);
                expectEquals (engine.getChosenDevice().input, juce::String ("Rig Pro"));
            }

            AudioEngine engine;
            forgetChosenDevice (engine);
        }

        struct Show
        {
            MainComponent& main;
            Rig& rig;
            OnRig on;
            juce::File session;

            Show (MainComponent& mainIn, Rig& rigIn, int strips)
                : main (mainIn), rig (rigIn), on (main.engine, rigIn),
                  session (juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zf-engine-" + juce::Uuid().toString()))
            {
                session.createDirectory();
                main.engine.setRecordStereoMix (false);   // an earlier suite may have left it on
                main.engine.setStripCount (strips);
                main.engine.prepareForTests (kRate, kBlock);
                main.engine.setActiveSessionDir (session);
                main.engine.claimMixerFor (session);
            }

            ~Show()
            {
                if (main.engine.isRecording()) main.stopActiveCaptureAsync (false);
                pumpUntil ([this]
                {
                    return ! main.engine.isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
                main.engine.stopPlayback();
                main.engine.clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                main.engine.setStripCount (0);
                main.engine.clearAllStripOverrides();
                main.lastTrackCount = -1;
                session.deleteRecursively();
            }

            void arm (std::initializer_list<int> strips)
            {
                for (int t = 0; t < main.engine.getRecorder().getNumTracks(); ++t)
                    main.engine.setTrackArmed (t, false);
                for (int t : strips) main.engine.setTrackArmed (t, true);
            }

            void patchStraight()
            {
                for (int t = 0; t < main.engine.getRecorder().getNumTracks(); ++t)
                    main.engine.setTrackInputRouting (t, t);
            }

            bool answer (int value)
            {
                if (main.deviceRecordQuestion == nullptr) return false;
                main.deviceRecordQuestion->exitModalState (value);
                return pumpUntil ([this] { return main.deviceRecordQuestion == nullptr; });
            }

            void stop()
            {
                main.stopActiveCaptureAsync (false);
                pumpUntil ([this]
                {
                    return ! main.engine.isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
            }

            // The poll runs once a second; a test need not wait for it.
            void servicePoll()
            {
                main.nextDeviceCheckMs = juce::Time::getMillisecondCounter();
                main.serviceChosenDevice();
            }
        };

        void recordAsksAboutTheDevice()
        {
            MainComponent main;
            main.setSize (1320, 820);
            forgetChosenDevice (main.engine);
            Rig rig;
            rig.inputs = { { "Mic", 1 }, { "Rig", 8 } };

            beginTest ("RECORD on a device that is not the chosen one asks, and Cancel starts nothing");
            {
                rig.inputs.erase ("Rig");
                rig.inputs["Rig"] = 8;
                Show show (main, rig, 2);
                show.on.choose ("Rig");
                rig.inputs.erase ("Rig");
                show.on.type->listChanged();
                expectEquals (show.on.input(), juce::String ("Mic"));
                show.patchStraight();
                show.arm ({ 0 });

                main.onRecordClicked();
                expect (! main.engine.isRecording(), "RECORD rolled on the wrong device");
                expect (main.deviceRecordQuestion != nullptr, "RECORD did not ask");
                const auto text = main.engine.getRecordDeviceQuestion (show.session.getFullPathName()).text;
                expect (text.contains ("Rig"), text);
                expect (text.contains ("Mic"), text);
                expect (show.answer (0));
                expect (! main.engine.isRecording(), "Cancel started a take");

                main.onRecordClicked();
                expect (main.deviceRecordQuestion != nullptr, "Cancel counted as an answer");
                expect (show.answer (1));
                expect (main.engine.isRecording(), main.statusLabel.getText());
                show.stop();

                main.onRecordClicked();
                expect (main.deviceRecordQuestion == nullptr, "the same session and device asked twice");
                expect (main.engine.isRecording(), main.statusLabel.getText());
                show.stop();

                rig.inputs["Rig"] = 8;
                show.on.type->listChanged();
            }

            beginTest ("A remote or twin RECORD is refused with the reason, and nothing is asked");
            {
                Show show (main, rig, 2);
                show.on.choose ("Rig");
                rig.inputs.erase ("Rig");
                show.on.type->listChanged();
                show.patchStraight();
                show.arm ({ 0 });

                juce::String error;
                const auto answer = main.handleRemoteTransport (AudioEngine::RemoteTransportAction::StartRecord, error);
                expect (answer.has_value() && ! *answer, "a remote RECORD rolled on the wrong device");
                expect (error.contains ("Rig"), "the remote was not told why: " + error);
                expect (main.deviceRecordQuestion == nullptr, "a remote press opened a question here");

                juce::String refusal;
                expect (! main.twinStartRecord (refusal), "a twin RECORD rolled on the wrong device");
                expect (refusal.contains ("Rig"), "the twin was not told why: " + refusal);
                expect (main.deviceRecordQuestion == nullptr);
                expect (! main.engine.isRecording());

                rig.inputs["Rig"] = 8;
                show.on.type->listChanged();
            }

            beginTest ("Armed strips past the device's inputs ask, even on the chosen device");
            {
                Show show (main, rig, 8);
                show.on.choose ("Mic");
                show.patchStraight();
                show.arm ({ 0, 1, 2, 3 });
                main.onRecordClicked();
                expect (! main.engine.isRecording(), "RECORD rolled with strips sharing one input");
                expect (main.deviceRecordQuestion != nullptr);
                expect (show.answer (0));

                show.arm ({ 0 });
                main.onRecordClicked();
                expect (main.deviceRecordQuestion == nullptr, "a strip with its own input was asked about");
                expect (main.engine.isRecording(), main.statusLabel.getText());
                show.stop();

                show.on.choose ("Rig");
            }

            beginTest ("Laptop prep records after one answer");
            {
                Show show (main, rig, 8);
                show.on.choose ("Mic");
                show.patchStraight();
                show.arm ({ 0, 1, 2, 3 });
                main.onRecordClicked();
                expect (show.answer (1));
                expect (main.engine.isRecording(), main.statusLabel.getText());
                show.stop();
                main.onRecordClicked();
                expect (main.deviceRecordQuestion == nullptr, "prep was asked again");
                show.stop();

                show.on.choose ("Rig");
            }

            beginTest ("With nothing chosen RECORD asks about the Mac's default");
            {
                Show show (main, rig, 1);
                {
                    const auto setup = show.on.manager().getAudioDeviceSetup();
                    expectEquals (setup.inputDeviceName, juce::String ("Rig"));
                }
                forgetChosenDevice (main.engine);
                main.engine.setChosenDeviceForTests ({});
                show.arm ({ 0 });
                main.onRecordClicked();
                expect (! main.engine.isRecording(), "RECORD rolled on a device nobody chose");
                expect (main.deviceRecordQuestion != nullptr);
                expect (show.answer (0));
                show.on.choose ("Rig");
            }

            beginTest ("A device that returns during a take waits for the take to end");
            {
                Show show (main, rig, 1);
                show.on.choose ("Rig");
                rig.inputs.erase ("Rig");
                show.on.type->listChanged();
                show.patchStraight();
                show.arm ({ 0 });
                main.onRecordClicked();
                expect (show.answer (1));
                expect (main.engine.isRecording(), main.statusLabel.getText());

                rig.inputs["Rig"] = 8;
                show.on.type->listChanged();
                const int opensBefore = rig.opens;
                show.servicePoll();
                expect (main.engine.isRecording(), "the device was touched during a take");
                expectEquals (rig.opens, opensBefore, "the device was touched during a take");
                expectEquals (show.on.input(), juce::String ("Mic"));

                show.stop();
                show.servicePoll();
                expectEquals (show.on.input(), juce::String ("Rig"), "it never came back after the take");
            }

            beginTest ("A device being tried in the DEVICE panel is left alone");
            {
                Show show (main, rig, 1);
                show.on.choose ("Rig");
                auto setup = show.on.manager().getAudioDeviceSetup();
                setup.inputDeviceName = "Mic";
                setup.useDefaultInputChannels = true;
                show.on.manager().setAudioDeviceSetup (setup, true);
                expect (main.engine.getDeviceState().verdict == Verdict::chosenNotOpen);

                std::unique_ptr<juce::Component> panel;
                auto* window = AudioDeviceDialog::launch (main.engine);
                panel.reset (window);
                show.servicePoll();
                expectEquals (show.on.input(), juce::String ("Mic"), "the watch fought the panel");
                panel.reset();
                show.servicePoll();
                expectEquals (show.on.input(), juce::String ("Rig"));
            }

            beginTest ("The input device stays on the main screen at the smallest window");
            {
                Show show (main, rig, 2);
                show.on.choose ("Rig");
                main.setSize (1120, 700);
                main.switchView (MainComponent::View::Mix);
                show.servicePoll();
                expect (main.deviceLabel.isVisible());
                expectGreaterOrEqual (main.deviceLabel.getWidth(), 140);
                expectGreaterOrEqual (main.deviceLabel.getHeight(), 14);
                expectEquals (main.deviceLabel.getText(), juce::String ("In: Rig"));

                rig.inputs.erase ("Rig");
                show.on.type->listChanged();
                show.servicePoll();
                expect (main.deviceLabel.getText().startsWith ("! In: Mic"), main.deviceLabel.getText());
                expect (main.deviceLabel.findColour (juce::Label::textColourId) == brand::alertAmber);
                main.setSize (1320, 820);
                rig.inputs["Rig"] = 8;
                show.on.type->listChanged();
            }

            forgetChosenDevice (main.engine);
        }
    };

    static EngineFixTests engineFixTests;
}
