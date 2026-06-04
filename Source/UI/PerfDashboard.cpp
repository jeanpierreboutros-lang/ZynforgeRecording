#include "PerfDashboard.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    PerfDashboard::PerfDashboard()
    {
        setOpaque (false);
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

    static juce::Colour ledColour (float pct)
    {
        if (pct < 50.0f) return brand::accentPlay;
        if (pct < 80.0f) return brand::accentSolo;
        return brand::accentRecord;
    }

    void PerfDashboard::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (4.0f);

        // Panel background.
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.05f, 0.12f));
        g.fillRoundedRectangle (r, brand::radius::md);
        // While recording, ring the panel in record-red so the load / buffer
        // / disk read is unmistakable on a dark stage.
        g.setColour (recording ? brand::accentRecord : brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, recording ? 1.5f : 1.0f);

        auto inner = r.reduced (8.0f, 6.0f);

        // Header line: live device format -- sample rate + actual buffer
        // (block) size in samples. The buffer is the real glitch-headroom
        // number; smaller = tighter. "-- " until the device reports.
        {
            auto hdr = inner.removeFromTop (recording ? 15.0f : 13.0f);
            g.setColour (brand::textSecondary);
            g.setFont (brand::type::mono (recording ? 11.0f : 10.0f, true));
            const juce::String fmt = (sampleRate > 0.0 && blockSize > 0)
                ? juce::String (sampleRate / 1000.0, 1) + " kHz  ·  "
                      + juce::String (blockSize) + " smp"
                : juce::String ("-- kHz  ·  -- smp");
            g.drawText (fmt, hdr, juce::Justification::centred, false);
            inner.removeFromTop (2.0f);
        }

        // Three rows: AUDIO (callback load), DISK, BUF. Each row =
        // [label 36px] [bar fills] [value 70px].
        const float rowH   = inner.getHeight() / 3.0f;
        const float labelW = 36.0f;
        const float valueW = 70.0f;
        const float gap    = 4.0f;

        auto drawRow = [&] (juce::Rectangle<float> row,
                            const char* label,
                            float pct,
                            const juce::String& valueText)
        {
            // Label (left) -- proportional UI font, muted colour
            g.setColour (brand::textTertiary);
            g.setFont (brand::type::ledLabel());
            g.drawText (label, row.removeFromLeft (labelW),
                        juce::Justification::centredLeft, false);

            // Value (right) -- tabular mono so the % digit doesn't twitch
            // every frame as CPU load swings.
            const auto col = ledColour (pct);
            g.setColour (col);
            g.setFont (brand::type::mono (recording ? 13.0f : 11.0f, true));
            g.drawText (valueText, row.removeFromRight (valueW),
                        juce::Justification::centredRight, false);

            row.removeFromLeft (gap);
            row.removeFromRight (gap);

            // LED bar -- segmented (12 segments) so a track-by-track read
            // is obvious even at a glance from across the room.
            const float barH = juce::jmin (8.0f, row.getHeight() - 4.0f);
            auto bar = row.withSizeKeepingCentre (row.getWidth(), barH);

            constexpr int   segments = 12;
            const float segW = (bar.getWidth() - (segments - 1)) / (float) segments;
            const int lit    = juce::jlimit (0, segments,
                                              (int) std::round (pct / 100.0f * segments));

            for (int i = 0; i < segments; ++i)
            {
                juce::Rectangle<float> seg (bar.getX() + i * (segW + 1.0f),
                                            bar.getY(), segW, bar.getHeight());
                if (i < lit)
                {
                    const float p = (float) (i + 1) / segments * 100.0f;
                    g.setColour (ledColour (p).withAlpha (brand::alpha::bold));
                }
                else
                {
                    g.setColour (brand::bgDeep.brighter (0.05f));
                }
                g.fillRoundedRectangle (seg, 1.5f);
            }
        };

        auto row1 = inner.removeFromTop (rowH);
        auto row2 = inner.removeFromTop (rowH);
        auto row3 = inner;

        // "AUDIO" = the real CoreAudio callback load (getAudioLoadPct) -- the
        // number that predicts dropouts -- NOT whole-app CPU.
        drawRow (row1, "AUDIO", cpu, juce::String ((int) std::round (cpu)) + " %");
        drawRow (row2, "DISK", juce::jmin (100.0f, disk * 2.0f),
                              juce::String (disk, 1) + " MB/s");
        drawRow (row3, "BUF",  buf,  juce::String ((int) std::round (buf)) + " %");

        // Missed-samples warning glows red at the bottom-right when non-zero.
        if (missed > 0)
        {
            g.setColour (brand::accentRecord);
            g.setFont (brand::type::mono (10.0f, true));
            const auto warn = "DROP " + juce::String (missed);
            g.drawText (warn, r.reduced (8.0f, 4.0f),
                        juce::Justification::bottomRight, false);
        }
    }
}
