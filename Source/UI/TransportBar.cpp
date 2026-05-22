#include "TransportBar.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    class TransportBar::IconButton final : public juce::Button
    {
    public:
        enum class Icon { GotoStart, GotoEnd, Play, Stop, Record, Loop };

        IconButton (const juce::String& name, Icon i)
            : juce::Button (name), icon (i) {}

        void setActive (bool a) { active = a; repaint(); }
        void setBaseColour (juce::Colour c) { baseColour = c; repaint(); }

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            // Square button body — gradient fill, 1px border. Matches the
            // ZynForge Live transport button finish.
            auto r = getLocalBounds().toFloat().reduced (2.0f);

            const auto base = (down ? brand::controlBgDown
                                    : over ? brand::controlBgHover
                                           : brand::controlBg);
            g.setGradientFill (brand::verticalGradient (base, r, 0.10f, 0.18f));
            g.fillRoundedRectangle (r, brand::radius::md);
            g.setColour (brand::controlBorder);
            g.drawRoundedRectangle (r, brand::radius::md, 1.0f);

            // Icon (centred inside the inner area).
            auto ic = r.reduced (8.0f, 6.0f);
            const float cx = ic.getCentreX();
            const float cy = ic.getCentreY();

            g.setColour (active ? baseColour.brighter (0.3f) : baseColour);

            switch (icon)
            {
                case Icon::GotoStart:
                {
                    // Vertical bar on left + leftward triangle.
                    g.fillRect (juce::Rectangle<float> (ic.getX(), ic.getY(),
                                                       2.5f, ic.getHeight()));
                    juce::Path p;
                    p.addTriangle (ic.getX() + 4.0f, cy,
                                   ic.getRight(),    ic.getY(),
                                   ic.getRight(),    ic.getBottom());
                    g.fillPath (p);
                    break;
                }
                case Icon::GotoEnd:
                {
                    juce::Path p;
                    p.addTriangle (ic.getRight() - 4.0f, cy,
                                   ic.getX(),            ic.getY(),
                                   ic.getX(),            ic.getBottom());
                    g.fillPath (p);
                    g.fillRect (juce::Rectangle<float> (ic.getRight() - 2.5f, ic.getY(),
                                                       2.5f, ic.getHeight()));
                    break;
                }
                case Icon::Play:
                {
                    juce::Path p;
                    p.addTriangle (ic.getX(),     ic.getY(),
                                   ic.getX(),     ic.getBottom(),
                                   ic.getRight(), cy);
                    g.fillPath (p);
                    break;
                }
                case Icon::Stop:
                {
                    g.fillRoundedRectangle (ic.reduced (1.0f), 1.5f);
                    break;
                }
                case Icon::Record:
                {
                    const float radius = juce::jmin (ic.getWidth(), ic.getHeight()) * 0.5f;
                    g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
                    break;
                }
                case Icon::Loop:
                {
                    // Circular arrow.
                    juce::Path p;
                    const float r2 = juce::jmin (ic.getWidth(), ic.getHeight()) * 0.4f;
                    p.addCentredArc (cx, cy, r2, r2,
                                     0.0f,
                                     juce::MathConstants<float>::pi * 0.25f,
                                     juce::MathConstants<float>::twoPi - 0.4f,
                                     true);
                    g.strokePath (p, juce::PathStrokeType (2.2f));
                    // Arrowhead at the open side.
                    juce::Path head;
                    const float ax = cx + r2 * std::cos (-0.4f);
                    const float ay = cy + r2 * std::sin (-0.4f);
                    head.addTriangle (ax,        ay - 5.0f,
                                      ax + 6.0f, ay,
                                      ax,        ay + 5.0f);
                    g.fillPath (head);
                    break;
                }
            }
        }

    private:
        Icon         icon;
        bool         active     { false };
        juce::Colour baseColour { juce::Colours::white };
    };

    TransportBar::TransportBar (AudioEngine& eng) : engine (eng)
    {
        auto make = [this] (const juce::String& name, IconButton::Icon i, juce::Colour c)
        {
            auto b = std::make_unique<IconButton> (name, i);
            b->setBaseColour (c);
            addAndMakeVisible (*b);
            return b;
        };

        gotoStart = make ("Go to start",  IconButton::Icon::GotoStart, brand::textPrimary);
        gotoEnd   = make ("Go to end",    IconButton::Icon::GotoEnd,   brand::textPrimary);
        play      = make ("Play",         IconButton::Icon::Play,      brand::accentPlay);
        stop      = make ("Stop",         IconButton::Icon::Stop,      brand::accentVS);
        record    = make ("Record",       IconButton::Icon::Record,    brand::accentRecord);
        loop      = make ("Loop region",  IconButton::Icon::Loop,      brand::textMuted);

        gotoStart->setTooltip ("Go to session start (rewind playback to 0).");
        gotoEnd  ->setTooltip ("Go to session end.");
        play     ->setTooltip ("Play / pause VSC playback.");
        stop     ->setTooltip ("Stop everything (recording + playback) and rewind.");
        record   ->setTooltip ("Toggle recording.");
        loop     ->setTooltip ("Toggle loop between Loop In / Out markers.");

        gotoStart->onClick = [this] { engine.getPlayer().setPositionSamples (0); };
        gotoEnd  ->onClick = [this]
        {
            auto& p = engine.getPlayer();
            p.setPositionSamples (p.getTotalLengthSamples());
        };
        play     ->onClick = [this]
        {
            auto& p = engine.getPlayer();
            if (! p.isLoaded()) return;
            if (p.isPlaying()) engine.stopPlayback(); else engine.startPlayback();
        };
        stop     ->onClick = [this]
        {
            if (engine.isRecording()) engine.stopRecording();
            engine.stopPlayback();
            engine.getPlayer().rewind();
        };
        record   ->onClick = [this]
        {
            if (engine.isRecording())
            {
                engine.stopRecording();
                return;
            }
            const auto root = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                                  .getChildFile ("Zynforge Sessions");
            const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
            engine.startRecording (root.getChildFile ("Session_" + stamp));
        };
        loop     ->onClick = [this]
        {
            auto& p = engine.getPlayer();
            if (p.hasLoopRegion())
            {
                p.clearLoopRegion();
            }
            else
            {
                // Default loop: 0 → total length.
                const auto total = p.getTotalLengthSamples();
                if (total > 0) p.setLoopRegion (0, total);
            }
        };

        refreshStates();
        startTimerHz (10);
    }

    TransportBar::~TransportBar() = default;

    void TransportBar::resized()
    {
        auto r = getLocalBounds();
        const int n = 6;
        const int gap = 4;

        // Square buttons sized to the bar's height — never wider than tall,
        // never taller than wide. Matches the reference mock-up's shape.
        const int side = juce::jmin (r.getHeight(),
                                     (r.getWidth() - (n - 1) * gap) / n);

        // Centre the row horizontally if it ends up narrower than the bar.
        const int totalW = n * side + (n - 1) * gap;
        if (totalW < r.getWidth())
            r.removeFromLeft ((r.getWidth() - totalW) / 2);

        for (auto* b : { gotoStart.get(), gotoEnd.get(), play.get(),
                         stop.get(), record.get(), loop.get() })
        {
            // Vertically centre each square inside the bar's full height.
            const int yOffset = (r.getHeight() - side) / 2;
            auto cell = r.removeFromLeft (side);
            b->setBounds (cell.withY (cell.getY() + yOffset).withHeight (side));
            r.removeFromLeft (gap);
        }
    }

    void TransportBar::timerCallback() { refreshStates(); }

    void TransportBar::refreshStates()
    {
        const bool rec  = engine.isRecording();
        const bool pl   = engine.isPlaying();
        const bool lp   = engine.getPlayer().hasLoopRegion();

        if (rec != lastRecording) { record->setActive (rec); lastRecording = rec; }
        if (pl  != lastPlaying)   { play  ->setActive (pl);  lastPlaying   = pl;  }
        if (lp  != lastLooping)   { loop  ->setActive (lp);  lastLooping   = lp;  }
    }
}
