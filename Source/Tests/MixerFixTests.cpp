// The MIXER, the master, the drive and sync dialogs and the main window's
// prompts: what each must do while a show runs. The window tests run on a
// silent fake device through the window's real paths.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/AddTracksDialog.h"
#include "../UI/BackupMirrorDialog.h"
#include "../UI/ChannelStrip.h"
#include "../UI/DeskMacroDialog.h"
#include "../UI/MasterStrip.h"
#include "../UI/Meterbridge.h"
#include "../UI/PreflightDialog.h"
#include "../UI/TimecodeSyncDialog.h"
#include "../UI/TwinRecorderDialog.h"
#include "../Audio/TimecodeChase.h"
#include "LtcTestSignal.h"
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
                .getChildFile ("zf-mixer-" + tag + "-" + juce::Uuid().toString());
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
        T* findTitled (juce::Component& root, const juce::String& title)
        {
            T* found = nullptr;
            walk (root, [&] (juce::Component& c)
            {
                if (found == nullptr && c.getTitle() == title)
                    found = dynamic_cast<T*> (&c);
            });
            return found;
        }

        template <typename T>
        std::vector<T*> allOf (juce::Component& root)
        {
            std::vector<T*> found;
            walk (root, [&] (juce::Component& c) { if (auto* t = dynamic_cast<T*> (&c)) found.push_back (t); });
            return found;
        }

        int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        float contrastOn (juce::Colour text, juce::Colour fill)
        {
            const auto a = brand::relativeLuminance (text), b = brand::relativeLuminance (fill);
            return (juce::jmax (a, b) + 0.05f) / (juce::jmin (a, b) + 0.05f);
        }

        // One audio block through the engine, with `level` on input 1.
        juce::AudioBuffer<float> runBlock (AudioEngine& engine, int outputs, float level)
        {
            juce::AudioBuffer<float> in (1, kBlock), out (outputs, kBlock);
            for (int i = 0; i < kBlock; ++i) in.setSample (0, i, level);
            out.clear();
            engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), 1,
                                                     out.getArrayOfWritePointers(), outputs, kBlock, {});
            return out;
        }

        // A one-strip engine whose input is heard on the master and nowhere else.
        void monitorOnMaster (AudioEngine& engine)
        {
            engine.setStripCount (1);
            engine.prepareForTests (kRate, kBlock);
            engine.getMasterState().muted.store (false);
            engine.getMasterState().gainDb.store (0.0f);
            auto& t = engine.getRecorder().getTrack (0);
            t.gainDb.store (0.0f); t.pan.store (0.0f);
            t.muted.store (false); t.soloed.store (false);
            t.armed.store (true);  t.monitor.store (true);
            t.isStereo.store (false);
            engine.setTrackInputRouting (0, 0);
            engine.setTrackOutputRouting (0, -1);
        }
    }

    class MixerFixTests final : public juce::UnitTest
    {
    public:
        MixerFixTests() : juce::UnitTest ("Mixer and main window fixes", "zynforge") {}

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
                        && ! main.captureMetadataPending && main.pendingMetadataSaves == 0;
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
        };

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            masterModeKeepsTwoOutputs();
            masterShowsWhereTheMixPlays();
            faderBoxesTakeOnlyNumbers();
            stereoStripsReadBothHalves();
            transportSyncNamesStrips();
            ltcDisconnectDropsLock();
            dialogRedReads();
            newTracksFreesRows();
            meterbridgeNumbersStrips();
            backupCanBeTurnedOff();
            sameDriveIsCalledOut();
            driveDialogHasATimeLimit();

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

            preflightEstimateWaitsForEveryDrive();   // first: it needs the engine's estimate still empty
            nothingModalOverATake();
            modalsCloseWhenATakeStarts();
            launchNoticesWaitForStop();
            pinnedSessionRaisesNoRateAlarm();
            idleClockDropsAnOldEstimate();
            missingDriveIsSaidAndRefused();
            cueRecallKeepsPairsWhole();
            nextCueIsTheNextOnTheTimeline();
            groupedPanKeepsStereoImages();
            colourGoesWhereItWasPicked();
            addTracksWaitsForFinalization();
            condemnedStripsRebuild();
            twinDisconnectAsksTwiceUnderATake();
            preflightCallsOutSameDriveBackup();
        }

    private:
        // ── MasterStrip ─────────────────────────────────────────────────

        void masterModeKeepsTwoOutputs()
        {
            beginTest ("Flipping the master from MONO to ST puts the mix back on two outputs");
            {
                AudioEngine engine;
                monitorOnMaster (engine);
                engine.setDeviceOutputCountForTests (8);
                engine.setMasterStereo (false);
                engine.setMasterOutputs (0, 0);   // a mono master on Out 1
                MasterStrip strip (engine);
                strip.setBounds (0, 0, 90, 420);
                auto* mode  = findTitled<juce::ToggleButton> (strip, "Mono / stereo");
                auto* combo = findTitled<juce::ComboBox> (strip, "Master output");
                expect (mode != nullptr && combo != nullptr);
                if (mode == nullptr || combo == nullptr) return;

                mode->setToggleState (true, juce::sendNotificationSync);
                expect (engine.getMasterStereo());
                expectEquals (engine.getMasterOutputL(), 0);
                expectEquals (engine.getMasterOutputR(), 1, "ST kept a one-output pair");
                expectEquals (combo->getSelectedId(), 1, "the list does not show Out 1-2");
                const auto out = runBlock (engine, 8, 0.5f);
                expectGreaterThan (out.getMagnitude (0, 0, kBlock), 0.2f);
                expectGreaterThan (out.getMagnitude (1, 0, kBlock), 0.2f, "Out 2 stayed silent under a strip that said ST");

                mode->setToggleState (false, juce::sendNotificationSync);
                expectEquals (engine.getMasterOutputL(), 0);
                expectEquals (engine.getMasterOutputR(), 0, "MONO kept a second output");
            }

            beginTest ("A mono master on an even output flips to the pair it belongs to");
            {
                AudioEngine engine;
                monitorOnMaster (engine);
                engine.setDeviceOutputCountForTests (8);
                engine.setMasterStereo (false);
                engine.setMasterOutputs (1, 1);   // Out 2
                MasterStrip strip (engine);
                auto* mode  = findTitled<juce::ToggleButton> (strip, "Mono / stereo");
                auto* combo = findTitled<juce::ComboBox> (strip, "Master output");
                if (mode == nullptr || combo == nullptr) { expect (false); return; }
                mode->setToggleState (true, juce::sendNotificationSync);
                expectEquals (engine.getMasterOutputL(), 0);
                expectEquals (engine.getMasterOutputR(), 1);
                expectEquals (combo->getSelectedId(), 1, "the list went blank");
            }

            beginTest ("Off stays off through a mode flip");
            {
                AudioEngine engine;
                monitorOnMaster (engine);
                engine.setMasterStereo (false);
                engine.setMasterOutputs (-1, -1);
                MasterStrip strip (engine);
                auto* mode = findTitled<juce::ToggleButton> (strip, "Mono / stereo");
                if (mode == nullptr) { expect (false); return; }
                mode->setToggleState (true, juce::sendNotificationSync);
                expectEquals (engine.getMasterOutputL(), -1);
                expectEquals (engine.getMasterOutputR(), -1);
            }
        }

        void masterShowsWhereTheMixPlays()
        {
            beginTest ("The master's output list shows the pair the mix plays on");
            {
                AudioEngine engine;
                monitorOnMaster (engine);
                engine.setDeviceOutputCountForTests (4);
                engine.setMasterStereo (true);
                engine.setMasterOutputs (8, 9);   // saved on the show rig's Out 9-10
                MasterStrip strip (engine);
                auto* combo = findTitled<juce::ComboBox> (strip, "Master output");
                if (combo == nullptr) { expect (false); return; }
                expect (combo->getText().contains ("Out 3-4"), combo->getText());
                expect (! combo->getText().contains ("(off)"), "the list says Off while the mix is audible");
                const auto out = runBlock (engine, 4, 0.5f);
                expectGreaterThan (out.getMagnitude (2, 0, kBlock), 0.2f, "the mix is not where the list says");

                engine.setDeviceOutputCountForTests (5);
                engine.setMasterOutputs (4, 5);
                pumpUntil ([combo] { return combo->getText().contains ("Out 4-5"); }, 1500);
                expect (combo->getText().contains ("Out 4-5"), combo->getText());

                engine.setDeviceOutputCountForTests (8);
                engine.setMasterOutputs (2, 3);
                pumpUntil ([combo] { return combo->getText() == "Out 3-4"; }, 1500);
                expectEquals (combo->getText(), juce::String ("Out 3-4"));
            }
        }

        // ── ChannelStrip ────────────────────────────────────────────────

        void faderBoxesTakeOnlyNumbers()
        {
            beginTest ("Typing a word in a fader's value box never sends it to 0 dB");
            {
                TrackState ts;
                float applied = 99.0f;
                ChannelStrip strip (0, ts, {}, {}, [&applied] (float dB) { applied = dB; });
                strip.setBounds (0, 0, 90, 420);
                auto& fader = strip.gainFader;
                fader.setValue (-60.0, juce::dontSendNotification);
                expectEquals (fader.getValueFromText ("-inf"), -60.0);
                expectEquals (fader.getValueFromText ("off"), -60.0);
                expectEquals (fader.getValueFromText (juce::String::fromUTF8 ("\xe2\x88\x9e")), -60.0);
                expectEquals (fader.getValueFromText ("-6.5 dB"), -6.5);
                expectEquals (fader.getValueFromText (" "), -60.0, "an empty entry moved the fader");
                fader.setValue (-12.0, juce::dontSendNotification);
                expectWithinAbsoluteError (fader.getValueFromText ("loud"), -12.0, 0.001, "a word moved the fader");

                fader.setValue (-60.0, juce::dontSendNotification);
                strip.faderValue.setText ("-inf", juce::sendNotificationSync);
                expectWithinAbsoluteError ((float) fader.getValue(), -60.0f, 0.01f, "-inf went to unity");
                strip.faderValue.setText (" ", juce::sendNotificationSync);
                expectWithinAbsoluteError ((float) fader.getValue(), -60.0f, 0.01f, "a stray Space went to unity");
                expect (applied < -59.0f || applied > 98.0f, "the strip played at " + juce::String (applied) + " dB");
            }

            beginTest ("A fader's value box opens on a double-click, and a click elsewhere cancels it");
            {
                TrackState ts;
                ChannelStrip strip (0, ts);
                expect (! strip.faderValue.isEditableOnSingleClick(), "one click on the readout opens it");
                expect (strip.faderValue.isEditableOnDoubleClick());
                expect (strip.faderValue.doesLossOfFocusDiscardChanges(), "a click elsewhere commits what was typed");

                AudioEngine engine;
                engine.setStripCount (1);
                MasterStrip master (engine);
                master.setBounds (0, 0, 90, 420);
                auto* masterFader = findTitled<juce::Slider> (master, "Master gain");
                if (masterFader == nullptr) { expect (false); return; }
                masterFader->setValue (-60.0, juce::dontSendNotification);
                expectEquals (masterFader->getValueFromText ("-inf"), -60.0, "the master's -inf went to unity");
                expectEquals (masterFader->getValueFromText ("off"), -60.0);
                const auto boxes = allOf<juce::Label> (*masterFader);
                expect (! boxes.empty());
                for (auto* box : boxes)
                {
                    expect (! box->isEditableOnSingleClick(), "one click on the master's readout opens it");
                    expect (box->isEditableOnDoubleClick());
                    expect (box->doesLossOfFocusDiscardChanges());
                }
                masterFader->setEnabled (false);
                masterFader->setEnabled (true);
                for (auto* box : allOf<juce::Label> (*masterFader))
                    expect (! box->isEditableOnSingleClick(), "LOCK and unlock put the single click back");
            }
        }

        void stereoStripsReadBothHalves()
        {
            beginTest ("A stereo strip's peak and clip count include its right half");
            {
                TrackState left, right;
                left.isStereo.store (true);
                ChannelStrip strip (0, left, {}, {}, {}, {}, {}, {}, &right, [] (float) {});
                left.peak.store (0.1f);
                right.peak.store (0.9f);
                right.clipCount.store (3);
                const auto wanted = juce::String (juce::Decibels::gainToDecibels (0.9f, -80.0f), 1) + " dB";
                pumpUntil ([&] { return strip.clipLabel.getText() == "CLIP x3" && strip.dbLabel.getText() == wanted; }, 1500);
                expectEquals (strip.clipLabel.getText(), juce::String ("CLIP x3"), "a clip on R left the strip clean");
                expectEquals (strip.dbLabel.getText(), wanted, "the readout shows the quieter half");
                left.clipCount.store (2);
                pumpUntil ([&] { return strip.clipLabel.getText() == "CLIP x5"; }, 1500);
                expectEquals (strip.clipLabel.getText(), juce::String ("CLIP x5"));
            }
        }

        // ── Transport Sync ──────────────────────────────────────────────

        void transportSyncNamesStrips()
        {
            beginTest ("Transport Sync lists strips by number, name and input, one per stereo pair");
            {
                AudioEngine engine;
                engine.setDeviceInputCountForTests (32);
                engine.setStripCount (3);
                expect (engine.linkStereoPair (0, true));
                engine.setTrackName (0, "Keys");
                engine.setTrackName (2, "LTC");
                engine.setTrackInputRouting (2, 31);
                tcsync_detail::Content content (engine);
                const auto combos = allOf<juce::ComboBox> (content);
                expectEquals ((int) combos.size(), 2);
                if (combos.size() < 2) return;
                auto& ltc = *combos[1];
                expectEquals (ltc.getNumItems(), 3, "both halves of a pair are listed");
                juce::StringArray texts;
                for (int i = 0; i < ltc.getNumItems(); ++i) texts.add (ltc.getItemText (i));
                expect (! texts.joinIntoString ("|").contains ("Input "), texts.joinIntoString ("|"));
                const auto last = ltc.getNumItems() > 0 ? ltc.getItemText (ltc.getNumItems() - 1) : juce::String();
                expect (last.contains ("Strip 3") && last.contains ("LTC") && last.contains ("In 32"), last);
                expectEquals (ltc.getItemId (ltc.getNumItems() - 1), 4, "the item no longer selects strip 3");
                engine.setStripCount (0);
            }
        }

        void ltcDisconnectDropsLock()
        {
            beginTest ("Clearing the LTC source drops LOCK at once, and the decoder relocks on its own");
            {
                TimecodeChase chase;
                const auto signal = ltctest::ltcSignal (kRate, 25.0, false, { 1, 0, 0, 0 }, 12);
                chase.feedLtc (signal.data(), (int) signal.size(), kRate);
                expect (chase.isRunning(), "the test signal never locked");
                chase.clearFreewheel();
                expect (! chase.isRunning(), "LOCK outlived a disconnect");
                const auto more = ltctest::ltcSignal (kRate, 25.0, false, { 1, 0, 1, 0 }, 12);
                chase.feedLtc (more.data(), (int) more.size(), kRate);
                expect (chase.isRunning(), "the decoder needed a reset to lock again");

                AudioEngine engine;
                engine.setStripCount (2);
                engine.setLtcSourceStrip (2);
                engine.setChaseMode (AudioEngine::ChaseMode::Ltc);
                engine.setLtcSourceStrip (0);
                expect (! engine.isChaseLive());
                engine.setChaseMode (AudioEngine::ChaseMode::Off);
                engine.setStripCount (0);
            }
        }

        // ── Dialog colours ──────────────────────────────────────────────

        void dialogRedReads()
        {
            beginTest ("Red text on a dialog uses the red that reads on dialog grey");
            {
                expectGreaterOrEqual (contrastOn (brand::textDanger, brand::bgElevated), 4.5f);
                for (auto state : { twin::LinkState::Lost, twin::LinkState::Failed, twin::LinkState::Refused })
                {
                    twin::Snapshot s;
                    s.state = state;
                    const auto colour = TwinRecorderDialog::Content::stateText (s).second;
                    expect (colour == brand::textDanger, "a twin alarm is in a red that measures "
                                                          + juce::String (contrastOn (colour, brand::bgElevated), 2) + ":1");
                }

                AudioEngine engine;
                deskmacro_detail::Panel panel (engine);
                engine.setControlsLocked (true);
                panel.enableToggle.setToggleState (true, juce::dontSendNotification);
                panel.applyEnable();
                engine.setControlsLocked (false);
                expect (panel.stateL.getText().isNotEmpty());
                expect (panel.stateL.findColour (juce::Label::textColourId) == brand::textDanger,
                        "Desk Macro's problem line is in a red that does not read on grey");
            }

            beginTest ("The mirror list's scrollbar keeps the theme's thumb");
            {
                AudioEngine engine;
                backupdlg::Content dialog (engine, {});
                expect (! dialog.rowView.isColourSpecified (juce::ScrollBar::thumbColourId),
                        "the thumb is drawn in the edge grey, invisible on the dialog");
            }
        }

        // ── New Tracks ──────────────────────────────────────────────────

        void newTracksFreesRows()
        {
            beginTest ("New Tracks frees its rows when it closes");
            {
                AddTracksDialog::launch ([] (const std::vector<AddTracksDialog::Entry>&) {});
                juce::Component::SafePointer<juce::DialogWindow> window (
                    dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()));
                expect (window != nullptr);
                if (window == nullptr) return;
                std::vector<juce::Component::SafePointer<juce::Component>> children;
                if (auto* content = window->getContentComponent())
                    for (auto* child : content->getChildren()) children.emplace_back (child);
                expect (children.size() >= 3, "no rows to watch");
                window->exitModalState (0);
                pumpUntil ([&window] { return window == nullptr; }, 3000);
                expect (window == nullptr, "the dialog stayed up");
                int alive = 0;
                for (auto& child : children) if (child != nullptr) ++alive;
                expectEquals (alive, 0, "rows outlived their dialog");
            }
        }

        // ── Meterbridge ─────────────────────────────────────────────────

        void meterbridgeNumbersStrips()
        {
            beginTest ("The meterbridge numbers each column by its strip, as MIXER, EDIT and PATCH do");
            {
                AudioEngine engine;
                engine.setStripCount (3);
                expect (engine.linkStereoPair (0, true));
                std::unique_ptr<juce::DialogWindow> window (Meterbridge::launch (engine));
                expect (window != nullptr);
                if (window == nullptr) return;
                juce::StringArray titles;
                walk (*window, [&titles] (juce::Component& c)
                {
                    if (dynamic_cast<juce::Label*> (&c) != nullptr && c.getTitle().startsWith ("Strip"))
                        titles.add (c.getTitle());
                });
                expectEquals (titles.joinIntoString (","), juce::String ("Strip 1,Strip 3"));
                window.reset();
                engine.setStripCount (0);
            }
        }

        // ── Backup & Mirror Drives ──────────────────────────────────────

        void backupCanBeTurnedOff()
        {
            beginTest ("The backup can be turned off from Backup & Mirror Drives");
            {
                AudioEngine engine;
                const auto saved = engine.getBackupDirectory();
                const auto drive = tempDir ("backup-off");
                expect (drive.createDirectory().wasOk());
                expect (engine.setBackupDirectory (drive));
                {
                    backupdlg::Content dialog (engine, {
                        {}, [&engine] (const juce::File& f) { return engine.setBackupDirectory (f); } });
                    expect (dialog.backupRow->removeBtn.isVisible(), "the backup row has no way to turn the backup off");
                    if (dialog.backupRow->removeBtn.onClick) dialog.backupRow->removeBtn.onClick();
                    dialog.applyAndClose();
                }
                expect (engine.getBackupDirectory() == juce::File(), "the backup stayed on after it was removed");
                expect (engine.setBackupDirectory (saved));
                drive.deleteRecursively();
            }
        }

        void sameDriveIsCalledOut()
        {
            beginTest ("A backup or mirror on the session's drive is called out as no second copy");
            {
                AudioEngine engine;
                const auto root = tempDir ("same-drive");
                const auto session = root.getChildFile ("Gig");
                const auto backup = root.getChildFile ("Backup");
                const auto mirror = root.getChildFile ("Mirror");
                for (auto d : { session, backup, mirror }) expect (d.createDirectory().wasOk());
                const auto saved = engine.getBackupDirectory();
                engine.setActiveSessionDir (session);
                expect (engine.setBackupDirectory (backup));
                expect (engine.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                {
                    backupdlg::Content dialog (engine, {});
                    pumpUntil ([&dialog] { return dialog.backupRow->pathLabel.getText().containsIgnoreCase ("same drive"); }, 3000);
                    expect (dialog.backupRow->pathLabel.getText().containsIgnoreCase ("same drive"),
                            dialog.backupRow->pathLabel.getText());
                    expect (! dialog.rows.empty() && dialog.rows.front()->pathLabel.getText().containsIgnoreCase ("same drive"),
                            dialog.rows.empty() ? juce::String() : dialog.rows.front()->pathLabel.getText());
                }
                expect (engine.setMirrors ({}));
                expect (engine.setBackupDirectory (saved));
                engine.setActiveSessionDir ({});
                root.deleteRecursively();
            }
        }

        void driveDialogHasATimeLimit()
        {
            beginTest ("Backup & Mirror Drives opens in bounded time over a drive that stopped answering");
            {
                AudioEngine engine;
                const auto mirror = tempDir ("dead-mirror");
                expect (mirror.createDirectory().wasOk());
                expect (engine.setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                auto held = std::make_shared<juce::WaitableEvent> (true);
                auto checkedOffThread = std::make_shared<std::atomic<bool>> (false);
                const auto messageThread = std::this_thread::get_id();
                backupdlg::rootCheckStallForTests = [held, checkedOffThread, messageThread]
                {
                    if (std::this_thread::get_id() != messageThread) checkedOffThread->store (true);
                    held->wait (20000);
                };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                {
                    backupdlg::Content dialog (engine, {});
                    const auto took = juce::Time::getMillisecondCounterHiRes() - started;
                    expectLessThan (took, 6000.0, "the dialog waited on the dead drive");
                    expect (checkedOffThread->load(), "the folders were checked on the message thread");
                    expect (! dialog.rows.empty() && dialog.rows.front()->pathLabel.getText().containsIgnoreCase ("not answering"),
                            dialog.rows.empty() ? juce::String() : dialog.rows.front()->pathLabel.getText());
                }
                backupdlg::rootCheckStallForTests = {};
                held->signal();
                expect (engine.setMirrors ({}));
                mirror.deleteRecursively();
            }
        }

        // ── Main window ─────────────────────────────────────────────────

        static bool itemEnabled (MainComponent& main, int menu, int id)
        {
            const auto built = main.getMenuForIndex (menu, {});
            for (juce::PopupMenu::MenuItemIterator it (built, true); it.next();)
                if (it.getItem().itemID == id) return it.getItem().isEnabled;
            return false;
        }

        void preflightEstimateWaitsForEveryDrive()
        {
            beginTest ("Pre-flight never stats a mirror drive that has not answered");
            {
                Window w;
                w.arm ({ 0 });
                const auto mirror = tempDir ("stalled-mirror");
                w.dirs.add (mirror);
                expect (mirror.createDirectory().wasOk());
                expect (w.engine().setMirrors ({ { mirror, CaptureFormat::Wav24 } }));
                expectEquals (w.engine().getEstimatedMinutesRemaining(), 0, "the engine already had an estimate");
                auto held = std::make_shared<juce::WaitableEvent> (true);
                w.main.preflightStallForTests = [held] { held->wait (20000); };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                w.main.showPreflightChecklist();
                const auto took = juce::Time::getMillisecondCounterHiRes() - started;
                w.main.preflightStallForTests = {};
                held->signal();
                expectLessThan (took, 12000.0, "pre-flight hung on the dead drive");
                expectEquals (w.engine().getEstimatedMinutesRemaining(), 0,
                              "pre-flight statted every drive from the message thread after one did not answer");
                dismissModals();
                expect (w.engine().setMirrors ({}));
            }
        }

        void nothingModalOverATake()
        {
            beginTest ("Sync, surfaces, macros, console connect, stream, auto-save and Help open nothing over a take");
            {
                Window w;
                w.arm ({ 0 });
                expect (w.roll());
                struct Item { int menu, id; };
                for (auto item : { Item { 3, 600 }, Item { 3, 720 }, Item { 3, 964 }, Item { 3, 954 }, Item { 3, 990 },
                                   Item { 0, 951 }, Item { 2, 322 }, Item { 4, 900 }, Item { 4, 901 }, Item { 4, 902 },
                                   Item { 4, 903 } })
                    expect (! itemEnabled (w.main, item.menu, item.id),
                            "menu item " + juce::String (item.id) + " is live over a take");
                for (int id : { 600, 720, 964, 954, 990, 951, 900, 901, 902, 903 })
                {
                    w.main.menuItemSelected (id, 0);
                    expectEquals (modalCount(), 0, "menu item " + juce::String (id) + " opened a modal in front of STOP");
                    dismissModals();
                }
                expect (w.stop());
                for (auto item : { Item { 3, 600 }, Item { 3, 720 }, Item { 3, 964 }, Item { 3, 954 }, Item { 3, 990 },
                                   Item { 0, 951 }, Item { 2, 322 }, Item { 4, 900 }, Item { 4, 903 } })
                    expect (itemEnabled (w.main, item.menu, item.id),
                            "menu item " + juce::String (item.id) + " stayed grey after STOP");
            }
        }

        void modalsCloseWhenATakeStarts()
        {
            beginTest ("Desk Macro and Backup & Mirror close when a take starts behind them");
            {
                Window w;
                w.arm ({ 0 });
                w.main.menuItemSelected (964, 3);
                expectEquals (modalCount(), 1, "Desk Macro did not open between takes");
                expect (w.roll(), "the take never started");
                w.main.timerCallback();
                pumpUntil ([] { return modalCount() == 0; }, 3000);
                expectEquals (modalCount(), 0, "Desk Macro stood over a take that started behind it");
                expect (w.stop());
                dismissModals();

                w.main.onBackupClicked();
                expectEquals (modalCount(), 1, "Backup & Mirror did not open between takes");
                expect (w.roll());
                w.main.timerCallback();
                pumpUntil ([] { return modalCount() == 0; }, 3000);
                expectEquals (modalCount(), 0, "Backup & Mirror stood over a take that started behind it");
                expect (w.stop());
            }
        }

        void launchNoticesWaitForStop()
        {
            beginTest ("A crash notice and the recovery list wait for STOP when a take is rolling");
            {
                Window w;
                w.arm ({ 0 });
                const auto reports = tempDir ("crash");
                w.dirs.add (reports);
                expect (reports.createDirectory().wasOk());
                const auto report = reports.getChildFile ("Zynforge Recording-2026-11-04-120000.ips");
                expect (report.replaceWithText ("{\"timestamp\":\"2026-11-04 12:00:00\"}"));
                const auto orphan = w.main.getSessionsRoot().getChildFile ("zf-mixer-orphan-" + juce::Uuid().toString());
                w.dirs.add (orphan);
                expect (orphan.createDirectory().wasOk());
                expect (orphan.getChildFile ("recording.session").create().wasOk());

                expect (w.roll());
                w.main.showCrashReportNotice ({ report });
                expectEquals (modalCount(), 0, "the crash notice opened over the take");
                const bool shown = w.main.offerSessionRecovery ({});
                expect (! shown, "the recovery list opened over the take");
                expectEquals (modalCount(), 0, "the recovery list opened over the take");
                dismissModals();
                expect (w.stop());
                w.main.timerCallback();
                expect (pumpUntil ([] { return modalCount() > 0; }, 3000), "the held notices never came back after STOP");
                dismissModals();
                w.main.timerCallback();
                pumpUntil ([] { return modalCount() > 0; }, 1000);
                dismissModals();
            }
        }

        void pinnedSessionRaisesNoRateAlarm()
        {
            beginTest ("A session pinned but not yet loaded raises no sample-rate alarm");
            {
                Window w;
                w.engine().clearSessionState();
                w.main.forgetSessionUiState();
                const auto pinned = tempDir ("pinned");
                w.dirs.add (pinned);
                expect (pinned.createDirectory().wasOk());
                w.engine().setActiveSessionDir (pinned);
                w.main.pendingSampleRate = 48000.0;
                w.main.srMismatchWarned = 0.0;
                w.main.checkDeviceSampleRate (96000.0);
                expect (w.main.sampleRateWarning.isEmpty(), "a false alarm before the session loaded: " + w.main.sampleRateWarning);
                expectEquals (modalCount(), 0, "a false mismatch modal before the session loaded");
                dismissModals();
                w.main.pendingSampleRate = 48000.0;
                w.main.srMismatchWarned = 0.0;
                w.engine().setActiveSessionDir ({});
            }
        }

        void idleClockDropsAnOldEstimate()
        {
            beginTest ("After a take the idle clock stops showing pre-flight's record time");
            {
                Window w;
                w.arm ({ 0 });
                w.main.showPreflightChecklist();
                dismissModals();
                expectGreaterThan (w.engine().getEstimatedMinutesRemaining(), 0, "pre-flight made no estimate");
                w.main.updateHeaderFromStatus (w.engine().captureStatus(), false);
                expectGreaterThan (w.main.bigClock.getRemainingSeconds(), 0.0, "pre-flight's figure never reached the clock");

                expect (w.roll());
                w.main.updateHeaderFromStatus (w.engine().captureStatus(), false);
                expect (w.stop());
                w.main.updateHeaderFromStatus (w.engine().captureStatus(), false);
                expectEquals (w.main.bigClock.getRemainingSeconds(), 0.0,
                              "the clock went back to the figure from before the take");
            }
        }

        void missingDriveIsSaidAndRefused()
        {
            beginTest ("With the sessions drive unplugged, RECORD says so and the clock shows no boot-disk figure");
            {
                Window w;
                auto* props = w.engine().getAppProps();
                expect (props != nullptr);
                if (props == nullptr) return;
                const auto savedRoot = props->getValue ("sessionsRoot");
                const auto unplugged = "/Volumes/ZF Not Plugged In " + juce::Uuid().toString() + "/Zynforge Sessions";
                updateSettings (props, [&unplugged] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", unplugged); });
                w.engine().clearSessionState();
                w.main.forgetSessionUiState();
                w.engine().setActiveSessionDir ({});
                w.engine().setStripCount (2);
                w.arm ({ 0 });

                w.main.onRecordClicked();
                const bool rolled = w.engine().isRecording();
                if (rolled)
                {
                    w.dirs.add (w.engine().getActiveSessionDir());
                    w.stop();
                }
                expect (! rolled, "a take went to another drive while the chosen one was unplugged");
                expect (w.main.recordRefusal.containsIgnoreCase ("not connected"), w.main.recordRefusal);

                w.main.lastDiskQueryMs = 0;
                w.main.updateHeaderFromStatus (w.engine().captureStatus(), false);
                expectLessThan (w.main.bigClock.getFreeGB(), 0.0, "the clock showed another drive's free space");
                updateSettings (props, [&savedRoot] (juce::PropertiesFile& p) { p.setValue ("sessionsRoot", savedRoot); });
                w.main.lastDiskQueryMs = 0;
            }
        }

        void cueRecallKeepsPairsWhole()
        {
            beginTest ("Recalling a cue taken before a link keeps the pair's right half on its left");
            {
                Window w (2);
                w.engine().setTrackInputRouting (0, 2);
                w.engine().setTrackInputRouting (1, 6);
                w.engine().setTrackGainDb (1, -20.0f);
                w.track (1).muted.store (true);

                SetlistBar::Cue cue;
                cue.name = "Song 1";
                for (int i = 0; i < 2; ++i)
                {
                    auto& t = w.track (i);
                    SetlistBar::StripSnapshot s;
                    s.stripId = t.stripId;
                    s.gainDb = t.gainDb.load();
                    s.pan = t.pan.load();
                    s.inputRouting = t.inputRouting.load();
                    s.outputRouting = t.outputRouting.load();
                    s.muted = t.muted.load();
                    cue.strips.push_back (s);
                }
                w.main.cues = { cue };

                expect (w.engine().linkStereoPair (0, true));
                expectEquals (w.track (1).inputRouting.load(), 3);
                w.main.jumpToCue (0);
                expectEquals (w.track (1).inputRouting.load(), 3, "R went back to the input it had before the link");
                expect (! w.track (1).muted.load(), "R came back muted under an unmuted strip");
                expectWithinAbsoluteError (w.track (1).rampTargetGainDb.load(), w.track (0).rampTargetGainDb.load(), 0.01f,
                                           "R's fader came back from before the link");
                expectWithinAbsoluteError (w.track (1).pan.load(), 1.0f, 0.01f, "the pair lost its image");
                w.main.cues.clear();
            }

            beginTest ("A cue recall leaves a strip on its own default input");
            {
                Window w (2);
                SetlistBar::Cue cue;
                cue.name = "Song 1";
                SetlistBar::StripSnapshot s;
                s.stripId = w.track (1).stripId;
                s.inputRouting = -2;
                cue.strips.push_back (s);
                w.main.cues = { cue };
                w.main.jumpToCue (0);
                expectEquals (w.engine().getEffectiveInputRouting (1), 1, "the cue unpatched a strip on its default input");
                w.main.cues.clear();
            }
        }

        void nextCueIsTheNextOnTheTimeline()
        {
            beginTest ("The next-cue countdown names the next cue on the timeline, not in the list");
            {
                Window w;
                const auto cue = [] (const char* name, juce::int64 at)
                {
                    SetlistBar::Cue c;
                    c.name = name;
                    c.samplePos = at;
                    return c;
                };
                w.main.cues = { cue ("Song 3", 40000), cue ("Song 1", 0), cue ("Song 2", 20000) };
                w.player().setPositionSamples (5000);
                w.engine().startPlayback();
                expect (w.player().isPlaying());
                w.main.updateNextCueCountdown();
                expect (w.main.nextCueLabel.getText().startsWith ("Next: Song 2"), w.main.nextCueLabel.getText());
                w.engine().stopPlayback();
                w.main.cues.clear();
            }
        }

        void groupedPanKeepsStereoImages()
        {
            beginTest ("A grouped pan move never collapses a stereo pair, and a pair's L knob leaves its R alone");
            {
                Window w (4);
                expect (w.engine().linkStereoPair (1, true));
                for (int t = 0; t < 4; ++t) w.engine().setTrackEditGroup (t, 0);
                w.rebuild();
                expectEquals ((int) w.main.strips.size(), 3);
                if (w.main.strips.size() < 3) return;

                w.main.strips[0]->panSlider.setValue (-0.1, juce::sendNotificationSync);
                expectWithinAbsoluteError (w.track (3).pan.load(), -0.1f, 0.001f, "a mono peer stopped following");
                expectWithinAbsoluteError (w.track (1).pan.load(), -1.0f, 0.001f, "the pair's L jumped to the kick's pan");
                expectWithinAbsoluteError (w.track (2).pan.load(),  1.0f, 0.001f, "the pair's R jumped to the kick's pan");

                w.main.strips[1]->panSlider.setValue (-0.5, juce::sendNotificationSync);
                expectWithinAbsoluteError (w.track (1).pan.load(), -0.5f, 0.001f);
                expectWithinAbsoluteError (w.track (2).pan.load(),  1.0f, 0.001f, "the pair's L knob dragged its R");
            }
        }

        void colourGoesWhereItWasPicked()
        {
            beginTest ("Colouring a strip outside the selection colours that strip only");
            {
                Window w (4);
                expectEquals ((int) w.main.strips.size(), 4);
                if (w.main.strips.size() < 4) return;
                const juce::Colour picked (0xff336699);
                w.main.selectedLogical = { 0, 1 };
                w.main.strips[3]->colourCb (picked);
                expect (w.track (3).colourARGB.load() == picked.getARGB(), "the strip the engineer clicked kept its colour");
                expect (w.track (0).colourARGB.load() == 0, "the selection was recoloured instead");

                w.main.strips[1]->colourCb (picked);
                expect (w.track (0).colourARGB.load() == picked.getARGB(), "a selected strip's colour skipped the selection");
                w.main.selectedLogical.clear();
            }
        }

        void addTracksWaitsForFinalization()
        {
            beginTest ("Add Tracks while a STOP is still finalizing adds nothing and says so");
            {
                Window w;
                w.arm ({ 0 });
                expect (w.roll());
                auto gate = std::make_shared<juce::WaitableEvent> (true);
                w.recorder().beforeFinalizationForTests = [gate] { gate->wait (8000); };
                const juce::ScopeGuard release { [gate] { gate->signal(); } };
                w.main.onStopClicked();
                w.main.onStopClicked();
                pumpUntil ([&w] { return w.engine().isCaptureFinalizing(); }, 3000);
                expect (w.engine().isCaptureFinalizing(), "the STOP never reached finalization");
                const int before = w.recorder().getNumTracks();
                AddTracksDialog::Entry entry;
                entry.count = 8;
                w.main.addTracks ({ entry }, w.main.questionScope());
                expectEquals (w.recorder().getNumTracks(), before);
                expect (! w.main.statusLabel.getText().startsWith ("Added"), w.main.statusLabel.getText());
                gate->signal();
                expect (w.stopped());
                w.recorder().beforeFinalizationForTests = {};
            }
        }

        void condemnedStripsRebuild()
        {
            beginTest ("Strips condemned for a session swap are rebuilt on the next tick, whatever the count");
            {
                Window w (3);
                expect (! w.main.strips.empty());
                if (w.main.strips.empty()) return;
                w.main.condemnAllStrips();
                w.main.rebuildStripsIfLayoutChanged();
                expect (! w.main.strips.empty() && w.main.strips[0]->stripValid,
                        "the MIXER kept strips it had condemned: frozen meters and stale pairs");
            }
        }

        void twinDisconnectAsksTwiceUnderATake()
        {
            beginTest ("Twin Disconnect needs a second press while a take rolls, and one between takes");
            {
                std::atomic<bool> rolling { true };
                auto& link = Window::shared->twinLink;
                TwinRecorderDialog::Content content (link, [&rolling] { return rolling.load(); },
                                                     [] { return (juce::PropertiesFile*) nullptr; }, [] {});
                const auto connect = [&content]
                {
                    content.setRole (twin::Role::Leader);
                    content.setPort (49883);
                    content.setPeerAddress ("127.0.0.1");
                    content.setCode ("7K3M-Q2XD-9HVB-T4WN");
                    content.pressEnable();
                };
                connect();
                expect (link.isEnabled());
                content.pressEnable();
                expect (link.isEnabled(), "one press beside Close dropped the link under a take");
                content.pressEnable();
                expect (! link.isEnabled(), "the second press did not disconnect");

                rolling.store (false);
                connect();
                expect (link.isEnabled());
                content.pressEnable();
                expect (! link.isEnabled(), "between takes Disconnect should take one press");
                link.stop();
            }
        }

        void preflightCallsOutSameDriveBackup()
        {
            beginTest ("Pre-flight calls a backup on the session's drive what it is");
            {
                Window w;
                const auto saved = w.recorder().getBackupDirectory();
                const auto backup = tempDir ("same-drive-backup");
                w.dirs.add (backup);
                expect (backup.createDirectory().wasOk());
                expect (w.engine().setBackupDirectory (backup));
                w.main.showPreflightChecklist();
                const PreflightRow* row = nullptr;
                std::vector<PreflightRow> rows;
                if (auto* window = dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()))
                    if (auto* list = dynamic_cast<PreflightDialog*> (window->getContentComponent()))
                        rows = list->getRows();
                for (const auto& r : rows) if (r.item == "Backup") row = &r;
                expect (row != nullptr, "no backup row");
                if (row != nullptr)
                {
                    expect (row->status == PreflightRow::Status::warning, "a same-drive backup got a tick: " + row->value);
                    expect (row->value.containsIgnoreCase ("same drive"), row->value);
                }
                dismissModals();
                expect (w.engine().setBackupDirectory (saved));
            }
        }
    };

    static MixerFixTests mixerFixTests;
}
