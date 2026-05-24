#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "StripColourPicker.h"

#include <array>

namespace zynforge
{
    // Compact VCA fader panel -- 8 mini-strips that each ride one VCA
    // bus. Engineers assign channel strips to a VCA via the
    // ChannelStrip right-click menu; this panel is where they actually
    // ride the bus fader during a show.
    //
    // Each strip paints: colour swatch / name / M / S buttons / dB
    // readout / vertical fader (−60..+12 dB). 24 Hz refresh polls the
    // engine's VcaBus atomics so cue recalls / OSC moves show live.
    //
    // The panel is toggled via a header button (engineer hides it when
    // they don't need 8 extra columns; default off so a fresh install
    // doesn't crowd the mixer).
    class VcaPanel final : public juce::Component, private juce::Timer
    {
    public:
        explicit VcaPanel (AudioEngine& eng) : engine (eng)
        {
            for (int i = 0; i < AudioEngine::kNumVcas; ++i)
            {
                auto strip = std::make_unique<VcaStripView> (engine, i);
                addAndMakeVisible (*strip);
                strips[(size_t) i] = std::move (strip);
            }
            startTimerHz (20);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setGradientFill (brand::verticalGradient (brand::bgDeep, r, 0.08f, 0.14f));
            g.fillRect (r);
            g.setColour (brand::edge);
            g.drawRect (getLocalBounds(), 1);
            g.setColour (brand::textSecondary);
            g.setFont (brand::type::channelName());
            g.drawText ("VCA GROUPS", getLocalBounds().removeFromTop (22),
                        juce::Justification::centred, false);
        }

        void resized() override
        {
            auto r = getLocalBounds().withTrimmedTop (24).reduced (6, 6);
            const int gap = 4;
            const int n = (int) strips.size();
            const int stripW = (r.getWidth() - (n - 1) * gap) / juce::jmax (1, n);
            for (auto& s : strips)
            {
                if (s != nullptr) s->setBounds (r.removeFromLeft (stripW));
                r.removeFromLeft (gap);
            }
        }

    private:
        void timerCallback() override
        {
            for (auto& s : strips) if (s != nullptr) s->repaint();
        }

        // Inner: one VCA mini-strip.
        class VcaStripView final : public juce::Component
        {
        public:
            VcaStripView (AudioEngine& eng, int idx) : engine (eng), index (idx)
            {
                fader.setSliderStyle (juce::Slider::LinearVertical);
                fader.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                fader.setRange (-60.0, 12.0, 0.1);
                fader.setValue (engine.getVca (index).gainDb.load (std::memory_order_relaxed),
                                juce::dontSendNotification);
                fader.setDoubleClickReturnValue (true, 0.0);
                fader.onValueChange = [this]
                {
                    engine.setVcaGainDb (index, (float) fader.getValue());
                };
                addAndMakeVisible (fader);

                mute.setButtonText ("M");
                mute.setColour (juce::ToggleButton::tickColourId, brand::signalMute());
                mute.onClick = [this]
                {
                    engine.setVcaMuted (index, mute.getToggleState());
                };
                addAndMakeVisible (mute);

                solo.setButtonText ("S");
                solo.setColour (juce::ToggleButton::tickColourId, brand::accentSolo);
                solo.onClick = [this]
                {
                    engine.setVcaSoloed (index, solo.getToggleState());
                };
                addAndMakeVisible (solo);

                nameLabel.setEditable (false, true, false);
                nameLabel.setJustificationType (juce::Justification::centred);
                nameLabel.setFont (brand::type::channelName());
                nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                nameLabel.setText (engine.getVca (index).name, juce::dontSendNotification);
                nameLabel.onTextChange = [this]
                {
                    engine.setVcaName (index, nameLabel.getText());
                };
                addAndMakeVisible (nameLabel);
            }

            void mouseEnter (const juce::MouseEvent&) override
            {
                if (! hovered) { hovered = true; repaint(); }
            }
            void mouseExit (const juce::MouseEvent& e) override
            {
                if (! getLocalBounds().contains (e.getEventRelativeTo (this).getPosition()))
                    if (hovered) { hovered = false; repaint(); }
            }

