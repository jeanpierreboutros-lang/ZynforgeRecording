#include "LedMeter.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace zynforge
{
    // Up to 20 discrete segments over -60..0 dBFS; each takes its colour from
    // brand::meterHeatAt (green to -12, ember to -6, hot to -2, white-hot).
    static constexpr int   kNumSegments    = 20;
    static constexpr float kMinDb          = brand::meterHeat::floorDb;
    static constexpr float kMaxDb          =   0.0f;
    static constexpr float kMinBarW        = 10.0f;   // a scale never squeezes the bar below this
    static constexpr int   kScaleGap       = 2;       // between the scale column and the bar

    LedMeter::LedMeter (TrackState& s) : state (s), keepAlive (s.weak_from_this().lock())
    {
        setTooltip ("Peak + RMS LED meter. Click to clear the clip indicator.");
        setScaleFont (brand::type::label());
        startTimerHz (30);
    }
    LedMeter::~LedMeter() = default;

    juce::String LedMeter::scaleLabel (float dB)
    {
        return juce::String (juce::roundToInt (dB));
    }

    int LedMeter::scaleColumnWidth (const juce::Font& f)
    {
        float widest = 0.0f;
        for (float dB : kScaleMarksDb)
            widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (f, scaleLabel (dB)));
        return (int) std::ceil (widest) + 1 + kScaleGap;
    }

    void LedMeter::setScaleFont (const juce::Font& f)
    {
        if (scaleColW > 0 && f == scaleFont) return;   // a strip re-sets it on every resize
        scaleFont = f;
        scaleColW = scaleColumnWidth (f);
        repaint();
    }

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
        if (detached || ! isEnabled()) return;   // condemned (state is freed), or LOCK
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
        // meter is simply too narrow to fit the label column without starving
        // the bar (the compact mixer strips), drop the labels and give the bar
        // the full width. This also prevents a NEGATIVE-width barArea, which
        // asserted/crashed in paintBar. The host's rung first, then label().
        static const int smallColW = scaleColumnWidth (brand::type::label());
        juce::Font font = scaleFont;
        int colW = scaleColW;
        if (bounds.getWidth() < (float) colW + kMinBarW)
        {
            font = brand::type::label();
            colW = smallColW;
        }
        const bool  roomForLabels = showLabels && bounds.getWidth() >= (float) colW + kMinBarW;
        const float labelW = roomForLabels ? (float) (colW - kScaleGap) : 0.0f;
        const float gap    = roomForLabels ? (float) kScaleGap : 0.0f;
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

        // dBFS scale -- labels live in their own column, ticks bridge the gap.
        // These are full-scale marks, not the fader's: the top of the meter is
        // clipping. With no room for numbers the same marks stay as ticks on
        // the bar, unless the meter is too short for them to mean anything.
        if (! roomForLabels && bounds.getHeight() >= 160.0f)
        {
            g.setColour (brand::textSecondary);
            for (float dB : kScaleMarksDb)
                g.drawHorizontalLine ((int) (bounds.getBottom() - bounds.getHeight() * (dB - kMinDb) / (kMaxDb - kMinDb)),
                                      barArea.getX(), barArea.getX() + 3.0f);
        }
        g.setFont (font);
        const float boxH = std::ceil (font.getHeight()) + 1.0f;
        if (roomForLabels) for (float dB : kScaleMarksDb)
        {
            const float frac = (dB - kMinDb) / (kMaxDb - kMinDb);
            const float y    = bounds.getBottom() - bounds.getHeight() * frac;

            // Tick -- short line bridging the label column and the bar.
            g.setColour (brand::textTertiary);
            g.drawHorizontalLine ((int) y, labelCol.getRight() - 2.0f, labelCol.getRight() + 1.0f);

            // Label -- signed, right-aligned inside the dedicated label column.
            // Clamp the label box to the widget bounds so the extreme labels
            // (0 dB at top, floor at bottom) don't get clipped off-screen.
            const float labelY = juce::jlimit (0.0f,
                                               (float) getHeight() - boxH,
                                               y - boxH * 0.5f);
            g.setColour (brand::textPrimary);
            g.drawText (scaleLabel (dB),
                        labelCol.withY (labelY).withHeight (boxH).toNearestInt(),
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
