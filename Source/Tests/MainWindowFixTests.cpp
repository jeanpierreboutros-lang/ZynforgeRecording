// The main window's alarms and transport presses: what the banner must never
// lose, and how a press from somewhere else stays what it is. These run on a
// silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../Audio/SessionBackup.h"
#include "SilentTestDevice.h"

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
                .getChildFile ("zf-mainwin-" + tag + "-" + juce::Uuid().toString());
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

        bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 6000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        // Keeps a metadata save open until released, as a slow drive would.
        struct SaveHeldOpen
        {
            juce::WaitableEvent entered, release;

            SaveHeldOpen()
            {
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::make_shared<const std::function<void()>> ([this] { entered.signal(); release.wait (5000); }));
            }

            ~SaveHeldOpen() { finish(); }

            void finish()
            {
                release.signal();
                std::atomic_store (&sessionbackup::beforeSnapshotForTests, std::shared_ptr<const std::function<void()>>());
            }
        };
    }

    class MainWindowFixTests final : public juce::UnitTest
    {
    public:
        MainWindowFixTests() : juce::UnitTest ("Main window fixes", "zynforge") {}

        struct Window
        {
            static inline MainComponent* shared = nullptr;

            MainComponent& main;
            juce::File session;
            juce::Array<juce::File> dirs;

            explicit Window (int tracks = 2, int blocks = 200) : main (*shared)
            {
                session = tempDir ("session");
                dirs.add (session);
                recordBaseTake (session, tracks, blocks);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Window()
            {
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.punchSessionActive = false;
                main.manualPunchActive = false;
                main.punchWindowTakeActive = false;
                main.stopArmedAtMs = 0;
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.updateCaptureWarnings (EngineStatus());
                main.lastTrackCount = -1;
                for (const auto& d : dirs) d.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            SessionPlayer& player() { return main.engine.getPlayer(); }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }

            void arm (std::initializer_list<int> tracks)
            {
                for (int t = 0; t < recorder().getNumTracks(); ++t) engine().setTrackArmed (t, false);
                for (int t : tracks) engine().setTrackArmed (t, true);
            }

            bool stopped()
            {
                return pumpUntil ([this]
                {
                    return ! engine().isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                });
            }

            // A save runs until `hold` lets it go.
            bool saveRunning (SaveHeldOpen& hold)
            {
                main.enqueueMetadataSave (session, {}, {}, true, true);
                return hold.entered.wait (5000) && main.pendingMetadataSaves != 0;
            }

            void saveFinishes (SaveHeldOpen& hold)
            {
                hold.finish();
                pumpUntil ([this] { return main.pendingMetadataSaves == 0; });
            }
        };

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
            useSilentDevice (main.engine);
            main.setSize (1320, 820);
            Window::shared = &main;

            heldPresses();
        }

    private:
        using Action = AudioEngine::RemoteTransportAction;

        void heldPresses()
        {
            beginTest ("A remote RECORD during a save never comes back as a local press");
            {
                const auto refused = [this] (std::function<void (Window&)> setUp)
                {
                    Window w;
                    w.arm ({ 0 });
                    setUp (w);
                    SaveHeldOpen hold;
                    expect (w.saveRunning (hold));
                    juce::String error;
                    expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                    expect (error.containsIgnoreCase ("saving"), error);
                    expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a remote press was held");
                    w.saveFinishes (hold);
                    expect (! w.engine().isRecording(), "the remote press ran after the save");
                    expect (! w.main.punchSessionActive, "the remote press rolled a punch");
                    expect (w.main.cursorPunchQuestion == nullptr, "the remote press asked this Mac a question");
                    w.engine().stopPlayback();
                };
                refused ([] (Window& w)
                {
                    w.player().setPositionSamples (20000);
                    w.engine().startPlayback();
                });
                refused ([] (Window& w) { w.player().selectRange (1000, 2000); });
                refused ([] (Window& w) { w.engine().setEditCursorSample (20000); });
            }

            beginTest ("A twin RECORD during a save is refused, and no take follows");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                juce::String refusal;
                expect (! w.main.twinStartRecord (refusal));
                expect (refusal.isNotEmpty());
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a link press was held");
                w.saveFinishes (hold);
                expect (! w.engine().isRecording(), "the follower took a take the leader was told it refused");
            }

            beginTest ("A remote press leaves a press made here alone");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                w.main.onRecordClicked();
                expect (w.main.recordPressHeldAtMs != 0);
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error));
                expect (w.main.recordPressHeldAtMs != 0, "the local press was lost");
                w.saveFinishes (hold);
                expect (w.engine().isRecording(), "the local press did not run");
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("Ending a punch during a save never leaves a press behind");
            {
                const auto ends = [this] (std::function<void (Window&)> press)
                {
                    Window w;
                    w.arm ({ 0 });
                    w.player().setPositionSamples (20000);
                    w.engine().startPlayback();
                    w.main.punchSessionActive = true;
                    w.engine().setPunchModeOn (true);
                    SaveHeldOpen hold;
                    expect (w.saveRunning (hold));
                    press (w);
                    expectEquals ((int) w.main.recordPressHeldAtMs, 0, "the press that ends a punch was held");
                    expect (! w.main.punchSessionActive, "the press did not end the punch");
                    expect (! w.engine().isPlaying(), "the post-roll kept rolling");
                    w.saveFinishes (hold);
                    expect (! w.engine().isRecording(), "the press started a take");
                };
                ends ([] (Window& w) { w.main.onStopClicked(); });
                ends ([] (Window& w) { w.main.onRecordClicked(); });
                ends ([] (Window& w) { juce::String e; w.engine().performRemoteTransport (Action::StopAll, e); });
            }

            beginTest ("A held press goes when STOP, a session change or the transport says so");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold));
                w.main.onRecordClicked();
                w.main.onStopClicked();
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "STOP left the press held");
                w.main.onRecordClicked();
                w.main.forgetSessionUiState();
                expectEquals ((int) w.main.recordPressHeldAtMs, 0, "a press outlived its session");
                w.main.onRecordClicked();
                w.main.recordPressHeldRolling = true;   // it was pressed while rolling, and that has stopped
                w.saveFinishes (hold);
                expect (! w.engine().isRecording(), "a press meant as a punch ran as a new part");
                expect (w.main.statusLabel.getText().contains ("press RECORD again"), w.main.statusLabel.getText());
            }
        }

        static void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0; }, 2000);
        }
    };

    static MainWindowFixTests mainWindowFixTests;
}
