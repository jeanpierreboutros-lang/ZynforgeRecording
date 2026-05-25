#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // PlaceholderView -- the one reusable "this content area has nothing
    // real to show right now" surface. It replaces the ad-hoc empty
    // labels scattered across views (EDIT's emptyLabel, etc.) with a
    // single accessible, responsive, brand-themed component that renders
    // exactly one of four states:
    //
    //   Hidden   -- nothing; real content is present (the view paints).
    //   Loading  -- pulsing dots + caption (a scan / open in progress).
    //   Empty    -- glyph + title + hint + optional call-to-action button.
    //   Error    -- warning glyph + title + detail + optional Retry button.
    //
    // Design goals it bakes in so callers don't re-solve them:
    //   * Loading / empty / error / edge (missing strings, tiny bounds)
    //     are all handled here, once.
    //   * Responsive: a centred content column (capped at 360 px) that
    //     drops the glyph on short heights and wraps long text.
    //   * Accessible: every state change sets the component's title +
    //     description and posts a VoiceOver announcement; the action is a
    //     real focusable, keyboard-activatable button. (The app shipped
    //     with zero accessibility -- this sets the baseline.)
    //   * Clean DX: three show* verbs + clear(); colours/spacing/fonts
    //     come from brand tokens, never literals.
    //
    // Usage:
    //   PlaceholderView placeholder;
    //   addAndMakeVisible (placeholder);                 // size in resized()
    //   placeholder.showEmpty ("No session loaded",
    //                          "Load or record a session to see waveforms.",
    //                          "Load session", [this] { openSession(); });
    //   placeholder.showLoading ("Scanning waveforms");
    //   placeholder.showError ("Couldn't open session", detail,
    //                          "Retry", [this] { retry(); });
    //   placeholder.clear();                             // real content present
    //
    // The view paints transparently over the host background and only
    // intercepts clicks on its action button, so it is safe to overlay a
    // populated content area and toggle with clear().
    class PlaceholderView final : public juce::Component,
                                  private juce::Timer
    {
    public:
        enum class State { Hidden, Loading, Empty, Error };

        PlaceholderView()
        {
            setInterceptsMouseClicks (false, true);   // only the button is clickable
            setFocusContainerType (juce::Component::FocusContainerType::focusContainer);

            action.setVisible (false);
            action.onClick = [this] { if (onAction) onAction(); };
            addChildComponent (action);

            setVisible (false);
        }

        // ---- API ------------------------------------------------------

        void showLoading (const juce::String& caption = "Loading")
        {
            title  = caption;
            detail = {};
            setAction ({}, {});
            enter (State::Loading);
        }

        void showEmpty (const juce::String& heading,
                        const juce::String& hint = {},
                        const juce::String& actionLabel = {},
                        std::function<void()> onActionFn = {})
        {
            title  = heading;
            detail = hint;
            setAction (actionLabel, std::move (onActionFn));
            enter (State::Empty);
        }

        void showError (const juce::String& heading,
                        const juce::String& errorDetail = {},
                        const juce::String& retryLabel = {},
                        std::function<void()> onRetryFn = {})
        {
            title  = heading;
            detail = errorDetail;
            setAction (retryLabel, std::move (onRetryFn));
            enter (State::Error);
        }

        void clear() { enter (State::Hidden); }

        State getState() const noexcept { return state; }

        // Swap the empty-state glyph for a caller-supplied painter (e.g. a
        // view-specific icon). Bounds are the glyph's square cell.
        void setEmptyIconPainter (std::function<void (juce::Graphics&, juce::Rectangle<float>)> fn)
        {
            iconPainter = std::move (fn);
            repaint();
        }

        // ---- Component ------------------------------------------------

        void resized() override
        {
            const auto L = computeLayout();
            if (! L.button.isEmpty()) action.setBounds (L.button);
        }

        void paint (juce::Graphics& g) override
        {
            if (state == State::Hidden) return;

            const auto L = computeLayout();

            if (L.showIcon)
            {
                const auto cell = L.icon.toFloat();
                if (state == State::Loading)        paintLoadingDots (g, cell);
                else if (state == State::Error)     paintErrorGlyph  (g, cell);
                else if (iconPainter)               iconPainter (g, cell);
                else                                paintEmptyGlyph  (g, cell);
            }

            if (title.isNotEmpty())
            {
                g.setColour (brand::textPrimary);
                g.setFont (brand::type::ui (16.0f, true));
                g.drawFittedText (title, L.title, juce::Justification::centred, 2);
            }

            if (detail.isNotEmpty())
            {
                g.setColour (brand::textMuted);
                g.setFont (brand::type::ui (13.0f, false));
                g.drawFittedText (detail, L.hint, juce::Justification::centredTop, 3);
            }
        }

        // Accessible role: a labelled group; its name/description come
        // from setTitle/setDescription set on each state change.
        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            return std::make_unique<juce::AccessibilityHandler> (
                *this, juce::AccessibilityRole::group);
        }

    private:
        struct Layout
        {
            juce::Rectangle<int> icon, title, hint, button;
            bool showIcon = true;
        };

        bool hasAction() const noexcept { return action.getButtonText().isNotEmpty(); }

        Layout computeLayout() const
        {
            auto area = getLocalBounds().reduced (brand::space::xl);
            const int colW = juce::jmin (area.getWidth(), 360);
            const auto col = area.withSizeKeepingCentre (colW, area.getHeight());

            Layout L;
            L.showIcon = (getHeight() >= 160);

            const int iconSz = 44;
            const int titleH = 26;
            const int gap    = brand::space::md;
            const int hintH  = detail.isNotEmpty() ? 52 : 0;
            const int btnH   = hasAction()         ? 36 : 0;

            const int totalH = (L.showIcon ? iconSz + gap : 0)
                             + titleH
                             + (hintH > 0 ? gap + hintH : 0)
                             + (btnH  > 0 ? gap + btnH  : 0);

            int y = col.getY() + juce::jmax (0, (col.getHeight() - totalH) / 2);

            if (L.showIcon)
            {
                L.icon = { col.getCentreX() - iconSz / 2, y, iconSz, iconSz };
                y += iconSz + gap;
            }
            L.title = { col.getX(), y, colW, titleH };
            y += titleH;
            if (hintH > 0) { y += gap; L.hint = { col.getX(), y, colW, hintH }; y += hintH; }
            if (btnH  > 0)
            {
                y += gap;
                const int bw = juce::jmin (colW, 200);
                L.button = { col.getCentreX() - bw / 2, y, bw, btnH };
            }
            return L;
        }

        void setAction (const juce::String& label, std::function<void()> fn)
        {
            onAction = std::move (fn);
            action.setButtonText (label);
            const bool show = label.isNotEmpty() && onAction != nullptr;
            action.setVisible (show);
            if (show) action.setEnabled (true);
        }

        void enter (State next)
        {
            state = next;

            const bool showing = (state != State::Hidden);
            setVisible (showing);
            if (state == State::Loading) startTimerHz (20);
            else                         stopTimer();

            // Accessibility: expose a readable name + post an announcement
            // so a VoiceOver user hears the change immediately.
            if (showing)
            {
                const juce::String prefix = state == State::Loading ? "Loading. "
                                          : state == State::Error   ? "Error. "
                                                                    : "";
                setTitle (title);
                setDescription (detail);
                juce::AccessibilityHandler::postAnnouncement (
                    prefix + title + (detail.isNotEmpty() ? ". " + detail : juce::String()),
                    juce::AccessibilityHandler::AnnouncementPriority::medium);
            }

            resized();
            repaint();
        }

        void timerCallback() override { phase += 0.12f; if (phase > 1.0e6f) phase = 0.0f; repaint(); }

        void paintLoadingDots (juce::Graphics& g, juce::Rectangle<float> cell) const
        {
            const float r   = 4.0f;
            const float gapX = r * 3.0f;
            const auto  c   = cell.getCentre();
            for (int i = 0; i < 3; ++i)
            {
                const float ph = phase * juce::MathConstants<float>::twoPi - (float) i * 0.6f;
                const float a  = 0.35f + 0.45f * (0.5f + 0.5f * std::sin (ph));
                g.setColour (brand::accentStatus.withAlpha (a));
                g.fillEllipse (c.x + ((float) i - 1.0f) * gapX - r, c.y - r, r * 2.0f, r * 2.0f);
            }
        }

        void paintEmptyGlyph (juce::Graphics& g, juce::Rectangle<float> cell) const
        {
            auto box = cell.reduced (cell.getWidth() * 0.12f);
            g.setColour (brand::textTertiary);
            g.drawRoundedRectangle (box, brand::radius::md, 2.0f);
            // a single centred lane line -- reads as "an empty track lane"
            g.drawLine (box.getX() + box.getWidth() * 0.2f, box.getCentreY(),
                        box.getRight() - box.getWidth() * 0.2f, box.getCentreY(), 2.0f);
        }

        void paintErrorGlyph (juce::Graphics& g, juce::Rectangle<float> cell) const
        {
            auto box = cell.reduced (cell.getWidth() * 0.12f);
            g.setColour (brand::accentStatus);
            g.drawEllipse (box, 2.0f);
            g.setFont (brand::type::mono (cell.getHeight() * 0.55f, true));
            g.drawText ("!", box.toNearestInt(), juce::Justification::centred);
        }

        State state { State::Hidden };
        juce::String title, detail;
        std::function<void()> onAction;
        std::function<void (juce::Graphics&, juce::Rectangle<float>)> iconPainter;
        juce::TextButton action;
        float phase { 0.0f };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaceholderView)
    };
}
