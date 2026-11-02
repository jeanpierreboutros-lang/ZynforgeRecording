#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // One line of the pre-flight checklist. Pre-show, the engineer needs what's
    // wrong first, so the dialog sorts by status and keeps each row to a mark,
    // what was checked, and what it found.
    struct PreflightRow
    {
        enum class Status { failed, warning, ok, info };

        Status       status { Status::info };
        juce::String item, value;

        // The control that deals with this problem. Empty for a problem with
        // nothing to press, such as a slow drive.
        juce::String          fixLabel {};
        std::function<void()> fix {};

        bool isProblem() const noexcept { return status == Status::failed || status == Status::warning; }

        // The clock and backup checks report as "[OK]  Item: value" lines.
        static PreflightRow fromLine (const juce::String& line)
        {
            PreflightRow row;
            row.status = line.startsWith ("[OK]") ? Status::ok
                       : line.startsWith ("[X]")  ? Status::failed
                       : line.startsWith ("[!]")  ? Status::warning
                                                  : Status::info;
            const auto text = line.fromFirstOccurrenceOf ("]", false, false).trim();
            row.item  = text.upToFirstOccurrenceOf (":", false, false).trim();
            row.value = text.fromFirstOccurrenceOf (":", false, false).trim();
            return row;
        }
    };

    // A copy writes the pre-roll while live audio waits in memory. Memory sets how
    // much can be written; a drive below real time cannot keep up with any of it.
    inline PreflightRow preRollRow (int seconds, double secondsThatFit, double gotMBps, double needMBps)
    {
        if (gotMBps > 0.0 && gotMBps <= needMBps)
            return { PreflightRow::Status::warning, "Pre-roll",
                     "this drive cannot write " + juce::String (seconds) + " s in time; live audio would be lost" };
        if (secondsThatFit + 0.05 < (double) seconds)
            return { PreflightRow::Status::warning, "Pre-roll",
                     juce::String (secondsThatFit, 1) + " s of " + juce::String (seconds)
                         + " s will be written; these channels and copies fill the capture memory" };
        return { PreflightRow::Status::ok, "Pre-roll", juce::String (seconds) + " s" };
    }

    class PreflightDialog final : public juce::Component
    {
    public:
        explicit PreflightDialog (std::vector<PreflightRow> rowsIn) : rows (std::move (rowsIn))
        {
            std::stable_sort (rows.begin(), rows.end(), [] (const PreflightRow& a, const PreflightRow& b)
            { return rank (a) < rank (b); });

            for (const auto& row : rows)
            {
                if (row.status == PreflightRow::Status::failed)  ++failed;
                if (row.status == PreflightRow::Status::warning) ++warnings;
            }

            dialog::stylePrimary (okButton);
            okButton.setButtonText ("OK");
            okButton.addShortcut (juce::KeyPress (juce::KeyPress::returnKey));
            okButton.onClick = [this]
            {
                if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                    window->exitModalState (0);
            };
            addAndMakeVisible (okButton);

            for (const auto& row : rows)
                if (hasFix (row))
                {
                    auto* button = fixButtons.add (new juce::TextButton (row.fixLabel));
                    dialog::styleSecondary (*button);
                    button->setTitle (row.fixLabel + ": " + row.item);
                    button->onClick = [this, fix = row.fix]
                    {
                        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
                            window->exitModalState (0);
                        juce::MessageManager::callAsync (fix);
                    };
                    addAndMakeVisible (button);
                }

            int height = dialog::titleH + kSummaryH + dialog::footerH + brand::space::md;
            for (const auto& row : rows) height += rowHeight (row);
            if (failed + warnings > 0) height += kGroupGap;
            setTitle ("Pre-flight checklist");
            setDescription (summaryText());
            setSize (kWidth, height);
        }

        static void launch (std::vector<PreflightRow> rows)
        {
            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (new PreflightDialog (std::move (rows)));
            options.dialogTitle                  = "Pre-flight checklist";
            options.dialogBackgroundColour       = brand::bgElevated;
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar            = true;
            options.resizable                    = false;
            options.launchAsync();
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "PRE-FLIGHT CHECKLIST");

            auto area = dialog::bodyBounds (*this).reduced (kPad, 0);
            auto summary = area.removeFromTop (kSummaryH);
            g.setColour (failed > 0 ? brand::textDanger : warnings > 0 ? brand::alertAmber : brand::accentStatus);
            g.setFont (brand::type::headline());
            g.drawText (summaryText(), summary, juce::Justification::centredLeft, false);

            bool inProblems = rows.empty() ? false : rows.front().isProblem();
            for (const auto& row : rows)
            {
                if (inProblems && ! row.isProblem())
                {
                    inProblems = false;
                    auto gap = area.removeFromTop (kGroupGap);
                    g.setColour (brand::borderSubtle);
                    g.drawHorizontalLine (gap.getCentreY(), (float) gap.getX(), (float) gap.getRight());
                }
                paintRow (g, row, area.removeFromTop (rowHeight (row)));
            }
        }

        void resized() override
        {
            auto footer = dialog::footerBounds (*this).reduced (0, brand::space::xs);
            okButton.setBounds (footer.removeFromRight (dialog::btnPrimary));

            auto area = dialog::bodyBounds (*this).reduced (kPad, 0);
            area.removeFromTop (kSummaryH);
            int next = 0;
            for (const auto& row : rows)
            {
                auto line = area.removeFromTop (rowHeight (row));
                if (hasFix (row))
                    fixButtons[next++]->setBounds (line.removeFromRight (kFixW).removeFromTop (kRowH).reduced (0, 1));
            }
        }

        int getNumFixes() const noexcept { return fixButtons.size(); }
        juce::TextButton* getFixButton (int index) const noexcept { return fixButtons[index]; }

    private:
        static constexpr int kWidth = 620, kPad = brand::space::xl, kRowH = 24, kSummaryH = 40,
                             kMarkW = 28, kItemW = 196, kGroupGap = 13, kFixW = 76;

        static bool hasFix (const PreflightRow& row) { return row.isProblem() && row.fix != nullptr; }

        static int rank (const PreflightRow& row) noexcept
        {
            return row.status == PreflightRow::Status::failed ? 0
                 : row.status == PreflightRow::Status::warning ? 1 : 2;
        }

        static int valueWidth (const PreflightRow& row) noexcept
        {
            return kWidth - 2 * kPad - kMarkW - kItemW - (hasFix (row) ? kFixW + brand::space::sm : 0);
        }

        // A value too long for its column, such as a drive path, takes two lines:
        // what is wrong with a path comes after the path.
        static int rowHeight (const PreflightRow& row)
        {
            return juce::GlyphArrangement::getStringWidth (brand::type::uiBody(), row.value) > (float) valueWidth (row)
                       ? kRowH + 16 : kRowH;
        }

        juce::String summaryText() const
        {
            if (failed + warnings == 0) return "All checks passed";
            juce::StringArray parts;
            if (failed > 0)   parts.add (juce::String (failed)   + (failed   == 1 ? " problem" : " problems"));
            if (warnings > 0) parts.add (juce::String (warnings) + (warnings == 1 ? " warning" : " warnings"));
            return parts.joinIntoString (", ");
        }

        static juce::Colour statusColour (const PreflightRow& row)
        {
            switch (row.status)
            {
                case PreflightRow::Status::failed:  return brand::accentRecord;
                case PreflightRow::Status::warning: return brand::alertAmber;
                case PreflightRow::Status::ok:      return brand::accentStatus;
                case PreflightRow::Status::info:    break;
            }
            return brand::textTertiary;
        }

        static void paintMark (juce::Graphics& g, const PreflightRow& row, juce::Rectangle<float> box)
        {
            const auto colour = statusColour (row);
            const auto stroke = juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            juce::Path p;
            if (row.status == PreflightRow::Status::ok)
            {
                p.startNewSubPath (box.getX() + 2.0f, box.getCentreY() + 1.0f);
                p.lineTo (box.getX() + 6.0f, box.getBottom() - 3.0f);
                p.lineTo (box.getRight() - 2.0f, box.getY() + 3.0f);
                g.setColour (colour);
                g.strokePath (p, stroke);
                return;
            }
            if (row.status == PreflightRow::Status::info)
            {
                g.setColour (colour);
                g.drawEllipse (box.reduced (1.5f), 1.5f);
                g.setFont (brand::type::captionBold());
                g.drawText ("i", box, juce::Justification::centred, false);
                return;
            }
            g.setColour (colour);
            g.fillEllipse (box);
            g.setColour (brand::onSignal (colour));
            if (row.status == PreflightRow::Status::failed)
            {
                const auto x = box.reduced (5.0f);
                p.startNewSubPath (x.getTopLeft());     p.lineTo (x.getBottomRight());
                p.startNewSubPath (x.getTopRight());    p.lineTo (x.getBottomLeft());
                g.strokePath (p, stroke);
            }
            else
            {
                g.setFont (brand::type::uiLabel());
                g.drawText ("!", box, juce::Justification::centred, false);
            }
        }

        static void paintRow (juce::Graphics& g, const PreflightRow& row, juce::Rectangle<int> r)
        {
            const auto problem = row.isProblem();
            paintMark (g, row, r.removeFromLeft (kMarkW).removeFromTop (kRowH).toFloat()
                                .withSizeKeepingCentre (16.0f, 16.0f).translated (-6.0f, 0.0f));

            g.setFont (problem ? brand::type::channelName() : brand::type::uiBody());
            g.setColour (problem ? brand::textPrimary : brand::textSecondary);
            g.drawText (row.item, r.removeFromLeft (kItemW).removeFromTop (kRowH),
                        juce::Justification::centredLeft, true);

            g.setFont (brand::type::uiBody());
            g.setColour (row.status == PreflightRow::Status::failed ? brand::textDanger
                         : problem ? statusColour (row) : brand::textPrimary);
            g.drawFittedText (row.value, r.withWidth (valueWidth (row)).withTrimmedTop (brand::space::xs + 1),
                              juce::Justification::topLeft, 2, 1.0f);
        }

        std::vector<PreflightRow> rows;
        int failed { 0 }, warnings { 0 };
        juce::TextButton okButton;
        juce::OwnedArray<juce::TextButton> fixButtons;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreflightDialog)
    };
}
