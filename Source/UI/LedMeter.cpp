#include "LedMeter.h"
#include "../Theme/BrandColors.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace zynforge
{
    static constexpr int   kNumSegments    = 32;
    static constexpr float kMinDb          = -60.0f;
    static constexpr float kMaxDb          =   0.0f;
    static constexpr int   kRedSegments    = 3;   // top N segments are red
    static constexpr int   kAmberSegments  = 6;   // next N segments are amber

    LedMeter::LedMeter (TrackState& s) : state (s) { startTimerHz (30); }
    LedMeter::~LedMeter() = default;

    void LedMeter::timerCallback()
    {
        const float peak = state.peak.load (std::memory_order_relaxed);
        const float rms  = state.rms .load (std::memory_order_relaxed);
        const bool  clip = state.clipped.load (std::memory_order_relaxed);

        // smooth display values
        displayPeak = juce::jmax (peak, displayPeak * 0.85f);
        displayRms  = juce::jmax (rms,  displayRms  * 0.7f);
        if (clip) showClip = true;
        repaint();
    }

    void LedMeter::mouseDown (const juce::MouseEvent&)
    {
        showClip = false;
        state.clipped  .store (false, std::memory_order_relaxed);
        state.clipCount.store (0,     std::memory_order_relaxed);
        repaint();
    }

    static float linearToNormalisedDb (float lin)
    {
        if (lin <= 0.0f) return 0.0f;
        const float db = juce::Decibels::gainToDecibels (lin, kMinDb);
        return juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));
    }

    void LedMeter::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);

        g.setColour (brand::bgDeep);
        g.fillRoundedRectangle (r, 3.0f);

        const float litPeak = linearToNormalisedDb (displayPeak);
        const float litRms  = linearToNormalisedDb (displayRms);

        const float segH = r.getHeight() / (float) kNumSegments;
        const float gap  = juce::jmax (1.0f, segH * 0.15f);

        for (int i = 0; i < kNumSegments; ++i)
        {
            const float yTop = r.getBottom() - segH * (float) (i + 1);
            juce::Rectangle<float> seg (r.getX() + 1.0f, yTop + gap * 0.5f,
                                        r.getWidth() - 2.0f, segH - gap);

            const float frac = (float) (i + 1) / (float) kNumSegments;
            const bool  litByPeak = frac <= litPeak;
            const bool  litByRms  = frac <= litRms;

            juce::Colour base;
            if (i >= kNumSegments - kRedSegments)        base = brand::meterRed;
            else if (i >= kNumSegments - kRedSegments - kAmberSegments)
                                                          base = brand::meterAmber;
            else                                          base = brand::meterGreen;

            if (litByRms)        g.setColour (base);
            else if (litByPeak)  g.setColour (base.withAlpha (0.45f));
            else                 g.setColour (brand::meterIdle);
            g.fillRoundedRectangle (seg, 1.5f);
        }

        // Side tick marks at standard dB values, on the right edge.
        const float dBTicks[] = { -3.0f, -6.0f, -12.0f, -20.0f, -40.0f };
        g.setColour (brand::textMuted);
        for (float dB : dBTicks)
        {
            const float frac = (dB - kMinDb) / (kMaxDb - kMinDb);
            const float y = r.getBottom() - r.getHeight() * frac;
            g.drawHorizontalLine ((int) y, r.getRight() - 4.0f, r.getRight());
        }

        // Clip pip
        if (showClip)
        {
            g.setColour (brand::meterRed);
            g.fillRoundedRectangle (r.removeFromTop (5.0f).reduced (2.0f, 0.0f), 1.5f);
        }
    }
}
