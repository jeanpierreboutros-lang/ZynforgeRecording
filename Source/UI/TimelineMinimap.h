#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <functional>

namespace zynforge
{
    // Thin overview navigator for the EDIT view. Shows the whole session
    // timeline with a draggable "viewport" box marking the visible window,
    // so the engineer can jump anywhere in a long capture at a glance --
    // standard DAW minimap. Only shown when zoomed in (content wider than
    // the viewport); EditPage polls it from its timer with the live scroll
    // geometry and routes clicks back to the viewport.
    class TimelineMinimap final : public juce::Component
    {
    public:
        TimelineMinimap()
        {
            setTitle ("Timeline overview");
            setDescription ("Drag to scroll the EDIT timeline");
        }

        // contentW = full scrollable width; headerW = left header column;
        // viewX / viewW = the visible window in content coordinates.
        void setView (int contentW_, int headerW_, int viewX_, int viewW_)
        {
            if (contentW_ == contentW && headerW_ == headerW
                && viewX_ == viewX && viewW_ == viewW) return;
            contentW = juce::jmax (1, contentW_);
            headerW  = headerW_;
            viewX    = viewX_;
            viewW    = juce::jmax (1, viewW_);
            repaint();
        }

        // Fired with the desired content X for the LEFT of the view.
        std::function<void (int)> onScrollToContentX;

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (brand::bgDeep);
            g.fillRoundedRectangle (r, brand::radius::sm);
            g.setColour (brand::edge);
            g.drawRoundedRectangle (r.reduced (0.5f), brand::radius::sm, 1.0f);

            const auto box = viewBox();
            g.setColour (brand::toolActive().withAlpha (zynforge::brand::alpha::subtle));
            g.fillRoundedRectangle (box, brand::radius::sm);
            g.setColour (brand::toolActive());
            g.drawRoundedRectangle (box.reduced (0.5f), brand::radius::sm, 1.2f);
        }

        // The box: the part of the lane the engineer can see. The row header is
        // pinned over the first headerW px of the view (TrackRow::headerOriginX
        // == viewX), so the lane on screen is content x [viewX + headerW,
        // viewX + viewW], not [max (headerW, viewX), ...], which slid the box a
        // header's width left of the audio once the view scrolled.
        juce::Rectangle<float> viewBox() const
        {
            const float w = (float) juce::jmax (1, getWidth());
            auto toMM = [&] (int contentX)
            { return juce::jlimit (0.0f, w, (float) (contentX - laneLeft()) / (float) laneWidth() * w); };
            const float xL = toMM (viewX + headerW);
            const float xR = juce::jmax (xL, toMM (viewX + viewW));
            return { xL, 1.0f, juce::jmax (3.0f, xR - xL), (float) getHeight() - 2.0f };
        }

        void mouseDown (const juce::MouseEvent& e) override { scrollTo (e.x); }
        void mouseDrag (const juce::MouseEvent& e) override { scrollTo (e.x); }

    private:
        // The lane maps the timeline across content x [headerW + inset,
        // contentW - inset], TrackRow's inner rect and EditPage::scrollToSample's.
        int laneLeft()  const noexcept { return headerW + brand::space::editWaveInset; }
        int laneWidth() const noexcept { return juce::jmax (1, contentW - headerW - 2 * brand::space::editWaveInset); }

        // The clicked time goes to the middle of the lane right of the header,
        // as scrollToSample centres a marker jump. Centring it in the whole view
        // put it half a header's width left of the lane's middle.
        void scrollTo (int mmX)
        {
            const double frac = juce::jlimit (0.0, 1.0, (double) mmX / (double) juce::jmax (1, getWidth()));
            const int contentX = laneLeft() + (int) (frac * (double) laneWidth());
            const int laneMiddleInView = headerW + juce::jmax (0, viewW - headerW) / 2;
            const int leftX = juce::jmax (0, contentX - laneMiddleInView);
            if (onScrollToContentX) onScrollToContentX (leftX);
        }

        int contentW { 1 }, headerW { brand::space::editHeaderW }, viewX { 0 }, viewW { 1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineMinimap)
    };
}
