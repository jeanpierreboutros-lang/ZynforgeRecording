#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <vector>

#include "../Audio/DisplayText.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // The main window's loud line. An engineer mid-show watches the strips, not
    // the header, so whatever threatens a take shows here, the one bright fill
    // on screen. It lies over the header's wordmark and status text and takes no
    // clicks, so it never moves a fader or stands between the engineer and STOP.
    //
    // It holds every alarm at once, danger first. What fits stays on screen;
    // when not everything fits it takes turns, so the banner over the header
    // can never be the reason something goes unseen.
    class AlarmBanner final : public juce::Component
    {
    public:
        enum class Level { Notice, Danger };
        struct Alarm { juce::String text; Level level { Level::Danger }; };

        static constexpr juce::uint32 kPageMs = 3000;

        AlarmBanner()
        {
            setInterceptsMouseClicks (false, false);
            setTitle ("Capture alarm");
            setAccessible (true);
        }

        // `alarms` is what stands now and `nowMs` drives the turns, so the
        // caller's 10 Hz tick is the only clock. A repeat of the same list costs
        // nothing unless a turn is due.
        void show (std::vector<Alarm> alarms, juce::uint32 nowMs)
        {
            std::stable_partition (alarms.begin(), alarms.end(),
                                   [] (const Alarm& a) { return a.level == Level::Danger; });
            juce::StringArray texts;
            for (const auto& alarm : alarms) texts.add (alarm.text);
            const auto newText = texts.joinIntoString (kJoin);
            const auto newLevel = ! alarms.empty() && alarms.front().level == Level::Danger
                                      ? Level::Danger : Level::Notice;

            if (newText != text || newLevel != level)
            {
                const bool announce = newText.isNotEmpty() && newText != text;
                const bool more = texts.size() > shown.size();
                text = newText;
                level = newLevel;
                shown = texts;
                repaginate();
                // Something new goes in danger-first order, so the turns start over.
                if (more) { page = 0; pageShownAtMs = nowMs; }
                setDescription (text);
                setVisible (text.isNotEmpty());
                if (isVisible()) toFront (false);
                repaint();
                if (announce)
                    juce::AccessibilityHandler::postAnnouncement (
                        text, juce::AccessibilityHandler::AnnouncementPriority::high);
            }

            if (pages.size() > 1 && nowMs - pageShownAtMs >= kPageMs)
            {
                page = (page + 1) % pages.size();
                pageShownAtMs = nowMs;
                repaint();
            }
        }

        // A refused press is answered now: its alarm goes on screen at once
        // instead of waiting its turn.
        void showPageHolding (const juce::String& wanted, juce::uint32 nowMs)
        {
            for (int i = 0; i < pages.size(); ++i)
                if (pages[i].contains (wanted) || wanted.startsWith (pages[i]))
                {
                    if (i != page) repaint();
                    page = i;
                    pageShownAtMs = nowMs;
                    return;
                }
        }

        // Every alarm standing, whatever is on screen at this moment.
        const juce::String& getText() const noexcept { return text; }
        // The page on screen at this moment.
        juce::String getShownText() const { return pages[page]; }
        int getPageCount() const noexcept { return pages.size(); }
        int getPageIndex() const noexcept { return page; }
        // Danger while any alarm is. The fill stays put through the turns: a
        // banner flashing between red and amber would say the trouble came and went.
        Level getLevel() const noexcept { return level; }

        void resized() override { repaginate(); }

        void paint (juce::Graphics& g) override
        {
            const auto fill = level == Level::Danger ? brand::accentRecord : brand::alertAmber;
            g.setColour (fill);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), brand::radius::md);
            g.setColour (brand::onSignal (fill));
            auto area = getLocalBounds().reduced (brand::space::lg, 0);
            if (pages.size() > 1)
            {
                g.setFont (brand::type::uiLabel());
                g.drawText (juce::String (page + 1) + " of " + juce::String (pages.size()),
                            area.removeFromRight (kCounterW), juce::Justification::centredRight, false);
            }
            g.setFont (brand::type::headline());
            g.drawFittedText (typeset (getShownText()), area, juce::Justification::centredLeft,
                              kLines, kMinScale);
        }

        // A string of alarms as updateCaptureWarnings builds it: each is
        // followed by two spaces.
        static juce::StringArray splitAlarms (const juce::String& joined)
        {
            juce::StringArray parts;
            auto rest = joined.trim();
            while (rest.isNotEmpty())
            {
                const int gap = rest.indexOf (kJoin);
                parts.add (gap < 0 ? rest : rest.substring (0, gap).trimEnd());
                rest = gap < 0 ? juce::String() : rest.substring (gap).trimStart();
            }
            return parts;
        }

        // What one screen of the banner can hold, at a text width in pixels.
        // Alarms stay whole and in order, and share a page while they fit. One
        // too long for a page alone is cut between words and runs on.
        static juce::StringArray paginate (const juce::StringArray& alarms, float width)
        {
            juce::StringArray pages;
            if (width <= 0.0f)
            {
                if (! alarms.isEmpty()) pages.add (alarms.joinIntoString (kJoin));
                return pages;
            }
            const auto font = brand::type::headline();
            const auto fits = [&font, width] (const juce::String& s)
            {
                return font.getStringWidthFloat (typeset (s)) <= width * kWidthsPerPage;
            };

            juce::String building;
            for (auto alarm : alarms)
            {
                while (alarm.isNotEmpty())
                {
                    const auto joined = building.isEmpty() ? alarm : building + kJoin + alarm;
                    if (fits (joined)) { building = joined; break; }
                    if (building.isNotEmpty()) { pages.add (building); building.clear(); continue; }
                    const int cut = cutPoint (alarm, fits);
                    pages.add (alarm.substring (0, cut).trimEnd());
                    alarm = alarm.substring (cut).trimStart();
                }
            }
            if (building.isNotEmpty()) pages.add (building);
            return pages;
        }

    private:
        static constexpr const char* kJoin = "  ";
        static constexpr int   kLines = 2;
        static constexpr float kMinScale = 0.75f;
        // kLines lines squeezed to kMinScale, less what wrapping leaves at each
        // line's end. Past this, text would end in an ellipsis.
        static constexpr float kWidthsPerPage = kLines * 0.85f / kMinScale;
        static constexpr int   kCounterW = 60;    // "12 of 12"

        // The longest head of `text` that fits, ending at a word if there is one.
        template <typename Fits>
        static int cutPoint (const juce::String& text, const Fits& fits)
        {
            int cut = 1;
            for (int end = 1; end < text.length() && fits (text.substring (0, end)); ++end) cut = end;
            const int space = text.substring (0, cut + 1).lastIndexOfChar (' ');
            return space > 0 ? space : cut;
        }

        void repaginate()
        {
            const auto full = (float) (getWidth() - 2 * brand::space::lg);
            pages = paginate (shown, full);
            if (pages.size() > 1) pages = paginate (shown, full - (float) kCounterW);
            page = juce::jlimit (0, juce::jmax (0, pages.size() - 1), page);
        }

        juce::String text;
        Level level { Level::Danger };
        juce::StringArray shown, pages;
        int page { 0 };
        juce::uint32 pageShownAtMs { 0 };
    };
}
