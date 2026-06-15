#include "TimelineStrip.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    static constexpr int kMarkerHitRadius = 8;
    static constexpr int kHMargin         = 10;

    TimelineStrip::TimelineStrip (AudioEngine& eng) : engine (eng)
    {
        startTimerHz (20);
    }

    TimelineStrip::~TimelineStrip() = default;

    void TimelineStrip::timerCallback()
    {
        const auto pos   = engine.getPlayer().getPositionSamples();
        const auto total = engine.getPlayer().getTotalLengthSamples();
        if (pos != cachedPos || total != cachedTotal)
        {
            cachedPos   = pos;
            cachedTotal = total;
            repaint();
        }
    }

    int TimelineStrip::xForSample (juce::int64 sample) const
    {
        if (cachedTotal <= 0) return kHMargin;
        const auto w = getWidth() - 2 * kHMargin;
        const auto frac = juce::jlimit (0.0, 1.0, (double) sample / (double) cachedTotal);
        return kHMargin + (int) (frac * (double) w);
    }

    juce::int64 TimelineStrip::sampleAtX (int x) const
    {
        if (cachedTotal <= 0) return 0;
        const auto w = juce::jmax (1, getWidth() - 2 * kHMargin);
        const double frac = juce::jlimit (0.0, 1.0, (double) (x - kHMargin) / (double) w);
        return (juce::int64) (frac * (double) cachedTotal);
    }

    int TimelineStrip::findMarkerAtX (int x) const
    {
        const auto& markers = engine.getMarkers().getAll();
        for (int i = 0; i < (int) markers.size(); ++i)
        {
            const int mx = xForSample (markers[(std::size_t) i].sampleOffset);
            if (std::abs (mx - x) <= kMarkerHitRadius)
                return i;
        }
        return -1;
    }

    void TimelineStrip::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (2.0f);
        g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.10f, 0.14f));
        g.fillRoundedRectangle (r, brand::radius::md);
        g.setColour (brand::edge);
        g.drawRoundedRectangle (r, brand::radius::md, 1.0f);

        // Track baseline
        const auto trackY = r.getCentreY();
        g.setColour (brand::bgDeep);
        g.fillRect (juce::Rectangle<float> (r.getX() + kHMargin, trackY - 4.0f,
                                            r.getWidth() - 2 * kHMargin, 8.0f));

        if (cachedTotal <= 0)
        {
            g.setColour (brand::textMuted);
            g.setFont (brand::type::caption());
            g.drawText ("Load a session to see the timeline.",
                        r, juce::Justification::centred, false);
            return;
        }

        // Progress fill (0 → playhead)
        const int px = xForSample (cachedPos);
        g.setColour (brand::accentPlay.withAlpha (zynforge::brand::alpha::soft));
        g.fillRect (juce::Rectangle<float> ((float) kHMargin, trackY - 4.0f,
                                            (float) (px - kHMargin), 8.0f));

        // Loop region band
        auto& player = engine.getPlayer();
        if (player.hasLoopRegion())
        {
            const int x0 = xForSample (player.getLoopStart());
            const int x1 = xForSample (player.getLoopEnd());
            g.setColour (brand::accentVS.withAlpha (zynforge::brand::alpha::dimmed));
            g.fillRect (juce::Rectangle<float> ((float) x0, trackY - 8.0f,
                                                (float) (x1 - x0), 16.0f));
            g.setColour (brand::accentVS);
            g.drawRect (juce::Rectangle<float> ((float) x0, trackY - 8.0f,
                                                (float) (x1 - x0), 16.0f), 1.0f);
        }

        // Playhead
        g.setColour (brand::accentPlay);
        g.drawLine ((float) px, r.getY() + 4.0f, (float) px, r.getBottom() - 4.0f, 2.0f);

        // Markers
        const auto& markers = engine.getMarkers().getAll();
        for (int i = 0; i < (int) markers.size(); ++i)
        {
            const auto& m = markers[(std::size_t) i];
            const int mx = xForSample (m.sampleOffset);
            const float cy = trackY - 12.0f;
            juce::Path tri;
            tri.addTriangle ((float) mx - 4.0f, cy - 6.0f,
                             (float) mx + 4.0f, cy - 6.0f,
                             (float) mx,        cy + 2.0f);
            g.setColour (brand::accentRecord);
            g.fillPath (tri);

            g.setColour (brand::textPrimary);
            g.setFont (brand::type::hint());
            g.drawText (m.name,
                        mx - 30, (int) (r.getBottom() - 14.0f), 60, 12,
                        juce::Justification::centred, false);
        }
    }

    void TimelineStrip::mouseDown (const juce::MouseEvent& e)
    {
        if (! engine.getPlayer().isLoaded()) return;

        const int markerIdx = findMarkerAtX (e.x);

        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            if (markerIdx >= 0)
                showMarkerMenu (markerIdx, e.getScreenPosition());
            return;
        }

        if (markerIdx >= 0)
        {
            const auto m = engine.getMarkers().getMarker (markerIdx);
            engine.getPlayer().setPositionSamples (m.sampleOffset);
        }
        else
        {
            engine.getPlayer().setPositionSamples (sampleAtX (e.x));
        }
        repaint();
    }

    void TimelineStrip::showMarkerMenu (int idx, juce::Point<int>)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Rename...");
        menu.addItem (2, "Delete");
        menu.addSeparator();
        menu.addItem (3, "Set as Loop In");
        menu.addItem (4, "Set as Loop Out");
        menu.addItem (5, "Clear Loop", engine.getPlayer().hasLoopRegion());

        menu.showMenuAsync (juce::PopupMenu::Options(),
                            [this, idx] (int chosen)
        {
            auto& markers = engine.getMarkers();
            auto& player  = engine.getPlayer();
            if (chosen == 1) renameMarkerDialog (idx);
            else if (chosen == 2) markers.removeMarker (idx);
            else if (chosen == 3)
            {
                const auto s = markers.getMarker (idx).sampleOffset;
                const auto e = player.hasLoopRegion() ? player.getLoopEnd() : (s + 1);
                player.setLoopRegion (s, juce::jmax (e, s + 1));
            }
            else if (chosen == 4)
            {
                const auto e = markers.getMarker (idx).sampleOffset;
                const auto s = player.hasLoopRegion() ? player.getLoopStart() : 0;
                player.setLoopRegion (juce::jmin (s, e - 1), e);
            }
            else if (chosen == 5) player.clearLoopRegion();
            repaint();
        });
    }

    void TimelineStrip::renameMarkerDialog (int idx)
    {
        auto current = engine.getMarkers().getMarker (idx).name;

        auto* aw = new juce::AlertWindow ("Rename marker",
                                          "Enter a new name:",
                                          juce::MessageBoxIconType::NoIcon);
        aw->setLookAndFeel (&getLookAndFeel());   // grey ZynForge chrome, not JUCE default
        aw->addTextEditor ("name", current, {});
        dialog::primeNameEditor (*aw, "name");
        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true,
            juce::ModalCallbackFunction::create (
                [this, idx, aw] (int result)
                {
                    if (result == 1)
                    {
                        engine.getMarkers().renameMarker (idx, aw->getTextEditorContents ("name"));
                        repaint();
                    }
                    delete aw;
                }),
            false);
    }
}
