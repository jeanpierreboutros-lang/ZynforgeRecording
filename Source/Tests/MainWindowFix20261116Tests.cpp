// Main window fixes from the 2026-11-16 audit: keyboard focus and LOCK, a
// remote RECORD over a short save, ending a punch, the sample-rate guard
// before the first take, the banner, the strip-move menu and the tooltips.
// They run on a silent fake device through the window's real paths.
//
// JUCE's real focus path (a click handing focus over, a key going to the
// focused component) needs a window on screen, which these tests do not have.
// They check the flags that decide it on the real components, and deliver
// Return straight to the LOCK button as a focused button would get it.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/PreflightDialog.h"
#include "../UI/EditPage.h"
#include "../UI/EditToolsBar.h"
#include "../Audio/SessionBackup.h"
#include "SilentAudioDevice.h"

#include <functional>
#include <thread>
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
                .getChildFile ("zf-main1116-" + tag + "-" + juce::Uuid().toString());
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

        void feedAll (MultitrackRecorder& rec, float value, int blocks, juce::int64 playhead)
        {
            const int tracks = rec.getNumTracks();
            std::vector<float> audio ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) tracks, audio.data());
            for (int b = 0; b < blocks; ++b)
                rec.processBlock (inputs.data(), tracks, kBlock, playhead + (juce::int64) b * kBlock);
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

        // Keeps a metadata save open until released, as a slow drive would.
        struct SaveHeldOpen
        {
            juce::WaitableEvent entered, release;

            SaveHeldOpen()
            {
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::make_shared<const std::function<void()>> ([this] { entered.signal(); release.wait (8000); }));
            }

            ~SaveHeldOpen() { finish(); }

            void finish()
            {
                release.signal();
                std::atomic_store (&sessionbackup::beforeSnapshotForTests, std::shared_ptr<const std::function<void()>>());
            }
        };
    }

    class MainWindowFix20261116Tests final : public juce::UnitTest
    {
    public:
        MainWindowFix20261116Tests() : juce::UnitTest ("Main window fixes 2026-11-16", "zynforge") {}

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

            // A session made but not recorded yet, declared at `rate`.
            Window (double rate, int tracks) : main (*shared)
            {
                session = tempDir ("empty");
                dirs.add (session);
                for (auto* name : { "Audio Files", "Export Files", "Session File Backups" })
                    session.getChildFile (name).createDirectory();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setActiveSessionDir (session);
                engine().claimMixerFor (session);
                engine().setStripCount (tracks);
                main.pendingSampleRate = rate;
                engine().setSessionSampleRate (rate);
                main.lastTrackCount = -1;
                main.engine.prepareForTests (kRate, kBlock);
            }

            ~Window()
            {
                dismissModals();
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.punchSessionActive = false;
                main.manualPunchActive = false;
                main.punchWindowTakeActive = false;
                main.stopArmedAtMs = 0;
                if (engine().isPunchModeOn()) engine().setPunchModeOn (false);
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.selectedLogical.clear();
                main.sampleRateWarning.clear();
                main.srMismatchWarned = 0.0;
                main.pendingSampleRate = kRate;
                engine().setSessionSampleRate (kRate);
                main.recordRefusal.clear();
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

            bool stopped (int timeoutMs = 6000)
            {
                return pumpUntil ([this]
                {
                    return ! engine().isRecording() && ! main.captureMetadataPending
                        && main.pendingMetadataSaves == 0;
                }, timeoutMs);
            }

            bool saveRunning (SaveHeldOpen& hold)
            {
                main.enqueueMetadataSave (session, {}, {}, true, true);
                return hold.entered.wait (5000) && main.pendingMetadataSaves != 0;
            }

            // Moves the playhead the way the transport would, past a punch's seek lock.
            void playheadTo (juce::int64 sample)
            {
                const bool locked = player().isSeekLocked();
                player().setSeekLocked (false);
                player().setPositionSamples (sample);
                player().setSeekLocked (locked);
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
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            Window::shared = &main;

            noClickTakesFocus();
            remoteRecordOverAShortSave();
            punchWindowEndsOnOnePress();
            stopDuringPunchFinalization();
            punchEndSaysWhatItReplaced();
            sampleRateBeforeTheFirstTake();
            bannerDeviceNotice();
            moveSelectionDuringATake();
            tooltipsSayWhatTheyDo();

            Window::shared = nullptr;
        }

    private:
        using Action = AudioEngine::RemoteTransportAction;

        void noClickTakesFocus()
        {
            beginTest ("No click leaves keyboard focus on a header button, the setlist or a strip's routing");
            {
                Window w (2);
                w.main.rebuildStrips();
                auto& m = w.main;
                for (juce::Button* b : { (juce::Button*) &m.deviceButton, (juce::Button*) &m.lockButton,
                                         (juce::Button*) &m.backupButton, (juce::Button*) &m.patchButton,
                                         (juce::Button*) &m.vscButton, (juce::Button*) &m.metersButton,
                                         (juce::Button*) &m.addChannelButton, (juce::Button*) &m.mixViewButton,
                                         (juce::Button*) &m.editViewButton, (juce::Button*) &m.stripXsButton,
                                         (juce::Button*) &m.stripSButton, (juce::Button*) &m.stripMButton,
                                         (juce::Button*) &m.stripLButton, (juce::Button*) &m.gridButton })
                    expect (! b->getMouseClickGrabsKeyboardFocus(), b->getButtonText() + " takes focus on a click");

                std::vector<juce::Button*> setlistButtons;
                collect (m.setlistBar, setlistButtons);
                expect (setlistButtons.size() >= 5, "setup: the setlist has no buttons");
                for (auto* b : setlistButtons)
                    expect (! b->getMouseClickGrabsKeyboardFocus(), "setlist " + b->getTitle() + " takes focus on a click");
                std::vector<juce::ComboBox*> setlistCombos;
                collect (m.setlistBar, setlistCombos);
                for (auto* c : setlistCombos)
                    expect (! c->getMouseClickGrabsKeyboardFocus(), "the cue list takes focus on a click");

                expect (! m.strips.empty(), "setup: no strips");
                for (auto& strip : m.strips)
                {
                    if (strip == nullptr) continue;
                    std::vector<juce::ComboBox*> combos;
                    collect (*strip, combos);
                    expect (combos.size() >= 2, "setup: no routing combos on the strip");
                    for (auto* c : combos)
                        expect (! c->getMouseClickGrabsKeyboardFocus(), c->getTitle() + " takes focus on a click");
                }

                expect (! m.lockButton.getWantsKeyboardFocus(), "LOCK can still hold keyboard focus");
            }

            // Lead, after the merge: the same root as MAIN-01 on the strip's
            // R / I / M / S, the master strip and the EDIT tools. A focused
            // ToggleButton takes Return; a focused tool takes Return and Space,
            // so Space after clicking a tool picked the tool, not the transport.
            beginTest ("No click leaves keyboard focus on a strip's R/I/M/S, the master strip or an EDIT tool");
            {
                Window w (2);
                w.main.rebuildStrips();
                auto& m = w.main;
                for (auto& strip : m.strips)
                {
                    if (strip == nullptr) continue;
                    std::vector<juce::Button*> buttons;
                    collect (*strip, buttons);
                    expect (buttons.size() >= 4, "setup: no R/I/M/S on the strip");
                    for (auto* b : buttons)
                        expect (! b->getMouseClickGrabsKeyboardFocus(), "strip " + b->getButtonText() + " takes focus on a click");
                }
                expect (m.masterStrip != nullptr, "setup: no master strip");
                if (m.masterStrip != nullptr)
                {
                    std::vector<juce::Button*> buttons;
                    collect (*m.masterStrip, buttons);
                    expect (buttons.size() >= 2, "setup: no master buttons");
                    for (auto* b : buttons)
                        expect (! b->getMouseClickGrabsKeyboardFocus(), "master " + b->getButtonText() + " takes focus on a click");
                }
                auto* tools = m.editPage != nullptr ? m.editPage->getEditToolsBar() : nullptr;
                expect (tools != nullptr, "setup: no EDIT tools");
                if (tools != nullptr)
                {
                    std::vector<juce::Component*> all;
                    collect (*tools, all);
                    int focusable = 0;
                    for (auto* c : all)
                        if (c->getWantsKeyboardFocus())
                        {
                            ++focusable;
                            expect (! c->getMouseClickGrabsKeyboardFocus(), "EDIT tool " + c->getTitle() + " takes focus on a click");
                        }
                    expect (focusable >= 4, "setup: no keyboard-reachable EDIT tools");
                }
            }

            beginTest ("Return never turns LOCK off");
            {
                Window w;
                w.main.onLockToggled();
                expect (w.main.sessionLocked.load(), "setup: LOCK did not engage");
                juce::Component& lock = w.main.lockButton;
                lock.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
                lock.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
                pumpUntil ([] { return false; }, 150);
                expect (w.main.sessionLocked.load(), "a Return on the LOCK button unlocked the show");
                w.main.keyPressed (juce::KeyPress (juce::KeyPress::returnKey), &w.main);
                pumpUntil ([] { return false; }, 50);
                expect (w.main.sessionLocked.load(), "a Return in the window unlocked the show");
                w.main.onLockToggled();
                expect (! w.main.sessionLocked.load(), "a click no longer unlocks");
            }
        }

        void remoteRecordOverAShortSave()
        {
            const auto releasesIn = [] (SaveHeldOpen& hold, int ms)
            {
                return std::thread ([&hold, ms] { juce::Thread::sleep (ms); hold.release.signal(); });
            };

            beginTest ("A remote RECORD waits out a short automatic save and starts the take");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold), "setup: no save in flight");
                auto release = releasesIn (hold, 300);
                juce::String error;
                const bool started = w.engine().performRemoteTransport (Action::StartRecord, error);
                release.join();
                expect (started, "refused: " + error);
                expect (w.engine().isRecording(), "no take started behind a save that lasted 0.3 s");
                hold.finish();
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("A twin RECORD waits out a short automatic save and starts the take");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold), "setup: no save in flight");
                auto release = releasesIn (hold, 300);
                juce::String refusal;
                const bool started = w.main.twinStartRecord (refusal);
                release.join();
                expect (started, "refused: " + refusal);
                expect (w.engine().isRecording(), "the follower missed the take over a save that lasted 0.3 s");
                hold.finish();
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("A save that outlasts the wait refuses a remote RECORD, and the refusal names the save");
            {
                Window w;
                w.arm ({ 0 });
                SaveHeldOpen hold;
                expect (w.saveRunning (hold), "setup: no save in flight");
                juce::String error;
                expect (! w.engine().performRemoteTransport (Action::StartRecord, error), "a take started over a stuck save");
                expect (error.containsIgnoreCase ("sav"), error);
                expect (w.main.recordRefusal.containsIgnoreCase ("save")
                            && ! w.main.recordRefusal.containsIgnoreCase ("finaliz"),
                        "the refusal does not name the save: " + w.main.recordRefusal);
                hold.finish();
                expect (w.stopped());
                expect (! w.engine().isRecording(), "the refused press ran after the save");
            }
        }

        void punchWindowEndsOnOnePress()
        {
            const auto rollIntoTheWindow = [this] (Window& w)
            {
                w.arm ({ 0 });
                w.main.togglePunchMode();
                w.player().selectRange (1000, 90000);
                w.player().setPositionSamples (0);
                w.engine().startPlayback();
                w.main.servicePunch();
                w.player().setPositionSamples (5000);
                w.main.servicePunch();
                expect (w.engine().isRecording() && w.main.punchWindowTakeActive,
                        "setup: the punch window did not open: " + w.main.statusLabel.getText());
                feedAll (w.recorder(), -0.3f, 4, 5000);
            };

            beginTest ("A PUNCH-mode take ends on one STOP press");
            {
                Window w;
                rollIntoTheWindow (w);
                w.main.onStopClicked();
                expect (w.stopped (2500), "the punch-window take needed a second STOP");
                w.main.togglePunchMode();
            }

            beginTest ("A PUNCH-mode take ends on one RECORD press");
            {
                Window w;
                rollIntoTheWindow (w);
                w.main.onRecordClicked();
                expect (w.stopped (2500), "the punch-window take needed a second RECORD");
                w.main.togglePunchMode();
            }
        }

        void stopDuringPunchFinalization()
        {
            const auto reachTheOutPoint = [this] (Window& w)
            {
                w.arm ({ 0 });
                const auto in = (juce::int64) 150 * kBlock, out = (juce::int64) 160 * kBlock;
                w.player().selectRange (in, out);
                w.main.onRecordClicked();
                expect (w.main.punchSessionActive && w.engine().isRecording(),
                        "setup: no selection punch: " + w.main.statusLabel.getText());
                feedAll (w.recorder(), -0.3f, 10, in);
                w.playheadTo (out + kBlock);
                w.main.servicePunch();
                expect (w.main.captureMetadataPending, "setup: the punch did not start closing");
            };

            beginTest ("STOP while a selection punch finalizes cancels its post-roll");
            {
                Window w;
                reachTheOutPoint (w);
                w.main.onStopClicked();
                expect (w.stopped());
                pumpUntil ([] { return false; }, 200);
                expect (! w.engine().isPlaying(), "the post-roll started by itself after STOP");
                expect (! w.main.punchSessionActive, "the punch outlived STOP");
            }

            // Lead, after the merge: a surface or desk STOP reached
            // handleRemoteTransport's "finalization pending" answer before the
            // post-roll was cancelled, so it rolled on by itself.
            for (const auto action : { zynforge::AudioEngine::RemoteTransportAction::StopPlay,
                                       zynforge::AudioEngine::RemoteTransportAction::StopAll })
            {
                beginTest (juce::String ("A remote ")
                           + (action == zynforge::AudioEngine::RemoteTransportAction::StopPlay ? "StopPlay" : "StopAll")
                           + " while a selection punch finalizes cancels its post-roll");
                Window w;
                reachTheOutPoint (w);
                juce::String error;
                w.engine().performRemoteTransport (action, error);
                expect (w.stopped());
                pumpUntil ([] { return false; }, 200);
                expect (! w.engine().isPlaying(), "the post-roll started by itself after a remote stop");
                expect (! w.main.punchSessionActive, "the punch outlived a remote stop");
            }

            // Lead, after the merge: a surface or desk STOP reached
            // handleRemoteTransport's "finalization pending" answer before the
            // post-roll was cancelled, so it rolled on by itself.
            for (const auto action : { zynforge::AudioEngine::RemoteTransportAction::StopPlay,
                                       zynforge::AudioEngine::RemoteTransportAction::StopAll })
            {
                beginTest (juce::String ("A remote ")
                           + (action == zynforge::AudioEngine::RemoteTransportAction::StopPlay ? "StopPlay" : "StopAll")
                           + " while a selection punch finalizes cancels its post-roll");
                Window w;
                reachTheOutPoint (w);
                juce::String error;
                w.engine().performRemoteTransport (action, error);
                expect (w.stopped());
                pumpUntil ([] { return false; }, 200);
                expect (! w.engine().isPlaying(), "the post-roll started by itself after a remote stop");
                expect (! w.main.punchSessionActive, "the punch outlived a remote stop");
            }

            beginTest ("Space while a selection punch finalizes cancels its post-roll");
            {
                Window w;
                reachTheOutPoint (w);
                w.main.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey), &w.main);
                expect (w.stopped());
                pumpUntil ([] { return false; }, 200);
                expect (! w.engine().isPlaying(), "the post-roll started by itself after Space");
                expect (! w.main.punchSessionActive, "the punch outlived Space");
            }
        }

        void punchEndSaysWhatItReplaced()
        {
            const auto in = (juce::int64) 150 * kBlock, out = (juce::int64) 400 * kBlock;
            const auto secs = [] (juce::int64 samples) { return juce::String ((double) samples / kRate, 1) + " s"; };

            beginTest ("STOP before the punch-in says nothing was replaced");
            {
                Window w (1, 600);
                w.arm ({ 0 });
                w.player().selectRange (in, out);
                w.main.onRecordClicked();
                expect (w.main.punchSessionActive, "setup: no selection punch");
                w.main.onStopClicked();
                expect (w.stopped());
                expect (w.main.statusLabel.getText().containsIgnoreCase ("nothing was replaced"),
                        w.main.statusLabel.getText());
            }

            beginTest ("STOP inside the punch says what it replaced");
            {
                Window w (1, 600);
                w.arm ({ 0 });
                w.player().selectRange (in, out);
                w.main.onRecordClicked();
                expect (w.main.punchSessionActive, "setup: no selection punch");
                const auto at = (juce::int64) 300 * kBlock;
                feedAll (w.recorder(), -0.3f, 150, in);
                w.playheadTo (at);
                w.main.onRecordClicked();
                expect (w.stopped());
                const auto said = w.main.statusLabel.getText();
                expect (said.containsIgnoreCase ("replaced") && said.contains (secs (in)) && said.contains (secs (at)),
                        said);
            }
        }

        void sampleRateBeforeTheFirstTake()
        {
            beginTest ("Pre-flight checks the session rate RECORD refuses on, before the first take");
            {
                Window w (44100.0, 1);
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (! w.engine().isRecording(), "setup: RECORD took a 48 kHz device into a 44.1 kHz session");
                dismissModals();
                w.main.showPreflightChecklist();
                std::vector<PreflightRow> rows;
                if (auto* window = dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()))
                    if (auto* list = dynamic_cast<PreflightDialog*> (window->getContentComponent()))
                        rows = list->getRows();
                const PreflightRow* rate = nullptr;
                for (const auto& r : rows)
                    if (r.item.containsIgnoreCase ("Session SR") || r.item.containsIgnoreCase ("session rate")) rate = &r;
                expect (rate != nullptr, "pre-flight has no session-rate row before the first take");
                if (rate != nullptr)
                {
                    expect (rate->status == PreflightRow::Status::failed, "a rate RECORD refuses passed: " + rate->value);
                    expect (rate->value.contains ("44100") || rate->value.contains ("44.1"), rate->value);
                }
                dismissModals();
            }

            beginTest ("Refused remote RECORDs at a rate mismatch open one notice, not a stack");
            {
                Window w (44100.0, 1);
                w.arm ({ 0 });
                for (int press = 0; press < 3; ++press)
                {
                    juce::String error;
                    expect (! w.engine().performRemoteTransport (Action::StartRecord, error), "a take started at the wrong rate");
                }
                juce::String refusal;
                expect (! w.main.twinStartRecord (refusal));
                w.main.onRecordClicked();
                expectEquals (modalCount(), 1, "each refused RECORD stacked another rate notice");
                dismissModals();
                w.main.onRecordClicked();
                expectEquals (modalCount(), 1, "a press after the notice closed did not explain itself again");
                dismissModals();
            }
        }

        void bannerDeviceNotice()
        {
            beginTest ("A device name with two spaces stays one alarm in the banner");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                status.deviceNotice = "!! AUDIO DEVICE STOPPED -- the take is held open while Dante  Virtual Soundcard comes back  ";
                status.missedSamples = 480;
                w.main.updateCaptureWarnings (status);
                const auto alarms = AlarmBanner::splitAlarms (w.main.activeCaptureWarning);
                bool whole = false, missed = false;
                for (const auto& alarm : alarms)
                {
                    whole = whole || (alarm.contains ("AUDIO DEVICE STOPPED") && alarm.contains ("Soundcard comes back"));
                    missed = missed || (alarm.contains ("480 SAMPLES MISSED") && ! alarm.contains ("DEVICE"));
                }
                expect (whole, "the device alarm split at the name: " + alarms.joinIntoString (" / "));
                expect (missed, "the next alarm ran into the device alarm: " + alarms.joinIntoString (" / "));
            }
        }

        void moveSelectionDuringATake()
        {
            const auto moveItems = [] (MainComponent& m)
            {
                std::pair<bool, bool> enabled { true, true };
                const auto menu = m.getMenuForIndex (2, "Track");
                for (juce::PopupMenu::MenuItemIterator item (menu, true); item.next();)
                {
                    if (item.getItem().itemID == 330) enabled.first  = item.getItem().isEnabled;
                    if (item.getItem().itemID == 331) enabled.second = item.getItem().isEnabled;
                }
                return enabled;
            };

            beginTest ("Move selected up/down is grey during a take, and a refused move says why");
            {
                Window w (3);
                w.arm ({ 0 });
                w.main.onRecordClicked();
                expect (w.engine().isRecording(), "setup: no take");
                w.main.selectedLogical = { 1 };
                const auto rolling = moveItems (w.main);
                expect (! rolling.first && ! rolling.second, "Move selected is lit during a take");
                w.main.statusLabel.setText ({}, juce::dontSendNotification);
                w.main.moveSelectedStrips (-1);
                expect (w.main.statusLabel.getText().isNotEmpty(), "a refused move said nothing");
                w.main.onStopClicked();
                w.main.onStopClicked();
                expect (w.stopped());
            }

            beginTest ("Move selected up/down is grey during a session job, and lit again after");
            {
                Window w (3);
                w.main.selectedLogical = { 1 };
                w.main.sessionIoBusy.store (true);
                const auto busy = moveItems (w.main);
                expect (! busy.first && ! busy.second, "Move selected is lit during a session job");
                w.main.statusLabel.setText ({}, juce::dontSendNotification);
                w.main.moveSelectedStrips (-1);
                expect (w.main.statusLabel.getText().isNotEmpty(), "a refused move said nothing");
                w.main.sessionIoBusy.store (false);
                const auto idle = moveItems (w.main);
                expect (idle.first && idle.second, "Move selected stayed grey after the job");
            }
        }

        void tooltipsSayWhatTheyDo()
        {
            beginTest ("STOP, RECORD and +CH say what they do");
            {
                Window w;
                auto& m = w.main;
                expect (m.transportBar != nullptr, "setup: no transport");
                if (m.transportBar == nullptr) return;
                juce::String stop, record;
                std::vector<juce::Button*> buttons;
                collect (*m.transportBar, buttons);
                for (auto* b : buttons)
                {
                    if (b->getTitle() == "Stop")   stop = b->getTooltip();
                    if (b->getTitle() == "Record") record = b->getTooltip();
                }
                expect (! stop.containsIgnoreCase ("everything") && stop.containsIgnoreCase ("twice"),
                        "STOP's tooltip: " + stop);
                expect (! record.containsIgnoreCase ("toggle") && record.containsIgnoreCase ("twice"),
                        "RECORD's tooltip: " + record);
                const auto plus = m.addChannelButton.getTooltip();
                expect (plus.containsIgnoreCase ("add") && ! plus.containsIgnoreCase ("set the number"),
                        "+CH's tooltip: " + plus);
            }
        }
    };

    static MainWindowFix20261116Tests mainWindowFix20261116Tests;
}
