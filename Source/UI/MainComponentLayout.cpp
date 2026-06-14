// MainComponent's paint + resized methods. Extracted from
// MainComponent.cpp as part of the 2026-05-24 god-class split so the
// layout math is editable without scrolling past the other 5000 lines.

#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "EditToolsBar.h"

using namespace zynforge;

void MainComponent::paint (juce::Graphics& g)
{
    // Whole-window vertical gradient -- top sits 6% above bgDeep, bottom
    // sits 4% below. Subtle enough that the engineer reads it as 'deep
    // panel' rather than 'banded background', but it stops the canvas
    // feeling like a flat sheet of paint.
    {
        auto fullBounds = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (
            brand::bgDeep.brighter (0.06f), fullBounds.getCentreX(), fullBounds.getY(),
            brand::bgDeep.darker   (0.04f), fullBounds.getCentreX(), fullBounds.getBottom(),
            false));
        g.fillRect (fullBounds);
    }

    // Header = row 1 (44 px) + row 2 transport (52 px) = 96 px total.
    auto header = getLocalBounds().removeFromTop (44 + 52).toFloat();
    g.setGradientFill (brand::verticalGradient (brand::bgPanel, header, 0.08f, 0.18f));
    g.fillRect (header);

    // Brushed-steel grain on the header plate -- faint, irregular vertical
    // striations + a top bevel sheen, so the top bar reads as machined steel
    // (the "cold steel" half of the forge identity) rather than flat paint.
    // Fixed RNG seed -> a stable grain that never flickers between repaints,
    // and alphas kept to ~0.02-0.05 so it never competes with the controls.
    {
        juce::Random rng (0x57EE15);
        const int   x0 = (int) header.getX(), x1 = (int) header.getRight();
        const float top = header.getY(), bot = header.getBottom() - 1.0f;
        for (int x = x0; x < x1; x += 2)
        {
            const float a = 0.018f + 0.03f * rng.nextFloat();
            g.setColour (rng.nextBool() ? brand::gloss (a)
                                        : brand::shadow::elev1().withAlpha (a));
            g.drawVerticalLine (x, top, bot);
        }
        // Top bevel: a bright machined sheen along the very top edge.
        g.setColour (brand::gloss (0.16f));
        g.drawHorizontalLine ((int) top, header.getX(), header.getRight());
    }

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
            g.setColour (brand::brandOrange.withAlpha (0.16f)); g.fillPath (hex);
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
                const float w = f.getStringWidthFloat (cs);
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

    // Row 1 -- title + status + LOCK + + CH + DEVICE + RECORD
    auto row1 = r.removeFromTop (44).reduced (brand::space::lg, brand::space::md);
    // Mock parity: reserve the slot for the PAINTED wordmark lockup (badge +
    // ZYNFORGE RECORDING). The text is drawn in paint(); this just keeps the
    // status labels clear of it.
    titleLabel   .setBounds (row1.removeFromLeft (248));
    row1.removeFromLeft (brand::space::md);
    midiStatusLabel.setBounds (row1.removeFromLeft (180).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    danteLabel.setBounds (row1.removeFromLeft (180).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    srWarnLabel.setBounds (row1.removeFromLeft (340).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    recordButton .setBounds ({});
    deviceButton .setBounds (row1.removeFromRight (110).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    addChannelButton.setBounds (row1.removeFromRight (70).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    lockButton   .setBounds (row1.removeFromRight (76).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    backupButton .setBounds (row1.removeFromRight (96).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    metersButton .setBounds (row1.removeFromRight (92).reduced (0, 2));
    statusLabel  .setBounds (row1);

    formatButton .setBounds ({});
    preRollButton.setBounds ({});

    // Row 2 -- Transport bar | transport label | session label | view toggles
    auto row2 = r.removeFromTop (52).reduced (brand::space::lg, brand::space::xs);

    if (transportBar != nullptr)
        transportBar->setBounds (row2.removeFromLeft (340).reduced (0, 2));
    row2.removeFromLeft (brand::space::xl);

    loadButton.setBounds ({});

    patchButton    .setBounds (row2.removeFromRight (90).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    vcaToggleButton.setBounds (row2.removeFromRight (68).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    vscButton      .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    editViewButton .setBounds (row2.removeFromRight (60).reduced (0, 2));
    row2.removeFromRight (brand::space::xs);
    mixViewButton  .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (brand::space::xl);

    stripLButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripMButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripSButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripXsButton.setBounds (row2.removeFromRight (32).reduced (0, 2));
    row2.removeFromRight (brand::space::sm);
    gridButton   .setBounds (row2.removeFromRight (50).reduced (0, 2));

    stripXsButton.setToggleState (stripWidthPreset == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (stripWidthPreset == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (stripWidthPreset == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (stripWidthPreset == StripWidth::L,  juce::dontSendNotification);
    gridButton   .setToggleState (mixerGridView, juce::dontSendNotification);
    row2.removeFromRight (brand::space::lg);
    transportLabel.setBounds (row2.removeFromLeft (140));
    sessionLabel  .setBounds (row2);

    oscButton    .setBounds ({});
    playButton   .setBounds ({});
    stopButton   .setBounds ({});

    auto clockRow = r.removeFromTop (96).reduced (brand::space::lg, brand::space::sm);
    // Give the performance dashboard more room while recording so audio
    // load / buffer / disk are readable at a glance during a take.
    const int dashW = engine.isRecording() ? 320 : 240;
    perfDashboard.setBounds (clockRow.removeFromRight (dashW).reduced (brand::space::xs, brand::space::xs));
    bigClock.setBounds (clockRow);

    auto bar = r.removeFromTop (36).reduced (12, 2);
    tempoBar  .setBounds (bar.removeFromRight (348));   // room for TAP + full CLICK label
    bar.removeFromRight (brand::space::md);
    nextCueLabel.setBounds (bar.removeFromRight (200).reduced (brand::space::xs, brand::space::xs));
    bar.removeFromRight (brand::space::sm);
    setlistBar.setBounds (bar);

    if (timeline != nullptr)
        timeline->setBounds ({});

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
        case StripWidth::L:  targetPerPage = 6;  floorW = 160; ceilW = 300; break;
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

            if (vcaPanel != nullptr && showVcaPanel)
            {
                auto vcaArea = viewportArea.removeFromRight (540);
                viewportArea.removeFromRight (brand::space::md);
                vcaPanel->setBounds (vcaArea);
                vcaPanel->setVisible (true);
            }
            else if (vcaPanel != nullptr)
            {
                vcaPanel->setBounds ({});
                vcaPanel->setVisible (false);
            }
        }
        else
        {
            masterStrip->setBounds ({});
            if (vcaPanel != nullptr) { vcaPanel->setBounds ({}); vcaPanel->setVisible (false); }
        }
    }

    auto* editToolsBar = (editPage != nullptr) ? editPage->getEditToolsBar() : nullptr;
    if (automationToolbar->isVisible())
    {
        auto topRow = viewportArea.removeFromTop (44).reduced (2, 0);
        if (editToolsBar != nullptr && editToolsBar->isVisible())
        {
            const int toolsW = 458;
            editToolsBar->setBounds (topRow.removeFromRight (toolsW));
            topRow.removeFromRight (brand::space::sm);
        }
        else if (editToolsBar != nullptr)
        {
            editToolsBar->setBounds ({});
        }
        automationToolbar->setBounds (topRow);
        viewportArea.removeFromTop (brand::space::xs);
    }
    else
    {
        automationToolbar->setBounds ({});
        if (editToolsBar != nullptr) editToolsBar->setBounds ({});
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
        const int rowH  = juce::jmax (200, (viewportArea.getHeight() - gap) / 2);
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
