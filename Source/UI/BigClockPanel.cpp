#include "BigClockPanel.h"
#include "../Audio/DisplayText.h"
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
        // Called at the MainComponent timer rate (10 Hz). Without
        // change detection, the unconditional repaint here drives
        // the BigClock to repaint 10 times a second even when the
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

    void BigClockPanel::setArmedReady (bool ready)
    {
        if (armedReady != ready) { armedReady = ready; repaint(); }
    }

    void BigClockPanel::setEditLocked (bool locked)
    {
        if (editLocked != locked) { editLocked = locked; repaint(); }
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

    // ── Plate layout ─────────────────────────────────────────────────────
    // The plate is short, so it is laid out for reading from the mix position:
    // state, time and the time left are large; the small groups give way first.
    static constexpr float kLabelH    = 14.0f;    // a group's label line
    static constexpr float kLockLineH = 14.0f;    // EDIT LOCKED under the state
    static const char* const kEditLockedText = "EDIT LOCKED";

    static juce::Font timerFont (float innerH)
    {
        return brand::type::mono (juce::jmin (innerH * 0.95f, brand::type::h_hero), true);
    }

    static juce::Font valueFont (BigClockPanel::Group g, bool tall)
    {
        if (g == BigClockPanel::Group::TimeLeft)
            return brand::type::mono (tall ? brand::type::h_subhead : brand::type::h_headline, true);
        return brand::type::mono (brand::type::h_body, true);
    }

    static float valueH (BigClockPanel::Group g, bool tall)
    {
        if (g == BigClockPanel::Group::TimeLeft) return tall ? 26.0f : 21.0f;
        return 16.0f;
    }

    // The widest each value usually reads, so a ticking digit doesn't push the
    // groups along. A longer value still gets its room.
    static const char* valueTemplate (BigClockPanel::Group g)
    {
        switch (g)
        {
            case BigClockPanel::Group::Missed:    return "000";
            case BigClockPanel::Group::LastWrite: return "000 ms";
            case BigClockPanel::Group::Markers:   return "000";
            case BigClockPanel::Group::Free:      return "000.0 GB";
            case BigClockPanel::Group::TimeLeft:  return "00h00m";
        }
        return "";
    }

    float BigClockPanel::groupsWidth (const GroupWidths& widths, const GroupMask& shown, float gap) noexcept
    {
        float w = 0.0f;
        int n = 0;
        for (int i = 0; i < kNumGroups; ++i)
            if (shown[(size_t) i] && widths[(size_t) i] > 0.0f) { w += widths[(size_t) i]; ++n; }
        return w + gap * (float) juce::jmax (0, n - 1);
    }

    BigClockPanel::GroupMask BigClockPanel::fitGroups (const GroupWidths& widths, float room, float gap) noexcept
    {
        GroupMask shown {};
        for (int i = 0; i < kNumGroups; ++i) shown[(size_t) i] = widths[(size_t) i] > 0.0f;
        for (auto g : giveWayOrder())
        {
            if (groupsWidth (widths, shown, gap) <= room) break;
            shown[(size_t) g] = false;
        }
        return shown;
    }

    juce::String BigClockPanel::missedValueText (juce::int64 missedSamples)
    {
        return missedSamples > 0 ? juce::String (missedSamples) + "!" : juce::String ("0");
    }

    // With a copy's drive not answering the time left is the primary's alone,
    // so the label says that instead of implying the whole rig.
    juce::String BigClockPanel::timeLeftLabelText (bool freeIsBigSlot, bool copyStale)
    {
        return freeIsBigSlot ? "DISK FREE" : copyStale ? "PRIMARY TIME LEFT" : "TIME LEFT";
    }

    // With no estimate yet (nothing rolling) the large slot holds what is
    // known, free space, rather than a dash.
    bool BigClockPanel::freeIsBig() const noexcept
    {
        return remainingSeconds <= 0.0 || std::isinf (remainingSeconds);
    }

    juce::String BigClockPanel::freeText() const
    {
        return diskReading == DiskReading::PrimaryStale ? juce::String ("STALE")
             : diskReading == DiskReading::DriveMissing ? juce::String ("NO DRIVE")
             : freeGB < 0.0                             ? juce::String ("--")
                                                        : juce::String::formatted ("%.1f GB", freeGB);
    }

    BigClockPanel::Layout BigClockPanel::computeLayout() const
    {
        Layout L;
        auto r = getLocalBounds().toFloat();
        auto inner = r.reduced ((float) brand::space::lg, 2.0f + (float) brand::space::xs);
        L.tall = inner.getHeight() >= 48.0f;
        L.left = inner.removeFromLeft (L.tall ? 128.0f : 112.0f);

        // What each group reads.
        const bool bigFree = freeIsBig();
        const auto free = freeText();
        L.labels = { "MISSED", "LAST WRITE", "MARKERS", "FREE",
                     timeLeftLabelText (bigFree, diskReading == DiskReading::CopyStale) };
        L.values = { missedValueText (missed),
                     juce::String (lastWriteMs) + " ms",
                     juce::String (markerCount),
                     free,
                     bigFree ? free
                             : diskReading == DiskReading::PrimaryStale ? juce::String ("STALE")
                                                                        : recordTimeText (remainingSeconds) };

        GroupWidths widths {};
        for (int i = 0; i < kNumGroups; ++i)
        {
            const auto g = (Group) i;
            if (g == Group::Free && bigFree) continue;   // already the large figure
            const auto vf = valueFont (g, L.tall);
            const float w = juce::jmax (juce::GlyphArrangement::getStringWidth (brand::type::caption(), L.labels[(size_t) i]),
                                        juce::GlyphArrangement::getStringWidth (vf, L.values[(size_t) i]),
                                        juce::GlyphArrangement::getStringWidth (vf, valueTemplate (g)));
            widths[(size_t) i] = std::ceil (w);
        }

        // The timer keeps its room; the groups have what is left of the row.
        const auto tf = timerFont (inner.getHeight());
        const float timerW = juce::GlyphArrangement::getStringWidth (tf, elapsedText) + (float) brand::space::xl;
        const float room = juce::jmax (0.0f, inner.getWidth() - timerW);
        const float gap = (float) brand::space::xl;
        const auto shown = fitGroups (widths, room, gap);
        const float used = juce::jmin (groupsWidth (widths, shown, gap), juce::jmax (0.0f, inner.getWidth()));
        auto right = inner.removeFromRight (used);
        L.timer = inner;
        L.inner = getLocalBounds().toFloat().reduced ((float) brand::space::lg, 2.0f + (float) brand::space::xs);

        // One label line across the groups, each value under its label.
        const float stackH = kLabelH + valueH (Group::TimeLeft, L.tall);
        const float top = right.getY() + juce::jmax (0.0f, (right.getHeight() - stackH) * 0.5f);
        float x = right.getX();
        for (int i = 0; i < kNumGroups; ++i)
        {
            if (! shown[(size_t) i]) continue;
            const float w = juce::jmin (widths[(size_t) i], right.getRight() - x);
            L.groups[(size_t) i] = { x, top, juce::jmax (0.0f, w), stackH };
            x += widths[(size_t) i] + gap;
        }
        return L;
    }

    std::array<juce::Rectangle<float>, BigClockPanel::kNumGroups> BigClockPanel::getGroupBounds() const
    {
        return computeLayout().groups;
    }

    juce::String BigClockPanel::getGroupLabel (Group g) const { return computeLayout().labels[(size_t) g]; }
    juce::String BigClockPanel::getGroupValue (Group g) const { return computeLayout().values[(size_t) g]; }

    juce::String BigClockPanel::getTooltip()
    {
        if (! editLocked) return {};
        const auto p = getMouseXYRelative().toFloat();
        if (! computeLayout().left.contains (p)) return {};
        return "Timeline and marker edits are locked while recording; zoom, scroll and FOLLOW remain available";
    }

    void BigClockPanel::paint (juce::Graphics& g)
    {
        // Opaque base: cover every pixel with the body background (the 2px
        // margin around the plate showed bgDeep through it before). Required
        // for setOpaque(true) to be valid; visually identical.
        g.fillAll (brand::bgDeep);

        auto r = getLocalBounds().toFloat();

        // A rolling take must be unmistakable from across the room: the whole
        // plate turns record red and breathes at 1 Hz, with white type on it.
        const bool  rolling = mode == Mode::Recording;
        const bool  rm      = brand::reduceMotion();
        const float pulse   = rm ? 0.5f : 0.5f + 0.5f * std::sin (pulsePhase);   // 0..1 (static under reduced-motion)
        const float ignite  = rm ? 1.0f : igniteLevel;                          // full heat, no ramp

        juce::Colour bg = brand::bgPanel;
        if (rolling)                    bg = bg.interpolatedWith (brand::takePlate, (0.80f + 0.20f * pulse) * ignite);
        else if (mode == Mode::Playing) bg = brand::accentPlay.withAlpha (zynforge::brand::alpha::edgeSoft);
        auto bgRect = r.reduced (2.0f);
        g.setColour (bg);   // FLAT: solid plate (bg already breathes via pulse); no gradient
        g.fillRoundedRectangle (bgRect, brand::radius::lg);

        g.setColour (rolling ? brand::accentRecord : brand::edge);
        g.drawRoundedRectangle (bgRect, brand::radius::lg, rolling ? 2.0f : 1.0f);

        const auto labelCol = rolling ? brand::textPrimary : brand::textSecondary;
        const auto L = computeLayout();
        const bool tall = L.tall;

        // Left: state + lamp, and under it EDIT LOCKED while EDIT shows a take.
        {
            // Armed and waiting is a lamp, not a frame: bright is for the take.
            juce::Colour lamp = armedReady ? brand::signalArmedReady() : brand::textMuted;
            juce::String label = armedReady ? "ARMED" : "IDLE";
            if (rolling)                    { lamp = brand::textPrimary; label = "REC"; }
            else if (mode == Mode::Playing) { lamp = brand::accentPlay;  label = "PLAY"; }

            auto left = L.left;
            auto lockLine = editLocked ? left.removeFromBottom (kLockLineH) : juce::Rectangle<float>();
            const float lampSize = 14.0f;
            g.setColour (lamp);
            g.fillEllipse (left.withWidth (lampSize).withSizeKeepingCentre (lampSize, lampSize));

            const auto textX = lampSize + (float) brand::space::md;
            g.setColour (brand::textPrimary);
            g.setFont (brand::type::ui (rolling && tall ? brand::type::h_display : brand::type::h_subhead, true));
            g.drawText (label, left.withTrimmedLeft (textX), juce::Justification::centredLeft, false);

            if (editLocked)
            {
                g.setColour (labelCol);
                g.setFont (brand::type::captionBold());
                g.drawText (kEditLockedText, lockLine.withTrimmedLeft (textX), juce::Justification::centredLeft, false);
            }
        }

        // Right: the health groups, each a small label over its value. The
        // large one (TIME LEFT, or DISK FREE before a take) sits at the right
        // edge; the small ones give way first on a narrow plate.
        {
            const bool primaryStale = diskReading == DiskReading::PrimaryStale;
            const bool copyStale    = diskReading == DiskReading::CopyStale;
            const bool driveMissing = diskReading == DiskReading::DriveMissing;
            const auto valueCol     = primaryStale || driveMissing ? brand::alertAmber : brand::textPrimary;
            // On the record plate a miss has to be white to read at all.
            const auto missCol      = rolling ? brand::textPrimary : brand::accentRecord;

            for (int i = 0; i < kNumGroups; ++i)
            {
                const auto box = L.groups[(size_t) i];
                if (box.isEmpty()) continue;
                const auto grp = (Group) i;
                juce::Colour lc = labelCol, vc = brand::textPrimary;
                if (grp == Group::TimeLeft) { lc = copyStale ? brand::alertAmber : labelCol; vc = valueCol; }
                if (grp == Group::Free)     vc = valueCol;
                if (grp == Group::Missed && missed > 0) lc = vc = missCol;

                auto col = box;
                g.setColour (lc);
                g.setFont (brand::type::caption());
                g.drawText (L.labels[(size_t) i], col.removeFromTop (kLabelH), juce::Justification::centredLeft, false);
                g.setColour (vc);
                g.setFont (valueFont (grp, tall));
                g.drawText (L.values[(size_t) i], col.removeFromTop (valueH (grp, tall)), juce::Justification::centredLeft, false);
            }
        }

        // Centre: the timer. Tabular numerals -- the string can't shift width
        // as seconds tick. It keeps its room; the health groups give way.
        g.setFont (timerFont (L.inner.getHeight()));
        g.setColour (rolling ? brand::textPrimary
                             : mode == Mode::Playing ? brand::accentPlay : brand::textSecondary);
        g.drawText (elapsedText, L.timer, juce::Justification::centred, false);
    }
}
