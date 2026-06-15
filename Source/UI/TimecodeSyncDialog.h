#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // Transport Sync: chase the transport to an external timecode master --
    // MTC over a MIDI input, or LTC decoded off an audio input. Pick the
    // active master + its source, watch the incoming timecode live, and
    // choose whether to keep rolling through a sync dropout. Uses the
    // engine's existing TimecodeChase + chase service.
    class TimecodeSyncDialog
    {
    public:
        static void launch (AudioEngine& engine);
    };

    namespace tcsync_detail
    {
        class Content final : public juce::Component, private juce::Timer
        {
        public:
            explicit Content (AudioEngine& e) : engine (e)
            {
                auto label = [this] (juce::Label& l, const juce::String& t, juce::Colour c, float sz, bool b)
                {
                    l.setText (t, juce::dontSendNotification);
                    l.setColour (juce::Label::textColourId, c);
                    l.setFont (brand::type::ui (sz, b));
                    addAndMakeVisible (l);
                };
                label (colSelect, "Master", brand::textSecondary, 12.0f, true);
                label (colType,   "Type",   brand::textSecondary, 12.0f, true);
                label (colSource, "Source", brand::textSecondary, 12.0f, true);
                label (colStatus, "Incoming timecode", brand::textSecondary, 12.0f, true);

                label (mtcType, "MTC", brand::textPrimary, 14.0f, true);
                label (ltcType, "LTC", brand::textPrimary, 14.0f, true);
                label (mtcStatus, "-- idle", brand::textMuted, 14.0f, false);
                label (ltcStatus, "-- idle", brand::textMuted, 14.0f, false);
                mtcStatus.setFont (brand::type::monoCounter());
                ltcStatus.setFont (brand::type::monoCounter());

                // Master radios (which source drives the transport).
                for (auto* r : { &mtcSel, &ltcSel })
                {
                    r->setRadioGroupId (1);
                    r->setColour (juce::ToggleButton::tickColourId, brand::accentStatus);
                    addAndMakeVisible (*r);
                }
                masterIsMtc = (engine.getChaseMode() != AudioEngine::ChaseMode::Ltc);
                mtcSel.setToggleState (masterIsMtc,  juce::dontSendNotification);
                ltcSel.setToggleState (! masterIsMtc, juce::dontSendNotification);
                mtcSel.onClick = [this] { masterIsMtc = true;  recompute(); };
                ltcSel.onClick = [this] { masterIsMtc = false; recompute(); };

                // MTC source = MIDI input device.
                dialog::styleCombo (mtcSource);
                mtcSource.addItem ("Disconnected", 1);
                {
                    const auto names = engine.getMidiInputNames();
                    for (int i = 0; i < names.size(); ++i) mtcSource.addItem (names[i], i + 2);
                    const int idx = names.indexOf (engine.getMtcInputName());
                    mtcSource.setSelectedId (idx >= 0 ? idx + 2 : 1, juce::dontSendNotification);
                }
                mtcSource.onChange = [this] { onMtcSource(); };
                addAndMakeVisible (mtcSource);

                // LTC source = an audio input strip.
                dialog::styleCombo (ltcSource);
                ltcSource.addItem ("Disconnected", 1);
                {
                    const int nt = engine.getRecorder().getNumTracks();
                    for (int i = 0; i < nt; ++i) ltcSource.addItem ("Input " + juce::String (i + 1), i + 2);
                    const int cur = engine.getLtcSourceStrip();   // 1-based, 0 = none
                    ltcSource.setSelectedId (cur >= 1 ? cur + 1 : 1, juce::dontSendNotification);
                }
                ltcSource.onChange = [this] { onLtcSource(); };
                addAndMakeVisible (ltcSource);

                keepRolling.setButtonText ("Keep rolling if sync is lost");
                keepRolling.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                keepRolling.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);
                keepRolling.setToggleState (engine.isKeepRollingOnSyncLoss(), juce::dontSendNotification);
                keepRolling.onClick = [this] { engine.setKeepRollingOnSyncLoss (keepRolling.getToggleState()); };
                addAndMakeVisible (keepRolling);

                closeBtn.setButtonText ("Close");
                dialog::styleSecondary (closeBtn);
                closeBtn.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (closeBtn);

                setSize (600, 250);
                startTimerHz (15);
                refreshStatus();
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "TRANSPORT SYNC");
                g.setColour (brand::edge);
                g.drawHorizontalLine (rowY (0) - 8, (float) pad, (float) (getWidth() - pad));
            }

            void resized() override
            {
                auto hdr = juce::Rectangle<int> (pad, rowY (-1), getWidth() - pad * 2, 18);
                colSelect.setBounds (cSel (hdr));
                colType  .setBounds (cType (hdr));
                colSource.setBounds (cSrc (hdr));
                colStatus.setBounds (cStat (hdr));

                auto place = [this] (int row, juce::ToggleButton& sel, juce::Label& type,
                                     juce::ComboBox& src, juce::Label& stat)
                {
                    auto r = juce::Rectangle<int> (pad, rowY (row), getWidth() - pad * 2, 30);
                    sel .setBounds (cSel (r).withSizeKeepingCentre (24, 24));
                    type.setBounds (cType (r));
                    src .setBounds (cSrc (r).reduced (0, 2));
                    stat.setBounds (cStat (r));
                };
                place (0, mtcSel, mtcType, mtcSource, mtcStatus);
                place (1, ltcSel, ltcType, ltcSource, ltcStatus);

                keepRolling.setBounds (pad, rowY (2) + 8, 320, 24);
                closeBtn.setBounds (getWidth() - pad - 90, getHeight() - 44, 90, 28);
            }

        private:
            static constexpr int pad = 24;
            int rowY (int row) const { return 70 + (row + 1) * 36; }
            juce::Rectangle<int> cSel  (juce::Rectangle<int> r) const { return r.removeFromLeft (70); }
            juce::Rectangle<int> cType (juce::Rectangle<int> r) const { r.removeFromLeft (70); return r.removeFromLeft (60); }
            juce::Rectangle<int> cSrc  (juce::Rectangle<int> r) const { r.removeFromLeft (130); return r.removeFromLeft (220); }
            juce::Rectangle<int> cStat (juce::Rectangle<int> r) const { r.removeFromLeft (130 + 220); return r; }

            void onMtcSource()
            {
                const int id = mtcSource.getSelectedId();
                engine.setMtcInputByName (id <= 1 ? juce::String() : mtcSource.getText());
                if (id > 1) { masterIsMtc = true; mtcSel.setToggleState (true, juce::dontSendNotification); }
                recompute();
            }
            void onLtcSource()
            {
                const int id = ltcSource.getSelectedId();
                engine.setLtcSourceStrip (id <= 1 ? 0 : id - 1);   // 1-based strip, 0 = none
                if (id > 1) { masterIsMtc = false; ltcSel.setToggleState (true, juce::dontSendNotification); }
                recompute();
            }

            void recompute()
            {
                const bool mtcConnected = mtcSource.getSelectedId() > 1;
                const bool ltcConnected = ltcSource.getSelectedId() > 1;
                if (masterIsMtc)
                    engine.setChaseMode (mtcConnected ? AudioEngine::ChaseMode::Mtc
                                                      : AudioEngine::ChaseMode::Off);
                else
                    engine.setChaseMode (ltcConnected ? AudioEngine::ChaseMode::Ltc
                                                      : AudioEngine::ChaseMode::Off);
            }

            void timerCallback() override { refreshStatus(); }

            void refreshStatus()
            {
                auto& tc = engine.getTimecodeChase();
                const bool mode_mtc = engine.getChaseMode() == AudioEngine::ChaseMode::Mtc;
                const bool mode_ltc = engine.getChaseMode() == AudioEngine::ChaseMode::Ltc;
                const bool live = engine.isChaseLive();
                const auto tcStr = engine.getChaseTimecodeString();
                const int  fps   = (int) (tc.getFps() + 0.5f);

                auto set = [&] (juce::Label& l, bool active)
                {
                    if (active && live)
                    {
                        l.setText (tcStr + (fps > 0 ? "  " + juce::String (fps) + " fps  LOCK" : "  LOCK"),
                                   juce::dontSendNotification);
                        l.setColour (juce::Label::textColourId, brand::accentStatus);
                    }
                    else if (active)
                    {
                        l.setText ("--:--:--:--  waiting", juce::dontSendNotification);
                        l.setColour (juce::Label::textColourId, brand::textSecondary);
                    }
                    else
                    {
                        l.setText ("-- idle", juce::dontSendNotification);
                        l.setColour (juce::Label::textColourId, brand::textMuted);
                    }
                };
                set (mtcStatus, mode_mtc);
                set (ltcStatus, mode_ltc);
            }

            AudioEngine& engine;
            bool masterIsMtc { true };
            juce::Label colSelect, colType, colSource, colStatus;
            juce::Label mtcType, ltcType, mtcStatus, ltcStatus;
            juce::ToggleButton mtcSel, ltcSel, keepRolling;
            juce::ComboBox mtcSource, ltcSource;
            juce::TextButton closeBtn;
        };
    }

    inline void TimecodeSyncDialog::launch (AudioEngine& engine)
    {
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Transport Sync";
        opts.content.setOwned (new tcsync_detail::Content (engine));
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
