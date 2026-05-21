#include "ChannelStrip.h"
#include "../Theme/BrandColors.h"
#include "StripColourPicker.h"

namespace zynforge
{
    // Small clickable colour chip embedded at the top-left of every strip.
    class ChannelStrip::Swatch final : public juce::Component
    {
    public:
        std::function<void()> onClick;
        juce::Colour displayColour;

        void setDisplayColour (juce::Colour c) { displayColour = c; repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (1.5f);
            g.setColour (displayColour);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
        }

        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
    };

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

    juce::Colour ChannelStrip::getResolvedColour() const
    {
        const auto argb = state.colourARGB.load (std::memory_order_relaxed);
        if (argb != 0) return juce::Colour ((juce::uint32) argb);
        return personality;
    }

    void ChannelStrip::openColourPicker()
    {
        if (! colourCb) return;
        auto current = getResolvedColour();

        auto picker = std::make_unique<StripColourPicker> (
            current,
            [this] (juce::Colour chosen)
            {
                if (colourCb) colourCb (chosen);
                if (swatch != nullptr)
                {
                    swatch->setDisplayColour (getResolvedColour());
                }
                repaint();
            });

        auto screenBounds = swatch != nullptr ? swatch->getScreenBounds()
                                              : getScreenBounds();
        juce::CallOutBox::launchAsynchronously (std::move (picker), screenBounds, nullptr);
    }

    ChannelStrip::ChannelStrip (int index, TrackState& s, ColourCallback cb)
        : stripIndex (index),
          state (s),
          personality (brand::stripColour (index)),
          colourCb (std::move (cb)),
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

        swatch = std::make_unique<Swatch>();
        swatch->setDisplayColour (getResolvedColour());
        swatch->onClick = [this] { openColourPicker(); };
        addAndMakeVisible (*swatch);

        stripTimer = std::make_unique<StripTimer> (*this);
    }

    void ChannelStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto stripColour = getResolvedColour();

        g.setColour (stripColour);
        g.fillRoundedRectangle (r, 6.0f);

        g.setGradientFill (juce::ColourGradient (
            stripColour, r.getCentreX(), r.getY(),
            stripColour.darker (0.25f), r.getCentreX(), r.getBottom(),
            false));
        g.fillRoundedRectangle (r, 6.0f);

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, 6.0f, 1.0f);
    }

    void ChannelStrip::resized()
    {
        auto r = getLocalBounds().reduced (6, 10);

        auto nameRow = r.removeFromTop (18);
        if (swatch != nullptr)
            swatch->setBounds (nameRow.removeFromLeft (16).reduced (1, 2));
        nameLabel.setBounds (nameRow);
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
