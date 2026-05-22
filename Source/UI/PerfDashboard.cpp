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
        cpu    = juce::jlimit (0.0f, 100.0f, cpuPct);
        disk   = juce::jmax (0.0f, diskMBPerSec);
        buf    = juce::jlimit (0.0f, 100.0f, ringFillPct);
        missed = missedSamples;
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
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);

        // Three rows: CPU, DISK, BUF. Each row = [label 32px] [bar fills] [value 60px].
        auto inner = r.reduced (8.0f, 6.0f);
        const float rowH   = inner.getHeight() / 3.0f;
        const float labelW = 36.0f;
        const float valueW = 70.0f;
        const float gap    = 4.0f;

        auto drawRow = [&] (juce::Rectangle<float> row,
                            const char* label,
                            float pct,
                            const juce::String& valueText)
        {
            // Label (left)
            g.setColour (brand::textMuted);
            g.setFont (juce::FontOptions().withHeight (10.0f).withStyle ("Bold"));
            g.drawText (label, row.removeFromLeft (labelW),
                        juce::Justification::centredLeft, false);

            // Value (right)
            const auto col = ledColour (pct);
            g.setColour (col);
            g.setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
            g.drawText (valueText, row.removeFromRight (valueW),
                        juce::Justification::centredRight, false);

            row.removeFromLeft (gap);
            row.removeFromRight (gap);

            // LED bar — segmented (12 segments) so a track-by-track read
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
                    g.setColour (ledColour (p).withAlpha (0.95f));
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

        drawRow (row1, "CPU",  cpu,  juce::String ((int) std::round (cpu)) + " %");
        drawRow (row2, "DISK", juce::jmin (100.0f, disk * 2.0f),
                              juce::String (disk, 1) + " MB/s");
        drawRow (row3, "BUF",  buf,  juce::String ((int) std::round (buf)) + " %");

        // Missed-samples warning glows red at the bottom-right when non-zero.
        if (missed > 0)
        {
            g.setColour (brand::accentRecord);
            g.setFont (juce::FontOptions().withHeight (10.0f).withStyle ("Bold"));
            const auto warn = "DROP " + juce::String (missed);
            g.drawText (warn, r.reduced (8.0f, 4.0f),
                        juce::Justification::bottomRight, false);
        }
    }
}
