#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <deque>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Toast -- non-modal notification pill that slides in from the
    // bottom-right of its host, holds for ~3 s, then fades out. Calls
    // are queued, so two toasts fired back-to-back display in order
    // rather than stomping each other.
    //
    // Add a single instance to a host component (e.g. MainComponent),
    // position it via setBounds in resized(), and call
    // toast.show("Cue 4 added") from anywhere. Painted on top of
    // siblings via setAlwaysOnTop (true).
    class Toast final : public juce::Component, private juce::Timer
    {
    public:
        enum class Kind { Info, Success, Warning };

        Toast()
        {
            setAlwaysOnTop (true);
            setInterceptsMouseClicks (false, false);
            setVisible (false);
        }

        void show (const juce::String& message, Kind kind = Kind::Info)
        {
            queue.push_back ({ message, kind });
            if (! visible)
                advance();
        }

        void paint (juce::Graphics& g) override
        {
            if (current.message.isEmpty()) return;

            auto r = getLocalBounds().toFloat().reduced (4.0f);
            const auto base = colourForKind (current.kind);

            // Drop shadow (elev3 -- toasts float above all chrome).
            g.setColour (brand::shadow::elev3());
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (1.0f), brand::radius::lg);

            // Body -- gradient pill in the kind-tinted colour.
            g.setGradientFill (juce::ColourGradient (
                base.brighter (0.25f), r.getCentreX(), r.getY(),
                base.darker   (0.20f), r.getCentreX(), r.getBottom(),
                false));
            g.fillRoundedRectangle (r, brand::radius::lg);

            // 6 px accent stripe down the left edge -- colour-coded by Kind.
            const auto accent = accentForKind (current.kind);
            g.setColour (accent);
            g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), r.getY(), 4.0f, r.getHeight()),
                                    brand::radius::sm);

            g.setColour (base.brighter (0.45f).withAlpha (0.45f));
            g.drawRoundedRectangle (r, brand::radius::lg, 1.0f);

            // Message text.
            g.setColour (brand::textPrimary);
            g.setFont (brand::type::uiLabel());
            g.drawText (current.message,
                        r.withTrimmedLeft (14).toNearestInt(),
                        juce::Justification::centredLeft, true);
        }

    private:
        struct Entry { juce::String message; Kind kind; };

        std::deque<Entry> queue;
        Entry             current;
        bool              visible        { false };
        int               holdMsLeft     { 0 };
        float             fadeT          { 0.0f };
        enum class Phase  { In, Hold, Out, Idle };
        Phase             phase          { Phase::Idle };

        static constexpr int  kTickMs        = 16;       // ~60 Hz
        static constexpr int  kFadeInMs      = 200;
        static constexpr int  kFadeOutMs     = 320;
        static constexpr int  kHoldMs        = 2800;

        static juce::Colour colourForKind (Kind k)
        {
            switch (k)
            {
                case Kind::Success: return brand::accentPlay   .darker (0.30f);
                case Kind::Warning: return brand::alertAmber   .darker (0.30f);
                case Kind::Info:
                default:            return brand::bgElevated;
            }
        }
        static juce::Colour accentForKind (Kind k)
        {
            switch (k)
            {
                case Kind::Success: return brand::accentPlay;
                case Kind::Warning: return brand::alertAmber;
                case Kind::Info:
                default:            return brand::featureEngaged;
            }
        }

        void advance()
        {
            if (queue.empty()) { visible = false; setVisible (false); return; }
            current = queue.front();
            queue.pop_front();
            visible = true;
            setVisible (true);
            fadeT   = 0.0f;
            phase   = Phase::In;
            setAlpha (0.0f);
            startTimer (kTickMs);
            repaint();
        }

        void timerCallback() override
        {
            switch (phase)
            {
                case Phase::In:
                {
                    fadeT += (float) kTickMs / (float) kFadeInMs;
                    if (fadeT >= 1.0f) { fadeT = 1.0f; phase = Phase::Hold; holdMsLeft = kHoldMs; }
                    setAlpha (juce::jlimit (0.0f, 1.0f, fadeT));
                    break;
                }
                case Phase::Hold:
                {
                    holdMsLeft -= kTickMs;
                    if (holdMsLeft <= 0) { phase = Phase::Out; fadeT = 1.0f; }
                    break;
                }
                case Phase::Out:
                {
                    fadeT -= (float) kTickMs / (float) kFadeOutMs;
                    if (fadeT <= 0.0f)
                    {
                        fadeT = 0.0f;
                        setAlpha (0.0f);
                        stopTimer();
                        advance();
                        return;
                    }
                    setAlpha (juce::jlimit (0.0f, 1.0f, fadeT));
                    break;
                }
                case Phase::Idle: stopTimer(); break;
            }
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Toast)
    };
}
