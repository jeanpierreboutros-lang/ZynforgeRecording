#include "BigClockPanel.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    BigClockPanel::BigClockPanel()
    {
        // Opaque: sits over MainComponent's flat bgDeep body (paint() fills its
        // full bounds with bgDeep first), so the breathe-pulse repaint no longer
        // forces MainComponent to repaint the slice behind it.
        setOpaque (true);
    }

    void BigClockPanel::setMode (Mode m)
    {
        if (mode != m)
        {
            const bool igniting = (m == Mode::Recording && mode != Mode::Recording);
            mode = m;
            if (igniting) igniteLevel = 0.0f;   // start cold, ramp the heat in
            syncPulseTimer(); repaint();
        }
    }

    void BigClockPanel::setElapsed (juce::int64 samples, double sr)
    {
        if (sr <= 0) sr = 48000.0;
        const auto totalSec = (juce::int64) (samples / sr);
        const auto h = totalSec / 3600;
        const auto m = (totalSec / 60) % 60;
        const auto s = totalSec % 60;
        auto text = juce::String::formatted ("%02lld:%02lld:%02lld", h, m, s);
        if (text != elapsedText) { elapsedText = text; repaint(); }
    }

    void BigClockPanel::setMarkers (int count)
    {
        if (count != markerCount) { markerCount = count; repaint(); }
    }

    void BigClockPanel::setDiskInfo (double gb, int ms, juce::int64 mis, double remainingSec,
                                     DiskReading reading)
    {
        // Called at the MainComponent timer rate (24 Hz). Without
        // change detection, the unconditional repaint here drives
        // the BigClock to repaint 24 times a second even when the
        // shown numbers haven't moved a single digit. Round each
        // value to the visible precision so trivial float jitter
        // doesn't defeat the check.
        const double newFreeGB    = std::round (gb * 10.0) / 10.0;   // 1 dp
        const int    newRemainSec = (int) std::round (remainingSec);
        const bool changed = newFreeGB    != freeGB
                          || ms           != lastWriteMs
                          || mis          != missed
                          || newRemainSec != (int) std::round (remainingSeconds)
                          || reading      != diskReading;
        diskReading      = reading;
        freeGB           = newFreeGB;
        lastWriteMs      = ms;
        missed           = mis;
        remainingSeconds = remainingSec;
        if (changed) repaint();
    }

    void BigClockPanel::setLoudness (float integ, float momentary, float tp)
    {
        // 0.1-unit change detection so meter jitter doesn't force repaints.
        auto q = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
        const bool changed = q (integ)     != q (lufsIntegrated)
                          || q (momentary) != q (lufsMomentary)
                          || q (tp)        != q (truePeakDb);
        lufsIntegrated = integ;
        lufsMomentary  = momentary;
        truePeakDb     = tp;
        if (changed) repaint();
    }

    void BigClockPanel::setArmedReady (bool ready)
    {
        if (armedReady != ready) { armedReady = ready; repaint(); }
    }

    // Breathe animation tick rate. 15 Hz is plenty for a 1 Hz sine (still
    // visually smooth) and halves the idle repaint cost while a track is armed
    // — the pulse timer runs the whole time the rig sits armed-and-waiting.
    static constexpr int kPulseHz = 15;

    void BigClockPanel::syncPulseTimer()
    {
        const bool wantPulse = ! brand::reduceMotion() && mode == Mode::Recording;
        if (wantPulse)
        {
            if (! isTimerRunning())
                startTimerHz (kPulseHz);
        }
        else
        {
            stopTimer();
            pulsePhase = 0.0f;
        }
    }

    void BigClockPanel::timerCallback()
    {
        // 1 Hz breathe: a full sine cycle every second, so the phase step is
        // tied to the tick rate (2π / kPulseHz). Phase wraps at 2π so the
        // float stays bounded over a long recording session.
        constexpr float twoPi = juce::MathConstants<float>::twoPi;
        pulsePhase += twoPi / (float) kPulseHz;
        if (pulsePhase > twoPi) pulsePhase -= twoPi;
        // Heat ignite: ramp 0→1 over ~300 ms at record start (rate-independent).
        igniteLevel = juce::jlimit (0.0f, 1.0f,
                                    igniteLevel + (mode == Mode::Recording ? 1.0f : -1.0f)
                                                      / (0.3f * (float) kPulseHz));

        repaint();
    }

    static juce::String formatRemaining (double seconds)
    {
        if (seconds <= 0 || std::isinf (seconds)) return "--";
        const auto h = (int) (seconds / 3600.0);
        const auto m = ((int) (seconds / 60.0)) % 60;
        if (h > 99) return ">99h";
        return juce::String::formatted ("%dh%02dm", h, m);
    }

    void BigClockPanel::paint (juce::Graphics& g)
    {
        // Opaque base: cover every pixel with the body background (the 2px
        // margin around the plate showed bgDeep through it before). Required
        // for setOpaque(true) to be valid; visually identical.
        g.fillAll (brand::bgDeep);

        auto r = getLocalBounds().toFloat();

        // Background -- gradient fill in the state-tinted colour so the
        // hero panel reads as a glassy plate rather than a flat block.
        // While recording, the red tint breathes at 1 Hz so the engineer
        // can read 'rolling' from across the room without looking at
        // the timer value.
        juce::Colour bg = brand::bgPanel;
        const bool  rm     = brand::reduceMotion();
        const float pulse  = rm ? 0.5f : 0.5f + 0.5f * std::sin (pulsePhase);   // 0..1 (static under reduced-motion)
        const float ignite = rm ? 1.0f : igniteLevel;                          // full heat, no ramp
        if (mode == Mode::Recording)
            bg = brand::meterEmber.withAlpha ((0.14f + 0.10f * pulse) * ignite); // ember, breathing + igniting
        else if (mode == Mode::Playing) bg = brand::accentPlay.withAlpha (zynforge::brand::alpha::edgeSoft);
        auto bgRect = r.reduced (2.0f);
        g.setColour (bg);   // FLAT: solid plate (bg already breathes via pulse); no gradient
        g.fillRoundedRectangle (bgRect, brand::radius::lg);

        // Forge ember while recording -- a flat, breathing wash, not a glow gradient.
        if (mode == Mode::Recording)
        {
            g.setColour (brand::meterHot.withAlpha ((0.08f + 0.06f * pulse) * ignite));
            g.fillRoundedRectangle (bgRect, brand::radius::lg);
        }

        g.setColour (brand::edge);
        g.drawRoundedRectangle (r.reduced (2.0f), brand::radius::lg, 1.0f);

        // This row is short, so it is laid out for reading from the mix position:
        // state, time and record time left are large; the rest gives way first.
        auto inner = r.reduced ((float) brand::space::lg, 2.0f + (float) brand::space::xs);
        const bool tall = inner.getHeight() >= 48.0f;

        // Left: state + lamp
        auto left = inner.removeFromLeft (tall ? 128.0f : 112.0f);
        {
            // Armed and waiting is a lamp, not a frame: bright is for the take.
            juce::Colour lamp = armedReady ? brand::signalArmedReady() : brand::textMuted;
            juce::String label = armedReady ? "ARMED" : "IDLE";
            if (mode == Mode::Recording) { lamp = brand::accentRecord; label = "REC"; }
            else if (mode == Mode::Playing) { lamp = brand::accentPlay;  label = "PLAY"; }

            const float lampSize = 14.0f;
            g.setColour (lamp);
            g.fillEllipse (left.withWidth (lampSize).withSizeKeepingCentre (lampSize, lampSize));

            g.setColour (brand::textPrimary);
            g.setFont (brand::type::ui (brand::type::h_subhead, true));
            g.drawText (label, left.withTrimmedLeft (lampSize + (float) brand::space::md),
                        juce::Justification::centredLeft, false);
        }

        // Centre: the timer. Tabular numerals -- the string can't shift width
        // as seconds tick. It keeps its room; the health column gives way.
        const float timerPt = juce::jmin (inner.getHeight() * 0.95f, brand::type::h_hero + 4.0f);
        const auto  timerFont = brand::type::mono (timerPt, true);
        const float timerW = timerFont.getStringWidthFloat (elapsedText) + (float) brand::space::xl;

        // Right: record time left, then the disk-health rows if there is room.
        const float spare = inner.getWidth() - timerW;
        auto right = inner.removeFromRight (juce::jlimit (0.0f, 300.0f, spare));
        const bool primaryStale = diskReading == DiskReading::PrimaryStale;
        const bool copyStale    = diskReading == DiskReading::CopyStale;
        const auto valueCol     = primaryStale ? brand::alertAmber : brand::textPrimary;
        {
            // With a copy's drive not answering the time left is the primary's
            // alone, so the label says that instead of implying the whole rig.
            auto block = right.removeFromLeft (juce::jmin (124.0f, right.getWidth()));
            g.setColour (copyStale ? brand::alertAmber : brand::textSecondary);
            g.setFont (brand::type::caption());
            g.drawText (copyStale ? "PRIMARY TIME LEFT" : "RECORD TIME LEFT",
                        block.removeFromTop (14.0f), juce::Justification::centredLeft, false);
            g.setColour (valueCol);
            g.setFont (brand::type::mono (tall ? brand::type::h_subhead : brand::type::h_headline, true));
            g.drawText (primaryStale ? juce::String ("STALE") : formatRemaining (remainingSeconds),
                        block, juce::Justification::centredLeft, false);
        }

        // Each row is [label left, value right]. Label uses the UI font; value
        // uses tabular mono so the digits don't dance. A row that doesn't fit
        // isn't drawn.
        const float rowH = 13.0f;
        auto drawRow = [&] (const char* label, const juce::String& value,
                            juce::Colour labelCol = brand::textSecondary,
                            juce::Colour rowValueCol = brand::textPrimary)
        {
            if (right.getHeight() < rowH || right.getWidth() < 120.0f) return;
            auto line = right.removeFromTop (rowH);
            g.setColour (labelCol);
            g.setFont (brand::type::caption());
            g.drawText (label, line, juce::Justification::centredLeft, false);
            g.setColour (rowValueCol);
            g.setFont (zynforge::brand::type::monoStamp());
            g.drawText (value, line, juce::Justification::centredRight, false);
        };

        drawRow ("FREE",
                 primaryStale ? juce::String ("STALE")
                              : freeGB < 0.0 ? juce::String ("--")
                                             : juce::String::formatted ("%.1f GB", freeGB),
                 brand::textSecondary, valueCol);
        drawRow (missed > 0 ? "MISSED SAMPLES" : "NO MISSED WRITES",
                 missed > 0 ? juce::String (missed) : juce::String(),
                 missed > 0 ? brand::accentRecord : brand::textSecondary,
                 missed > 0 ? brand::accentRecord : brand::textPrimary);
        drawRow ("LAST WRITE", juce::String (lastWriteMs) + " ms");
        drawRow ("MARKERS", juce::String (markerCount));

        // Master loudness (BS.1770). Integrated program loudness + true-peak;
        // true-peak turns red as it approaches 0 dBTP.
        auto lufsStr = [] (float v)
        { return v <= -119.0f ? juce::String ("--") : juce::String (v, 1) + " LUFS"; };
        drawRow ("LOUDNESS (I)", lufsStr (lufsIntegrated));
        drawRow ("MOMENTARY",    lufsStr (lufsMomentary));
        const bool tpHot = truePeakDb > -1.0f;
        drawRow ("TRUE PEAK",
                 truePeakDb <= -119.0f ? juce::String ("--") : juce::String (truePeakDb, 1) + " dBTP",
                 tpHot ? brand::accentRecord : brand::textSecondary,
                 tpHot ? brand::accentRecord : brand::textPrimary);

        const auto timerCol = mode == Mode::Recording ? brand::accentRecord
                            : mode == Mode::Playing   ? brand::accentPlay
                                                      : brand::textSecondary;
        g.setFont (timerFont);
        g.setColour (mode == Mode::Recording ? brand::lift (timerCol, brand::tint::hover) : timerCol);
        g.drawText (elapsedText, inner, juce::Justification::centred, false);
    }
}
