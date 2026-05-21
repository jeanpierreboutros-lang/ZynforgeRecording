#include "PhaseMeter.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    PhaseMeter::PhaseMeter (AudioEngine& eng) : engine (eng)
    {
        title.setFont (juce::Font (juce::FontOptions().withHeight (10.0f).withStyle ("Bold")));
        title.setColour (juce::Label::textColourId, brand::textMuted);
        title.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (title);

        pairLabel.setFont (juce::Font (juce::FontOptions().withHeight (11.0f).withStyle ("Bold")));
        pairLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        pairLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (pairLabel);

        prevButton.onClick = [this] { cyclePair (-1); };
        addAndMakeVisible (prevButton);

        nextButton.onClick = [this] { cyclePair (+1); };
        addAndMakeVisible (nextButton);

        refreshLabel();
        startTimerHz (20);
    }

    PhaseMeter::~PhaseMeter() = default;

    void PhaseMeter::cyclePair (int delta)
    {
        const int maxCh = juce::jmax (2, engine.getRecorder().getNumTracks());
        int l = engine.getPhaseLeftChannel();
        int r = engine.getPhaseRightChannel();

        // Move the pair as a unit so they stay adjacent.
        l += delta;
        r += delta;
        if (l < 1)       { l = 1;       r = 2; }
        if (r > maxCh)   { r = maxCh;   l = maxCh - 1; }
        engine.setPhasePair (l, r);
        refreshLabel();
    }

    void PhaseMeter::refreshLabel()
    {
        pairLabel.setText (juce::String (engine.getPhaseLeftChannel())
                            + " / "
                            + juce::String (engine.getPhaseRightChannel()),
                           juce::dontSendNotification);
    }

    void PhaseMeter::timerCallback()
    {
        const float v = engine.getPhaseCorrelation();
        displayValue = displayValue * 0.6f + v * 0.4f;
        repaint();
    }

    void PhaseMeter::resized()
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (14);
        title.setBounds (top.removeFromLeft (60));
        prevButton.setBounds (top.removeFromLeft (22).reduced (1));
        pairLabel .setBounds (top.removeFromLeft (50));
        nextButton.setBounds (top.removeFromLeft (22).reduced (1));
        // remainder of r will host the bar
    }

    void PhaseMeter::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        r.removeFromTop (16.0f);
        r = r.reduced (2.0f);

        // background trough
        g.setColour (brand::bgDeep);
        g.fillRoundedRectangle (r, 2.0f);

        if (r.getWidth() < 4.0f || r.getHeight() < 3.0f) return;

        // tick marks at -1, 0, +1
        g.setColour (brand::edge);
        const float midX = r.getCentreX();
        g.drawVerticalLine ((int) midX, r.getY(), r.getBottom());

        // indicator
        const float clamped = juce::jlimit (-1.0f, 1.0f, displayValue);
        const float halfW = r.getWidth() * 0.5f;
        const float x = midX + clamped * halfW;

        juce::Colour c = clamped < -0.2f ? brand::meterRed
                       : clamped <  0.4f ? brand::meterAmber
                                         : brand::meterGreen;

        const float thickness = 4.0f;
        g.setColour (c);
        g.fillRect (juce::Rectangle<float> (x - thickness * 0.5f, r.getY(),
                                            thickness, r.getHeight()));

        // numeric value
        g.setColour (brand::textMuted);
        g.setFont (juce::FontOptions().withHeight (10.0f));
        g.drawText (juce::String (clamped, 2),
                    getLocalBounds().withTrimmedTop (1).withTrimmedRight (4),
                    juce::Justification::topRight, false);
    }
}
