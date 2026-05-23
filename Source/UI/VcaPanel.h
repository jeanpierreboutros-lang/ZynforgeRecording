#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <array>

namespace zynforge
{
    // Compact VCA fader panel — 8 mini-strips that each ride one VCA
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
            g.fillAll (brand::bgDeep);
            g.setColour (brand::edge);
            g.drawRect (getLocalBounds(), 1);
            g.setColour (brand::textTertiary);
            g.setFont (brand::type::label());
            g.drawText ("VCA", getLocalBounds().removeFromTop (16),
                        juce::Justification::centred, false);
        }

        void resized() override
        {
            auto r = getLocalBounds().withTrimmedTop (18).reduced (4, 4);
            const int gap = 2;
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
                nameLabel.setFont (brand::fonts::small());
                nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
                nameLabel.setText (engine.getVca (index).name, juce::dontSendNotification);
                nameLabel.onTextChange = [this]
                {
                    engine.setVcaName (index, nameLabel.getText());
                };
                addAndMakeVisible (nameLabel);
            }

            void paint (juce::Graphics& g) override
            {
                // Background + colour swatch strip.
                g.setColour (brand::bgPanel);
                g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1), 4);
                const juce::uint32 c = engine.getVca (index).colourARGB.load (std::memory_order_relaxed);
                const auto swatch = c != 0 ? juce::Colour (c) : brand::stripColour (index);
                g.setColour (swatch);
                g.fillRect (getLocalBounds().withTop (0).withHeight (3));
                g.setColour (brand::edge);
                g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1), 4, 1);

                // Live dB readout.
                const float dB = engine.getVca (index).gainDb.load (std::memory_order_relaxed);
                g.setColour (brand::textSecondary);
                g.setFont (brand::fonts::small());
                g.drawText (juce::String (dB, 1) + " dB",
                            getLocalBounds().withTop (getHeight() - 14).withHeight (12),
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
                auto r = getLocalBounds().reduced (3, 4);
                r.removeFromTop (3);   // colour swatch
                nameLabel.setBounds (r.removeFromTop (14));
                auto buttonRow = r.removeFromTop (16);
                const int half = buttonRow.getWidth() / 2;
                mute.setBounds (buttonRow.removeFromLeft (half).reduced (1));
                solo.setBounds (buttonRow.reduced (1));
                r.removeFromBottom (14);   // dB readout space
                fader.setBounds (r);
            }

        private:
            AudioEngine& engine;
            int          index;
            juce::Slider       fader;
            juce::ToggleButton mute;
            juce::ToggleButton solo;
            juce::Label        nameLabel;
        };

        AudioEngine& engine;
        std::array<std::unique_ptr<VcaStripView>, AudioEngine::kNumVcas> strips;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VcaPanel)
    };
}
