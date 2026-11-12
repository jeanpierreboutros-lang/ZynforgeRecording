// The main window, the MIXER and PATCH after the 2026-11-11 review: prompts
// that stood in front of a take started behind them, the toast, the clock and
// dashboard after STOP, batch dialogs on stereo pairs, PATCH rows that told
// nothing, and the shortcut sheet against the key handler. The window tests run
// on a silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/ChannelStrip.h"
#include "../UI/FloatingLaunch.h"
#include "../UI/MixerSnapshot.h"
#include "../UI/PatchPage.h"
#include "../UI/PreflightDialog.h"
#include "../UI/ShortcutSheet.h"
#include "../UI/StripColourPicker.h"
#include "../Audio/VirtualSoundcheck.h"
#include "SilentAudioDevice.h"

#include <functional>
#include <map>
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
                .getChildFile ("zf-ui-" + tag + "-" + juce::Uuid().toString());
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

        void feedAll (MultitrackRecorder& rec, float value, int blocks)
        {
            const int tracks = rec.getNumTracks();
            std::vector<float> audio ((size_t) kBlock, value);
            std::vector<const float*> inputs ((size_t) tracks, audio.data());
            for (int b = 0; b < blocks; ++b) rec.processBlock (inputs.data(), tracks, kBlock);
        }

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

        template <typename T>
        std::vector<T*> allOf (juce::Component& root)
        {
            std::vector<T*> found;
            walk (root, [&] (juce::Component& c) { if (auto* t = dynamic_cast<T*> (&c)) found.push_back (t); });
            return found;
        }

        int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        juce::Component* topModal() { return juce::Component::getCurrentlyModalComponent(); }

        bool marked (juce::Component* modal)
        {
            return modal != nullptr && (bool) modal->getProperties()[kClosesForTake];
        }

        // The modal count drops at once; a modal's answer runs a message later,
        // so a test that checks what the answer did pumps on a little.
        void closed()
        {
            pumpUntil ([] { return modalCount() == 0; }, 3000);
            pumpUntil ([] { return false; }, 300);
        }

        void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        juce::MouseEvent leftAt (juce::Component& c, juce::Point<int> p)
        {
            const auto now = juce::Time::getCurrentTime();
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                     juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &c, &c, now, p.toFloat(), now, 1, false };
        }
    }

    class UiFixTests final : public juce::UnitTest
    {
    public:
        UiFixTests() : juce::UnitTest ("Main window, MIXER and PATCH fixes (2026-11-11)", "zynforge") {}

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
                main.lastTrackCount = -1;
                main.rebuildStripsIfLayoutChanged();
            }

            ~Window()
            {
                dismissModals();
                if (main.sessionLocked) main.onLockToggled();
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.stopArmedAtMs = 0;
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.sampleRateWarning.clear();
                main.updateCaptureWarnings (EngineStatus());
                main.lastTrackCount = -1;
                main.selectedLogical.clear();
                main.switchView (MainComponent::View::Mix);
                for (const auto& d : dirs) d.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            SessionPlayer& player() { return main.engine.getPlayer(); }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }
            TrackState& track (int i) { return recorder().getTrack (i); }

            void arm (std::initializer_list<int> tracks)
            {
                for (int t = 0; t < recorder().getNumTracks(); ++t) engine().setTrackArmed (t, false);
                for (int t : tracks) engine().setTrackArmed (t, true);
            }

            bool stopped()
            {
                return pumpUntil ([this]
                {
                    return ! engine().isRecording() && ! engine().isCaptureFinalizing()
                        && ! main.captureMetadataPending && main.pendingMetadataSaves == 0
                        && ! main.sessionIoBusy.load();
                });
            }

            bool roll()
            {
                main.onRecordClicked();
                return engine().isRecording();
            }

            bool stop()
            {
                main.onStopClicked();
                main.onStopClicked();
                return stopped();
            }

            void rebuild()
            {
                main.lastTrackCount = -1;
                main.rebuildStripsIfLayoutChanged();
            }

            juce::String status() const { return main.statusLabel.getText(); }
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            toastsWrapAndMerge();
            stereoStripReleasesBothSpectra();
            undoKeepsEditGroups();
            patchRowsSayWhatTheyHold();

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

            promptsCloseForATake();
            batchDialogsCloseForATake();
            colourPickersWaitForStop();
            addChannelComesBackAfterUnlock();
            stopDuringPunchFinalizationStaysStopped();
            doubleSpacedNameIsOneAlarm();
            dropsBelongToTheirTake();
            batchDialogsTakeLogicalStrips();
            patchSaysWhenItIsLocked();
            offDeviceOutputHasARow();
            sharedOutputsAreCalledOut();
            shortcutSheetMatchesTheKeys();
        }

    private:
        // ── MIX-07 ──────────────────────────────────────────────────────

        void toastsWrapAndMerge()
        {
            beginTest ("A counter that keeps changing is one toast, not a queue of them");
            {
                Toast toast;
                toast.setBounds (0, 0, 320, 56);
                for (int k = 1; k <= 15; ++k)
                    toast.show (counted (k, "strip") + " selected");
                expect (toast.waitingCount() <= 1, "queued " + juce::String (toast.waitingCount()) + " selection counts");
                expect (toast.getCurrentMessage() == "15 strips selected" || toast.waitingCount() == 1,
                        "the newest count is lost: " + toast.getCurrentMessage());
            }

            beginTest ("The toast queue is bounded and drops neutral news before an error");
            {
                Toast toast;
                toast.setBounds (0, 0, 320, 56);
                const juce::StringArray words { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
                                                "golf", "hotel", "india", "juliet", "kilo", "lima" };
                for (int i = 0; i < 6; ++i) toast.show ("Exported " + words[i]);
                toast.show ("Export INCOMPLETE -- check the drive", Toast::Kind::Error);
                for (int i = 6; i < 12; ++i) toast.show ("Exported " + words[i]);
                expect (toast.waitingCount() <= 4, "queued " + juce::String (toast.waitingCount()) + " toasts");
                expect (toast.isWaiting ("Export INCOMPLETE -- check the drive")
                            || toast.getCurrentMessage().startsWith ("Export INCOMPLETE"),
                        "neutral news pushed an error out of the queue");
            }

            beginTest ("A long notice wraps instead of losing its second half");
            {
                Toast toast;
                toast.setBounds (0, 0, 320, 56);
                const juce::String notice ("Recovered 2 interrupted punch files; partial audio is in Session File Backups");
                toast.show (notice, Toast::Kind::Warning);
                expectEquals (toast.getCurrentMessage(), notice);
                expect (toast.showsWholeMessage(), "the notice is cut off");
                const juce::String longer ("3 take files from a crash could not be repaired; they are kept in Session "
                                           "File Backups and the session opened without them");
                toast.showNow (longer, Toast::Kind::Error);
                expect (toast.showsWholeMessage(), "the longer notice is cut off");
                expectEquals (toast.getBottom(), 56, "the toast left its corner");
            }
        }

        // ── MIX-04 ──────────────────────────────────────────────────────

        void stereoStripReleasesBothSpectra()
        {
            beginTest ("A stereo strip lets both halves publish their next spectrum");
            {
                TrackState left, right;
                left.isStereo.store (true);
                ChannelStrip strip (0, left, {}, {}, {}, {}, {}, {}, &right, {});
                left.fftBlockReady.store (true);
                right.fftBlockReady.store (true);
                pumpUntil ([&] { return ! left.fftBlockReady.load(); }, 2000);
                pumpUntil ([&] { return ! right.fftBlockReady.load(); }, 500);
                expect (! left.fftBlockReady.load(), "the left half was never released");
                expect (! right.fftBlockReady.load(), "the right half keeps its first spectrum for good");
                strip.invalidate();
            }
        }

        // ── MIX-08 ──────────────────────────────────────────────────────

        void undoKeepsEditGroups()
        {
            beginTest ("Mixer undo puts an edit group back");
            {
                AudioEngine eng;
                eng.setStripCount (3);
                eng.setTrackEditGroup (0, 2);
                const auto before = mixersnapshot::snapshotAll (eng);
                const auto print = mixersnapshot::fingerprint (eng);
                eng.setTrackEditGroup (0, -1);
                eng.setTrackEditGroup (1, 4);
                expect (mixersnapshot::fingerprint (eng) != print, "an edit-group change went unseen");
                if (auto* all = before.getArray())
                    for (int i = 0; i < all->size(); ++i) mixersnapshot::restoreStrip (eng, i, (*all)[i]);
                expectEquals (eng.getTrackEditGroup (0), 2, "undo left the strip out of its group");
                expectEquals (eng.getTrackEditGroup (1), -1, "undo left the other strip in a group");
                eng.clearAllStripOverrides();
            }
        }

        // ── MIX-10 / MIX-16, the rows on their own ─────────────────────

        void patchRowsSayWhatTheyHold()
        {
            beginTest ("OUTPUT PATCH marks a row two strips play into");
            {
                AudioEngine eng;
                eng.setStripCount (3);
                eng.setDeviceOutputCountForTests (4);
                eng.setTrackOutputRouting (0, 1);
                eng.setTrackOutputRouting (1, 1);
                eng.setTrackOutputRouting (2, 2);
                const auto rows = PatchPage::rowsFor (eng, false);
                expect (rows.size() >= 4);
                if (rows.size() >= 4)
                {
                    expect (rows[1].note.isNotEmpty(), "Out 2 carries two strips and says nothing");
                    expect (rows[1].note.contains ("2"), rows[1].note);
                    expect (rows[2].note.isEmpty(), "a row one strip holds is marked: " + rows[2].note);
                }
                eng.clearAllStripOverrides();
            }

            beginTest ("OUTPUT PATCH has a row for an output this device does not have");
            {
                AudioEngine eng;
                eng.setStripCount (2);
                eng.setDeviceOutputCountForTests (4);
                eng.setTrackOutputRouting (1, 32);
                const auto rows = PatchPage::rowsFor (eng, false);
                const PatchPage::RowInfo* off = nullptr;
                for (const auto& r : rows) if (r.channel < 0) off = &r;
                expect (off != nullptr, "a strip on Out 33 shows no dot anywhere");
                if (off != nullptr) expect (off->held);
                eng.setTrackOutputRouting (1, 3);
                bool stillOff = false;
                for (const auto& r : PatchPage::rowsFor (eng, false)) stillOff = stillOff || r.channel < 0;
                expect (! stillOff, "the off-device row stays when nothing is off the device");
                eng.clearAllStripOverrides();
            }
        }

        // ── MAIN-03 / MIX-02 ────────────────────────────────────────────

        void promptsCloseForATake()
        {
            beginTest ("Virtual Soundcheck's question closes, as Cancel, when a take starts behind it");
            {
                Window w;
                w.arm ({ 0 });
                w.main.onVscClicked();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "the question never opened");
                expect (marked (topModal()), "the repatch question is not marked");
                expect (w.roll(), "the take never started");
                w.main.timerCallback();
                closed();
                expectEquals (modalCount(), 0, "the repatch question stood over the take");
                expect (! vsc::hasSavedPatch (w.engine()), "a dropped question repatched");
                expect (w.stop());
            }

            beginTest ("Restore Outputs' question closes as Cancel");
            {
                Window w;
                vsc::repatch (w.engine());
                w.main.onVscClicked();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "the question never opened");
                expect (marked (topModal()), "the restore question is not marked");
                closeModalsForTake();
                closed();
                expectEquals (modalCount(), 0);
                expect (vsc::hasSavedPatch (w.engine()), "a dropped question restored the outputs");
                vsc::restore (w.engine());
            }

            beginTest ("About closes for a take");
            {
                Window w;
                w.main.showAboutDialog();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "About never opened");
                expect (marked (topModal()), "About is not marked");
                dismissModals();
            }

            beginTest ("The save-on-quit question closes as Cancel and quits nothing");
            {
                Window w;
                w.main.confirmAndQuit();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "the save question never opened");
                expect (marked (topModal()), "the save question is not marked");
                closeModalsForTake();
                closed();
                expectEquals (modalCount(), 0, "closing the save question opened another");
                expect (w.main.isEnabled(), "a dropped save question started a save");
                if (modalCount() > 0) topModal()->exitModalState (2);   // Cancel, never 0: see below
            }

            beginTest ("Quit with no session closes as Cancel and quits nothing");
            {
                Window w;
                w.engine().clearSessionState();
                w.main.forgetSessionUiState();
                w.main.confirmAndQuit();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "the quit question never opened");
                expect (marked (topModal()), "the quit question is not marked");
                closeModalsForTake();
                closed();
                expectEquals (modalCount(), 0, "closing the quit question opened another");
                // The old box quit on any answer but Cancel's own code.
                if (modalCount() > 0) topModal()->exitModalState (2);
                pumpUntil ([] { return modalCount() == 0; }, 2000);
            }

            beginTest ("RECORD's own questions close as Cancel");
            {
                Window w;
                w.arm ({ 0 });
                w.engine().setEditCursorSample (20000);
                w.main.onRecordClicked();
                expect (w.main.cursorPunchQuestion != nullptr, "the punch question never opened");
                expect (marked (topModal()), "the punch question is not marked");
                closeModalsForTake();
                closed();
                expect (! w.engine().isRecording(), "a dropped punch question recorded");
                w.engine().setEditCursorSample (-1);

                MainComponent::RecordStartPlan plan;
                plan.device.needed = true;
                plan.device.text = "This is not the device the session was recorded on.";
                w.main.askBeforeDeviceRecord (plan);
                expect (w.main.deviceRecordQuestion != nullptr, "the device question never opened");
                expect (marked (topModal()), "the device question is not marked");
                closeModalsForTake();
                closed();
                expect (! w.engine().isRecording(), "a dropped device question recorded");
            }
        }

        void batchDialogsCloseForATake()
        {
            beginTest ("Batch Rename closes when a take starts behind it, and renames nothing");
            {
                Window w;
                w.arm ({ 0 });
                const auto before = w.track (0).getNameThreadSafe();
                w.main.showBatchRenameDialog();
                expectEquals (modalCount(), 1, "Batch Rename did not open between takes");
                auto editors = allOf<juce::TextEditor> (*topModal());
                if (! editors.empty()) editors.front()->setText ("Typed but not applied");
                expect (w.roll(), "the take never started");
                w.main.timerCallback();
                closed();
                expectEquals (modalCount(), 0, "Batch Rename stood over a take that started behind it");
                expectEquals (w.track (0).getNameThreadSafe(), before, "a dropped Batch Rename renamed");
                expect (w.stop());
            }

            beginTest ("Batch Colour closes when a take starts behind it, and keeps what was picked");
            {
                Window w;
                w.arm ({ 0 });
                w.main.showBatchColourDialog();
                expectEquals (modalCount(), 1, "Batch Colour did not open between takes");
                w.engine().setTrackColour (1, juce::Colour (0xff3366aa));   // as a live pick does
                expect (w.roll(), "the take never started");
                w.main.timerCallback();
                closed();
                expectEquals (modalCount(), 0, "Batch Colour stood over a take that started behind it");
                expectEquals ((juce::int64) (juce::uint32) w.track (1).colourARGB.load(), (juce::int64) 0xff3366aa,
                              "closing for the take took back the colour on screen");
                expect (w.stop());
            }
        }

        void colourPickersWaitForStop()
        {
            beginTest ("Colour selected... closes when a take starts behind it");
            {
                Window w;
                w.arm ({ 0 });
                w.main.selectedLogical.insert (0);
                w.main.colourSelectedStrips();
                expectEquals (modalCount(), 1, "the picker did not open between takes");
                expect (marked (topModal()), "the selection's picker is not marked");
                dismissModals();
            }

            beginTest ("A strip's colour picker waits for STOP, and one open before the take closes");
            {
                Window w;
                w.arm ({ 0 });
                w.rebuild();
                expect (! w.main.strips.empty());
                if (w.main.strips.empty()) return;
                w.main.strips[0]->openColourPicker();
                expectEquals (modalCount(), 1, "the picker did not open between takes");
                expect (marked (topModal()), "the strip's picker is not marked");
                dismissModals();

                expect (w.roll(), "the take never started");
                w.main.strips[0]->openColourPicker();
                expectEquals (modalCount(), 0, "a colour picker opened in front of STOP");
                expect (w.status().containsIgnoreCase ("stop"), w.status());
                dismissModals();
                expect (w.stop());
            }
        }

        // ── MAIN-05 ─────────────────────────────────────────────────────

        void addChannelComesBackAfterUnlock()
        {
            beginTest ("+ CH comes back after STOP when the session was unlocked during the take");
            {
                Window w;
                w.arm ({ 0 });
                expect (w.roll());
                w.main.onLockToggled();
                w.main.onLockToggled();
                expect (! w.main.sessionLocked.load());
                expect (w.stop());
                w.main.timerCallback();
                expect (w.main.addChannelButton.isEnabled(), "+ CH stayed grey after STOP");
            }
        }

        // ── MAIN-06 ─────────────────────────────────────────────────────

        void stopDuringPunchFinalizationStaysStopped()
        {
            beginTest ("STOP while a rolling punch finalizes keeps playback from starting again");
            {
                Window w;
                w.arm ({ 0 });
                w.player().setPositionSamples (20000);
                w.player().start();
                w.main.onRecordClicked();
                expect (w.main.manualPunchActive, w.status());
                feedAll (w.recorder(), -0.3f, 4);
                w.main.onRecordClicked();   // punch out; playback is to carry on
                expect (w.main.captureMetadataPending, "the punch-out did not wait for finalization");
                w.main.onStopClicked();     // the engineer means stop
                expect (w.stopped());
                pumpUntil ([] { return false; }, 200);
                expect (! w.player().isPlaying(), "playback started again after STOP");
                w.engine().stopPlayback();
            }
        }

        // ── MAIN-08 ─────────────────────────────────────────────────────

        void doubleSpacedNameIsOneAlarm()
        {
            beginTest ("A channel named with two spaces is one alarm in the banner");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                channelalarm::Alarm alarm;
                alarm.kind = channelalarm::kChannel;
                alarm.name = "KICK  IN";
                alarm.strip = 0;
                status.channelAlarms.push_back (alarm);
                status.copyProblems.add ("Mirror  B lost 2 s");
                w.main.updateCaptureWarnings (status);
                const auto parts = AlarmBanner::splitAlarms (w.main.activeCaptureWarning);
                expectEquals (parts.size(), 2, w.main.activeCaptureWarning);
                expect (parts.size() > 0 && parts[0].contains ("KICK IN"), parts.joinIntoString (" | "));
                w.main.updateCaptureWarnings (EngineStatus());
            }
        }

        // ── MIX-05 ──────────────────────────────────────────────────────

        void dropsBelongToTheirTake()
        {
            beginTest ("Missed samples cover loudness only while rolling, and stay with their session");
            {
                Window w;
                EngineStatus status;
                status.recording = true;
                status.missedSamples = 1024;
                status.integratedLufs = -23.0f;
                status.momentaryLufs = -20.0f;
                status.truePeakDb = -3.0f;
                w.main.updateHeaderFromStatus (status, false);
                expect (w.main.perfDashboard.showsDrop(), "a rolling take's drop is not shown");
                expectEquals (w.main.bigClock.getMissedSamples(), (juce::int64) 1024);

                status.recording = false;
                w.main.updateHeaderFromStatus (status, true);
                expect (! w.main.perfDashboard.showsDrop(), "after STOP the drop still hides LUFS and true peak");
                expectEquals (w.main.bigClock.getMissedSamples(), (juce::int64) 1024,
                              "the take's drop vanished from its own session");

                const auto other = tempDir ("other");
                w.dirs.add (other);
                recordBaseTake (other, 2, 50);
                w.main.openSessionFolder (other, true);
                w.main.updateHeaderFromStatus (status, false);
                expectEquals (w.main.bigClock.getMissedSamples(), (juce::int64) 0,
                              "another session shows the last take's missed samples");
                w.main.updateHeaderFromStatus (EngineStatus(), false);
            }
        }

        // ── MIX-09 / MIX-15 ─────────────────────────────────────────────

        void batchDialogsTakeLogicalStrips()
        {
            beginTest ("Batch Rename lists a stereo pair once, names both halves, and Return applies");
            {
                Window w (3);
                expect (w.main.setStereoLink (0, true));
                w.rebuild();
                w.main.showBatchRenameDialog();
                expectEquals (modalCount(), 1);
                auto editors = modalCount() > 0 ? allOf<juce::TextEditor> (*topModal()) : std::vector<juce::TextEditor*>();
                expectEquals ((int) editors.size(), 2, "a stereo pair takes two rows");
                if (! editors.empty())
                {
                    editors.front()->setText ("Keys");
                    editors.front()->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
                }
                closed();
                expectEquals (modalCount(), 0, "Return in a name field did not apply");
                // The modal count drops at once; its answer runs a message later.
                pumpUntil ([&w] { return w.track (0).getNameThreadSafe() == "Keys"; }, 2000);
                expectEquals (w.track (0).getNameThreadSafe(), juce::String ("Keys"), w.status());
                expectEquals (w.track (1).getNameThreadSafe(), juce::String ("Keys R"),
                              "the pair's right half kept its old name");
            }

            beginTest ("Batch Colour lists a stereo pair once and colours both halves");
            {
                Window w (3);
                expect (w.main.setStereoLink (0, true));
                w.rebuild();
                w.engine().setTrackColour (0, juce::Colour (0xffaa3322));
                w.engine().setTrackColour (1, juce::Colour (0xff2233aa));
                w.main.showBatchColourDialog();
                expectEquals (modalCount(), 1);
                std::vector<juce::TextButton*> swatches;
                if (modalCount() > 0)
                    for (auto* b : allOf<juce::TextButton> (*topModal()))
                        if (b->getButtonText().isEmpty()) swatches.push_back (b);
                expectEquals ((int) swatches.size(), 2, "a stereo pair takes two rows");
                if (! swatches.empty())
                {
                    swatches.front()->triggerClick();
                    pumpUntil ([] { return modalCount() == 2; }, 3000);
                    StripColourPicker* picker = nullptr;
                    if (modalCount() == 2)
                        if (auto found = allOf<StripColourPicker> (*topModal()); ! found.empty()) picker = found.front();
                    expect (picker != nullptr, "the swatch opened no picker");
                    if (picker != nullptr)
                        for (auto* b : allOf<juce::TextButton> (*picker))
                            if (b->getButtonText() == "OK") b->triggerClick();
                    pumpUntil ([] { return modalCount() == 1; }, 3000);
                }
                expectEquals ((juce::int64) (juce::uint32) w.track (1).colourARGB.load(), (juce::int64) 0xffaa3322,
                              "the pair's right half kept another colour");
                dismissModals();
            }
        }

        // ── MIX-11 ──────────────────────────────────────────────────────

        static juce::Component* patchMatrix (juce::DialogWindow* window, int tab)
        {
            if (window == nullptr) return nullptr;
            auto tabs = allOf<juce::TabbedComponent> (*window);
            if (tabs.empty()) return nullptr;
            tabs.front()->setCurrentTabIndex (tab);
            return tabs.front()->getTabContentComponent (tab);
        }

        static juce::Point<int> dotAt (juce::Component& matrix, int columns, int col, int row)
        {
            constexpr int rowHeaderW = 90, colHeaderH = 92, rowH = 56, scrollW = 12;
            const int colW = juce::jmax (60, (matrix.getWidth() - rowHeaderW - scrollW) / juce::jmax (1, columns));
            return { rowHeaderW + col * colW + colW / 2, colHeaderH + row * rowH + rowH / 2 };
        }

        void patchSaysWhenItIsLocked()
        {
            beginTest ("A PATCH click during a take says the patch is locked");
            {
                Window w (2);
                w.arm ({ 0 });
                w.main.openPatchWindow();
                auto* matrix = patchMatrix (w.main.patchDialog.getComponent(), 0);
                expect (matrix != nullptr, "no input matrix");
                expect (w.roll(), "the take never started");
                if (matrix != nullptr)
                {
                    const int before = w.track (0).inputRouting.load();
                    w.main.statusLabel.setText ({}, juce::dontSendNotification);
                    matrix->mouseDown (leftAt (*matrix, dotAt (*matrix, 2, 0, 3)));
                    expectEquals (w.track (0).inputRouting.load(), before);
                    expect (w.status().containsIgnoreCase ("locked"), "the click said nothing: " + w.status());
                }
                expect (w.stop());
                if (auto* window = w.main.patchDialog.getComponent()) delete window;
                w.main.patchDialog = nullptr;
            }

            beginTest ("A strip's routing change during a take says the patch is locked");
            {
                Window w (2);
                w.arm ({ 0 });
                w.rebuild();
                expect (w.roll(), "the take never started");
                if (! w.main.strips.empty())
                {
                    w.main.statusLabel.setText ({}, juce::dontSendNotification);
                    w.main.strips[0]->outputCombo.setSelectedId (3, juce::sendNotificationSync);
                    expect (w.status().containsIgnoreCase ("locked"), "the change said nothing: " + w.status());
                }
                expect (w.stop());
            }
        }

        // ── MIX-10 ──────────────────────────────────────────────────────

        void offDeviceOutputHasARow()
        {
            beginTest ("A strip patched past the device's outputs has a dot that sends it back");
            {
                Window w (3);
                const int outputs = w.engine().getCurrentDeviceOutputCount();
                w.engine().setTrackOutputRouting (2, outputs + 30);
                w.main.openPatchWindow();
                auto* matrix = patchMatrix (w.main.patchDialog.getComponent(), 1);
                expect (matrix != nullptr, "no output matrix");
                if (matrix != nullptr)
                {
                    matrix->setSize (1180, 660);
                    matrix->mouseDown (leftAt (*matrix, dotAt (*matrix, 3, 2, outputs)));
                    expectEquals (w.track (2).outputRouting.load(), -1, "no dot sent the strip back to the master");
                }
                if (auto* window = w.main.patchDialog.getComponent()) delete window;
                w.main.patchDialog = nullptr;
            }
        }

        // ── MIX-16 ──────────────────────────────────────────────────────

        void sharedOutputsAreCalledOut()
        {
            beginTest ("Virtual Soundcheck's question and pre-flight say when two strips share a desk channel");
            {
                Window w (3);
                w.engine().setTrackInputRouting (1, 0);   // strips 1 and 2 both on In 1
                w.main.onVscClicked();
                expect (pumpUntil ([] { return modalCount() == 1; }, 3000), "the question never opened");
                juce::String question;
                if (auto* box = dynamic_cast<juce::AlertWindow*> (topModal()))
                    for (auto* label : allOf<juce::Label> (*box)) question << label->getText();
                expect (question.contains ("Out 1"), "the question names no shared output: " + question);
                dismissModals();

                vsc::repatch (w.engine());
                w.main.showPreflightChecklist();
                std::vector<PreflightRow> rows;
                if (auto* window = dynamic_cast<juce::DialogWindow*> (topModal()))
                    if (auto* list = dynamic_cast<PreflightDialog*> (window->getContentComponent()))
                        rows = list->getRows();
                juce::String patchRow;
                for (const auto& r : rows) if (r.item == "Output patch") patchRow = r.value;
                expect (patchRow.contains ("Out 1"), "pre-flight names no shared output: " + patchRow);
                dismissModals();
                vsc::restore (w.engine());
            }
        }

        // ── MAIN-10 ─────────────────────────────────────────────────────

        static juce::String describe (const juce::KeyPress& k)
        {
            return k.getTextDescription() + " [" + juce::String (k.getKeyCode()) + "]";
        }

        // A press is on the sheet when a row lists it, or lists it with fewer
        // modifiers: the handler ignores a modifier it doesn't look at.
        static bool onSheet (const juce::KeyPress& press, shortcuts::View view)
        {
            const int mods = press.getModifiers().withoutMouseButtons().getRawFlags();
            for (const auto& section : shortcuts::sheet())
                for (const auto& row : section.rows)
                {
                    if (row.view != shortcuts::View::Any && row.view != view) continue;
                    for (const auto& listed : row.presses)
                    {
                        const int listedMods = listed.getModifiers().withoutMouseButtons().getRawFlags();
                        if (listed.getKeyCode() == press.getKeyCode() && (listedMods & ~mods) == 0)
                            return true;
                    }
                }
            return false;
        }

        static std::vector<juce::KeyPress> candidates()
        {
            using M = juce::ModifierKeys;
            std::vector<juce::KeyPress> out;
            const int letterMods[] { 0, M::shiftModifier, M::commandModifier, M::commandModifier | M::shiftModifier,
                                     M::altModifier, M::altModifier | M::shiftModifier, M::ctrlModifier,
                                     M::ctrlModifier | M::commandModifier };
            for (int mods : letterMods)
            {
                for (char c = 'a'; c <= 'z'; ++c)
                {
                    juce::juce_wchar text = 0;
                    if ((mods & M::altModifier) != 0) text = 0x2022;   // Option types another character
                    else if ((mods & M::ctrlModifier) != 0 && (mods & M::commandModifier) == 0)
                        text = (juce::juce_wchar) (c & 0x1f);          // a control character
                    out.push_back (shortcuts::key (c, mods, text));
                }
                for (juce::juce_wchar c : juce::String ("0123456789,.=+[]-"))
                {
                    const bool option = (mods & M::altModifier) != 0 && ! juce::CharacterFunctions::isDigit (c);
                    out.push_back (shortcuts::key ((int) c, mods, option ? (juce::juce_wchar) 0x2022 : (juce::juce_wchar) 0));
                }
            }
            const int specialMods[] { 0, M::shiftModifier, M::altModifier, M::commandModifier };
            for (int mods : specialMods)
                for (int code : { (int) juce::KeyPress::spaceKey, juce::KeyPress::escapeKey, juce::KeyPress::deleteKey,
                                  juce::KeyPress::backspaceKey, juce::KeyPress::returnKey, juce::KeyPress::leftKey,
                                  juce::KeyPress::rightKey, juce::KeyPress::upKey, juce::KeyPress::downKey,
                                  juce::KeyPress::numberPadAdd, juce::KeyPress::numberPadSubtract,
                                  juce::KeyPress::homeKey, juce::KeyPress::endKey })
                    out.push_back (shortcuts::key (code, mods, code == juce::KeyPress::spaceKey ? (juce::juce_wchar) ' '
                                                                                                : (juce::juce_wchar) 0));
            return out;
        }

        // Back to a quiet window between presses. A view switch saves the
        // layout, and the window answers every key with "wait" while it does,
        // so the switch goes before the wait.
        void settle (Window& w, MainComponent::View view)
        {
            dismissModals();
            w.engine().stopPlayback();
            if (w.main.currentView != view) w.main.switchView (view);
            pumpUntil ([&w] { return ! w.main.sessionIoBusy.load() && w.main.pendingMetadataSaves == 0; }, 4000);
        }

        void shortcutSheetMatchesTheKeys()
        {
            beginTest ("The shortcut sheet lists every key the window answers, and each listed key works");
            {
                Window w (2);
                using View = MainComponent::View;
                const std::pair<View, shortcuts::View> views[] { { View::Mix, shortcuts::View::Mix },
                                                                 { View::Edit, shortcuts::View::Edit } };
                for (const auto& [view, sheetView] : views)
                {
                    settle (w, view);
                    juce::StringArray missing;
                    for (const auto& press : candidates())
                    {
                        // Tab is answered on purpose so focus never walks onto a
                        // routing control; it does nothing, so it isn't a shortcut.
                        if (press.getKeyCode() == juce::KeyPress::tabKey) continue;
                        const bool answered = w.main.keyPressed (press, nullptr);
                        settle (w, view);
                        if (answered && ! onSheet (press, sheetView)) missing.addIfNotAlreadyThere (describe (press));
                    }
                    expect (missing.isEmpty(), juce::String (view == View::Mix ? "MIXER" : "EDIT")
                                               + " answers keys the sheet leaves out: " + missing.joinIntoString (", "));

                    juce::StringArray dead;
                    for (const auto& section : shortcuts::sheet())
                        for (const auto& row : section.rows)
                        {
                            if (row.needs.isNotEmpty()) continue;
                            if (row.view != shortcuts::View::Any && row.view != sheetView) continue;
                            for (const auto& press : row.presses)
                            {
                                const bool answered = w.main.keyPressed (press, nullptr);
                                settle (w, view);
                                if (! answered) dead.add (row.keys + " (" + describe (press) + ")");
                            }
                        }
                    expect (dead.isEmpty(), juce::String (view == View::Mix ? "MIXER" : "EDIT")
                                            + " ignores keys the sheet lists: " + dead.joinIntoString (", "));
                }
            }
        }
    };

    static UiFixTests uiFixTests;
}
