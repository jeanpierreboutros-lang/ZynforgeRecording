// Fixes of 2026-11-22 on the strips. A right-click opens a menu and does
// nothing else: no fader or pan jump, no M / S / I / R press, no cue
// overwritten or stepped past, no clip count wiped, no palette under the
// menu. A double-click on a PATCH dot keeps the patch, the PATCH stereo pill
// says why it can't link, and Session Settings sends a sample rate only when
// the rate box was changed.
//
// JUCE hands a press to the control under the pointer, then to the strip's
// (or bar's) mouse listener. The tests drive both in that order with
// synthetic events, as the real dispatch does.

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../UI/ChannelStrip.h"
#include "../UI/LedMeter.h"
#include "../UI/LeftClickOnly.h"
#include "../UI/MasterStrip.h"
#include "../UI/PatchPage.h"
#include "../UI/SessionSettingsDialog.h"
#include "../UI/SetlistBar.h"

#include <functional>
#include <memory>
#include <vector>
#include <CoreFoundation/CoreFoundation.h>

namespace zynforge
{
    class Fix20261122StripTests final : public juce::UnitTest
    {
    public:
        Fix20261122StripTests() : juce::UnitTest ("Fixes 2026-11-22 strips", "zynforge") {}

        void runTest() override
        {
            // No real device: the PATCH matrix lists what the test device has.
            AudioEngine::setTestModeSkipAudioInit (true);
            theRule();
            faderIgnoresRightClick();
            panIgnoresRightClick();
            masterIgnoresRightClick();
            stripButtonsIgnoreRightClick();
            setlistButtonsIgnoreRightClick();
            patchDotsIgnoreDoubleClick();
            stereoPillSaysWhy();
            meterKeepsClipsOnRightClick();
            chipOpensOnLeftClickOnly();
            settingsKeepAnUnchangedRate();
        }

    private:
        // ── harness ─────────────────────────────────────────────────────

        static void pumpFor (int ms)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
            while (juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
        }

        static bool pumpUntil (const std::function<bool()>& done, int timeoutMs = 3000)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
            return done();
        }

        static int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

        // Closes every menu and modal left open, so a press is never blocked by
        // one and a menu is only ever this press's.
        static void dropMenusAndModals()
        {
            for (int tries = 0; tries < 100 && juce::PopupMenu::dismissAllActiveMenus(); ++tries)
                pumpFor (10);
            while (auto* modal = juce::Component::getCurrentlyModalComponent())
                modal->exitModalState (0);
            pumpUntil ([] { return modalCount() == 0; }, 2000);
        }

        // True when the press just made opened a menu; closes it.
        static bool menuOpened()
        {
            const bool opened = juce::PopupMenu::dismissAllActiveMenus();
            pumpFor (30);
            return opened;
        }

