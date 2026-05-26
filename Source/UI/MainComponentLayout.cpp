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
    g.setColour (brand::edge);
    g.drawHorizontalLine ((int) header.getBottom() - 1,
                          header.getX(), header.getRight());

    // (The MIXER empty state is now the reusable mixerPlaceholder overlay,
    // shown/hidden by updateMixerPlaceholder() -- no painted hint here.)
}

void MainComponent::resized()
{
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
    titleLabel   .setBounds ({});
    row1.removeFromLeft (brand::space::md);
    midiStatusLabel.setBounds (row1.removeFromLeft (220).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    danteLabel.setBounds (row1.removeFromLeft (180).reduced (0, 4));
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

    stripXsButton.setToggleState (stripWidthPreset == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (stripWidthPreset == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (stripWidthPreset == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (stripWidthPreset == StripWidth::L,  juce::dontSendNotification);
    row2.removeFromRight (brand::space::lg);
    transportLabel.setBounds (row2.removeFromLeft (140));
    sessionLabel  .setBounds (row2);

    oscButton    .setBounds ({});
    playButton   .setBounds ({});
    stopButton   .setBounds ({});

    auto clockRow = r.removeFromTop (96).reduced (brand::space::lg, brand::space::sm);
    perfDashboard.setBounds (clockRow.removeFromRight (240).reduced (brand::space::xs, brand::space::xs));
    bigClock.setBounds (clockRow);

    auto bar = r.removeFromTop (36).reduced (12, 2);
    tempoBar  .setBounds (bar.removeFromRight (320));
    bar.removeFromRight (brand::space::md);
    nextCueLabel.setBounds (bar.removeFromRight (200).reduced (brand::space::xs, brand::space::xs));
    bar.removeFromRight (brand::space::sm);
    setlistBar.setBounds (bar);

    if (timeline != nullptr)
        timeline->setBounds ({});

    int targetPerPage = 12;
    int floorW = 90;
    switch (stripWidthPreset)
    {
        case StripWidth::XS: targetPerPage = 24; floorW = 56;  break;
        case StripWidth::S:  targetPerPage = 16; floorW = 72;  break;
        case StripWidth::M:  targetPerPage = 12; floorW = 90;  break;
        case StripWidth::L:  targetPerPage = 8;  floorW = 130; break;
    }
    const int kStripsPerPage = targetPerPage;
    const int kMinStripW     = floorW;
    const int kMaxStripW     = 220;
    const int margin = 12;
    const int gap    = 6;
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

    {
        const int tallyH = 4;
        const auto tallyBounds = viewportArea.withHeight (tallyH);
        viewportArea = viewportArea.withTrimmedTop (tallyH);
        if (peakTally != nullptr)
            peakTally->setBounds (tallyBounds);
    }

    stripsViewport.setBounds (viewportArea);
    mixerPlaceholder.setBounds (viewportArea);   // overlay the MIXER strip area
    if (editPage != nullptr)
        editPage->setBounds (viewportArea);

    const int pageW  = viewportArea.getWidth();
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
