// Fixes from the 2026-11-24 audit in the mixer's area: a cue recalled while
// the last take is still being saved, a meterbridge wider than the screen, the
// MIXER's pan readout after a pan moved from somewhere else, removing a row in
// New Tracks, and picking a backup folder on a drive that does not answer.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../UI/AddTracksDialog.h"
#include "../UI/BackupMirrorDialog.h"
#include "../UI/ChannelStrip.h"
#include "../UI/Meterbridge.h"
#include "../UI/StripAlarm.h"
#include "SilentAudioDevice.h"

#include <atomic>
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
                .getChildFile ("zf-mix1124-" + tag + "-" + juce::Uuid().toString());
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

        int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        void dismissModals()
        {
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        std::vector<juce::TextButton*> buttonsSaying (juce::Component& root, const juce::String& text)
        {
            std::vector<juce::TextButton*> found;
            walk (root, [&] (juce::Component& c)
            {
                if (auto* b = dynamic_cast<juce::TextButton*> (&c))
                    if (b->getButtonText() == text && b->isVisible()) found.push_back (b);
            });
            return found;
        }

        int differingPixels (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area)
        {
            area = area.getIntersection (a.getBounds()).getIntersection (b.getBounds());
            int n = 0;
            for (int y = area.getY(); y < area.getBottom(); ++y)
                for (int x = area.getX(); x < area.getRight(); ++x)
                    if (a.getPixelAt (x, y) != b.getPixelAt (x, y)) ++n;
            return n;
        }
    }

    class Fix20261124MixerTests final : public juce::UnitTest
    {
    public:
        Fix20261124MixerTests() : juce::UnitTest ("Fixes 2026-11-24 mixer", "zynforge") {}

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

            panReadoutFollowsEveryPath();          // MIX-03
            meterbridgeReachesEveryStrip();        // MIX-02
            removingARowRunsNothingAfterIt();      // MIX-04
            backupPickHasATimeLimit();             // MIX-05

            MainComponent main;
            testaudio::useSilentDevice (main.engine);
            main.setSize (1320, 820);
            Window::shared = &main;
            cueDuringFinalizationHoldsItsInputs(); // MIX-01
            Window::shared = nullptr;
        }

    private:
        // ── A MainComponent on a silent device, with a session open ────────

        struct Window
        {
            static inline MainComponent* shared = nullptr;

            MainComponent& main;
            juce::File session;

            explicit Window (int tracks = 2, int blocks = 200) : main (*shared)
            {
                session = tempDir ("session");
                recordBaseTake (session, tracks, blocks);
                main.openSessionFolder (session, true);
                main.engine.prepareForTests (kRate, kBlock);
                rebuild();
            }

            ~Window()
            {
                dismissModals();
                engine().getRecorder().beforeFinalizationForTests = {};
                if (engine().isRecording()) main.stopActiveCaptureAsync (false);
                stopped();
                main.stopArmedAtMs = 0;
                main.cues.clear();
                main.heldCueInputs.clear();
                engine().stopPlayback();
                engine().clearSessionState();
                main.forgetSessionUiState();
                main.condemnAllStrips();
                engine().setStripCount (0);
                engine().clearAllStripOverrides();
                main.lastTrackCount = -1;
                main.selectedLogical.clear();
                session.deleteRecursively();
            }

            AudioEngine& engine() { return main.engine; }
            MultitrackRecorder& recorder() { return main.engine.getRecorder(); }
            TrackState& track (int i) { return recorder().getTrack (i); }

            void rebuild()
            {
                main.lastTrackCount = -1;
                main.rebuildStripsIfLayoutChanged();
            }

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

            // STOP, with the take's save held open until `gate` is signalled.
            bool stopIntoFinalization (const std::shared_ptr<juce::WaitableEvent>& gate)
            {
                recorder().beforeFinalizationForTests = [gate] { gate->wait (8000); };
                main.onStopClicked();
                main.onStopClicked();
                return pumpUntil ([this] { return engine().isCaptureFinalizing(); }, 3000);
            }

            SetlistBar::StripSnapshot snapshot (int t)
            {
                auto& s = track (t);
                SetlistBar::StripSnapshot snap;
                snap.stripId = s.stripId;
                snap.gainDb = s.gainDb.load();
                snap.pan = s.pan.load();
                snap.inputRouting = s.inputRouting.load();
                snap.outputRouting = s.outputRouting.load();
                snap.muted = s.muted.load();
                snap.soloed = s.soloed.load();
                snap.monitor = s.monitor.load();
                snap.armed = s.armed.load();
                return snap;
            }
        };

        // ── MIX-01 ──────────────────────────────────────────────────────

        void cueDuringFinalizationHoldsItsInputs()
        {
            beginTest ("A cue recalled while the last take is saved holds its inputs, says so, and patches them before the next RECORD");
            {
                Window w (2);
                w.engine().setTrackInputRouting (0, 0);
                w.engine().setTrackInputRouting (1, 1);

                SetlistBar::Cue cue;
                cue.name = "Song 2";
                for (int t = 0; t < 2; ++t)
                {
                    auto s = w.snapshot (t);
                    s.inputRouting = 4 + t;   // the next song's patch
                    s.armed = true;
                    cue.strips.push_back (s);
                }
                w.main.cues = { cue };

                w.arm ({ 0 });
                expect (w.roll(), "setup: the take did not start");
                auto gate = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard release { [gate] { gate->signal(); } };
                expect (w.stopIntoFinalization (gate), "setup: the STOP never reached finalization");

                w.main.jumpToCue (0);
                const auto said = w.main.statusLabel.getText();
                expect (said.contains ("inputs held until the take is saved"),
                        "the recall said nothing about the inputs it could not move: " + said);
                expect (w.track (1).armed.load(), "the cue's arm did not follow");
                expectEquals (w.track (0).inputRouting.load(), 0, "an input moved under the take's save");

                gate->signal();
                expect (pumpUntil ([&w] { return ! w.engine().isCaptureFinalizing(); }));
                expectEquals (w.track (0).inputRouting.load(), 4,
                              "the take's save ended and strip 1 still records the previous cue's input");
                expectEquals (w.track (1).inputRouting.load(), 5,
                              "the take's save ended and strip 2 still records the previous cue's input");
                expect (w.stopped());
                expect (w.main.heldCueInputs.empty(), "held inputs outlived the save they waited for");
                expectEquals (w.track (0).inputRouting.load(), 4);
            }

            beginTest ("A stereo pair recalled during the save lands on its two inputs when the save ends");
            {
                Window w (3);
                expect (w.engine().linkStereoPair (1, true));
                w.rebuild();
                w.engine().setTrackInputRouting (0, 0);

                SetlistBar::Cue cue;
                cue.name = "Song 3";
                auto mono = w.snapshot (0);
                mono.inputRouting = 3;
                auto pair = w.snapshot (1);
                pair.inputRouting = 6;
                cue.strips = { mono, pair, w.snapshot (2) };
                w.main.cues = { cue };

                w.arm ({ 0 });
                expect (w.roll(), "setup: the take did not start");
                auto gate = std::make_shared<juce::WaitableEvent> (true);
                const juce::ScopeGuard release { [gate] { gate->signal(); } };
                expect (w.stopIntoFinalization (gate), "setup: the STOP never reached finalization");

                w.main.jumpToCue (0);
                gate->signal();
                expect (w.stopped());
                expectEquals (w.track (0).inputRouting.load(), 3);
                expectEquals (w.track (1).inputRouting.load(), 6, "the pair's L kept the previous cue's input");
                expectEquals (w.track (2).inputRouting.load(), 7, "the pair's R is not on L + 1");
            }

            beginTest ("A cue recalled between takes still patches its inputs at once and holds nothing");
            {
                Window w (2);
                SetlistBar::Cue cue;
                cue.name = "Song 4";
                auto s = w.snapshot (0);
                s.inputRouting = 2;
                cue.strips = { s };
                w.main.cues = { cue };
                w.main.jumpToCue (0);
                expectEquals (w.track (0).inputRouting.load(), 2);
                expect (w.main.heldCueInputs.empty());
                expect (! w.main.statusLabel.getText().contains ("held"), w.main.statusLabel.getText());
            }
        }

        // ── MIX-02 ──────────────────────────────────────────────────────

        void meterbridgeReachesEveryStrip()
        {
            for (const int width : { 1440, 1920 })
            {
                beginTest ("The meterbridge reaches all 86 strips in a " + juce::String (width) + " px window, at 28 px a column");
                {
                    AudioEngine engine;
                    engine.setStripCount (86);
                    std::unique_ptr<juce::DialogWindow> window (Meterbridge::launch (engine));
                    expect (window != nullptr);
                    if (window == nullptr) continue;
                    window->setSize (width, 420);
                    auto* content = window->getContentComponent();
                    expect (content != nullptr);
                    if (content == nullptr) continue;
                    expect (content->getWidth() <= width, "the bridge is wider than its window");

                    std::vector<juce::Label*> labels;
                    juce::Viewport* view = nullptr;
                    walk (*content, [&] (juce::Component& c)
                    {
                        if (auto* l = dynamic_cast<juce::Label*> (&c); l != nullptr && c.getTitle().startsWith ("Strip"))
                            labels.push_back (l);
                        if (auto* v = dynamic_cast<juce::Viewport*> (&c); v != nullptr && view == nullptr)
                            view = v;
                    });
                    expectEquals ((int) labels.size(), 86);

                    // Each column brought into view the way the engineer would:
                    // scrolled to, then on screen inside the window.
                    int unreachable = 0, narrow = 0;
                    juce::String firstLost;
                    for (auto* label : labels)
                    {
                        if (view != nullptr && view->getViewedComponent() != nullptr)
                        {
                            const auto at = view->getViewedComponent()->getLocalPoint (label, juce::Point<int>());
                            view->setViewPosition (juce::jmax (0, at.x - 8), 0);
                        }
                        const auto area = content->getLocalArea (label, label->getLocalBounds());
                        if (! content->getLocalBounds().contains (area))
                        {
                            ++unreachable;
                            if (firstLost.isEmpty()) firstLost = label->getTitle();
                        }
                        if (label->getWidth() < 28 - 4) ++narrow;   // a 28 px column less its 2 px insets
                    }
                    expectEquals (unreachable, 0, "strips past the window edge cannot be reached, from " + firstLost);
                    expectEquals (narrow, 0, "columns were squeezed under 28 px");

                    // Every column carries its alarm: the last one, scrolled
                    // into view, changes when its input is lost.
                    if (view != nullptr) view->setViewPosition (1 << 20, 0);
                    auto* last = labels.empty() ? nullptr : labels.back();
                    if (last != nullptr)
                    {
                        const auto lastArea = content->getLocalArea (last, last->getLocalBounds())
                                                  .withY (0).withHeight (content->getHeight()).expanded (2, 0);
                        Meterbridge::setInputAlarms (std::vector<stripalarm::Level> (86, stripalarm::Level::None));
                        const auto quiet = content->createComponentSnapshot (content->getLocalBounds(), true, 1.0f);
                        Meterbridge::setInputAlarms (std::vector<stripalarm::Level> (86, stripalarm::Level::Lost));
                        const auto lost = content->createComponentSnapshot (content->getLocalBounds(), true, 1.0f);
                        expectGreaterThan (differingPixels (quiet, lost, lastArea), 50,
                                           "the last strip's INPUT LOST never reached the screen");
                    }
                }
            }

            beginTest ("A meterbridge that fits its window does not scroll");
            {
                AudioEngine engine;
                engine.setStripCount (8);
                std::unique_ptr<juce::DialogWindow> window (Meterbridge::launch (engine));
                expect (window != nullptr);
                if (window == nullptr) return;
                window->setSize (900, 420);
                auto* content = window->getContentComponent();
                juce::Viewport* view = nullptr;
                walk (*content, [&] (juce::Component& c)
                { if (auto* v = dynamic_cast<juce::Viewport*> (&c); v != nullptr && view == nullptr) view = v; });
                if (view != nullptr)
                {
                    expect (! view->getHorizontalScrollBar().isVisible(), "a scrollbar under eight strips that fit");
                    expect (view->getViewedComponent() != nullptr
                            && view->getViewedComponent()->getWidth() == view->getMaximumVisibleWidth(),
                            "eight strips did not fill the window");
                }
            }
        }

        // ── MIX-03 ──────────────────────────────────────────────────────

        void panReadoutFollowsEveryPath()
        {
            beginTest ("The MIXER's pan readout follows a pan moved by a cue, a group, a surface or EDIT");
            {
                TrackState ts;
                ChannelStrip strip (0, ts);
                strip.setBounds (0, 0, 120, 600);
                expect (strip.panLabel.getText().contains ("0"), strip.panLabel.getText());

                ts.pan.store (-0.4f);   // anything but the strip's own knob
                strip.refreshAppearance();
                expectWithinAbsoluteError ((float) strip.panSlider.getValue(), -0.4f, 0.001f);
                expect (strip.panLabel.getText().contains ("L40"), "the knob moved, the readout says " + strip.panLabel.getText());

                // A ramp read part-way, then its last step: under a hundredth.
                ts.pan.store (0.394f);
                strip.refreshAppearance();
                expect (strip.panLabel.getText().contains ("R39"), strip.panLabel.getText());
                ts.pan.store (0.4f);
                strip.refreshAppearance();
                expect (strip.panLabel.getText().contains ("R40"),
                        "the ramp's last step left the readout one off: " + strip.panLabel.getText());
            }

            beginTest ("A stereo strip's R readout follows its R pan");
            {
                TrackState left, right;
                left.pan.store (-1.0f);
                right.pan.store (1.0f);
                ChannelStrip strip (0, left, {}, {}, {}, {}, {}, {}, &right);
                strip.setBounds (0, 0, 200, 600);
                right.pan.store (0.3f);
                strip.refreshAppearance();
                expect (strip.panLabelR.getText().contains ("R30"), "R's readout says " + strip.panLabelR.getText());
                left.pan.store (-0.25f);
                strip.refreshAppearance();
                expect (strip.panLabel.getText().contains ("L25"), "L's readout says " + strip.panLabel.getText());
            }
        }

        // ── MIX-04 ──────────────────────────────────────────────────────

        void removingARowRunsNothingAfterIt()
        {
            beginTest ("New Tracks: '-' takes its row out at once and frees it only after the press returns");
            {
                dismissModals();
                AddTracksDialog::launch ([] (const std::vector<AddTracksDialog::Entry>&) {});
                juce::Component::SafePointer<juce::DialogWindow> window (
                    dynamic_cast<juce::DialogWindow*> (juce::Component::getCurrentlyModalComponent()));
                expect (window != nullptr);
                if (window == nullptr) return;
                auto* content = window->getContentComponent();

                auto plus = buttonsSaying (*content, "+");
                expectEquals ((int) plus.size(), 1);
                if (plus.empty()) return;
                plus.front()->onClick();
                expectEquals ((int) buttonsSaying (*content, "+").size(), 2, "setup: '+' added no row");
                const int twoRowHeight = content->getHeight();
                auto minus = buttonsSaying (*content, "-");
                expectEquals ((int) minus.size(), 1, "setup: the second row has no '-'");
                if (minus.empty()) return;

                juce::Component::SafePointer<juce::Component> pressed (minus.front());
                juce::Component::SafePointer<juce::Component> row (minus.front()->getParentComponent());
                minus.front()->onClick();   // as JUCE runs the press
                expect (pressed != nullptr, "the row was deleted inside its own '-' press, under the code still running");
                expectEquals ((int) buttonsSaying (*content, "+").size(), 1, "the row stayed in the dialog");
                expectLessThan (content->getHeight(), twoRowHeight, "the dialog kept the removed row's height");
                expect (pumpUntil ([&row] { return row == nullptr; }, 2000), "the removed row was never freed");

                window->exitModalState (0);
                pumpUntil ([&window] { return window == nullptr; }, 3000);
                expect (window == nullptr, "the dialog stayed up");
            }
        }

        // ── MIX-05 ──────────────────────────────────────────────────────

        struct StallProbe
        {
            juce::WaitableEvent release { true };
            std::atomic<bool> ran { false }, onMessageThread { false };
        };

        void backupPickHasATimeLimit()
        {
            beginTest ("A backup folder picked on a drive that does not answer is refused within the drive wait, never on the message thread");
            {
                AudioEngine engine;
                const auto root = tempDir ("stalled-pick");
                const auto session = root.getChildFile ("Gig");
                expect (session.createDirectory().wasOk());
                engine.setActiveSessionDir (session);
                const auto picked = root.getChildFile ("Backups");
                {
                    juce::String refusedWith;
                    BackupMirrorDialog::Host host;
                    host.backupRefused = [&refusedWith] (const juce::String& why) { refusedWith = why; };
                    backupdlg::Content dialog (engine, host);

                    auto probe = std::make_shared<StallProbe>();
                    backupdlg::rootCheckStallForTests = [probe]
                    {
                        probe->ran.store (true);
                        if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                            probe->onMessageThread.store (true);
                        probe->release.wait (6000);
                    };
                    const juce::ScopeGuard unstall { [probe]
                    {
                        backupdlg::rootCheckStallForTests = {};
                        probe->release.signal();
                    } };

                    const auto started = juce::Time::getMillisecondCounterHiRes();
                    const bool accepted = dialog.acceptBackup (picked);
                    const auto waited = juce::Time::getMillisecondCounterHiRes() - started;

                    expect (probe->ran.load(), "the pick never asked the drive under a time limit");
                    expect (! probe->onMessageThread.load(), "the pick asked the drive on the message thread");
                    expect (! accepted, "a folder whose drive never answered was taken as the backup");
                    expectLessThan (waited, (double) preflight::kDriveWaitMs + 1000.0, "the window froze past the drive wait");
                    expect (refusedWith.containsIgnoreCase ("not answer"), "refused without saying why: " + refusedWith);
                    expect (dialog.status.getText() == refusedWith, dialog.status.getText());
                }
                engine.setActiveSessionDir ({});
                pumpUntil ([] { return false; }, 200);   // let the released probe finish
                root.deleteRecursively();
            }

            beginTest ("A backup folder on a drive that answers is still made, checked and taken");
            {
                AudioEngine engine;
                const auto root = tempDir ("answering-pick");
                const auto session = root.getChildFile ("Gig");
                expect (session.createDirectory().wasOk());
                engine.setActiveSessionDir (session);
                {
                    backupdlg::Content dialog (engine, {});
                    const auto fresh = root.getChildFile ("Backups");
                    expect (dialog.acceptBackup (fresh), dialog.status.getText());
                    expect (fresh.isDirectory(), "the picked folder was not made");
                    expect (! dialog.acceptBackup (root), "the session's own parent was accepted");
                    expect (dialog.status.getText().startsWith ("Backup folder rejected"), dialog.status.getText());
                }
                engine.setActiveSessionDir ({});
                root.deleteRecursively();
            }

            beginTest ("Choose... opens at ~/Music, within the drive wait, when the row's drive does not answer");
            {
                auto probe = std::make_shared<StallProbe>();
                backupdlg::rootCheckStallForTests = [probe] { probe->ran.store (true); probe->release.wait (6000); };
                const juce::ScopeGuard unstall { [probe]
                {
                    backupdlg::rootCheckStallForTests = {};
                    probe->release.signal();
                } };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto start = backupdlg::DriveRow::chooserStart (juce::File ("/Volumes/ZF Stalled NAS/Backups"));
                const auto waited = juce::Time::getMillisecondCounterHiRes() - started;
                expect (probe->ran.load(), "the row's folder was asked on the message thread");
                expect (start == juce::File::getSpecialLocation (juce::File::userMusicDirectory), start.getFullPathName());
                expectLessThan (waited, (double) preflight::kDriveWaitMs + 1000.0);
            }
        }
    };

    static Fix20261124MixerTests fix20261124MixerTests;
}
