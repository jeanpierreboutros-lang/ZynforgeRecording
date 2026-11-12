#pragma once

#include "../Audio/DisplayText.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <deque>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Toast -- non-modal notification pill that fades in at the
    // bottom-right of its host, holds for ~3 s, then fades out. Calls
    // are queued, so two toasts fired back-to-back display in order
    // rather than stomping each other.
    //
    // The host gives it a slot (setBounds in resized()): the slot's width and
    // bottom-right corner hold, and a message that needs more lines grows the
    // toast upward, so a crash-recovery notice never loses its second half.
    // A notice that only counts on from one showing or waiting ("3 strips
    // selected", then "4 strips selected") takes its place, and at most
    // kMaxWaiting wait: neutral news is dropped first, never an error for it.
    // Painted on top of siblings via setAlwaysOnTop (true).
    class Toast final : public juce::Component, private juce::Timer
    {
    public:
        // Info  -- neutral feedback (cue added, exported).
        // Success -- a thing completed cleanly (take saved, mix bounced).
        // Warning -- recoverable / "are you sure" (tap STOP again).
        // Error -- a hard failure the engineer must see (disk full, device
        //          lost, primary writer failed). Red so it reads as danger
        //          even at the edge of vision.
        enum class Kind { Info, Success, Warning, Error };

        static constexpr int kMaxWaiting = 3;

        Toast()
        {
            setAlwaysOnTop (true);
            setInterceptsMouseClicks (false, false);
            setVisible (false);
        }

        void show (const juce::String& message, Kind kind = Kind::Info)
        {
            if (message.isEmpty()) return;
            // The same notice, or the same one counting on: the newer says it.
            if (visible && sameNotice (current, message, kind))
            {
                current.message = message;
                fitToMessage();
                if (phase == Phase::Hold || phase == Phase::Out) { phase = Phase::Hold; holdMsLeft = holdFor (current); setAlpha (1.0f); }
                repaint();
                return;
            }
            for (auto& waiting : queue)
                if (sameNotice (waiting, message, kind)) { waiting.message = message; return; }

            queue.push_back ({ message, kind });
            while ((int) queue.size() > kMaxWaiting) dropLeastUrgent();
            if (! visible)
                advance();
        }

        // For a prompt that is out of date by the time the queue reaches it:
        // it replaces what is showing and goes ahead of everything waiting.
        void showNow (const juce::String& message, Kind kind = Kind::Info)
        {
            queue.push_front ({ message, kind });
            advance();
        }

        const juce::String& getCurrentMessage() const noexcept { return current.message; }
        // How many messages wait behind the one showing.
        int waitingCount() const noexcept { return (int) queue.size(); }
        bool isWaiting (const juce::String& message) const
        {
            return std::any_of (queue.begin(), queue.end(), [&] (const Entry& e) { return e.message == message; });
        }
        // Whether the message showing is drawn whole, nothing cut off.
        bool showsWholeMessage() const
        {
            return layoutFor (current.message, (float) textArea().getWidth()).getHeight()
                   <= (float) textArea().getHeight() + 0.5f;
        }

        void resized() override
        {
            if (fitting) return;
            slot = getBounds();   // where the host wants it: width and bottom-right corner
            fitToMessage();
        }

        void paint (juce::Graphics& g) override
        {
            if (current.message.isEmpty()) return;

            auto r = getLocalBounds().toFloat().reduced (4.0f);
            const auto base = colourForKind (current.kind);

            // Drop shadow (elev3 -- toasts float above all chrome).
            g.setColour (brand::shadow::elev3());
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (1.0f), brand::radius::lg);

            // Body -- FLAT solid pill in the kind-tinted colour (no gradient).
            g.setColour (base);
            g.fillRoundedRectangle (r, brand::radius::lg);

            // 4 px accent stripe down the left edge -- colour-coded by Kind.
            const auto accent = accentForKind (current.kind);
            g.setColour (accent);
            g.fillRoundedRectangle (juce::Rectangle<float> (r.getX(), r.getY(), 4.0f, r.getHeight()),
                                    brand::radius::sm);

            g.setColour (brand::lift (base, 0.45f).withAlpha (zynforge::brand::alpha::scrim));
            g.drawRoundedRectangle (r, brand::radius::lg, 1.0f);

            // Message text, wrapped by word and centred in its area.
            const auto area = textArea().toFloat();
            const auto layout = layoutFor (current.message, area.getWidth());
            layout.draw (g, area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), layout.getHeight())));
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
        juce::Rectangle<int> slot;
        bool              fitting        { false };

        static constexpr int  kTickMs        = 16;       // ~60 Hz
        // Motion timings come from the shared brand::motion::
        // namespace so the toast picks up any future "make the
        // app feel snappier" pass without touching this file.
        static constexpr int  kFadeInMs      = brand::motion::quickFadeMs;
        static constexpr int  kFadeOutMs     = brand::motion::fadeOutMs;
        static constexpr int  kHoldMs        = brand::motion::toastHoldMs;
        static constexpr int  kInsetX = 4 + 14, kInsetRight = 4 + 10, kInsetY = 4 + 8;
        static constexpr int  kMaxHeight = 200;

        juce::Rectangle<int> textArea() const
        {
            return getLocalBounds().withTrimmedLeft (kInsetX).withTrimmedRight (kInsetRight).reduced (0, kInsetY);
        }

        static juce::TextLayout layoutFor (const juce::String& message, float width)
        {
            juce::AttributedString text;
            text.append (typeset (message), brand::type::uiLabel(), brand::textPrimary);
            text.setWordWrap (juce::AttributedString::byWord);
            text.setJustification (juce::Justification::centredLeft);
            juce::TextLayout layout;
            layout.createLayout (text, juce::jmax (1.0f, width));
            return layout;
        }

        // Grows up from the host's slot as far as the message needs.
        void fitToMessage()
        {
            if (slot.isEmpty()) return;
            const float textW = (float) (slot.getWidth() - kInsetX - kInsetRight);
            const int needed = (int) std::ceil (layoutFor (current.message, textW).getHeight()) + 2 * kInsetY;
            const int h = juce::jlimit (slot.getHeight(), juce::jmax (slot.getHeight(), kMaxHeight), needed);
            const juce::ScopedValueSetter<bool> guard (fitting, true);
            setBounds (slot.withTop (slot.getBottom() - h));
        }

        // Two notices are one when they differ only in their numbers (and the
        // plural those bring): the later one is what is true now.
        static juce::String noticeKey (const juce::String& message)
        {
            juce::StringArray words;
            words.addTokens (message.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz -:;,.!'()/"), " ", "");
            words.removeEmptyStrings();
            for (auto& word : words)
                if (word.length() > 3 && word.endsWithChar ('s')) word = word.dropLastCharacters (1);
            return words.joinIntoString (" ");
        }

        static bool sameNotice (const Entry& entry, const juce::String& message, Kind kind)
        {
            return entry.kind == kind && noticeKey (entry.message) == noticeKey (message);
        }

        static int urgency (Kind k) { return k == Kind::Error ? 2 : k == Kind::Warning ? 1 : 0; }

        // The oldest of the least urgent goes, so neutral news never pushes an
        // error out, and a full queue of errors drops the oldest of them.
        void dropLeastUrgent()
        {
            auto victim = queue.begin();
            for (auto it = queue.begin(); it != queue.end(); ++it)
                if (urgency (it->kind) < urgency (victim->kind)) victim = it;
            queue.erase (victim);
        }

        // A notice that wraps stays up longer, so it can be read to the end.
        int holdFor (const Entry& entry) const
        {
            const auto lines = layoutFor (entry.message, (float) juce::jmax (1, textArea().getWidth())).getNumLines();
            return kHoldMs + juce::jmax (0, lines - 1) * 1200;
        }

        static juce::Colour colourForKind (Kind k)
        {
            switch (k)
            {
                case Kind::Success: return brand::sink (brand::accentPlay,   0.30f);
                case Kind::Warning: return brand::sink (brand::alertAmber,   0.30f);
                case Kind::Error:   return brand::sink (brand::accentRecord, 0.30f);
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
                case Kind::Error:   return brand::accentRecord;
                case Kind::Info:
                default:            return brand::featureEngaged;
            }
        }

        void advance()
        {
            if (queue.empty()) { visible = false; setVisible (false); return; }
            current = queue.front();
            queue.pop_front();
            fitToMessage();
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
                    if (fadeT >= 1.0f) { fadeT = 1.0f; phase = Phase::Hold; holdMsLeft = holdFor (current); }
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
