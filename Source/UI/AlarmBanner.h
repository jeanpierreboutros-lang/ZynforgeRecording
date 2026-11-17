#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "../Audio/DisplayText.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // The main window's loud line. An engineer mid-show watches the strips, not
    // the header, so whatever threatens a take shows here, the one bright fill
    // on screen. It lies over the header's wordmark and status text only, never
    // over the transport or a header button, so raising it moves nothing and a
    // click on it can only open its list or bring a faulted strip into view.
    //
    // One alarm per line, the most serious first, each in its own colour (red
    // danger, amber notice) behind a drawn warning mark. As many lines as the
    // height holds stay on screen; when some don't fit the last line ends
    // "+N more", and a click lists every alarm standing. A long alarm ends in
    // an ellipsis rather than wrapping; the list has it whole. Nothing moves:
    // no turns, no flashing.
    class AlarmBanner final : public juce::Component
    {
    public:
        enum class Level { Notice, Danger };
        struct Alarm
        {
            juce::String text;
            Level level { Level::Danger };
            int revealTrack { -1 };   // the strip a click on this line brings into view, -1 = none
            bool operator== (const Alarm&) const = default;
        };

        // A click on a line (or list entry) that leads to a strip.
        std::function<void (int track)> onReveal;

        AlarmBanner()
        {
            setInterceptsMouseClicks (true, false);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
            setTitle ("Capture alarm");
            setAccessible (true);
        }

        ~AlarmBanner() override
        {
            if (listOpen) juce::PopupMenu::dismissAllActiveMenus();
        }

        // `newAlarms` is what stands now. The caller's 10 Hz tick hands it over
        // every time; a repeat of the same list costs a comparison.
        void show (std::vector<Alarm> newAlarms)
        {
            std::stable_partition (newAlarms.begin(), newAlarms.end(),
                                   [] (const Alarm& a) { return a.level == Level::Danger; });
            if (newAlarms == alarms) return;

            juce::StringArray texts;
            for (const auto& alarm : newAlarms) texts.add (alarm.text);
            const auto newText = texts.joinIntoString (kJoin);
            const bool announce = newText.isNotEmpty() && newText != text;

            alarms = std::move (newAlarms);
            text = newText;
            level = ! alarms.empty() && alarms.front().level == Level::Danger ? Level::Danger : Level::Notice;
            if (held.isNotEmpty() && indexOf (held) < 0) held.clear();

            setDescription (text);
            setVisible (! alarms.empty());
            if (isVisible()) toFront (false);
            else if (listOpen) juce::PopupMenu::dismissAllActiveMenus();
            repaint();
            if (announce)
                juce::AccessibilityHandler::postAnnouncement (
                    text, juce::AccessibilityHandler::AnnouncementPriority::high);
        }

        // A refused press is answered now: its alarm takes the last line when
        // it would otherwise sit in "+N more".
        void bringIntoView (const juce::String& wanted)
        {
            const int i = indexOf (wanted);
            if (i < 0) return;
            if (alarms[(std::size_t) i].text != held) { held = alarms[(std::size_t) i].text; repaint(); }
        }

        // Every alarm standing, whatever is on screen.
        const juce::String& getText() const noexcept { return text; }
        const std::vector<Alarm>& getAlarms() const noexcept { return alarms; }
        // The lines on screen at this moment, joined as getText() joins them.
        juce::String getShownText() const
        {
            juce::StringArray shown;
            for (int i : visible()) shown.add (alarms[(std::size_t) i].text);
            return shown.joinIntoString (kJoin);
        }
        int getVisibleLineCount() const { return (int) visible().size(); }
        // Alarms the "+N more" stands for.
        int getOverflowCount() const { return (int) alarms.size() - linesFor ((int) alarms.size(), getHeight()); }
        // Danger while any alarm is.
        Level getLevel() const noexcept { return level; }
        bool isListOpen() const noexcept { return listOpen; }

        // True when the alarm holding `wanted` is on a line and none of it is
        // lost to the ellipsis.
        bool isFullyShown (const juce::String& wanted) const
        {
            const int i = indexOf (wanted);
            const auto shown = visible();
            for (std::size_t k = 0; k < shown.size(); ++k)
                if (shown[k] == i)
                {
                    const auto words = typeset (displayText (alarms[(std::size_t) i].text));
                    const auto area = lineTextArea ((int) k, (int) shown.size());
                    return juce::GlyphArrangement::getStringWidth (fontFor (words, area, (int) shown.size()), words)
                        <= (float) area.getWidth();
                }
            return false;
        }

        // How many lines `count` alarms take at `height` px. One alarm reads
        // large on one line; more share the height, kLineH each.
        static int linesFor (int count, int height) noexcept
        {
            if (count <= 1) return juce::jmax (0, count);
            return juce::jlimit (1, count, (height - 2 * kPadY) / kLineH);
        }

        // Which alarms are on screen, top to bottom: the first `lines`, except
        // that a held alarm sitting past them takes the last line.
        static std::vector<int> visibleIndices (int count, int lines, int heldIndex)
        {
            std::vector<int> shown;
            for (int i = 0; i < juce::jmin (count, lines); ++i) shown.push_back (i);
            if (! shown.empty() && heldIndex >= (int) shown.size() && heldIndex < count) shown.back() = heldIndex;
            return shown;
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

        // What a line says: the drawn mark stands in for the leading '!'.
        static juce::String displayText (const juce::String& alarm)
        {
            return alarm.trimCharactersAtStart ("! ");
        }

        static juce::String moreText (int overflow) { return "+" + juce::String (overflow) + " more"; }

        // The line under a point, or -1.
        int lineAt (juce::Point<int> p) const
        {
            const auto shown = visible();
            for (int k = 0; k < (int) shown.size(); ++k)
                if (lineBounds (k, (int) shown.size()).contains (p)) return k;
            return -1;
        }

        // Where the "+N more" sits, empty when everything fits.
        juce::Rectangle<int> moreBounds() const
        {
            const int lines = (int) visible().size();
            const int overflow = getOverflowCount();
            if (lines == 0 || overflow <= 0) return {};
            auto row = lineBounds (lines - 1, lines).reduced (brand::space::lg, 0);
            return row.removeFromRight (moreWidth (overflow));
        }

        // What a click at `p` does: the strip it reveals, or -1 to open the list.
        int revealTrackAt (juce::Point<int> p) const
        {
            const int k = lineAt (p);
            if (k < 0 || moreBounds().contains (p)) return -1;
            return alarms[(std::size_t) visible()[(std::size_t) k]].revealTrack;
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (! getLocalBounds().contains (e.getPosition()) || alarms.empty()) return;
            const int track = revealTrackAt (e.getPosition());
            if (track >= 0) { if (onReveal) onReveal (track); return; }
            showList();
        }

        // Every alarm standing, whole, in a neutral grey list that opens below
        // the banner's right end: over the header buttons, never the transport.
        void showList()
        {
            juce::PopupMenu menu;
            std::vector<int> tracks;
            for (std::size_t i = 0; i < alarms.size(); ++i)
            {
                menu.addCustomItem ((int) i + 1, std::make_unique<ListItem> (alarms[i]), nullptr,
                                    typeset (displayText (alarms[i].text)));
                tracks.push_back (alarms[i].revealTrack);
            }
            const auto corner = localPointToGlobal (juce::Point<int> (getWidth() - 1, getHeight()));
            juce::Component::SafePointer<AlarmBanner> safe (this);
            listOpen = true;
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ corner.x, corner.y, 1, 1 }),
                                [safe, tracks] (int chosen)
                                {
                                    if (safe == nullptr) return;
                                    safe->listOpen = false;
                                    if (chosen < 1 || chosen > (int) tracks.size()) return;
                                    const int track = tracks[(std::size_t) chosen - 1];
                                    if (track >= 0 && safe->onReveal) safe->onReveal (track);
                                });
        }

        void paint (juce::Graphics& g) override
        {
            const auto shown = visible();
            if (shown.empty()) return;
            const int lines = (int) shown.size();
            juce::Path outline;
            outline.addRoundedRectangle (getLocalBounds().toFloat(), brand::radius::md);
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (outline);

            const int overflow = getOverflowCount();
            for (int k = 0; k < lines; ++k)
            {
                const auto& alarm = alarms[(std::size_t) shown[(std::size_t) k]];
                const auto fill = fillFor (alarm.level);
                const auto ink = brand::onSignal (fill);
                const auto row = lineBounds (k, lines);
                g.setColour (fill);
                g.fillRect (row);
                if (k > 0)
                {
                    g.setColour (brand::sink (fill, brand::tint::deep));
                    g.fillRect (row.withHeight (1));
                }

                auto area = row.reduced (brand::space::lg, 0);
                const float mark = lines == 1 ? kMarkLarge : kMarkSmall;
                drawMark (g, area.removeFromLeft ((int) mark).toFloat().withSizeKeepingCentre (mark, mark),
                          alarm.level, ink, fill);
                g.setColour (ink);
                if (k == lines - 1 && overflow > 0)
                {
                    g.setFont (lineFont (lines));
                    g.drawText (moreText (overflow), moreBounds(), juce::Justification::centredRight, false);
                }
                const auto words = typeset (displayText (alarm.text));
                const auto textArea = lineTextArea (k, lines);
                g.setFont (fontFor (words, textArea, lines));
                g.drawText (words, textArea, juce::Justification::centredLeft, true);
            }
        }

        // The warning mark, drawn: a filled triangle for danger, an outlined
        // one for a notice, so the level doesn't rest on colour alone.
        static void drawMark (juce::Graphics& g, juce::Rectangle<float> r, Level lvl,
                              juce::Colour ink, juce::Colour ground)
        {
            juce::Path triangle;
            triangle.addTriangle (r.getCentreX(), r.getY() + 0.5f, r.getRight(), r.getBottom(),
                                  r.getX(), r.getBottom());
            triangle = triangle.createPathWithRoundedCorners (1.5f);
            const float w = r.getWidth(), h = r.getHeight();
            const auto bar = juce::Rectangle<float> (r.getCentreX() - w * 0.065f, r.getY() + h * 0.36f,
                                                     w * 0.13f, h * 0.30f);
            const auto dot = juce::Rectangle<float> (w * 0.15f, w * 0.15f)
                                 .withCentre ({ r.getCentreX(), r.getBottom() - h * 0.17f });
            g.setColour (ink);
            if (lvl == Level::Danger)
            {
                g.fillPath (triangle);
                g.setColour (ground);
            }
            else
            {
                g.strokePath (triangle, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
            }
            g.fillRect (bar);
            g.fillEllipse (dot);
        }

        static juce::Colour fillFor (Level lvl) noexcept
        {
            return lvl == Level::Danger ? brand::accentRecord : brand::alertAmber;
        }

    private:
        static constexpr const char* kJoin = "  ";
        static constexpr int   kLineH = 17;     // a 14 pt line: two share the 36 px header row
        static constexpr int   kPadY = 1;
        static constexpr float kMarkLarge = 16.0f;
        static constexpr float kMarkSmall = 12.0f;
        static constexpr int   kListW = 460;

        static juce::Font lineFont (int lines)
        {
            return lines <= 1 ? brand::type::headline() : brand::type::sectionTitle();
        }

        // A lone alarm reads large; one too long for that steps down to the
        // lines' size before anything is cut.
        static juce::Font fontFor (const juce::String& words, juce::Rectangle<int> area, int lines)
        {
            const auto font = lineFont (lines);
            if (lines <= 1 && juce::GlyphArrangement::getStringWidth (font, words) > (float) area.getWidth())
                return lineFont (2);
            return font;
        }

        static int moreWidth (int overflow)
        {
            return (int) std::ceil (juce::GlyphArrangement::getStringWidth (brand::type::sectionTitle(),
                                                                            moreText (overflow))) + 2;
        }

        std::vector<int> visible() const
        {
            const int count = (int) alarms.size();
            return visibleIndices (count, linesFor (count, getHeight()), held.isEmpty() ? -1 : indexOf (held));
        }

        int indexOf (const juce::String& wanted) const
        {
            if (wanted.isEmpty()) return -1;
            for (std::size_t i = 0; i < alarms.size(); ++i)
                if (alarms[i].text.contains (wanted) || wanted.startsWith (alarms[i].text))
                    return (int) i;
            return -1;
        }

        juce::Rectangle<int> lineBounds (int k, int lines) const
        {
            auto all = getLocalBounds();
            if (lines <= 1) return all;
            const int h = (all.getHeight() - 2 * kPadY) / lines;
            auto row = all.withY (kPadY + k * h).withHeight (h);
            // The first and last lines reach the banner's edges.
            if (k == 0) row = row.withTop (0);
            if (k == lines - 1) row = row.withBottom (all.getBottom());
            return row;
        }

        juce::Rectangle<int> lineTextArea (int k, int lines) const
        {
            auto area = lineBounds (k, lines).reduced (brand::space::lg, 0);
            area.removeFromLeft ((int) (lines == 1 ? kMarkLarge : kMarkSmall) + brand::space::sm);
            const int overflow = (int) alarms.size() - lines;
            if (k == lines - 1 && overflow > 0) area.removeFromRight (moreWidth (overflow) + brand::space::md);
            return area;
        }

        // One alarm in the list: its mark in its colour, and all of its words.
        class ListItem final : public juce::PopupMenu::CustomComponent
        {
        public:
            explicit ListItem (Alarm a) : juce::PopupMenu::CustomComponent (true), alarm (std::move (a)) {}

            void getIdealSize (int& w, int& h) override
            {
                w = kListW;
                h = juce::jmax (brand::space::rowH + brand::space::sm,
                                (int) std::ceil (layoutFor (textWidth()).getHeight()) + 2 * brand::space::sm);
            }

            void paint (juce::Graphics& g) override
            {
                if (isItemHighlighted())
                {
                    g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
                    g.fillRect (getLocalBounds());
                }
                auto area = getLocalBounds().reduced (brand::space::md, brand::space::sm);
                auto markBox = area.removeFromLeft ((int) kMarkLarge).removeFromTop ((int) kMarkLarge + 2);
                drawMark (g, markBox.toFloat().withSizeKeepingCentre (kMarkLarge, kMarkLarge), alarm.level,
                          fillFor (alarm.level), findColour (juce::PopupMenu::backgroundColourId));
                area.removeFromLeft (brand::space::md);
                layoutFor (area.getWidth()).draw (g, area.toFloat());
            }

        private:
            static int textWidth() { return kListW - 3 * brand::space::md - (int) kMarkLarge; }

            juce::TextLayout layoutFor (int width) const
            {
                juce::AttributedString words;
                words.setWordWrap (juce::AttributedString::byWord);
                words.append (typeset (displayText (alarm.text)), brand::type::uiBody(),
                              findColour (juce::PopupMenu::textColourId));
                juce::TextLayout layout;
                layout.createLayout (words, (float) juce::jmax (1, width));
                return layout;
            }

            Alarm alarm;
        };

        std::vector<Alarm> alarms;
        juce::String text;
        Level level { Level::Danger };
        juce::String held;      // a refused press kept on screen
        bool listOpen { false };
    };
}
