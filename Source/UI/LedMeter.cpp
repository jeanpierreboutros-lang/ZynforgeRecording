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
        if (detached) return;   // owning strip condemned -- state is freed
        // Throttle to ~8 Hz when the transport is stopped and this strip
        // isn't being actively watched (armed / monitored) -- the meter still
        // moves so signal-present is visible, but at a fraction of the idle
        // cost across 32 strips. Snaps back to 30 Hz on record/play/arm.
        {
            const bool active = gTransportActive.load (std::memory_order_relaxed)
                             || state.armed  .load (std::memory_order_relaxed)
                             || state.monitor.load (std::memory_order_relaxed);
            const int wantHz = active ? 60 : 8;   // gig-one field report: meters felt laggy live
            if (getTimerInterval() != 1000 / wantHz)
                startTimerHz (wantHz);
        }

        const float peak = state.peak.load (std::memory_order_relaxed);
        const float rms  = state.rms .load (std::memory_order_relaxed);
        const bool  clip = state.clipped.load (std::memory_order_relaxed);

        const float prevP = displayPeak, prevR = displayRms;
        // smooth display values
        displayPeak = juce::jmax (peak, displayPeak * 0.80f);   // faster fall (field report)
        displayRms  = juce::jmax (rms,  displayRms  * 0.62f);
        const bool prevClip = showClip;
        if (clip) showClip = true;

        float prevPR = displayPeakR, prevRR = displayRmsR;
        if (stereoR != nullptr)
        {
            const float pR = stereoR->peak.load (std::memory_order_relaxed);
            const float rR = stereoR->rms .load (std::memory_order_relaxed);
            const bool  cR = stereoR->clipped.load (std::memory_order_relaxed);
            displayPeakR = juce::jmax (pR, displayPeakR * 0.80f);
            displayRmsR  = juce::jmax (rR, displayRmsR  * 0.62f);
            if (cR) showClip = true;
        }

        // Skip the repaint when nothing visibly changed. The decay
        // step would still nudge the displayed value by a fraction of
        // a dB, but if it's smaller than ~0.4 dB the segmented LED
        // ladder doesn't show a new step, so the repaint is wasted.
        // Idle mixers spend most of their life here: peak == 0 and
        // displayPeak == 0, so the early-out saves N strips * 30 Hz
        // of paint churn.
        constexpr float kRedrawEps = 0.004f;   // ~0.4 dB on the segment scale
        const bool moved = std::abs (displayPeak  - prevP)  > kRedrawEps
                        || std::abs (displayRms   - prevR)  > kRedrawEps
                        || std::abs (displayPeakR - prevPR) > kRedrawEps
                        || std::abs (displayRmsR  - prevRR) > kRedrawEps
                        || showClip != prevClip;
        if (moved)
            repaint();
    }

    void LedMeter::mouseDown (const juce::MouseEvent&)
    {
        if (detached) return;   // owning strip condemned -- state is freed
        showClip = false;
        state.clipped  .store (false, std::memory_order_relaxed);
        state.clipCount.store (0,     std::memory_order_relaxed);
        // Clear the stereo partner too, else the next timer tick re-latches
        // showClip from stereoR->clipped and the clip pip can never be cleared.
        if (stereoR != nullptr)
        {
            stereoR->clipped  .store (false, std::memory_order_relaxed);
            stereoR->clipCount.store (0,     std::memory_order_relaxed);
        }
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
        g.fillRoundedRectangle (r, brand::radius::sm);

        const float litPeak = linearToNormalisedDb (displayPeakLocal);
        const float litRms  = linearToNormalisedDb (displayRmsLocal);

        // Pick a colour for any height-fraction (0..1) along the meter -- the
        // forge-heat ramp (green safe zone -> ember -> forge-orange -> white-hot).
        auto colourAt = [] (float frac) { return brand::meterHeatAt (frac); };

        // ── Adaptive rendering ──────────────────────────────────────────
        // The segmented LED ladder is the brand look -- match it
        // everywhere (mixer + EDIT rows). Only the very-tiny case
        // (under ~24 px tall, e.g. micro-row meters in EDIT) falls back
        // to the smooth gradient, because 8 LED segments inside 24 px
        // become indistinct stripes. The segmented path itself adapts
        // its segment count from 8 (at 32 px) up to the full 20 (at
        // 80+ px), so EDIT rows at 'small' / 'medium' size now show
        // the same LED ladder as the mixer strip meter.
        const float h = r.getHeight();

        if (h < 24.0f)
        {
            // FLAT: solid forge-heat colour picked by LEVEL (no gradient) -- the
            // fill is green in the safe zone and climbs to forge-orange / white-hot
            // as the level rises, but each region is a solid colour.
            // Background -- idle bar.
            g.setColour (brand::meterIdle);
            g.fillRoundedRectangle (r.reduced (1.0f), brand::radius::sm);

            // RMS region: solid, peak above it: dimmed.
            const float rmsH  = h * litRms;
            const float peakH = h * litPeak;
            if (peakH > rmsH)
            {
                auto peakRect = juce::Rectangle<float> (
                    r.getX(), r.getBottom() - peakH, r.getWidth(), peakH - rmsH);
                g.setColour (colourAt (litPeak));
                g.fillRect (peakRect.reduced (1.0f, 0.0f));
                g.setColour (brand::shadow::elev3());
                g.fillRect (peakRect.reduced (1.0f, 0.0f));
            }
            if (rmsH > 0.0f)
            {
                auto rmsRect = juce::Rectangle<float> (
                    r.getX(), r.getBottom() - rmsH, r.getWidth(), rmsH);
                g.setColour (colourAt (litRms));
                g.fillRect (rmsRect.reduced (1.0f, 0.0f));
            }
            return;
        }

        // ── Discrete LED segments ──────────────────────────────────────
        // Scale segment count so each segment is at least ~3 px tall.
        const int nSegments = juce::jlimit (8, kNumSegments,
                                            (int) std::round (h / 4.0f));
        const float segH = h / (float) nSegments;
        const float gap  = juce::jmax (1.0f, segH * 0.15f);

        for (int i = 0; i < nSegments; ++i)
        {
            const float yTop = r.getBottom() - segH * (float) (i + 1);
            juce::Rectangle<float> seg (r.getX() + 1.0f, yTop + gap * 0.5f,
                                        r.getWidth() - 2.0f, segH - gap);

            const float frac = (float) (i + 1) / (float) nSegments;
            const bool  litByPeak = frac <= litPeak;
            const bool  litByRms  = frac <= litRms;

            // Forge-heat ramp by segment height: green safe zone, then ember /
            // forge-orange / white-hot as it climbs.
            const auto base = brand::meterHeatAt (frac);

            if (litByRms)        g.setColour (base);
            else if (litByPeak)  g.setColour (base.withAlpha (zynforge::brand::alpha::scrim));
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
        // When the host turned labels off (narrow meter -- EDIT view), OR the
        // meter is simply too narrow to fit the 14px label column without
        // starving the bar (the compact mixer strips), drop the labels and
        // give the bar the full width. This also prevents a NEGATIVE-width
        // barArea, which asserted/crashed in paintBar.
        const bool  roomForLabels = showLabels && bounds.getWidth() > 30.0f;
        const float labelW = roomForLabels ? 14.0f : 0.0f;
        const float gap    = roomForLabels ? 2.0f  : 0.0f;
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

        // dB scale -- labels live in their own column, ticks bridge the gap.
        // Skipped entirely when there's no room (compact strips / EDIT meter).
        const float dBLabels[] = { 0.0f, -3.0f, -6.0f, -10.0f, -16.0f, -22.0f, -32.0f, -60.0f };
        g.setFont (brand::type::label());
        if (roomForLabels) for (float dB : dBLabels)
        {
            const float frac = (dB - kMinDb) / (kMaxDb - kMinDb);
            const float y    = bounds.getBottom() - bounds.getHeight() * frac;

            // Tick -- short line bridging the label column and the bar.
            g.setColour (brand::textTertiary);
            g.drawHorizontalLine ((int) y, labelCol.getRight() - 2.0f, labelCol.getRight() + 1.0f);

            // Label -- right-aligned inside the dedicated label column.
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

        // Clip pip -- white-hot, like metal that's been pushed past temper.
        if (showClip)
        {
            g.setColour (brand::meterWhiteHot);
            g.fillRoundedRectangle (barArea.removeFromTop (5.0f).reduced (1.0f, 0.0f), 1.5f);
        }
    }
}
