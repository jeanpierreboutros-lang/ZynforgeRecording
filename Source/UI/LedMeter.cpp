#include "LedMeter.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace zynforge
{
    // Spec: 20 discrete segments, 70 / 15 / 15 split (green / amber / red).
    static constexpr int   kNumSegments    = 20;
    static constexpr float kMinDb          = -60.0f;
    static constexpr float kMaxDb          =   0.0f;
    static constexpr int   kRedSegments    = 3;   // top 15%
    static constexpr int   kAmberSegments  = 3;   // next 15%

    LedMeter::LedMeter (TrackState& s) : state (s)
    {
        setTooltip ("Peak + RMS LED meter. Click to clear the clip indicator.");
        startTimerHz (30);
    }
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

        if (stereoR != nullptr)
        {
            const float pR = stereoR->peak.load (std::memory_order_relaxed);
            const float rR = stereoR->rms .load (std::memory_order_relaxed);
            const bool  cR = stereoR->clipped.load (std::memory_order_relaxed);
            displayPeakR = juce::jmax (pR, displayPeakR * 0.85f);
            displayRmsR  = juce::jmax (rR, displayRmsR  * 0.7f);
            if (cR) showClip = true;
        }
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

    void LedMeter::paintBar (juce::Graphics& g, juce::Rectangle<float> r,
                             float displayPeakLocal, float displayRmsLocal) const
    {
        g.setColour (brand::bgDeep);
        g.fillRoundedRectangle (r, 3.0f);

        const float litPeak = linearToNormalisedDb (displayPeakLocal);
        const float litRms  = linearToNormalisedDb (displayRmsLocal);

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
    }

    void LedMeter::paint (juce::Graphics& g)
    {
        // Inset the bar area vertically so the extreme labels (0 dB at the
        // top, the floor at the bottom) have room to render without being
        // clipped at the widget edges.
        const int   vPad  = 7;
        auto bounds = getLocalBounds().toFloat().reduced (2.0f, (float) vPad);
        const bool  stereo = (stereoR != nullptr);

        // Reserve a dedicated left-hand gutter for the dB labels so they
        // never overlap the meter segments. The bar(s) get the right side.
        // When the host turned labels off (narrow meter — EDIT view),
        // the bar gets the full width.
        const float labelW = showLabels ? 14.0f : 0.0f;
        const float gap    = showLabels ? 2.0f  : 0.0f;
        auto labelCol = bounds.withWidth (labelW);
        auto barArea  = bounds.withTrimmedLeft (labelW + gap);

        if (stereo)
        {
            const float half = barArea.getWidth() * 0.5f;
            auto leftBar  = barArea.withWidth (half - 1.0f);
            auto rightBar = barArea.withTrimmedLeft (half + 1.0f);
            paintBar (g, leftBar,  displayPeak,  displayRms);
            paintBar (g, rightBar, displayPeakR, displayRmsR);
        }
        else
        {
            paintBar (g, barArea, displayPeak, displayRms);
        }

        // dB scale — labels live in their own column, ticks bridge the gap.
        const float dBLabels[] = { 0.0f, -3.0f, -6.0f, -10.0f, -16.0f, -22.0f, -32.0f, -60.0f };
        g.setFont (brand::type::label());
        for (float dB : dBLabels)
        {
            const float frac = (dB - kMinDb) / (kMaxDb - kMinDb);
            const float y    = bounds.getBottom() - bounds.getHeight() * frac;

            // Tick — short line bridging the label column and the bar.
            g.setColour (brand::textTertiary);
            g.drawHorizontalLine ((int) y, labelCol.getRight() - 2.0f, labelCol.getRight() + 1.0f);

            // Label — right-aligned inside the dedicated label column.
            // Clamp the label box to the widget bounds so the extreme labels
            // (0 dB at top, floor at bottom) don't get clipped off-screen.
            const float labelY = juce::jlimit (0.0f,
                                               (float) getHeight() - 10.0f,
                                               y - 5.0f);
            g.setColour (brand::textPrimary);
            g.drawText (juce::String (std::abs ((int) dB)),
                        labelCol.withY (labelY).withHeight (10.0f).toNearestInt(),
                        juce::Justification::centredRight, false);
        }

        // Clip pip — only over the bar area, not the label column.
        if (showClip)
        {
            g.setColour (brand::meterRed);
            g.fillRoundedRectangle (barArea.removeFromTop (5.0f).reduced (1.0f, 0.0f), 1.5f);
        }
    }
}
