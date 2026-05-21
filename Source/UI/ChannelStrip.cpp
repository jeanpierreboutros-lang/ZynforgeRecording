#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    class ChannelStrip::StripTimer final : public juce::Timer
    {
    public:
        explicit StripTimer (ChannelStrip& s) : strip (s) { startTimerHz (10); }
        void timerCallback() override
        {
            const float peakLin = strip.state.peak.load (std::memory_order_relaxed);
            const float dB      = juce::Decibels::gainToDecibels (peakLin, -80.0f);
            strip.dbLabel.setText (dB <= -80.0f ? "-inf"
                                                 : juce::String (dB, 1) + " dB",
                                   juce::dontSendNotification);

            const int clips = strip.state.clipCount.load (std::memory_order_relaxed);
            if (clips > 0)
            {
                strip.clipLabel.setText ("CLIP x" + juce::String (clips),
                                         juce::dontSendNotification);
                strip.clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
            }
            else
            {
                strip.clipLabel.setText ("", juce::dontSendNotification);
            }
        }
    private:
        ChannelStrip& strip;
    };

    ChannelStrip::~ChannelStrip() = default;

    ChannelStrip::ChannelStrip (int index, TrackState& s)
        : stripIndex (index),
          state (s),
          personality (brand::stripColour (index)),
          spectrum (s),
          meter (s)
    {
        nameLabel.setText (s.name, juce::dontSendNotification);
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        nameLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold")));
        addAndMakeVisible (nameLabel);

        armButton.setToggleState (s.armed.load(), juce::dontSendNotification);
        armButton.onClick = [this]
        {
            state.armed.store (armButton.getToggleState(), std::memory_order_relaxed);
        };
        armButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        armButton.setColour (juce::ToggleButton::tickColourId, brand::accentRecord);
        addAndMakeVisible (armButton);

        monButton.setToggleState (s.monitor.load(), juce::dontSendNotification);
        monButton.onClick = [this]
        {
            state.monitor.store (monButton.getToggleState(), std::memory_order_relaxed);
        };
        monButton.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        monButton.setColour (juce::ToggleButton::tickColourId, brand::accentPlay);
        addAndMakeVisible (monButton);

        dbLabel.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
        dbLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        dbLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (dbLabel);

        clipLabel.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
        clipLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        clipLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (clipLabel);

        addAndMakeVisible (spectrum);
        addAndMakeVisible (meter);

        stripTimer = std::make_unique<StripTimer> (*this);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (brand::bgStrip);
        g.fillRoundedRectangle (r.reduced (2.0f), 6.0f);

        // top "personality" band
        auto top = r.reduced (2.0f).removeFromTop (4.0f);
        g.setColour (personality);
        g.fillRoundedRectangle (top, 2.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r.reduced (2.0f), 6.0f, 1.0f);
    }

    void ChannelStrip::resized()
    {
        auto r = getLocalBounds().reduced (6, 10);
        r.removeFromTop (4); // leave room for the personality band

        nameLabel.setBounds (r.removeFromTop (18));
        armButton.setBounds (r.removeFromTop (20).reduced (4, 1));
        monButton.setBounds (r.removeFromTop (20).reduced (4, 1));
        r.removeFromTop (4);
        spectrum .setBounds (r.removeFromTop (40));
        r.removeFromTop (2);
        dbLabel  .setBounds (r.removeFromTop (14));
        clipLabel.setBounds (r.removeFromTop (12));
        r.removeFromTop (2);
        meter.setBounds (r);
    }
}
