// Session fixes from the 2026-11-16 audit: what an import is written at and
// which session it lands in, a session's own Audio Files folder, the recovery
// list at launch, and a document open the launch refused. They run on a
// silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/SessionRecoveryDialog.h"
#include "../Audio/TakeNames.h"
#include "SilentAudioDevice.h"

#include <functional>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    namespace
    {
        constexpr double kRate = 48000.0;
        constexpr int kBlock = 256;

        juce::File tempDir (const juce::String& tag)
        {
            return juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-ses1116-" + tag + "-" + juce::Uuid().toString());
        }

        void recordBaseTake (const juce::File& dir, int tracks, int blocks)
        {
            MultitrackRecorder rec;
            rec.prepare (kRate, kBlock, tracks);
            std::vector<std::vector<float>> audio;
            std::vector<const float*> inputs;
            for (int t = 0; t < tracks; ++t)
            {
                rec.getTrack (t).armed.store (true);
                audio.emplace_back ((size_t) kBlock, 0.1f * (float) (t + 1));
                inputs.push_back (audio.back().data());
            }
            rec.startRecording (dir);
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
            rec.stopRecording();
        }

        bool writeWav (const juce::File& file, double rate, int samples = 4800)
        {
            file.getParentDirectory().createDirectory();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
            if (out == nullptr) return false;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (out.get(), rate, 1, 24, {}, 0));
            if (writer == nullptr) return false;
            out.release();
            juce::AudioBuffer<float> buffer (1, samples);
            for (int i = 0; i < samples; ++i) buffer.setSample (0, i, 0.1f);
            return writer->writeFromAudioSampleBuffer (buffer, 0, samples);
        }

        double rateOf (const juce::File& file)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
            return reader != nullptr ? reader->sampleRate : 0.0;
        }

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        template <typename Type>
        void collect (juce::Component& root, std::vector<Type*>& found)
        {
            for (auto* child : root.getChildren())
            {
                if (auto* hit = dynamic_cast<Type*> (child)) found.push_back (hit);
                collect (*child, found);
            }
        }

        juce::String labelsOf (juce::Component& root)
        {
            std::vector<juce::Label*> labels;
            collect (root, labels);
            juce::StringArray text;
            for (auto* label : labels) text.add (label->getText());
            return text.joinIntoString (" | ");
        }

        bool press (juce::Component& root, const juce::String& buttonText)
        {
            std::vector<juce::TextButton*> buttons;
            collect (root, buttons);
            for (auto* button : buttons)
                if (button->getButtonText() == buttonText && button->onClick != nullptr)
                {
                    button->onClick();
                    return true;
                }
            return false;
        }

        SessionRecoveryDialog* recoveryList()
        {
            if (auto* window = dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()))
                return dynamic_cast<SessionRecoveryDialog*> (window->getContentComponent());
            return nullptr;
        }
    }

    class SessionFix20261116Tests final : public juce::UnitTest
    {
    public:
        SessionFix20261116Tests() : juce::UnitTest ("Session fixes 2026-11-16", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;
            const juce::ScopeGuard restoreTestMode { []
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
            } };
            MainComponent main;
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            host = &main;

            importTakesTheSessionRate();
            importAnswerKeepsItsSession();
            audioFilesFolderIsNotASession();
            recoveryResultStands();
            refusedDocumentLeavesTheLaunch();

            reset();
            host = nullptr;
        }

    private:
        MainComponent* host = nullptr;
        juce::Array<juce::File> dirs;

        MainComponent& main() { return *host; }
        AudioEngine& engine() { return host->engine; }

        // Back to no session between tests, as a fresh launch would be.
        void reset()
        {
            auto& m = main();
            dismissModals();
            pumpUntil ([&m]
            {
                return ! m.engine.isRecording() && ! m.captureMetadataPending
                    && m.pendingMetadataSaves == 0 && ! m.sessionIoBusy.load();
            }, 10000);
            if (m.sessionLocked) m.onLockToggled();
            m.engine.stopPlayback();
            m.engine.clearSessionState();
            m.forgetSessionUiState();
            m.condemnAllStrips();
            m.engine.setStripCount (0);
            m.engine.clearAllStripOverrides();
            m.engine.setActiveSessionDir ({});
            m.lastTrackCount = -1;
            m.pendingSampleRate = kRate;
            m.explicitDocumentOpened = false;
            for (const auto& d : dirs) d.deleteRecursively();
            dirs.clear();
        }

        juce::File newDir (const juce::String& tag)
        {
            const auto dir = tempDir (tag);
            dirs.add (dir);
            dir.createDirectory();
            return dir;
        }

        // A session with a take, its documents saved, then closed again.
        juce::File savedSession (const juce::String& tag, int tracks = 1)
        {
            const auto dir = newDir (tag);
            recordBaseTake (dir, tracks, 50);
            expect (main().openSessionFolder (dir, true) >= 0, "setup: " + main().statusLabel.getText());
            expect (main().saveSessionStateTo (dir), "setup: the session was not saved");
            return dir;
        }

        // What the Import panel does with its answer.
        void answerImport (const juce::Array<juce::File>& picks, const MainComponent::QuestionScope& scope)
        {
            if (main().importAnswerInScope (picks, scope)) main().completeAudioImport (picks);
        }

        void closeSession()
        {
            auto& m = main();
            m.engine.stopPlayback();
            m.engine.clearSessionState();
            m.forgetSessionUiState();
            m.condemnAllStrips();
            m.engine.setStripCount (0);
            m.engine.setActiveSessionDir ({});
            m.lastTrackCount = -1;
        }

        void importTakesTheSessionRate()
        {
            beginTest ("An import before the first take is written at the session's rate, not the device's");
            {
                reset();
                // New Session at 44.1 kHz; the device is held at 48 kHz by its clock.
                const auto session = newDir ("declared");
                for (auto* name : { "Audio Files", "Export Files", "Session File Backups" })
                    session.getChildFile (name).createDirectory();
                engine().setActiveSessionDir (session);
                engine().claimMixerFor (session);
                main().pendingSampleRate = 44100.0;

                const auto source = newDir ("source").getChildFile ("Guitar.wav");
                expect (writeWav (source, kRate));
                main().completeAudioImport ({ source });
                expect (pumpUntil ([this] { return engine().getRecorder().getNumTracks() == 1
                                                && ! main().sessionIoBusy.load(); }, 10000),
                        "setup: the import never finished: " + main().statusLabel.getText());
                const auto takes = takenames::trackFiles (session.getChildFile ("Audio Files"));
                expectEquals (takes.size(), 1, "setup: no imported file");
                if (! takes.isEmpty())
                    expectWithinAbsoluteError (rateOf (takes[0]), 44100.0, 0.5,
                                               "the import was written at the device's rate, not the session's");
            }

            beginTest ("An import with no session at all still follows the device");
            {
                reset();
                main().pendingSampleRate = kRate;   // the timer keeps it on the device's rate with no session
                const auto source = newDir ("source").getChildFile ("Bass.wav");
                expect (writeWav (source, 44100.0));
                main().completeAudioImport ({ source });
                expect (pumpUntil ([this] { return engine().getRecorder().getNumTracks() == 1
                                                && ! main().sessionIoBusy.load(); }, 10000),
                        "setup: the import never finished: " + main().statusLabel.getText());
                const auto made = engine().getActiveSessionDir();
                if (made.isDirectory()) dirs.add (made);
                const auto takes = takenames::trackFiles (made.getChildFile ("Audio Files"));
                expectEquals (takes.size(), 1, "setup: no imported file");
                if (! takes.isEmpty())
                    expectWithinAbsoluteError (rateOf (takes[0]), kRate, 0.5);
            }
        }

        void importAnswerKeepsItsSession()
        {
            beginTest ("An Import panel answered after the session changed writes nothing");
            {
                reset();
                const auto first = savedSession ("first");
                const auto second = savedSession ("second");
                closeSession();
                expect (main().openSessionFolder (first, true) >= 0);
                const auto asked = main().questionScope();
                expect (main().openSessionFolder (second, true) >= 0);
                const int before = engine().getRecorder().getNumTracks();

                const auto source = newDir ("source").getChildFile ("Keys.wav");
                expect (writeWav (source, kRate));
                answerImport ({ source }, asked);
                pumpUntil ([this] { return ! main().sessionIoBusy.load(); }, 10000);
                pumpUntil ([] { return false; }, 200);
                expectEquals (engine().getRecorder().getNumTracks(), before,
                              "the answer imported into the session that opened after the panel");
                expectEquals (takenames::trackFiles (second.getChildFile ("Audio Files")).size(), 1,
                              "a file was written into the other session");
                expect (main().statusLabel.getText().containsIgnoreCase ("changed"), main().statusLabel.getText());
            }

            beginTest ("An Import panel answered for the session it was opened over imports");
            {
                reset();
                const auto show = savedSession ("same");
                const auto asked = main().questionScope();
                const auto source = newDir ("source").getChildFile ("Vox.wav");
                expect (writeWav (source, kRate));
                answerImport ({ source }, asked);
                expect (pumpUntil ([this] { return engine().getRecorder().getNumTracks() == 2
                                                && ! main().sessionIoBusy.load(); }, 10000),
                        "the import did not run: " + main().statusLabel.getText());
                expectEquals (takenames::trackFiles (show.getChildFile ("Audio Files")).size(), 2);
            }
        }

        void audioFilesFolderIsNotASession()
        {
            beginTest ("A session's own Audio Files folder is refused, and the session is named");
            {
                reset();
                const auto show = savedSession ("show", 2);
                closeSession();
                const auto audio = show.getChildFile ("Audio Files");
                const int opened = main().openSessionFolder (audio, true);
                expectEquals (opened, -1, "the Audio Files folder opened as a session");
                expect (! engine().mixerBelongsTo (audio), "the Audio Files folder was claimed as a session");
                expect (audio.findChildFiles (juce::File::findFiles, false, "*.zfproj").isEmpty()
                            && ! audio.getChildFile ("session_mix.json").existsAsFile(),
                        "session files were written into the Audio Files folder");
                const auto said = main().statusLabel.getText();
                expect (said.contains (show.getFileName()) && said.containsIgnoreCase ("open"), said);

                // Every other way in goes through the same funnel.
                main().explicitDocumentOpened = false;
                expect (! main().openSessionFromWelcome (audio), "the Welcome opened the Audio Files folder");
                expect (! engine().mixerBelongsTo (audio));
            }

            beginTest ("A loose folder that happens to be called Audio Files still opens");
            {
                reset();
                const auto loose = newDir ("loose").getChildFile ("Audio Files");
                recordBaseTake (loose, 1, 50);
                expect (main().openSessionFolder (loose, true) >= 0, main().statusLabel.getText());
            }
        }

        void recoveryResultStands()
        {
            auto* props = engine().getAppProps();
            expect (props != nullptr, "setup: no settings");
            if (props == nullptr) return;
            const auto savedRoot = props->getValue ("sessionsRoot");
            const juce::ScopeGuard restoreRoot { [props, savedRoot]
            {
                updateSettings (props, [&savedRoot] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", savedRoot); });
            } };

            beginTest ("A failed Recover at launch keeps the list open with its reason, and nothing reopens behind it");
            {
                reset();
                const auto pinned = savedSession ("pinned");
                closeSession();
                engine().setActiveSessionDir (pinned);   // pinned from the last run, not loaded yet
                expect (! engine().mixerBelongsTo (pinned), "setup: the pinned session is loaded");

                const auto root = newDir ("root");
                updateSettings (props, [&root] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", root.getFullPathName()); });
                const auto crashed = root.getChildFile ("Crashed");
                expect (crashed.createDirectory().wasOk());
                expect (crashed.getChildFile ("recording.session").create().wasOk());   // nothing else: no session to open

                bool proceeded = false;
                expect (main().offerSessionRecovery ([this, &proceeded] { proceeded = true; main().showStartupWelcome(); }),
                        "setup: no recovery list");
                juce::Component::SafePointer<SessionRecoveryDialog> list (recoveryList());
                expect (list != nullptr, "setup: the recovery list is not the modal");
                if (list != nullptr) expect (press (*list, "Recover selected"), "setup: no Recover button");
                pumpUntil ([] { return false; }, 100);
                expect (list != nullptr && modalCount() == 1, "a failed Recover closed the list");
                if (list != nullptr)
                    expect (labelsOf (*list).contains ("not a session folder"),
                            "the list does not say why: " + labelsOf (*list));
                expect (! proceeded, "the launch went on while the list stood");

                dismissModals();   // Skip
                expect (pumpUntil ([&proceeded] { return proceeded; }, 3000), "Skip never let the launch go on");
                expect (! engine().mixerBelongsTo (pinned), "the pinned session reopened behind the failed Recover");
                expect (! main().statusLabel.getText().contains ("Loaded"), main().statusLabel.getText());
                dismissModals();   // the Welcome
            }

            beginTest ("A Recover that works stays on the status line, with no Welcome over it");
            {
                reset();
                const auto pinned = savedSession ("pinned");
                closeSession();
                engine().setActiveSessionDir (pinned);

                const auto root = newDir ("root");
                updateSettings (props, [&root] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", root.getFullPathName()); });
                const auto crashed = root.getChildFile ("Crashed");
                recordBaseTake (crashed, 1, 50);
                expect (crashed.getChildFile ("recording.session").create().wasOk());

                bool proceeded = false;
                expect (main().offerSessionRecovery ([this, &proceeded] { proceeded = true; main().showStartupWelcome(); }),
                        "setup: no recovery list");
                if (auto* list = recoveryList()) expect (press (*list, "Recover selected"));
                expect (pumpUntil ([&proceeded] { return proceeded; }, 3000), "the launch never went on");
                expect (engine().mixerBelongsTo (crashed), "the recovered session is not the one open");
                expect (! engine().mixerBelongsTo (pinned));
                expect (main().statusLabel.getText().contains ("Recovered"), main().statusLabel.getText());
                expect (! crashed.getChildFile ("recording.session").exists(), "the session stays on the recovery list");
                expectEquals (modalCount(), 0, "the Welcome opened over the recovered session");
                dismissModals();
            }
        }

        void refusedDocumentLeavesTheLaunch()
        {
            beginTest ("A document the launch refused leaves the launch to the Welcome or the last session");
            {
                reset();
                const auto show = savedSession ("show");
                closeSession();
                const auto snapshot = show.getChildFile ("Session File Backups").getChildFile ("2026-11-16_10-00-00");
                expect (snapshot.createDirectory().wasOk());
                const auto document = snapshot.getChildFile (show.getFileName() + ".zfproj");
                expect (document.replaceWithText ("{}"));

                main().explicitDocumentOpened = false;
                expect (! main().openSessionDocument (document), "setup: a backup's document opened");
                expect (! main().explicitDocumentOpened, "a refused open still tells the launch a document opened");

                expect (main().openSessionDocument (show.getChildFile (show.getFileName() + ".zfproj")),
                        main().statusLabel.getText());
                expect (main().explicitDocumentOpened, "an open that worked did not stop the launch's own reopen");
            }
        }
    };

    static SessionFix20261116Tests sessionFix20261116Tests;
}
