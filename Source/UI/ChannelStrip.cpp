#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    ChannelStrip::ChannelStrip (int index, TrackState& s)
        : stripIndex (index),
          state (s),
          personality (brand::stripColour (index)),
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

        addAndMakeVisible (meter);
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

        nameLabel.setBounds (r.removeFromTop (20));
        armButton.setBounds (r.removeFromTop (24).reduced (4, 2));
        r.removeFromTop (6);
        meter.setBounds (r);
    }
}