            void paint (juce::Graphics& g) override
            {
                // Background + colour swatch strip. The swatch is a
                // thicker (6 px) bar so it's an obvious click target
                // for the colour picker.
                auto bg = hovered ? brand::bgPanel.brighter (0.06f) : brand::bgPanel;
                g.setColour (bg);
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1), 4);
                const juce::uint32 c = engine.getVca (index).colourARGB.load (std::memory_order_relaxed);
                const auto swatch = c != 0 ? juce::Colour (c) : brand::stripColour (index);
                g.setColour (swatch);
                g.fillRect (getLocalBounds().withTop (0).withHeight (6));
                g.setColour (brand::edge);
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1), 4, 1);

                // Live dB readout.
                const float dB = engine.getVca (index).gainDb.load (std::memory_order_relaxed);
                g.setColour (brand::textSecondary);
                g.setFont (brand::type::uiBody());
                g.drawText (juce::String (dB, 1) + " dB",
                            getLocalBounds().withTop (getHeight() - 18).withHeight (16),
                            juce::Justification::centred, false);

                // Sync button visuals with the bus state (cue recall /
                // OSC may have flipped them under us).
                auto& v = engine.getVca (index);
                if (mute.getToggleState() != v.muted.load (std::memory_order_relaxed))
                    mute.setToggleState (v.muted.load (std::memory_order_relaxed),
                                         juce::dontSendNotification);
                if (solo.getToggleState() != v.soloed.load (std::memory_order_relaxed))
                    solo.setToggleState (v.soloed.load (std::memory_order_relaxed),
                                         juce::dontSendNotification);
                // Sync fader from ramps / cue recall.
                if (! fader.isMouseButtonDown())
                {
                    const float live = v.gainDb.load (std::memory_order_relaxed);
                    if (std::abs ((float) fader.getValue() - live) > 0.05f)
                        fader.setValue (live, juce::dontSendNotification);
                }
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (4, 6);
                r.removeFromTop (6);   // colour swatch (matches paint)
                nameLabel.setBounds (r.removeFromTop (20));
                auto buttonRow = r.removeFromTop (24);
                const int half = buttonRow.getWidth() / 2;
                mute.setBounds (buttonRow.removeFromLeft (half).reduced (2));
                solo.setBounds (buttonRow.reduced (2));
                r.removeFromTop (4);
                r.removeFromBottom (18);   // dB readout space
                fader.setBounds (r);
            }

            // Click in the swatch band → colour picker.
            // Right-click anywhere → full menu (rename / colour /
            // reset / list assigned strips).
            void mouseDown (const juce::MouseEvent& e) override
            {
                if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
                {
                    showContextMenu();
                    return;
                }
                // Top 6 px swatch band -- open the colour picker.
                if (e.y < 8)
                    openColourPicker();
            }

            void openColourPicker()
            {
                const juce::uint32 cur = engine.getVca (index).colourARGB.load (std::memory_order_relaxed);
                const auto current = cur != 0 ? juce::Colour (cur) : brand::stripColour (index);

                auto picker = std::make_unique<StripColourPicker> (current,
                    [this] (juce::Colour chosen)
                    {
                        engine.setVcaColour (index, chosen);
                        repaint();
                    });
                juce::CallOutBox::launchAsynchronously (std::move (picker),
                    getScreenBounds(), nullptr);
            }

            void showContextMenu()
            {
                juce::PopupMenu menu;
                menu.addItem (1, "Rename...");
                menu.addItem (2, "Change colour...");
                menu.addItem (3, "Reset colour to default");
                menu.addSeparator();
                // List every strip on this VCA bus, click to unassign.
                int assignedCount = 0;
                juce::PopupMenu assigned;
                for (int t = 0; t < engine.getRecorder().getNumTracks(); ++t)
                {
                    auto& ts = engine.getRecorder().getTrack (t);
                    if (ts.vcaGroup.load (std::memory_order_relaxed) == index)
                    {
                        const auto nm = ts.name.isEmpty()
                                          ? juce::String ("Strip ") + juce::String (t + 1)
                                          : ts.name;
                        assigned.addItem (100 + t, "Unassign " + nm);
                        ++assignedCount;
                    }
                }
                if (assignedCount > 0)
                    menu.addSubMenu ("Assigned (" + juce::String (assignedCount) + ")", assigned);
                else
                    menu.addItem (-1, "(no strips assigned)", false);

                juce::Component::SafePointer<VcaStripView> safe (this);
                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                                    [safe] (int chosen)
                {
                    if (safe == nullptr || chosen == 0) return;
                    if (chosen == 1) safe->nameLabel.showEditor();
                    else if (chosen == 2) safe->openColourPicker();
                    else if (chosen == 3)
                    {
                        safe->engine.setVcaColour (safe->index, juce::Colour ((juce::uint32) 0));
                        safe->repaint();
                    }
                    else if (chosen >= 100)
                    {
                        // Unassign a strip from this VCA.
                        safe->engine.setTrackVcaGroup (chosen - 100, -1);
                    }
                });
            }

        private:
            AudioEngine& engine;
            int          index;
            juce::Slider       fader;
            juce::ToggleButton mute;
            juce::ToggleButton solo;
            juce::Label        nameLabel;
            bool               hovered { false };
        };

        AudioEngine& engine;
        std::array<std::unique_ptr<VcaStripView>, AudioEngine::kNumVcas> strips;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VcaPanel)
    };
}
