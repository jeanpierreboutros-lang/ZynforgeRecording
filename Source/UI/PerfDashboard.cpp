#include "PerfDashboard.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    PerfDashboard::PerfDashboard()
    {
        // Opaque: the dashboard sits over MainComponent's flat bgDeep body, so
        // paint() fills its full bounds with bgDeep before the rounded plate.
        // Being opaque means a value-change repaint no longer forces
        // MainComponent to repaint the slice behind it (it repaints ~24x/s as
        // the live AUDIO load jitters).
        setOpaque (true);
    }

    void PerfDashboard::setMetrics (float cpuPct,
                                    float diskMBPerSec,
                                    float ringFillPct,
                                    juce::int64 missedSamples)
    {
        // Called at MainComponent's 24 Hz timer rate. Round inputs to
        // the precision actually shown on screen so float jitter
        // doesn't repeatedly trigger a repaint when the number isn't
        // visibly moving. ~1 percentage point on CPU / buf, ~0.1 MB/s
        // on disk -- enough resolution to track real changes, coarse
        // enough to drop the no-op repaints.
        const float newCpu  = std::round (juce::jlimit (0.0f, 100.0f, cpuPct));
        const float newDisk = std::round (juce::jmax (0.0f, diskMBPerSec) * 10.0f) / 10.0f;
        const float newBuf  = std::round (juce::jlimit (0.0f, 100.0f, ringFillPct));
        const bool changed = newCpu  != cpu
                          || newDisk != disk
                          || newBuf  != buf
                          || missedSamples != missed;
        cpu    = newCpu;
        disk   = newDisk;
        buf    = newBuf;
        missed = missedSamples;
        if (changed) repaint();
    }

    void PerfDashboard::setDeviceInfo (double sr, int blockSizeSamples)
    {
        if (sr == sampleRate && blockSizeSamples == blockSize) return;
        sampleRate = sr;
        blockSize  = blockSizeSamples;
        repaint();
    }

    void PerfDashboard::setRecording (bool isRecording)
    {
        if (isRecording == recording) return;
        recording = isRecording;
        repaint();
    }

    // Silence and anything below the BS.1770 gate read as no figure at all.
    static juce::String loudnessFigure (float value)
    {
        return value > -70.0f ? juce::String (value, 1) : juce::String ("--");
    }

    static constexpr float kPanelInsetX = 12.0f;    // 4 px margin + 8 px padding, each side
    static constexpr float kFormatW = 60.0f;        // the widest format line, "192.0 kHz"
    static constexpr float kFormatGap = 8.0f;

    static juce::Font loudnessFont() { return brand::type::mono (brand::type::h_body, true); }

    static float loudnessWidth (const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (loudnessFont(), text);
    }

    void PerfDashboard::setLoudness (float integratedLufs, float momentaryLufs, float truePeak)
    {
        // Rounded to what is shown, so meter jitter doesn't force a repaint.
        auto shown = [] (float v) { return std::round (v * 10.0f) / 10.0f; };
        const float newIntegrated = shown (integratedLufs);
        const float newMomentary  = shown (momentaryLufs);
        const float newPeak       = shown (truePeak);
        if (newIntegrated == integrated && newMomentary == momentary && newPeak == truePeakDb) return;
        integrated = newIntegrated;
        momentary  = newMomentary;
        truePeakDb = newPeak;
        repaint();
    }

    // A hot peak is marked as well as red, so it doesn't rest on colour alone.
    juce::String PerfDashboard::getTruePeakText() const
    {
        return "TP " + loudnessFigure (truePeakDb) + (truePeakIsHot() ? "!" : "");
    }

    juce::String PerfDashboard::getLoudnessText() const
    {
        return "LUFS M " + loudnessFigure (momentary) + " I " + loudnessFigure (integrated)
             + " " + getTruePeakText();
    }

    bool PerfDashboard::truePeakIsHot() const noexcept { return truePeakDb >= -1.0f; }

    bool PerfDashboard::loudnessFits() const
    {
        // Measured on the widest this row can ever read, not on today's figures.
        const float room = (float) getWidth() - 2.0f * kPanelInsetX - kFormatW - kFormatGap;
        return loudnessWidth ("LUFS M -69.9 I -69.9 TP -69.9!") <= room;
    }

    static juce::Colour ledColour (float pct)
    {
        if (pct < 50.0f) return brand::accentPlay;
        if (pct < 80.0f) return brand::accentSolo;
        return brand::accentRecord;
    }

    void PerfDashboard::paint (juce::Graphics& g)
    {
        // Opaque base: fill every pixel with the body background the panel sits
        // on (MainComponent's flat bgDeep), so the 4px margin + rounded corners
        // read identically to the old non-opaque version. Required for
        // setOpaque(true) to be valid.
        g.fillAll (brand::bgDeep);

        auto r = getLocalBounds().toFloat().reduced (4.0f);

        g.setColour (brand::bgPanel);
        g.fillRoundedRectangle (r, brand::radius::md);
        // While recording, ring the panel in record-red so the load / buffer
        // / disk read is unmistakable on a dark stage.
        g.setColour (recording ? brand::accentRecord : brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, recording ? 1.5f : 1.0f);

        auto inner = r.reduced (8.0f, 5.0f);

        // One thin bar: how hard the disk is being pushed, full at 50 MB/s,
        // the same scale as the colour on the DISK figure above it.
        const float diskPct = juce::jmin (100.0f, disk * 2.0f);
        auto bar = inner.removeFromBottom (3.0f);
        inner.removeFromBottom (2.0f);
        g.setColour (brand::lift (brand::bgDeep, 0.05f));
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (ledColour (diskPct).withAlpha (zynforge::brand::alpha::bold));
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * diskPct / 100.0f), 1.5f);

        // Device format in its own narrow column, so the loudness line and the
        // load figures beside it can be large enough to read from the desk.
        {
            auto format = inner.removeFromLeft (kFormatW);
            inner.removeFromLeft (kFormatGap);
            const auto known = sampleRate > 0.0 && blockSize > 0;
            g.setColour (brand::textSecondary);
            g.setFont (brand::type::ledLabel());
            g.drawText (known ? juce::String (sampleRate / 1000.0, 1) + " kHz" : juce::String ("-- kHz"),
                        format.removeFromTop (format.getHeight() * 0.5f), juce::Justification::centredLeft, false);
            g.drawText (known ? juce::String (blockSize) + " smp" : juce::String ("-- smp"),
                        format, juce::Justification::centredLeft, false);
        }

        auto top = inner.removeFromTop (inner.getHeight() * 0.55f);
        g.setFont (loudnessFont());

        // A dropped sample outranks everything else on this row while it rolls.
        if (showsDrop())
        {
            g.setColour (brand::accentRecord);
            g.drawText ("DROP " + juce::String (missed), top, juce::Justification::centredRight, false);
        }
        else
        {
            const auto peak = getTruePeakText();
            g.setColour (truePeakIsHot() ? brand::accentRecord : brand::textPrimary);
            g.drawText (peak, top, juce::Justification::centredRight, false);
            top.removeFromRight (loudnessWidth (peak + " "));
            g.setColour (brand::textPrimary);
            g.drawText (getLoudnessText().upToLastOccurrenceOf (" TP", false, false),
                        top, juce::Justification::centredRight, false);
        }

        // Value widths in em for the longest each can read, so the three sit
        // evenly and a changing digit can't push its neighbour along.
        const float valuePt = recording && inner.getHeight() >= 16.0f ? brand::type::h_body : brand::type::h_caption;
        const float pctW = valuePt * 2.5f, diskW = valuePt * 6.2f;
        const float labelsW = 34.0f + 28.0f + 22.0f;
        const float gap = juce::jmax (4.0f, (inner.getWidth() - labelsW - 2.0f * pctW - diskW) / 2.0f);
        auto cell = [&] (const char* label, float labelW, const juce::String& value, float valueW, juce::Colour colour)
        {
            g.setColour (brand::textTertiary);
            g.setFont (brand::type::ledLabel());
            g.drawText (label, inner.removeFromLeft (labelW), juce::Justification::centredLeft, false);
            g.setColour (colour);
            g.setFont (brand::type::mono (valuePt, true));
            g.drawText (value, inner.removeFromLeft (valueW), juce::Justification::centredLeft, false);
            inner.removeFromLeft (gap);
        };
        // "AUDIO" = the real CoreAudio callback load (getAudioLoadPct) -- the
        // number that predicts dropouts -- NOT whole-app CPU.
        cell ("AUDIO", 34.0f, juce::String ((int) std::round (cpu)) + "%", pctW, ledColour (cpu));
        cell ("DISK",  28.0f, juce::String (disk, 1) + " MB/s", diskW, ledColour (diskPct));
        cell ("BUF",   22.0f, juce::String ((int) std::round (buf)) + "%", pctW, ledColour (buf));
    }
}
