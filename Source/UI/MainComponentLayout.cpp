// MainComponent's paint + resized methods. Extracted from
// MainComponent.cpp as part of the 2026-05-24 god-class split so the
// layout math is editable without scrolling past the other 5000 lines.

#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "EditToolsBar.h"

using namespace zynforge;

namespace
{
    constexpr int kEditToolsW = 330;
    constexpr int kPillW      = 176;
}

void MainComponent::paint (juce::Graphics& g)
{
    // Whole-window vertical gradient -- top sits 6% above bgDeep, bottom
    // sits 4% below. Subtle enough that the engineer reads it as 'deep
    // panel' rather than 'banded background', but it stops the canvas
    // feeling like a flat sheet of paint.
    {
        auto fullBounds = getLocalBounds().toFloat();
        g.setColour (brand::bgDeep);   // FLAT: solid background (no gradient)
        g.fillRect (fullBounds);
    }

    // Header = row 1 (44 px) + row 2 transport (52 px) = 96 px total.
    auto header = getLocalBounds().removeFromTop (44 + 52).toFloat();
    g.setGradientFill (brand::verticalGradient (brand::bgPanel, header, 0.08f, 0.18f));
    g.fillRect (header);

    g.setColour (brand::edge);
    g.drawHorizontalLine ((int) header.getBottom() - 1,
                          header.getX(), header.getRight());

    // ── Heated Steel wordmark lockup: forge-mark badge + two-tone text ──
    // Painted (not a Label) so we get the glyph, two colours, and letter
    // tracking the mock uses. Sits in the 248px slot reserved in resized().
    {
        const float cy = 22.0f;            // vertical centre of the 44px row 1
        float x = 14.0f;

        // Forge-mark badge: dark rounded square + orange hex + rising chevron.
        const float bs = 26.0f;
        juce::Rectangle<float> badge (x, cy - bs * 0.5f, bs, bs);
        g.setColour (brand::controlBg);
        g.fillRoundedRectangle (badge, brand::radius::lg);
        g.setColour (brand::gloss (0.07f));
        g.drawRoundedRectangle (badge, brand::radius::lg, 1.0f);
        {
            auto u = badge.reduced (5.0f);
            const auto xf = juce::AffineTransform::scale (u.getWidth(), u.getHeight())
                                                   .translated (u.getX(), u.getY());
            juce::Path hex;
            hex.startNewSubPath (0.50f, 0.05f); hex.lineTo (0.92f, 0.28f); hex.lineTo (0.92f, 0.72f);
            hex.lineTo (0.50f, 0.95f); hex.lineTo (0.08f, 0.72f); hex.lineTo (0.08f, 0.28f); hex.closeSubPath();
            juce::Path chv;
            chv.startNewSubPath (0.28f, 0.64f); chv.lineTo (0.50f, 0.41f); chv.lineTo (0.72f, 0.64f);
            hex.applyTransform (xf); chv.applyTransform (xf);
            g.setColour (brand::brandOrange.withAlpha (zynforge::brand::alpha::edgeSoft)); g.fillPath (hex);
            g.setColour (brand::brandOrange);
            g.strokePath (hex, juce::PathStrokeType (1.4f));
            g.strokePath (chv, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
            const float sp = u.getWidth() * 0.05f;
            g.fillEllipse (u.getCentreX() - sp, u.getY() + u.getHeight() * 0.27f - sp, sp * 2.0f, sp * 2.0f);
        }
        x = badge.getRight() + 11.0f;

        // Two-tone, letter-tracked wordmark.
        auto drawTracked = [&g, cy] (const juce::String& s, juce::Font f, juce::Colour col,
                                     float startX, float tracking) -> float
        {
            g.setFont (f); g.setColour (col);
            float cx = startX;
            for (int i = 0; i < s.length(); ++i)
            {
                const auto cs = s.substring (i, i + 1);
                const float w = juce::GlyphArrangement::getStringWidth (f, cs);
                g.drawText (cs, juce::Rectangle<float> (cx, cy - f.getHeight() * 0.5f,
                                                        w + 4.0f, f.getHeight()),
                            juce::Justification::centredLeft, false);
                cx += w + tracking;
            }
            return cx;
        };
        x = drawTracked ("ZYNFORGE",  brand::type::ui (14.0f, true),  brand::textPrimary,  x, 2.0f);
        x += 6.0f;
        drawTracked      ("RECORDING", brand::type::ui (14.0f, false), brand::textTertiary, x, 2.0f);
    }

    // (The MIXER empty state is now the reusable mixerPlaceholder overlay,
    // shown/hidden by updateMixerPlaceholder() -- no painted hint here.)

    // Master separator (mock parity §4): a 1px edge rule in the gap between
    // the scrolling strip wall and the fixed master strip, so the master
    // reads as a distinct fixed pane and not just another carded strip.
    if (currentView == View::Mix && masterStrip != nullptr && masterStrip->isVisible())
    {
        const int mx = masterStrip->getX() - brand::space::md / 2;   // mid-gap
        g.setColour (brand::edge);
        g.drawVerticalLine (mx, (float) masterStrip->getY(),
                                (float) masterStrip->getBottom());
    }
}

void MainComponent::resized()
{
    // Guard against laying out stale strips: a ChannelStrip holds a
    // TrackState& that is destroyed when its track is removed
    // (setStripCount shrink). rebuildStrips normally runs on the next
    // timer tick, but a resized() in that window (e.g. a view switch)
    // would dereference a dangling reference and crash in
    // ChannelStrip::resized. Rebuild synchronously first. The
    // rebuildingStrips guard prevents recursion (rebuildStrips() ends
    // by calling resized()).
    if (! rebuildingStrips
        && (engine.getRecorder().getNumTracks()       != lastTrackCount
         || engine.getRecorder().getTrackGeneration() != lastTrackGen))
    {
        rebuildStrips();
        return;   // rebuildStrips() re-invokes resized() with fresh strips
    }

    auto r = getLocalBounds();

    // Toast anchored to bottom-right of the window -- independent of
    // the rest of the layout flow because it floats over everything.
    {
        const int tW = 320;
        const int tH = 56;
        const int margin = 18;
        toast.setBounds (getWidth() - tW - margin,
                         getHeight() - tH - margin,
                         tW, tH);
    }

    // Keep the safety controls anchored at the right. Optional device pills
    // use only space left after a readable status and sample-rate warning.
    auto row1 = r.removeFromTop (44).reduced (brand::space::lg, brand::space::md);
    // Mock parity: reserve the slot for the PAINTED wordmark lockup (badge +
    // ZYNFORGE RECORDING). The text is drawn in paint(); this just keeps the
    // status labels clear of it.
    deviceButton .setBounds (row1.removeFromRight (110).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    vscButton    .setBounds (row1.removeFromRight (84).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    addChannelButton.setBounds (row1.removeFromRight (70).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    // An empty setlist takes no row: its + Cue waits in the header with + CH.
    setlistBar.setCollapsed (cues.empty());
    if (setlistBar.isCollapsed())
    {
        setlistBar.setBounds (row1.removeFromRight (64).reduced (0, 2));
        row1.removeFromRight (brand::space::sm);
    }
    lockButton   .setBounds (row1.removeFromRight (76).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    backupButton .setBounds (row1.removeFromRight (96).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    metersButton .setBounds (row1.removeFromRight (92).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    // Over the wordmark and the status line, which is all that is left of this
    // row. An alarm moves nothing, so no fader or strip shifts under the mouse
    // at the moment a drive fails, and every header button stays reachable.
    alarmBanner.setBounds (row1.expanded (0, brand::space::xs));
    titleLabel.setBounds (row1.removeFromLeft (248));
    row1.removeFromLeft (brand::space::md);

    twinStatusLabel.setBounds (twinStatusLabel.getText().isNotEmpty()
        ? row1.removeFromLeft (juce::jmin (124, row1.getWidth() / 2)).reduced (0, brand::space::xs)
        : juce::Rectangle<int>());
    if (twinStatusLabel.isVisible()) row1.removeFromLeft (brand::space::sm);
    danteLabel.setBounds (danteLabel.getText().isNotEmpty()
        ? row1.removeFromLeft (juce::jmin (140, juce::jmax (0, row1.getWidth() - 220))).reduced (0, 4)
        : juce::Rectangle<int>());
    statusLabel.setBounds (row1);


    // Row 2 -- Transport bar | transport label | session label | view toggles
    auto row2 = r.removeFromTop (52).reduced (brand::space::lg, brand::space::xs);

    if (transportBar != nullptr)
        transportBar->setBounds (row2.removeFromLeft (340).reduced (0, 2));
    row2.removeFromLeft (brand::space::xl);


    patchButton    .setBounds (row2.removeFromRight (90).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    editViewButton .setBounds (row2.removeFromRight (60).reduced (0, 2));
    row2.removeFromRight (brand::space::xs);
    mixViewButton  .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (brand::space::xl);

    // Strip width and GRID shape the MIXER and nothing else.
    const bool mixerOnly = currentView == View::Mix;
    for (auto* b : { &stripLButton, &stripMButton, &stripSButton, &stripXsButton, &gridButton })
        b->setVisible (mixerOnly);
    if (mixerOnly)
    {
        stripLButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
        stripMButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
        stripSButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
        stripXsButton.setBounds (row2.removeFromRight (32).reduced (0, 2));
        row2.removeFromRight (brand::space::sm);
        gridButton   .setBounds (row2.removeFromRight (50).reduced (0, 2));
        row2.removeFromRight (brand::space::lg);
    }

    stripXsButton.setToggleState (stripWidthPreset == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (stripWidthPreset == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (stripWidthPreset == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (stripWidthPreset == StripWidth::L,  juce::dontSendNotification);
    gridButton   .setToggleState (mixerGridView, juce::dontSendNotification);
    // Under the clock, which nothing on this row can squeeze: the session
    // label beside it can be left with no room at all.
    {
        auto clockColumn = row2.removeFromLeft (140);
        transportLabel.setBounds (clockColumn.removeFromTop (clockColumn.getHeight() - 18));
        deviceLabel   .setBounds (clockColumn);
    }
    // The two pills share one column and stack when both show. Each warns about
    // what happens next, so neither may squeeze the other out.
    const bool punchUp = punchPill.isVisible(), deskUp = soundcheckPill.isVisible();
    auto pills = punchUp || deskUp ? row2.removeFromLeft (juce::jmin (kPillW, row2.getWidth()))
                                   : juce::Rectangle<int>();
    if (punchUp || deskUp) row2.removeFromLeft (brand::space::sm);
    if (punchUp && deskUp)
    {
        punchPill     .setBounds (pills.removeFromTop (pills.getHeight() / 2).withTrimmedBottom (1));
        soundcheckPill.setBounds (pills.withTrimmedTop (1));
    }
    else
    {
        punchPill     .setBounds (punchUp ? pills.reduced (0, brand::space::sm) : juce::Rectangle<int>());
        soundcheckPill.setBounds (deskUp  ? pills.reduced (0, brand::space::sm) : juce::Rectangle<int>());
    }
    sessionLabel  .setBounds (row2);

    // Tracks are what a laptop screen is for, so this row stays short, more so
    // in EDIT, which also puts its tools here instead of on a row of their own.
    const int clockRowH = currentView == View::Edit ? 56 : 72;
    auto clockRow = r.removeFromTop (clockRowH).reduced (brand::space::lg, brand::space::xs);
    // Wide enough for the loudness line at every window size, and the same
    // width rolling or not, so RECORD doesn't move the clock.
    perfDashboard.setBounds (clockRow.removeFromRight (332));
    clockRow.removeFromRight (brand::space::sm);
    auto* editToolsBar = (editPage != nullptr) ? editPage->getEditToolsBar() : nullptr;
    if (editToolsBar != nullptr)
    {
        const bool showTools = editToolsBar->isVisible();
        editToolsBar->setBounds (showTools ? clockRow.removeFromRight (kEditToolsW) : juce::Rectangle<int>());
        if (showTools) clockRow.removeFromRight (brand::space::sm);
    }
    bigClock.setBounds (clockRow);

    if (setlistBar.isCollapsed())
    {
        nextCueLabel.setBounds ({});
    }
    else
    {
        auto bar = r.removeFromTop (36).reduced (12, 2);
        nextCueLabel.setBounds (bar.removeFromRight (200).reduced (brand::space::xs, brand::space::xs));
        bar.removeFromRight (brand::space::sm);
        setlistBar.setBounds (bar);
    }

    // Per-preset strip sizing. floorW = minimum width (so the strip stays
    // readable); ceilW = MAXIMUM width so strips don't stretch wastefully wide
    // on a big window -- M now stays compact (≤140) so a normal window fits
    // its full 12 per page without scrolling. The channel strip hides its dB
    // ruler below ~100px (the meter scale suffices) to keep the fader usable
    // at these narrow widths while still leaving room for the name.
    int targetPerPage = 12;
    int floorW = 70;
    int ceilW  = 120;
    switch (stripWidthPreset)
    {
        case StripWidth::XS: targetPerPage = 24; floorW = 48;  ceilW = 78;  break;
        case StripWidth::S:  targetPerPage = 16; floorW = 58;  ceilW = 100; break;
        case StripWidth::M:  targetPerPage = 12; floorW = 70;  ceilW = 120; break;
        case StripWidth::L:  targetPerPage = 8;  floorW = 150; ceilW = 280; break;
    }
    const int kStripsPerPage = targetPerPage;
    const int kMinStripW     = floorW;
    const int kMaxStripW     = ceilW;
    const int margin = 12;
    const int gap    = 4;   // tighter carded-strip gap (was 8) -- packs more in
    const int total  = (int) strips.size();

    auto viewportArea = r.reduced (margin);

    const int masterW = 140;
    if (masterStrip != nullptr)
    {
        masterStrip->setVisible (currentView == View::Mix);
        if (currentView == View::Mix)
        {
            auto masterArea = viewportArea.removeFromRight (masterW);
            viewportArea.removeFromRight (brand::space::md);
            masterStrip->setBounds (masterArea);
        }
        else
        {
            masterStrip->setBounds ({});
        }
    }

    stripsViewport.setBounds (viewportArea);
    mixerPlaceholder.setBounds (viewportArea);   // overlay the MIXER strip area
    if (editPage != nullptr)
        editPage->setBounds (viewportArea);

    const int pageW = viewportArea.getWidth();

    if (mixerGridView && total > 0)
    {
        // ── Grid / table view ────────────────────────────────────────────
        // 12 strips per row, 2 rows visible at a time (24 faders on a page);
        // more than 24 strips scroll VERTICALLY. Each strip fills 1/12 of the
        // width and half the viewport height.
        const int cols  = 12;
        const int rows  = (total + cols - 1) / cols;
        const int gridW = (pageW - (cols - 1) * gap) / cols;
        // Two rows when they fit. On a smaller window a row keeps what a strip
        // needs, and the viewport scrolls to the rest.
        const int rowH  = juce::jmax (zynforge::ChannelStrip::heightNeededAt (gridW),
                                      (viewportArea.getHeight() - gap) / 2);
        const int containerH = rows * rowH + (rows - 1) * gap;

        stripsViewport.setScrollBarsShown (true, false);   // vertical scroll
        stripsContainer.setSize (pageW, juce::jmax (containerH, viewportArea.getHeight()));

        for (int i = 0; i < total; ++i)
        {
            const int rr = i / cols, cc = i % cols;
            strips[(std::size_t) i]->setBounds (cc * (gridW + gap),
                                                rr * (rowH + gap),
                                                gridW, rowH);
        }
    }
    else
    {
        // ── Single horizontal row (width-preset driven) ──────────────────
        stripsViewport.setScrollBarsShown (false, true);   // horizontal scroll
        const int stripW = juce::jlimit (kMinStripW, kMaxStripW,
                                         (pageW - (kStripsPerPage - 1) * gap) / kStripsPerPage);

        const int containerW = total > 0
                                ? total * stripW + (total - 1) * gap
                                : pageW;
        stripsContainer.setSize (juce::jmax (containerW, pageW),
                                  viewportArea.getHeight());

        int x = 0;
        for (int i = 0; i < total; ++i)
        {
            strips[(std::size_t) i]->setBounds (x, 0, stripW,
                                                stripsContainer.getHeight());
            x += stripW + gap;
        }
    }
}
