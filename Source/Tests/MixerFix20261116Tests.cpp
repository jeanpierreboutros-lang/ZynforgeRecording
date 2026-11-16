// Fixes from the 2026-11-16 audit in the mixer's area: where New Session puts
// a show when the chosen drive is unplugged, which device the Welcome saves,
// what Backup & Mirror Drives and pre-flight say about a drive that is not
// there, Session Settings answered after the session changed, and what a
// cleared strip name leaves behind (take files are named for strips).

#include <juce_audio_utils/juce_audio_utils.h>

#include "../UI/MainComponent.h"
#include "../UI/AddTracksDialog.h"
#include "../UI/BackupMirrorDialog.h"
#include "../UI/ChannelStrip.h"
#include "../UI/EditPage.h"
#include "../UI/EditTrackRow.h"
#include "../UI/NewSessionDialog.h"
#include "../UI/PreflightDialog.h"
#include "../UI/WelcomeDialog.h"
#include "../Audio/DeviceChoice.h"
#include "../Audio/SettingsFile.h"
#include "../Audio/TakeNames.h"
#include "SilentAudioDevice.h"

#include <functional>
#include <map>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    namespace
    {
        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        void walk (juce::Component& c, const std::function<void (juce::Component&)>& fn)
        {
            for (int i = 0; i < c.getNumChildComponents(); ++i)
            {
                auto* child = c.getChildComponent (i);
                fn (*child);
                walk (*child, fn);
            }
        }

        int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-mix1116-" + tag + "-" + juce::Uuid().toString());
        }

        // A folder on a drive that is not plugged in.
        juce::File unpluggedFolder (const juce::String& leaf)
        {
            return juce::File ("/Volumes/ZF Not Plugged In " + juce::Uuid().toString()).getChildFile (leaf);
        }

        // Where the engine puts a session when the chosen folder cannot be used.
        juce::File bootDiskFallback()
        {
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Zynforge Sessions");
        }

        // The chosen sessions folder for one test, put back after it.
        struct SessionsRootSetting
        {
            juce::PropertiesFile* props;
            juce::String previous;
            SessionsRootSetting (AudioEngine& engine, const juce::File& root)
                : props (engine.getAppProps()), previous (props != nullptr ? props->getValue ("sessionsRoot") : juce::String())
            {
                updateSettings (props, [&root] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", root.getFullPathName()); });
            }
            ~SessionsRootSetting()
            {
                updateSettings (props, [this] (juce::PropertiesFile& p)
                {
                    if (previous.isEmpty()) p.removeValue ("sessionsRoot"); else p.setValue ("sessionsRoot", previous);
                });
            }
            juce::String now() const { return props != nullptr ? props->getValue ("sessionsRoot") : juce::String(); }
        };

        // The saved device choice for one test, put back after it.
        struct ChosenDeviceSetting
        {
            juce::PropertiesFile* props;
            juce::StringPairArray previous;
            explicit ChosenDeviceSetting (AudioEngine& engine) : props (engine.getAppProps())
            {
                if (props != nullptr)
                    for (const auto* key : keys())
                        if (props->containsKey (key)) previous.set (key, props->getValue (key));
            }
            ~ChosenDeviceSetting()
            {
                updateSettings (props, [this] (juce::PropertiesFile& p)
                {
                    for (const auto* key : keys())
                    {
                        if (previous.containsKey (key)) p.setValue (key, previous[key]);
                        else p.removeValue (key);
                    }
                });
            }
            static std::initializer_list<const char*> keys()
            {
                static const std::initializer_list<const char*> list { "chosenInputDevice", "chosenOutputDevice", "chosenInputUid",
                                                                       "chosenOutputUid", "chosenInputCount", "chosenBufferSize" };
                return list;
            }
        };

        // A session folder as the app saves one.
        juce::File savedShow (const juce::File& parent, const juce::String& name)
        {
            const auto dir = parent.getChildFile (name);
            dir.getChildFile ("Audio Files").createDirectory();
            auto* root = new juce::DynamicObject();
            root->setProperty ("formatVersion", 2);
            root->setProperty ("trackCount", 1);
            juce::Array<juce::var> strips;
            auto* strip = new juce::DynamicObject();
            strip->setProperty ("index", 0);
            strip->setProperty ("uid", "strip-" + name);
            strip->setProperty ("name", "Vox");
            strips.add (juce::var (strip));
            root->setProperty ("strips", strips);
            dir.getChildFile ("session_mix.json").replaceWithText (juce::JSON::toString (juce::var (root), true));
            dir.getChildFile (name + ".zfproj").replaceWithText (R"({"zynforgeSession":true,"setlist":[],"playlists":[]})");
            dir.getChildFile ("session_settings.json").replaceWithText (R"({"captureFormat":6,"preRollSeconds":0,"sampleRate":48000.0})");
            return dir;
        }

        // ── A rig with named devices, none of it CoreAudio ──────────────

        struct Rig
        {
            std::map<juce::String, int> inputs;   // device name -> its input count
            juce::StringArray outputs;
            juce::String defaultInput;
            juce::StringArray inputNames() const
            {
                juce::StringArray names;
                for (const auto& device : inputs) names.add (device.first);
                return names;
            }
        };

        class RigDevice final : public juce::AudioIODevice
        {
        public:
            RigDevice (const juce::String& nameIn, Rig& rig)
                : juce::AudioIODevice (nameIn, "Rig"), inputs (rig.inputs.count (nameIn) > 0 ? rig.inputs[nameIn] : 0) {}
            juce::StringArray getOutputChannelNames() override { return { "1", "2" }; }
            juce::StringArray getInputChannelNames() override
            {
                juce::StringArray names;
                for (int i = 0; i < inputs; ++i) names.add (juce::String (i + 1));
                return names;
            }
            juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
            juce::Array<int> getAvailableBufferSizes() override { return { 256 }; }
            int getDefaultBufferSize() override { return 256; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { opened = true; return {}; }
            void close() override { opened = false; }
            bool isOpen() override { return opened; }
            void start (juce::AudioIODeviceCallback*) override { playing = true; }
            void stop() override { playing = false; }
            bool isPlaying() override { return playing; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return 256; }
            double getCurrentSampleRate() override { return 48000.0; }
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
            int inputs;
            bool opened { false }, playing { false };
        };

        class RigType final : public juce::AudioIODeviceType
        {
        public:
            explicit RigType (Rig& rigIn) : juce::AudioIODeviceType ("Rig"), rig (rigIn) {}
            void scanForDevices() override {}
            juce::StringArray getDeviceNames (bool wantInputs) const override
            { return wantInputs ? rig.inputNames() : rig.outputs; }
            int getDefaultDeviceIndex (bool forInput) const override
            { return forInput ? juce::jmax (0, rig.inputNames().indexOf (rig.defaultInput)) : 0; }
            int getIndexOfDevice (juce::AudioIODevice*, bool) const override { return 0; }
            bool hasSeparateInputsAndOutputs() const override { return true; }
            juce::AudioIODevice* createDevice (const juce::String& output, const juce::String& input) override
            { return new RigDevice (input.isNotEmpty() ? input : output, rig); }

        private:
            Rig& rig;
        };

        // The window opens what was chosen, as it does at launch.
        struct OnRig
        {
            AudioEngine& engine;
            OnRig (AudioEngine& engineIn, Rig& rig, const devicechoice::Chosen& chosen) : engine (engineIn)
            {
                engine.setChosenDeviceForTests (chosen);
                engine.getDeviceManager().addAudioDeviceType (std::make_unique<RigType> (rig));
                engine.getDeviceManager().setCurrentAudioDeviceType ("Rig", true);
                engine.openChosenDevice();
            }
            ~OnRig() { engine.getDeviceManager().closeAudioDevice(); }
            juce::String input()  { return engine.getDeviceManager().getAudioDeviceSetup().inputDeviceName; }
            juce::String output() { return engine.getDeviceManager().getAudioDeviceSetup().outputDeviceName; }
        };
    }

    class MixerFix20261116Tests final : public juce::UnitTest
    {
    public:
        MixerFix20261116Tests() : juce::UnitTest ("Mixer fixes 2026-11-16", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            const auto previousConstruct = MainComponent::s_testConstruct;
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restore { [previousConstruct]
            {
                MainComponent::s_testConstruct = previousConstruct;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };

            newSessionDialogShowsTheChosenDrive();
            newSessionRefusesAnUnpluggedDrive();
            csvAndConsoleSessionsStayOffTheBootDisk();
            importWithNoSessionStaysOffTheBootDisk();
            welcomeSavesOnlyThePickedSide();
            driveDialogSaysAnUnpluggedDrive();
            preflightSaysAnUnpluggedDrive();
            sessionsFolderIsNoCopy();
            sessionSettingsAnswersItsOwnSession();
            clearedNamesKeepTrackNumbers();
        }

    private:
        static void forget (MainComponent& host)
        {
            dismissModals();
            host.engine.clearSessionState();
            host.engine.setActiveSessionDir ({});
            host.condemnAllStrips();
            host.engine.setStripCount (0);
            host.engine.clearAllStripOverrides();
        }

        struct Host
        {
            MainComponent main;
            Host()
            {
                main.stopTimer();
                main.engine.prepareForTests (48000, 256);
                main.setSize (1320, 820);
            }
            ~Host() { forget (main); }
            juce::String status() { return main.statusLabel.getText(); }
        };

        // ── MIX-01: New Session on an unplugged sessions drive ──────────

        struct OpenNewSession
        {
            juce::String path;
            bool saysMissing { false };
            juce::Button* create { nullptr };
        };

        static OpenNewSession readNewSessionDialog()
        {
            OpenNewSession seen;
            auto* window = juce::Component::getCurrentlyModalComponent();
            if (window == nullptr) return seen;
            walk (*window, [&seen] (juce::Component& c)
            {
                if (auto* b = dynamic_cast<juce::TextButton*> (&c))
                {
                    const auto text = b->getButtonText();
                    if (text == "Create") seen.create = b;
                    else if (text.startsWith ("/") || text.startsWith ("~")) seen.path = text;
                }
                if (auto* l = dynamic_cast<juce::Label*> (&c))
                    if (l->isVisible() && l->getText().containsIgnoreCase ("not connected")) seen.saysMissing = true;
            });
            return seen;
        }

        void newSessionDialogShowsTheChosenDrive()
        {
            beginTest ("File > New Session with the sessions drive unplugged shows that folder, says so and does not create");
            {
                Host host;
                const auto unplugged = unpluggedFolder ("Zynforge Sessions");
                SessionsRootSetting chosen (host.main.engine, unplugged);
                dismissModals();
                host.main.launchNewSessionDialog();
                expectEquals (modalCount(), 1, "setup: the dialog opened");
                const auto seen = readNewSessionDialog();
                const bool showsIt = seen.path.contains (unplugged.getParentDirectory().getFileName());
                expect (showsIt, "the dialog offered " + seen.path + " for " + unplugged.getFullPathName());
                expect (seen.saysMissing, "nothing in the dialog says the drive is not connected");
                // Only once it offers the unplugged folder: the old dialog would build in ~/Music.
                if (showsIt && seen.create != nullptr && seen.create->onClick)
                {
                    seen.create->onClick();
                    expectEquals (modalCount(), 1, "Create on the unplugged drive closed the dialog");
                }
                dismissModals();
                expectEquals (chosen.now(), unplugged.getFullPathName(), "the chosen drive was replaced");
                expect (host.main.engine.getActiveSessionDir() == juce::File(),
                        "a session was made: " + host.main.engine.getActiveSessionDir().getFullPathName());
            }

            beginTest ("New Session never offers the default folder in place of a chosen one it cannot make");
            {
                Host host;
                const auto base = tempDir ("blocked");
                expect (base.createDirectory().wasOk());
                const auto blocker = base.getChildFile ("Blocker");
                expect (blocker.replaceWithText ("a file where the folder should be"));
                const auto chosenRoot = blocker.getChildFile ("Sessions");
                SessionsRootSetting chosen (host.main.engine, chosenRoot);
                dismissModals();
                host.main.launchNewSessionDialog();
                const auto seen = readNewSessionDialog();
                const bool showsIt = seen.path.contains ("Blocker");
                expect (showsIt, "the dialog offered " + seen.path + " for " + chosenRoot.getFullPathName());
                if (showsIt && seen.create != nullptr && seen.create->onClick)
                {
                    seen.create->onClick();
                    pumpUntil ([] { return modalCount() == 0; }, 2000);
                    expect (host.status().containsIgnoreCase ("couldn't create"), host.status());
                }
                dismissModals();
                expectEquals (chosen.now(), chosenRoot.getFullPathName(), "the chosen folder was replaced");
                expect (host.main.engine.getActiveSessionDir() == juce::File(),
                        "a session was made: " + host.main.engine.getActiveSessionDir().getFullPathName());
                base.deleteRecursively();
            }
        }

        void newSessionRefusesAnUnpluggedDrive()
        {
            beginTest ("New Session (and from a template) answered on an unplugged drive is refused, naming the drive");
            {
                Host host;
                const auto unplugged = unpluggedFolder ("Zynforge Sessions");
                SessionsRootSetting chosen (host.main.engine, unplugged);
                const auto templateFile = tempDir ("template").withFileExtension ("zftemplate");
                expect (templateFile.replaceWithText (R"({"strips":[{"name":"Kick"}]})"));
                for (const auto& from : { juce::File(), templateFile })
                {
                    NewSessionDialog::ResultCallback answer;
                    NewSessionDialog::launchForTests = [&answer] (NewSessionDialog::ResultCallback cb) { answer = std::move (cb); };
                    host.main.launchNewSessionDialog (from);
                    NewSessionDialog::launchForTests = nullptr;
                    expect (answer != nullptr, "setup: the dialog opened");
                    NewSessionDialog::Result r;
                    r.name = "Show";
                    r.location = unplugged;
                    r.sampleRate = 48000.0;
                    if (answer != nullptr) answer (r);
                    expect (host.status().containsIgnoreCase ("not connected"), host.status());
                    expect (host.status().contains (unplugged.getFullPathName()), host.status());
                    expect (host.main.engine.getActiveSessionDir() == juce::File(), "a session was made");
                    expectEquals (chosen.now(), unplugged.getFullPathName(), "the chosen drive was replaced");
                }
                templateFile.deleteFile();
            }
        }

        // A session the old code made on the boot disk is removed again.
        void expectNoBootDiskSession (MainComponent& main, const juce::String& what)
        {
            const auto made = main.engine.getActiveSessionDir();
            expect (made == juce::File(), what + " made a session: " + made.getFullPathName());
            if (made.isAChildOf (bootDiskFallback()))
            {
                forget (main);
                made.deleteRecursively();
            }
        }

        void csvAndConsoleSessionsStayOffTheBootDisk()
        {
            beginTest ("A session from CSV or from the console with the sessions drive unplugged is refused, not made on the boot disk");
            {
                Host host;
                const auto unplugged = unpluggedFolder ("Zynforge Sessions");
                SessionsRootSetting chosen (host.main.engine, unplugged);
                const auto csvDir = tempDir ("csv");
                expect (csvDir.createDirectory().wasOk());
                const auto csv = csvDir.getChildFile ("zf-mix01-" + juce::Uuid().toString() + ".csv");
                expect (csv.replaceWithText ("Kick\nSnare\n"));

                bool asked = false;
                host.main.sessionPickerForTests = [&asked] (const juce::String&, std::function<void (const juce::File&)>)
                { asked = true; };
                host.main.createSessionFromCsv();
                host.main.sessionPickerForTests = {};
                expect (! asked, "the CSV panel opened over an unplugged sessions drive");
                expect (host.status().containsIgnoreCase ("not connected"), host.status());

                host.main.completeSessionFromCsv (csv);
                expect (host.status().containsIgnoreCase ("not connected"), host.status());
                expectNoBootDiskSession (host.main, "New Session from CSV");

                host.main.createSessionFromConsole();
                expect (host.status().containsIgnoreCase ("not connected"), host.status());

                host.main.buildSessionFromConsole ({ { 1, "Kick" }, { 2, "Snare" } }, {}, 0, "ZF MIX01 ");
                expect (host.status().containsIgnoreCase ("not connected"), host.status());
                expectNoBootDiskSession (host.main, "New Session from Console");
                expectEquals (chosen.now(), unplugged.getFullPathName(), "the chosen drive was replaced");
                csvDir.deleteRecursively();
            }
        }

        void importWithNoSessionStaysOffTheBootDisk()
        {
            beginTest ("Import with no session open and the sessions drive unplugged makes no session on the boot disk");
            {
                Host host;
                const auto unplugged = unpluggedFolder ("Zynforge Sessions");
                SessionsRootSetting chosen (host.main.engine, unplugged);
                const auto sourceDir = tempDir ("import-source");
                expect (sourceDir.createDirectory().wasOk());
                const auto source = sourceDir.getChildFile ("Guitar.wav");
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
                        source.createOutputStream().release(), 48000.0, 1, 24, {}, 0));
                    expect (writer != nullptr, "setup: no source file");
                    juce::AudioBuffer<float> audio (1, 4800);
                    audio.clear();
                    if (writer != nullptr) expect (writer->writeFromAudioSampleBuffer (audio, 0, 4800));
                }
                const auto before = bootDiskFallback().findChildFiles (juce::File::findDirectories, false, "Imported_*");
                juce::Array<juce::File> picks;
                picks.add (source);
                host.main.completeAudioImport (picks);
                pumpUntil ([&host] { return ! host.main.sessionIoBusy.load(); }, 20000);
                expect (host.status().containsIgnoreCase ("not connected"), host.status());
                for (const auto& made : bootDiskFallback().findChildFiles (juce::File::findDirectories, false, "Imported_*"))
                    if (! before.contains (made))
                    {
                        expect (false, "the import made a session on the boot disk: " + made.getFullPathName());
                        forget (host.main);
                        made.deleteRecursively();
                    }
                sourceDir.deleteRecursively();
            }
        }

        // ── MIX-02: the Welcome saves only the side picked by hand ──────

        void welcomeSavesOnlyThePickedSide()
        {
            beginTest ("Welcome: changing only the output keeps the chosen input that is not up yet");
            {
                Host host;
                ChosenDeviceSetting keepChoice (host.main.engine);
                Rig rig;
                rig.inputs = { { "Mac Mic", 2 } };
                rig.outputs = { "Speakers", "Phones" };
                rig.defaultInput = "Mac Mic";
                devicechoice::Chosen dante;
                dante.saved = true; dante.input = "Dante Virtual Soundcard"; dante.output = "Speakers"; dante.inputs = 64;
                const auto root = tempDir ("welcome-out");
                SessionsRootSetting keepRoot (host.main.engine, root);
                {
                    OnRig on (host.main.engine, rig, dante);
                    expectEquals (on.input(), juce::String ("Mac Mic"), "setup: the fallback input is open");

                    WelcomeDialog::NewResult r;
                    r.name = "Show"; r.location = root; r.sampleRate = 48000.0;
                    r.inputDeviceName = "Mac Mic";   // what the input picker showed, untouched
                    r.outputDeviceName = "Phones";
                    r.outputPicked = true;
                    expect (host.main.createSessionFromWelcome (r), host.status());
                    expectEquals (on.output(), juce::String ("Phones"), "setup: the output moved");
                    expectEquals (host.main.engine.getChosenDevice().input, juce::String ("Dante Virtual Soundcard"),
                                  "the open fallback input replaced the chosen device");
                    expectEquals (host.main.engine.getChosenDevice().inputs, 64, "the chosen device's input count was replaced");
                    expectEquals (host.main.engine.getChosenDevice().output, juce::String ("Phones"),
                                  "the output picked by hand was not kept");
                    expect (host.main.engine.getDeviceState().verdict == devicechoice::Verdict::chosenMissing,
                            "the missing chosen device stopped being reported");
                }
                forget (host.main);
                root.deleteRecursively();
            }

            beginTest ("Welcome: changing only the input keeps the chosen output");
            {
                Host host;
                ChosenDeviceSetting keepChoice (host.main.engine);
                Rig rig;
                rig.inputs = { { "Mac Mic", 2 }, { "Stage Box", 32 } };
                rig.outputs = { "Speakers" };
                rig.defaultInput = "Mac Mic";
                devicechoice::Chosen chosen;
                chosen.saved = true; chosen.input = "Mac Mic"; chosen.output = "Monitors"; chosen.inputs = 2;
                const auto root = tempDir ("welcome-in");
                SessionsRootSetting keepRoot (host.main.engine, root);
                {
                    OnRig on (host.main.engine, rig, chosen);
                    expectEquals (on.output(), juce::String ("Speakers"), "setup: the fallback output is open");

                    WelcomeDialog::NewResult r;
                    r.name = "Show"; r.location = root; r.sampleRate = 48000.0;
                    r.inputDeviceName = "Stage Box";
                    r.outputDeviceName = "Speakers";   // what the output picker showed, untouched
                    r.inputPicked = true;
                    expect (host.main.createSessionFromWelcome (r), host.status());
                    expectEquals (on.input(), juce::String ("Stage Box"), "setup: the input moved");
                    expectEquals (host.main.engine.getChosenDevice().input, juce::String ("Stage Box"));
                    expectEquals (host.main.engine.getChosenDevice().output, juce::String ("Monitors"),
                                  "the open fallback output replaced the chosen one");
                }
                forget (host.main);
                root.deleteRecursively();
            }
        }

        // ── MIX-03: a drive that is not plugged in ──────────────────────

        void driveDialogSaysAnUnpluggedDrive()
        {
            beginTest ("Backup & Mirror Drives says a backup or mirror drive is not connected, never 'same drive'");
            {
                AudioEngine engine;
                const auto root = tempDir ("unplugged-copies");
                const auto session = root.getChildFile ("Gig");
                expect (session.createDirectory().wasOk());
                engine.setActiveSessionDir (session);
                {
                    backupdlg::Content dialog (engine, {});
                    dialog.backup.root = unpluggedFolder ("Backups");
                    dialog.working = { { unpluggedFolder ("Mirror"), CaptureFormat::Wav24 } };
                    dialog.rebuildRows();
                    dialog.refreshAllRows();
                    for (auto* row : { dialog.backupRow.get(), dialog.rows.empty() ? nullptr : dialog.rows.front().get() })
                    {
                        expect (row != nullptr, "setup: no row");
                        if (row == nullptr) continue;
                        const auto text = row->pathLabel.getText();
                        expect (text.containsIgnoreCase ("not connected"), text);
                        expect (! text.containsIgnoreCase ("same drive"), text);
                    }
                }
                engine.setActiveSessionDir ({});
                root.deleteRecursively();
            }

            beginTest ("Backup & Mirror Drives: a session whose drive is unplugged makes no copy 'same drive'");
            {
                // A pinned session whose drive was pulled; the engine will not
                // pin a folder that is not there, so the check is asked directly.
                const auto backup = tempDir ("boot-backup");
                expect (backup.createDirectory().wasOk());
                const auto checks = backupdlg::Content::checkRoots (unpluggedFolder ("Gig"), backup, {});
                expect (checks.has_value() && ! checks->empty(), "setup: the check did not answer");
                if (checks.has_value() && ! checks->empty())
                    expect (! checks->front().onSessionDrive, "a boot-disk backup reads as on the unplugged session's drive");
                backup.deleteRecursively();
            }
        }

        static std::vector<PreflightRow> preflightRows()
        {
            if (auto* window = dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()))
                if (auto* list = dynamic_cast<PreflightDialog*> (window->getContentComponent()))
                    return list->getRows();
            return {};
        }

        void preflightSaysAnUnpluggedDrive()
        {
            beginTest ("Pre-flight with the sessions drive unplugged calls no boot-disk backup 'same drive', and names an unplugged mirror");
            {
                Host host;
                ChosenDeviceSetting keepChoice (host.main.engine);
                testaudio::useSilentDevice (host.main.engine);
                const auto unplugged = unpluggedFolder ("Zynforge Sessions");
                SessionsRootSetting chosen (host.main.engine, unplugged);
                const auto savedBackup = host.main.engine.getBackupDirectory();
                const auto savedMirrors = host.main.engine.getMirrors();
                const auto backup = tempDir ("preflight-backup");
                expect (backup.createDirectory().wasOk());
                const auto mirror = unpluggedFolder ("Mirror");
                expect (host.main.engine.setBackupDirectory (backup));
                expect (host.main.engine.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                dismissModals();
                host.main.showPreflightChecklist();
                const auto rows = preflightRows();
                dismissModals();
                expect (! rows.empty(), "setup: no checklist");
                bool sawBackup = false, sawMirror = false;
                for (const auto& row : rows)
                {
                    if (row.item == "Backup")
                    {
                        sawBackup = true;
                        expect (! row.value.containsIgnoreCase ("same drive"), row.value);
                    }
                    if (row.item.contains (mirror.getFullPathName()))
                    {
                        sawMirror = true;
                        expect (row.value.containsIgnoreCase ("not connected"), row.value);
                    }
                    if (row.item == "Disk free")
                        expect (row.status != PreflightRow::Status::ok, "the boot disk's space passed for the unplugged drive: " + row.value);
                }
                expect (sawBackup && sawMirror, "setup: a row is missing");
                expect (host.main.engine.setMirrors (savedMirrors));
                expect (host.main.engine.setBackupDirectory (savedBackup));
                backup.deleteRecursively();
            }
        }

        // ── MIX-04: the sessions folder, with no session open ───────────

        void sessionsFolderIsNoCopy()
        {
            beginTest ("With no session open, the sessions folder is refused as the backup or a mirror");
            {
                AudioEngine engine;
                engine.setActiveSessionDir ({});
                const auto sessions = tempDir ("sessions-root");
                expect (sessions.createDirectory().wasOk());
                SessionsRootSetting chosen (engine, sessions);
                const auto savedMirrors = engine.getMirrors();
                {
                    backupdlg::Content dialog (engine, {});
                    expect (! dialog.acceptBackup (sessions), "the sessions folder was taken as the backup");
                    dialog.backup.root = sessions;   // one saved before
                    dialog.refreshAllRows();
                    expect (dialog.backupRow->pathLabel.findColour (juce::Label::textColourId) == brand::accentRecord,
                            "the backup row does not show the sessions folder refused: " + dialog.backupRow->pathLabel.getText());
                }
                {
                    backupdlg::Content dialog (engine, {});
                    dialog.working = { { sessions, CaptureFormat::Wav24 } };
                    dialog.rebuildRows();
                    dialog.applyAndClose();
                    expect (engine.getMirrors().empty(), "the sessions folder was saved as a mirror");
                    expect (dialog.status.getText().startsWith ("Mirror 1"), dialog.status.getText());
                }
                {
                    // A folder beside the sessions is still a copy's home (on the same drive, said so).
                    backupdlg::Content dialog (engine, {});
                    const auto beside = sessions.getChildFile ("Copies");
                    expect (beside.createDirectory().wasOk());
                    dialog.working = { { beside, CaptureFormat::Wav24 } };
                    dialog.rebuildRows();
                    dialog.applyAndClose();
                    expectEquals ((int) engine.getMirrors().size(), 1, dialog.status.getText());
                }
                expect (engine.setMirrors (savedMirrors));
                sessions.deleteRecursively();
            }
        }

        // ── MIX-05: Session Settings applies to its own session only ────

        static void findSettingsControls (juce::ComboBox*& preRoll, juce::ComboBox*& rate, juce::Button*& apply)
        {
            preRoll = nullptr; rate = nullptr; apply = nullptr;
            auto* dialog = juce::Component::getCurrentlyModalComponent();
            if (dialog == nullptr) return;
            walk (*dialog, [&] (juce::Component& c)
            {
                if (auto* box = dynamic_cast<juce::ComboBox*> (&c))
                {
                    if (box->getTitle() == "Pre-roll") preRoll = box;
                    else if (box->indexOfItemId (6) >= 0 && box->getItemText (box->indexOfItemId (6)) == "192 kHz") rate = box;
                }
                if (auto* b = dynamic_cast<juce::Button*> (&c))
                    if (b->getButtonText() == "Apply") apply = b;
            });
        }

        void sessionSettingsAnswersItsOwnSession()
        {
            beginTest ("Session Settings applies nothing to a session opened behind it");
            {
                Host host;
                const auto base = tempDir ("settings");
                const auto a = savedShow (base, "Gig A"), b = savedShow (base, "Gig B");
                expect (host.main.openSessionFolder (a) >= 0, host.status());
                dismissModals();
                host.main.menuItemSelected (255, 3);
                juce::ComboBox* preRoll; juce::ComboBox* rate; juce::Button* apply;
                findSettingsControls (preRoll, rate, apply);
                expect (preRoll != nullptr && rate != nullptr && apply != nullptr, "setup: the dialog's controls");
                expect (host.main.openSessionFolder (b) >= 0, host.status());   // a Finder open behind it
                expect (host.main.engine.getActiveSessionDir() == b, "setup: B is open");
                const int preRollB = host.main.engine.getRecorder().getPreRollSeconds();
                const double rateB = host.main.pendingSampleRate;
                if (preRoll != nullptr && rate != nullptr && apply != nullptr)
                {
                    preRoll->setSelectedId (preRollB == 10 ? 31 : 11, juce::dontSendNotification);
                    rate->setSelectedId (juce::approximatelyEqual (rateB, 96000.0) ? 2 : 4, juce::dontSendNotification);
                    apply->onClick();
                }
                expectEquals (host.main.engine.getRecorder().getPreRollSeconds(), preRollB,
                              "Apply wrote the first session's pre-roll into the second");
                expectEquals (host.main.pendingSampleRate, rateB, "Apply wrote the first session's rate into the second");
                expect (host.status().contains ("changed"), host.status());
                dismissModals();

                // Unchanged, it still applies.
                host.main.menuItemSelected (255, 3);
                findSettingsControls (preRoll, rate, apply);
                if (preRoll != nullptr && apply != nullptr)
                {
                    preRoll->setSelectedId (preRollB == 10 ? 31 : 11, juce::dontSendNotification);
                    apply->onClick();
                    expectEquals (host.main.engine.getRecorder().getPreRollSeconds(), preRollB == 10 ? 30 : 10,
                                  "Apply on the session it was opened for did nothing");
                    host.main.engine.getRecorder().setPreRollSeconds (preRollB);
                }
                dismissModals();
                forget (host.main);
                base.deleteRecursively();
            }
        }

        // ── MIX-07: a cleared name leaves the strip unnamed ─────────────

        void expectUnnamed (AudioEngine& engine, int track, const juce::String& how)
        {
            const auto name = engine.getRecorder().getTrack (track).getNameThreadSafe();
            expectEquals (takenames::freshStem (track, name), takenames::legacyStem (track),
                          how + ": strip " + juce::String (track + 1) + " is named \"" + name + "\"");
            expectEquals (name, juce::String (track + 1), how + ": strip " + juce::String (track + 1));
        }

        static juce::Label* labelShowing (juce::Component& root, const juce::String& text)
        {
            juce::Label* found = nullptr;
            walk (root, [&] (juce::Component& c)
            {
                if (auto* l = dynamic_cast<juce::Label*> (&c))
                    if (found == nullptr && l->getText() == text) found = l;
            });
            return found;
        }

        void clearedNamesKeepTrackNumbers()
        {
            beginTest ("An unnamed stereo strip's right half keeps Track_NN: Add Tracks, MIXER and EDIT renames agree");
            {
                Host host;
                auto& engine = host.main.engine;
                engine.setStripCount (1);
                engine.setTrackName (0, "Vox");
                host.main.addTracks ({ { 1, true, {} }, { 2, false, {} } }, host.main.questionScope());
                expectEquals (engine.getRecorder().getNumTracks(), 5, "setup: the tracks were added");
                for (int t = 1; t < 5; ++t) expectUnnamed (engine, t, "Add Tracks with no name");

                // Named, then cleared by typing nothing on the MIXER strip.
                engine.setTrackName (1, "Keys");
                engine.setTrackName (2, "Keys R");
                host.main.lastTrackCount = -1;
                host.main.rebuildStripsIfLayoutChanged();
                juce::Label* mixerName = nullptr;
                for (auto& strip : host.main.strips)
                    if (strip != nullptr && strip->getStripIndex() == 1) mixerName = labelShowing (*strip, "Keys");
                expect (mixerName != nullptr, "setup: the MIXER name");
                if (mixerName != nullptr) mixerName->setText ({}, juce::sendNotificationSync);
                for (int t : { 1, 2 }) expectUnnamed (engine, t, "A MIXER name cleared");

                // Named, then cleared by typing nothing on the EDIT row.
                engine.setTrackName (1, "Keys");
                engine.setTrackName (2, "Keys R");
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                juce::AudioThumbnailCache cache { 4 };
                {
                    EditPage::TrackRow row (1, true, engine, formats, cache);
                    auto* editName = labelShowing (row, "Keys");
                    expect (editName != nullptr, "setup: the EDIT name");
                    if (editName != nullptr) editName->setText ({}, juce::sendNotificationSync);
                }
                for (int t : { 1, 2 }) expectUnnamed (engine, t, "An EDIT name cleared");
                expectEquals (engine.getRecorder().getTrack (0).getNameThreadSafe(), juce::String ("Vox"), "a named strip lost its name");
            }
        }
    };

    static MixerFix20261116Tests mixerFix20261116Tests;
}
