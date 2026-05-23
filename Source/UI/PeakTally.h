#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"

namespace zynforge
{
    // PeakTally — a thin red bar painted across the top of the strip
    // area. Lights when ANY strip is clipping; holds for one second
    // after the last clip then fades. Visible from across the room
    // under stage glare — the engineer doesn't need to scan the
    // meter ladder, they see one global "you have a clip somewhere"
    // signal and look down to find it.
    //
    // Audio cost: zero. Reads `clipped` atomic on each strip from a
    // 30 Hz UI timer, never crosses the audio thread.
    class PeakTally final : public juce::Component,
                            public  juce::SettableTooltipClient,
                            private juce::Timer
    {
    public:
        explicit PeakTally (AudioEngine& eng) : engine (eng)
        {
            startTimerHz (30);
            setInterceptsMouseClicks (true, false);
            setTooltip ("Any strip clipped — click to clear all clip latches");
        }

        void paint (juce::Graphics& g) override
        {
            // Pulse alpha when active: a bright steady bar can look
            // like a normal divider line; a pulsing one reads as an
            // alert.
            const float a = active
                ? 0.55f + 0.45f * std::sin (phase)   // 0.10..1.00
                : 0.0f;

            if (a > 0.01f)
            {
                auto r = getLocalBounds().toFloat();
                g.setColour (brand::accentRecord.withAlpha (a));
                g.fillRect (r);
                // 1 px crisp top edge — makes the bar feel like a
                // surface, not a smear.
                g.setColour (brand::accentRecord.brighter (0.40f).withAlpha (a));
                g.drawHorizontalLine ((int) r.getY(), r.getX(), r.getRight());
            }
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            // Clicking the tally clears every strip's clip counter +
            // latched-clipped flag. Same gesture the engineer already
            // knows from clicking a strip's meter.
            auto& rec = engine.getRecorder();
            for (int i = 0, n = rec.getNumTracks(); i < n; ++i)
            {
                auto& t = rec.getTrack (i);
                t.clipped.store (false, std::memory_order_relaxed);
                t.clipCount.store (0, std::memory_order_relaxed);
            }
            holdSamplesLeft = 0;
            repaint();
        }

    private:
        AudioEngine& engine;
        bool         active           { false };
        float        phase            { 0.0f };
        juce::int64  holdSamplesLeft  { 0 };   // ticks at 30 Hz, ~1s = 30

        void timerCallback() override
        {
            // Sweep every strip's clipped flag. Audio thread already
            // sets it via TrackState::clipped when a sample exceeds
            // 0 dBFS; we just observe it.
            bool anyClipped = false;
            auto& rec = engine.getRecorder();
            for (int i = 0, n = rec.getNumTracks(); i < n; ++i)
            {
                if (rec.getTrack (i).clipped.load (std::memory_order_relaxed))
                {
                    anyClipped = true;
                    break;
                }
            }

            if (anyClipped)
                holdSamplesLeft = 30;        // 1.0 s at 30 Hz

            if (holdSamplesLeft > 0)
                --holdSamplesLeft;

            const bool wasActive = active;
            active = holdSamplesLeft > 0;

            if (active)
            {
                phase += juce::MathConstants<float>::twoPi / 15.0f;  // 2 Hz
                if (phase > juce::MathConstants<float>::twoPi)
                    phase -= juce::MathConstants<float>::twoPi;
                repaint();
            }
            else if (wasActive)
            {
                repaint();   // one final clear paint
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PeakTally)
    };
}
