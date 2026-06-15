#include "TempoBar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    TempoBar::TempoBar()
    {
        titleLabel.setText ("TEMPO", juce::dontSendNotification);
        titleLabel.setFont (brand::type::uiLabel());
        titleLabel.setColour (juce::Label::textColourId, brand::textMuted);
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (titleLabel);

        valueLabel.setText ("120.0 BPM", juce::dontSendNotification);
        // Tabular mono so BPM doesn't shift width when the engineer
        // taps a new tempo.
        valueLabel.setFont (brand::type::mono (16.0f, true));
        valueLabel.setColour (juce::Label::textColourId, brand::accentStatus);
        valueLabel.setJustificationType (juce::Justification::centred);
        valueLabel.setEditable (false, true, false);     // double-click to edit
        valueLabel.onTextChange = [this]
        {
            auto raw = valueLabel.getText()
                                  .retainCharacters ("0123456789.")
                                  .getFloatValue();
            if (raw > 0.0f && onBpmChanged) onBpmChanged (raw);
            // Re-render the canonical formatted value (caller will call
            // setBpm with the clamped value).
            setBpm (currentBpm);
        };
        valueLabel.setTooltip ("Session tempo in BPM -- double-click to type a value");
        addAndMakeVisible (valueLabel);

        auto styleNudge = [] (juce::TextButton& b)
        {
            b.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
            b.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        };
        styleNudge (minusButton);
        styleNudge (plusButton);
        minusButton.setTooltip ("Lower tempo by 1 BPM (Shift = 0.1)");
        plusButton .setTooltip ("Raise tempo by 1 BPM (Shift = 0.1)");
        minusButton.onClick = [this]
        {
            const float step = juce::ModifierKeys::currentModifiers.isShiftDown() ? -0.1f : -1.0f;
            nudge (step);
        };
        plusButton.onClick = [this]
        {
            const float step = juce::ModifierKeys::currentModifiers.isShiftDown() ? 0.1f : 1.0f;
            nudge (step);
        };
        addAndMakeVisible (minusButton);
        addAndMakeVisible (plusButton);

        tapButton.setButtonText ("TAP");
        tapButton.setColour (juce::TextButton::buttonColourId,  brand::accentStatus);
        tapButton.setColour (juce::TextButton::textColourOffId, brand::onSignal (brand::accentStatus));
        tapButton.setTooltip ("Tap repeatedly to set tempo (2+ taps, < 2 s apart)");
        tapButton.onClick = [this] { doTap(); };
        addAndMakeVisible (tapButton);

        // CLICK = render a metronome track at the current tempo into
        // the session's Audio Files/ folder. Following calls re-render
        // the same file in place so a tempo change keeps the click in
        // sync without piling up tracks.
        // Click track button uses accentVS (warm VSC amber) instead
        // of brandOrange -- see brandOrange consolidation: the orange
        // token is now reserved for signalMute() + brand assertion.
        clickButton.setColour (juce::TextButton::buttonColourId,  brand::accentVS);
        clickButton.setColour (juce::TextButton::textColourOffId, brand::onSignal (brand::accentVS));
        clickButton.setTooltip ("Generate / regenerate a metronome track at the current tempo. "
                                "Re-pressing after a tempo change updates it in place.");
        clickButton.onClick = [this] { if (onCreateClickTrack) onCreateClickTrack(); };
        addAndMakeVisible (clickButton);

        startTimerHz (30);
    }

    TempoBar::~TempoBar() = default;

    void TempoBar::setBpm (float bpm)
    {
        currentBpm = juce::jlimit (20.0f, 999.0f, bpm);
        valueLabel.setText (juce::String (currentBpm, 1) + " BPM",
                            juce::dontSendNotification);
        repaint();
    }

    void TempoBar::nudge (float delta)
    {
        const auto next = currentBpm + delta;
        if (onBpmChanged) onBpmChanged (next);
        setBpm (next);
    }

    void TempoBar::doTap()
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        // Drop the oldest tap if older than 2 s -- engineer is starting
        // a fresh count rather than tapping continuously.
        if (tapCount > 0 && now - taps[(tapCount - 1) % kMaxTaps] > 2000.0)
            tapCount = 0;

        taps[tapCount % kMaxTaps] = now;
        ++tapCount;

        if (tapCount < 2) return;

        const int sampleCount = juce::jmin (tapCount, kMaxTaps);
        double total = 0.0;
        int intervals = 0;
        for (int i = 1; i < sampleCount; ++i)
        {
            const int a = (tapCount - sampleCount + i - 1 + kMaxTaps * 2) % kMaxTaps;
            const int b = (tapCount - sampleCount + i     + kMaxTaps * 2) % kMaxTaps;
            const double dt = taps[b] - taps[a];
            if (dt > 50.0 && dt < 2000.0)   // 30-1200 BPM range
            {
                total += dt;
                ++intervals;
            }
        }
        if (intervals == 0) return;
        const double avgMs = total / intervals;
        const float bpm = (float) (60000.0 / avgMs);
        if (onBpmChanged) onBpmChanged (bpm);
        setBpm (bpm);

        // Light the LED on every tap so the engineer can confirm the
        // tap landed even before the third tap kicks the average.
        beatOn = true;
        beatPhase = 0.0;
        repaint();
    }

    void TempoBar::timerCallback()
    {
        // Visual click LED -- pulses at the current tempo. Pure cosmetic.
        const double tickMs = 60000.0 / juce::jmax (20.0f, currentBpm);
        beatPhase += 1000.0 / 30.0;
        if (beatPhase >= tickMs)
        {
            beatPhase = 0.0;
            beatOn = true;
            repaint();
        }
        else if (beatOn && beatPhase > juce::jmin (80.0, tickMs * 0.25))
        {
            beatOn = false;
            repaint();
        }
    }

    void TempoBar::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.10f));
        g.fillRoundedRectangle (r, brand::radius::md);
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
        // Heated Steel: hairline orange seam on the left edge ties the bar in.
        g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::scrim));
        g.fillRect (r.getX(), r.getY() + 3.0f, 2.0f, r.getHeight() - 6.0f);

        // Beat LED on the right edge.
        const float ledD = 10.0f;
        const auto  ledR = juce::Rectangle<float> (
            r.getRight() - ledD - 8.0f,
            r.getCentreY() - ledD * 0.5f,
            ledD, ledD);
        g.setColour (beatOn ? brand::accentStatus
                            : brand::lift (brand::bgDeep, 0.10f));
        g.fillEllipse (ledR);
        g.setColour (brand::edge);
        g.drawEllipse (ledR, 1.0f);
    }

    void TempoBar::resized()
    {
        auto r = getLocalBounds().reduced (brand::space::md, brand::space::xs);

        // Lay the title on the left, then anchor the buttons from the RIGHT
        // so each gets its full width and its label never truncates (the old
        // left-to-right flow overflowed the bar and squeezed CLICK down to
        // "C..."). The BPM value label absorbs any leftover middle space.
        titleLabel.setBounds (r.removeFromLeft (52));
        r.removeFromLeft (brand::space::xs);

        clickButton.setBounds (r.removeFromRight (74).reduced (0, 2));
        r.removeFromRight (brand::space::xs);
        tapButton  .setBounds (r.removeFromRight (52).reduced (0, 2));
        r.removeFromRight (brand::space::sm);
        plusButton .setBounds (r.removeFromRight (24).reduced (0, 2));
        r.removeFromRight (2);
        minusButton.setBounds (r.removeFromRight (24).reduced (0, 2));
        r.removeFromRight (brand::space::xs);

        valueLabel.setBounds (r.reduced (0, 2));   // remaining middle
        // The right-edge LED is painted in paint(); leave breathing room.
    }
}