        static juce::ModifierKeys leftButton()  { return juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier); }
        static juce::ModifierKeys rightButton() { return juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier); }
        static juce::ModifierKeys ctrlClick()   { return juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier
                                                                             | juce::ModifierKeys::ctrlModifier); }

        // A press, drag or release on `target` at `p`, pressed at `down`.
        static juce::MouseEvent at (juce::Component& target, juce::Point<int> p, juce::ModifierKeys mods,
                                    int clicks = 1, juce::Point<int> down = { -1, -1 }, bool dragged = false)
        {
            const auto now = juce::Time::getCurrentTime();
            const auto from = down.x < 0 ? p : down;
            return { juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods,
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &target, &target, now, from.toFloat(), now,
                     clicks, dragged };
        }

        // The same press as the parent's mouse listener sees it.
        static juce::MouseEvent relayed (juce::Component& parent, juce::Component& child,
                                         juce::Point<int> p, juce::ModifierKeys mods)
        {
            const auto now = juce::Time::getCurrentTime();
            const auto q = parent.getLocalPoint (&child, p).toFloat();
            return { juce::Desktop::getInstance().getMainMouseSource(), q, mods,
                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                     juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                     juce::MouseInputSource::defaultTiltY, &parent, &child, now, q, now, 1, false };
        }

        // A full click on `child` the way JUCE delivers it: the child's press,
        // then the parent's listener (which opens its menu), then the release.
        static void clickThrough (juce::Component& parent, juce::Component& child, juce::ModifierKeys mods)
        {
            const auto p = child.getLocalBounds().getCentre();
            child.mouseDown (at (child, p, mods));
            parent.mouseDown (relayed (parent, child, p, mods));
            child.mouseUp (at (child, p, mods));
        }

        template <typename T>
        static T* findTitled (juce::Component& root, const juce::String& title)
        {
            if (auto* t = dynamic_cast<T*> (&root); t != nullptr && root.getTitle() == title) return t;
            for (auto* child : root.getChildren())
                if (auto* found = findTitled<T> (*child, title)) return found;
            return nullptr;
        }

        static juce::String patchOf (AudioEngine& engine, int track)
        {
            const auto& state = engine.getRecorder().getTrack (track);
            return juce::String (state.inputRouting.load()) + "/" + juce::String (state.outputRouting.load());
        }

        static juce::TabbedComponent* tabsOf (juce::Component& root)
        {
            if (auto* t = dynamic_cast<juce::TabbedComponent*> (&root)) return t;
            for (auto* child : root.getChildren())
                if (auto* found = tabsOf (*child)) return found;
            return nullptr;
        }

        // ── the rule ────────────────────────────────────────────────────

        void theRule()
        {
            beginTest ("A menu press is the right or middle button or Ctrl-click; a plain press is a left click");
            {
                juce::Component c;
                c.setBounds (0, 0, 20, 20);
                const juce::Point<int> p (5, 5);
                expect (leftclick::isMenuPress (at (c, p, rightButton())));
                expect (leftclick::isMenuPress (at (c, p, ctrlClick())), "Ctrl-click is the Mac's right-click");
                expect (leftclick::isMenuPress (at (c, p, juce::ModifierKeys (juce::ModifierKeys::middleButtonModifier))));
                expect (! leftclick::isMenuPress (at (c, p, leftButton())));
                expect (! leftclick::isMenuPress (at (c, p, juce::ModifierKeys())),
                        "a press with no button flags must still count as a left click");
                expect (! leftclick::isMenuPress (at (c, p, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier
                                                                                 | juce::ModifierKeys::commandModifier))),
                        "Cmd-click is a left click");
            }
        }

        // ── 1: the fader ────────────────────────────────────────────────

        void faderIgnoresRightClick()
        {
            beginTest ("A right-click on a channel fader opens the strip menu and leaves the level");
            {
                dropMenusAndModals();
                TrackState ts;
                ts.gainDb.store (-10.0f);
                int sent = 0;
                ChannelStrip strip (0, ts, {}, {}, [&sent] (float) { ++sent; });
                strip.setBounds (0, 0, 90, 520);
                auto& fader = strip.gainFader;
                expect (fader.getHeight() > 100, "setup: the fader has room");
                expectWithinAbsoluteError ((float) fader.getValue(), -10.0f, 0.01f, "setup");

                const juce::Point<int> top (fader.getWidth() / 2, 2);   // +12 dB on a snapping fader
                const juce::Point<int> low (fader.getWidth() / 2, fader.getHeight() - 2);
                for (const auto mods : { rightButton(), ctrlClick() })
                {
                    fader.mouseDown (at (fader, top, mods));
                    strip.mouseDown (relayed (strip, fader, top, mods));
                    expect (menuOpened(), "the strip menu no longer opens from a right-click on the fader");
                    fader.mouseDrag (at (fader, low, mods, 1, top, true));
                    fader.mouseUp   (at (fader, low, mods, 1, top, true));
                    // Two right-clicks make a double-click: 0 dB, sent after the second release.
                    fader.mouseDown (at (fader, top, mods, 2));
                    fader.mouseUp   (at (fader, top, mods, 2));
                    fader.mouseDoubleClick (at (fader, top, mods, 2));
                    menuOpened();
                }
                expectWithinAbsoluteError (ts.gainDb.load(), -10.0f, 0.01f, "a right-click moved the channel's gain");
                expectWithinAbsoluteError ((float) fader.getValue(), -10.0f, 0.01f, "a right-click moved the cap");
                expectEquals (sent, 0, "a right-click sent a gain to the engine");

                // The same press with the left button does move it: the harness reaches the fader.
                fader.mouseDown (at (fader, top, leftButton()));
                fader.mouseUp   (at (fader, top, leftButton()));
                expect (ts.gainDb.load() > 0.0f, "setup: a left press at the top of the fader moves it");
                expect (sent > 0);
            }
        }

        void panIgnoresRightClick()
        {
            beginTest ("A right press, drag or double-click never turns a pan knob");
            {
                dropMenusAndModals();
                TrackState ts;
                ts.pan.store (0.5f);
                ChannelStrip strip (0, ts);
                strip.setBounds (0, 0, 90, 520);
                auto* pan = findTitled<juce::Slider> (strip, "Pan");
                expect (pan != nullptr, "setup: the pan knob");
                if (pan == nullptr) return;
                const auto c = pan->getLocalBounds().getCentre();
                for (const auto mods : { rightButton(), ctrlClick() })
                {
                    pan->mouseDown (at (*pan, c, mods));
                    pan->mouseDrag (at (*pan, c + juce::Point<int> (60, -60), mods, 1, c, true));
                    pan->mouseUp   (at (*pan, c + juce::Point<int> (60, -60), mods, 1, c, true));
                    pan->mouseDown (at (*pan, c, mods, 2));
                    pan->mouseUp   (at (*pan, c, mods, 2));
                    pan->mouseDoubleClick (at (*pan, c, mods, 2));
                }
                expectWithinAbsoluteError (ts.pan.load(), 0.5f, 0.001f, "a right press turned the pan");

                // A left double-click still centres it.
                pan->mouseDown (at (*pan, c, leftButton()));
                pan->mouseUp   (at (*pan, c, leftButton()));
                pan->mouseDown (at (*pan, c, leftButton(), 2));
                pan->mouseUp   (at (*pan, c, leftButton(), 2));
                pan->mouseDoubleClick (at (*pan, c, leftButton(), 2));
                expectWithinAbsoluteError (ts.pan.load(), 0.0f, 0.001f, "setup: a left double-click centres the pan");
            }
        }

        void masterIgnoresRightClick()
        {
            beginTest ("A right-click on the master changes nothing: fader, MUTE and ST stay");
            {
                dropMenusAndModals();
                AudioEngine engine;
                engine.setStripCount (1);
                engine.setMasterGainDb (-10.0f);
                engine.setMasterMuted (false);
                const bool stereo = engine.getMasterStereo();
                {
                    MasterStrip master (engine);
                    master.setBounds (0, 0, 120, 560);
                    auto* fader = findTitled<juce::Slider> (master, "Master gain");
                    auto* mute  = findTitled<juce::Button> (master, "Master mute");
                    auto* mode  = findTitled<juce::Button> (master, "Mono / stereo");
                    expect (fader != nullptr && mute != nullptr && mode != nullptr, "setup: the master's controls");
                    if (fader != nullptr && mute != nullptr && mode != nullptr)
                    {
                        const juce::Point<int> top (fader->getWidth() / 2, 4);
                        const juce::Point<int> low (fader->getWidth() / 2, fader->getHeight() - 30);
                        for (const auto mods : { rightButton(), ctrlClick() })
                        {
                            fader->mouseDown (at (*fader, top, mods));
                            fader->mouseDrag (at (*fader, low, mods, 1, top, true));
                            fader->mouseUp   (at (*fader, low, mods, 1, top, true));
                            fader->mouseDown (at (*fader, top, mods, 2));
                            fader->mouseUp   (at (*fader, top, mods, 2));
                            fader->mouseDoubleClick (at (*fader, top, mods, 2));
                            clickThrough (master, *mute, mods);
                            clickThrough (master, *mode, mods);
                        }
                        expectWithinAbsoluteError (engine.getMasterGainDb(), -10.0f, 0.01f, "a right-drag moved the master");
                        expect (! engine.getMasterMuted(), "a right-click muted the PA");
                        expect (engine.getMasterStereo() == stereo, "a right-click flipped the master's mode");

                        // Left still works.
                        fader->mouseDown (at (*fader, top, leftButton()));
                        fader->mouseDrag (at (*fader, low, leftButton(), 1, top, true));
                        fader->mouseUp   (at (*fader, low, leftButton(), 1, top, true));
                        expect (engine.getMasterGainDb() < -10.5f, "setup: a left drag moves the master");
                        clickThrough (master, *mute, leftButton());
                        expect (engine.getMasterMuted(), "setup: a left click mutes the master");
                        engine.setMasterMuted (false);
                    }
                }
                engine.setStripCount (0);
            }
        }

        // ── 2: the buttons ──────────────────────────────────────────────

        void stripButtonsIgnoreRightClick()
        {
            beginTest ("A right-click on R / I / M / S opens the strip menu and presses nothing");
            {
                dropMenusAndModals();
                TrackState ts;
                ChannelStrip strip (0, ts);
                int hooks = 0;
                strip.onAfterArmedToggle   = [&hooks] (int) { ++hooks; };
                strip.onAfterMonitorToggle = [&hooks] (int) { ++hooks; };
                strip.onAfterMuteToggle    = [&hooks] (int) { ++hooks; };
                strip.onAfterSoloToggle    = [&hooks] (int) { ++hooks; };
                strip.setBounds (0, 0, 90, 520);

                for (const auto* title : { "Record arm", "Input monitor", "Mute", "Solo" })
                {
                    auto* b = findTitled<juce::Button> (strip, title);
                    expect (b != nullptr, juce::String ("setup: ") + title);
                    if (b == nullptr) continue;
                    juce::Component& c = *b;   // a Button's mouse handlers are protected
                    for (const auto mods : { rightButton(), ctrlClick() })
                    {
                        const auto p = b->getLocalBounds().getCentre();
                        c.mouseDown (at (c, p, mods));
                        strip.mouseDown (relayed (strip, c, p, mods));
                        c.mouseUp (at (c, p, mods));   // the menu is open, as it is for real
                        expect (menuOpened(), juce::String ("no strip menu from a right-click on ") + title);
                        expect (! b->getToggleState(), juce::String ("a right-click pressed ") + title);
                    }
                }
                expect (! ts.armed.load(),   "a right-click armed the channel");
                expect (! ts.monitor.load(), "a right-click turned on the input monitor");
                expect (! ts.muted.load(),   "a right-click muted the channel");
                expect (! ts.soloed.load(),  "a right-click soloed the channel");
                expectEquals (hooks, 0, "a right-click told the edit group");

                // A left click still presses it.
                if (auto* mute = findTitled<juce::Button> (strip, "Mute"))
                {
                    clickThrough (strip, *mute, leftButton());
                    expect (ts.muted.load(), "setup: a left click on M mutes");
                    expectEquals (hooks, 1);
                }
            }
        }

        void setlistButtonsIgnoreRightClick()
        {
            beginTest ("A right-click on a setlist button opens the cue menu and presses nothing");
            {
                dropMenusAndModals();
                SetlistBar bar;
                bar.setBounds (0, 0, 900, 36);
                std::vector<SetlistBar::Cue> cues (3);
                for (int i = 0; i < 3; ++i) { cues[(size_t) i].name = "Song " + juce::String (i + 1); cues[(size_t) i].samplePos = i * 48000; }
                bar.setCues (cues, 1);
                int pressed = 0, deleted = 0;
                bar.onPrev = bar.onNext = bar.onAddCue = bar.onUpdateCue = bar.onRecallCue = [&pressed] { ++pressed; };
                bar.onPick = [&pressed] (int) { ++pressed; };
                bar.onDeleteCue = [&deleted] { ++deleted; };
                bar.onRenameCue = [] {};
                bar.onMoveCue = [] (int) {};

                for (const auto* title : { "Update cue", "Next cue", "Previous cue", "Add cue", "Recall cue" })
                {
                    auto* b = findTitled<juce::Button> (bar, title);
                    expect (b != nullptr, juce::String ("setup: ") + title);
                    if (b == nullptr) continue;
                    juce::Component& c = *b;   // a Button's mouse handlers are protected
                    for (const auto mods : { rightButton(), ctrlClick() })
                    {
                        const auto p = b->getLocalBounds().getCentre();
                        c.mouseDown (at (c, p, mods));
                        bar.mouseDown (relayed (bar, c, p, mods));
                        c.mouseUp (at (c, p, mods));
                        expect (menuOpened(), juce::String ("no cue menu from a right-click on ") + title);
                    }
                    expectEquals (pressed, 0, juce::String ("a right-click pressed ") + title);
                }
                expectEquals (deleted, 0, "a dismissed menu deleted a cue");

                // A left click still presses it.
                if (auto* update = findTitled<juce::Button> (bar, "Update cue"))
                {
                    clickThrough (bar, *update, leftButton());
                    expectEquals (pressed, 1, "setup: a left click on Update updates");
                }
            }
        }

        // ── 3: PATCH dots ───────────────────────────────────────────────

        void patchDotsIgnoreDoubleClick()
        {
            beginTest ("A double-click or right-click on a PATCH dot keeps the strip's input and output");
            {
                dropMenusAndModals();
                AudioEngine engine;
                engine.setDeviceInputCountForTests (8);
                engine.setDeviceOutputCountForTests (8);
                engine.setStripCount (2);
                {
                    std::unique_ptr<juce::DialogWindow> window (PatchPage::launch (engine));
                    auto* tabs = window != nullptr ? tabsOf (*window) : nullptr;
                    expect (tabs != nullptr, "setup: the patch tabs");
                    if (tabs != nullptr)
                    {
                        // Matrix geometry: 90 px row header, 92 px column header, 56 px rows.
                        auto* inputs  = tabs->getTabContentComponent (0);
                        auto* outputs = tabs->getTabContentComponent (1);
                        inputs->setBounds (0, 0, 1000, 700);
                        outputs->setBounds (0, 0, 1000, 700);
                        constexpr int colW = (1000 - 90 - 12) / 2;
                        const auto dot = [] (int column, int row)
                        { return juce::Point<int> (90 + column * colW + 10, 92 + row * 56 + 10); };

                        inputs->mouseDown (at (*inputs, dot (1, 3), leftButton()));
                        inputs->mouseUp   (at (*inputs, dot (1, 3), leftButton()));
                        expectEquals (patchOf (engine, 1), juce::String ("3/3"), "setup: one click patches the dot");

                        for (int clicks : { 2, 3, 4 })
                        {
                            inputs->mouseDown (at (*inputs, dot (1, 3), leftButton(), clicks));
                            inputs->mouseUp   (at (*inputs, dot (1, 3), leftButton(), clicks));
                        }
                        expectEquals (patchOf (engine, 1), juce::String ("3/3"),
                                      "the second click of a double-click cleared the strip's input and output");

                        for (const auto mods : { rightButton(), ctrlClick() })
                            for (int row : { 3, 5 })
                            {
                                inputs->mouseDown (at (*inputs, dot (1, row), mods));
                                inputs->mouseUp   (at (*inputs, dot (1, row), mods));
                                outputs->mouseDown (at (*outputs, dot (1, row), mods));
                                outputs->mouseUp   (at (*outputs, dot (1, row), mods));
                            }
                        expectEquals (patchOf (engine, 1), juce::String ("3/3"), "a right-click repatched the strip");

                        outputs->mouseDown (at (*outputs, dot (1, 4), leftButton()));
                        outputs->mouseUp   (at (*outputs, dot (1, 4), leftButton()));
                        outputs->mouseDown (at (*outputs, dot (1, 4), leftButton(), 2));
                        outputs->mouseUp   (at (*outputs, dot (1, 4), leftButton(), 2));
                        expectEquals (patchOf (engine, 1), juce::String ("4/4"),
                                      "a double-click on an output dot sent the strip to the master");

                        // A later single click still clears it, as before.
                        inputs->mouseDown (at (*inputs, dot (1, 4), leftButton()));
                        inputs->mouseUp   (at (*inputs, dot (1, 4), leftButton()));
                        expectEquals (patchOf (engine, 1), juce::String ("-1/-1"), "setup: a click on a lit dot clears it");
                    }
                }
                engine.setStripCount (0);
                engine.setDeviceInputCountForTests (0);
                engine.setDeviceOutputCountForTests (0);
            }
        }

        // ── 4: the PATCH stereo pill ────────────────────────────────────

        void stereoPillSaysWhy()
        {
            beginTest ("The PATCH stereo pill says why it can't link, in the MIXER's words");
            {
                dropMenusAndModals();
                AudioEngine engine;
                engine.setDeviceInputCountForTests (8);
                engine.setDeviceOutputCountForTests (8);
                engine.setStripCount (3);
                expect (engine.linkStereoPair (1, true), "setup: strips 2-3 are a pair");
                juce::StringArray notices;
                engine.getRemotes().setHost ({ [&notices] (const juce::String& s) { notices.add (s); }, {}, {} });

                expectEquals (PatchPage::linkRefusal (engine, 0, false), juce::String ("Unlink the neighbouring stereo pair first"));
                expect (PatchPage::linkRefusal (engine, 1, true).isEmpty(), "unlinking a pair needs no neighbour");
                expectEquals (PatchPage::linkRefusedByEngine(),
                              juce::String ("Stop recording before linking or unlinking a stereo pair"));
                {
                    std::unique_ptr<juce::DialogWindow> window (PatchPage::launch (engine));
                    auto* tabs = window != nullptr ? tabsOf (*window) : nullptr;
                    expect (tabs != nullptr, "setup: the patch tabs");
                    if (tabs != nullptr)
                    {
                        auto* inputs = tabs->getTabContentComponent (0);
                        inputs->setBounds (0, 0, 1000, 700);
                        // The pill band runs from 60 to 92 px, over each column.
                        const auto pill = [] (int column, int columns)
                        { return juce::Point<int> (90 + column * ((1000 - 90 - 12) / columns) + 10, 75); };

                        inputs->mouseDown (at (*inputs, pill (0, 2), leftButton()));
                        expect (! engine.getRecorder().getTrack (0).isStereo.load(), "strip 1 took half of the pair");
                        expect (notices.size() > 0 && notices[notices.size() - 1] == "Unlink the neighbouring stereo pair first",
                                notices.joinIntoString (" | "));

                        inputs->mouseDown (at (*inputs, pill (1, 2), leftButton()));
                        expect (! engine.getRecorder().getTrack (1).isStereo.load(), "setup: the pill unlinks the pair");

                        notices.clear();
                        inputs->mouseDown (at (*inputs, pill (2, 3), leftButton()));
                        expect (! engine.getRecorder().getTrack (2).isStereo.load());
                        expect (notices.size() > 0 && notices[notices.size() - 1].contains ("last channel")
                                    && notices[notices.size() - 1].contains ("Convert to Stereo"),
                                "the last strip's pill did nothing and said nothing: " + notices.joinIntoString (" | "));
                    }
                }
                engine.getRemotes().setHost ({});
                engine.setStripCount (0);
                engine.setDeviceInputCountForTests (0);
                engine.setDeviceOutputCountForTests (0);
            }
        }

        // ── 5: the meter ────────────────────────────────────────────────

        void meterKeepsClipsOnRightClick()
        {
            beginTest ("A right-click on a meter keeps CLIP xN; a left click clears both halves");
            {
                dropMenusAndModals();
                TrackState left, right;
                left.isStereo.store (true);
                LedMeter meter (left);
                meter.setStereoPartner (&right);
                meter.setBounds (0, 0, 30, 200);
                left.clipped.store (true);  left.clipCount.store (3);
                right.clipped.store (true); right.clipCount.store (2);
                const juce::Point<int> p (15, 100);
                for (const auto mods : { rightButton(), ctrlClick() })
                {
                    meter.mouseDown (at (meter, p, mods));
                    meter.mouseUp   (at (meter, p, mods));
                }
                expectEquals (left.clipCount.load() + right.clipCount.load(), 5, "a right-click wiped the clip count");
                expect (left.clipped.load() && right.clipped.load(), "a right-click cleared the clip flag");

                meter.mouseDown (at (meter, p, leftButton()));
                expectEquals (left.clipCount.load() + right.clipCount.load(), 0, "setup: a left click clears the count");
                expect (! left.clipped.load() && ! right.clipped.load());
                meter.detach();
            }

            beginTest ("A right-click on a strip's meter opens the menu and keeps the count");
            {
                dropMenusAndModals();
                TrackState ts;
                ChannelStrip strip (0, ts);
                strip.setBounds (0, 0, 90, 520);
                ts.clipped.store (true);
                ts.clipCount.store (4);
                auto& meter = strip.meter;
                const auto p = meter.getLocalBounds().getCentre();
                meter.mouseDown (at (meter, p, rightButton()));
                strip.mouseDown (relayed (strip, meter, p, rightButton()));
                expect (menuOpened(), "no strip menu from a right-click on the meter");
                expectEquals (ts.clipCount.load(), 4, "opening the strip menu wiped CLIP x4");
            }
        }

        // ── 6: the colour chip ──────────────────────────────────────────

        void chipOpensOnLeftClickOnly()
        {
            beginTest ("The colour chip opens its palette on a left click only");
            {
                dropMenusAndModals();
                TrackState ts;
                ChannelStrip strip (0, ts, [] (juce::Colour) {});
                int asked = 0;   // the palette asks first; answering no keeps it shut
                strip.canOpenColourPicker = [&asked] { ++asked; return false; };
                strip.setBounds (0, 0, 90, 520);

                juce::Component* chip = nullptr;
                for (auto* child : strip.getChildren())
                    if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (child))
                        if (tip->getTooltip().startsWith ("Click for a colour palette")) chip = child;
                expect (chip != nullptr, "the colour chip has no tooltip");
                if (chip == nullptr) return;

                const auto p = chip->getLocalBounds().getCentre();
                for (const auto mods : { rightButton(), ctrlClick() })
                {
                    chip->mouseDown (at (*chip, p, mods));
                    strip.mouseDown (relayed (strip, *chip, p, mods));
                    expect (menuOpened(), "no strip menu from a right-click on the chip");
                }
                expectEquals (asked, 0, "a right-click opened the palette under the strip menu");

                chip->mouseDown (at (*chip, p, leftButton()));
                expectEquals (asked, 1, "setup: a left click opens the palette");
            }
        }

        // ── 7: Session Settings ─────────────────────────────────────────

        struct SettingsControls
        {
            juce::ComboBox* rate = nullptr;
            juce::ComboBox* preRoll = nullptr;
            juce::Button*   apply = nullptr;
        };

        static SettingsControls findSettings()
        {
            SettingsControls found;
            auto* dialog = juce::Component::getCurrentlyModalComponent();
            if (dialog == nullptr) return found;
            const std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                if (auto* box = dynamic_cast<juce::ComboBox*> (&c))
                {
                    if (box->getTitle() == "Sample Rate") found.rate = box;
                    if (box->getTitle() == "Pre-roll")    found.preRoll = box;
                }
                if (auto* b = dynamic_cast<juce::Button*> (&c))
                    if (b->getButtonText() == "Apply") found.apply = b;
                for (auto* child : c.getChildren()) walk (*child);
            };
            walk (*dialog);
            return found;
        }

        void settingsKeepAnUnchangedRate()
        {
            beginTest ("Session Settings sends a sample rate only when the rate box was changed");
            {
                dropMenusAndModals();
                AudioEngine engine;
                std::vector<double> applied;
                const auto launch = [&engine, &applied]
                {
                    SessionSettingsDialog::launch (engine, {}, [&applied] (double sr) { applied.push_back (sr); });
                    return findSettings();
                };

                // A rate the list doesn't hold is shown as it is, and kept.
                engine.setSessionSampleRate (32000.0);
                auto c = launch();
                expect (c.rate != nullptr && c.apply != nullptr, "setup: the dialog's rate box and Apply");
                if (c.rate != nullptr && c.apply != nullptr)
                {
                    expect (c.rate->getText().startsWith ("32 kHz"), "an unlisted rate showed as " + c.rate->getText());
                    if (c.preRoll != nullptr) c.preRoll->setSelectedId (6, juce::dontSendNotification);   // 5 s
                    c.apply->onClick();
                    expect (applied.empty(), "Apply for the pre-roll moved the session to "
                                                 + juce::String (applied.empty() ? 0.0 : applied.back()));
                }
                dropMenusAndModals();

                // No rate at all (no session rate, no device): "Keep current", kept.
                engine.setSessionSampleRate (0.0);
                const bool noDeviceRate = engine.getDeviceManager().getAudioDeviceSetup().sampleRate <= 0.0;
                c = launch();
                if (c.rate != nullptr && c.apply != nullptr)
                {
                    if (noDeviceRate)
                        expectEquals (c.rate->getText(), juce::String ("Keep current"), "no rate showed as a rate");
                    c.apply->onClick();
                    expect (applied.empty(), "Apply with no rate set one");
                }
                dropMenusAndModals();

                // A listed rate left alone is not sent; a changed one is.
                engine.setSessionSampleRate (48000.0);
                c = launch();
                if (c.rate != nullptr && c.apply != nullptr)
                {
                    expectEquals (c.rate->getText(), juce::String ("48 kHz"));
                    c.apply->onClick();
                    expect (applied.empty(), "Apply re-sent an unchanged rate");
                }
                dropMenusAndModals();

                c = launch();
                if (c.rate != nullptr && c.apply != nullptr)
                {
                    c.rate->setSelectedId (4, juce::dontSendNotification);   // 96 kHz
                    c.apply->onClick();
                    expect (applied.size() == 1 && juce::approximatelyEqual (applied.back(), 96000.0),
                            "a changed rate was not applied");
                }
                dropMenusAndModals();
                engine.setSessionSampleRate (0.0);
            }
        }
    };

    static Fix20261122StripTests fix20261122StripTests;
}
