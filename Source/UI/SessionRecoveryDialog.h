#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <functional>
#include <vector>

namespace zynforge
{
    // Surfaces every `recording.session` orphan the engine found at
    // launch as a sortable table. Each row shows the session's name,
    // total size on disk, last modified date, and Track_NN.wav count
    // so the engineer can see at a glance which orphan is worth
    // recovering before opening it. Recover loads the session and
    // clears the marker; Delete removes the session directory (with
    // a confirm); Skip closes the dialog and leaves every orphan in
    // place so they reappear on the next launch.
    class SessionRecoveryDialog final : public juce::Component,
                                         private juce::TableListBoxModel
    {
    public:
        struct Row
        {
            juce::File   dir;
            juce::String name;
            juce::int64  sizeBytes  { 0 };
            juce::int64  modifiedMs { 0 };
            int          trackCount { 0 };
        };

        using OpenCallback = std::function<void (const juce::File&)>;

        SessionRecoveryDialog (juce::Array<juce::File> orphans,
                               OpenCallback onOpen)
            : openCb (std::move (onOpen))
        {
            rows.reserve ((size_t) orphans.size());
            for (auto& d : orphans)
            {
                Row r;
                r.dir        = d;
                r.name       = d.getFileName();
                r.sizeBytes  = computeDirSize (d);
                r.modifiedMs = d.getLastModificationTime().toMilliseconds();
                r.trackCount = d.findChildFiles (juce::File::findFiles, false, "Track_*.wav").size();
                rows.push_back (std::move (r));
            }
            sortedIndices.resize (rows.size());
            for (size_t i = 0; i < rows.size(); ++i) sortedIndices[i] = (int) i;
            // Default sort: most-recently-modified first -- the
            // engineer almost always wants the session they crashed
            // out of, which is the newest one.
            sortIndicesBy (3, false);

            auto& header = table.getHeader();
            header.addColumn ("Session",   1, 260);
            header.addColumn ("Tracks",    2,  70);
            header.addColumn ("Size",      3,  90);
            header.addColumn ("Modified",  4, 160);
            header.setSortColumnId (4, false);

            table.setHeaderHeight (22);
            table.setRowHeight (24);
            table.setColour (juce::ListBox::backgroundColourId, brand::bgPanel);
            table.setMultipleSelectionEnabled (false);
            table.setModel (this);
            table.selectRow (0);
            addAndMakeVisible (table);

            recoverButton.setButtonText ("Recover selected");
            recoverButton.setColour (juce::TextButton::buttonColourId,
                                     brand::accentStatus.withMultipliedAlpha (0.20f));
            recoverButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus);
            recoverButton.onClick = [this] { recoverSelected(); };
            addAndMakeVisible (recoverButton);

            deleteButton.setButtonText ("Delete...");
            deleteButton.setColour (juce::TextButton::buttonColourId,
                                    brand::accentRecord.withMultipliedAlpha (0.18f));
            deleteButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord);
            deleteButton.onClick = [this] { deleteSelected(); };
            addAndMakeVisible (deleteButton);

            dismissButton.setButtonText ("Skip");
            dismissButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
            dismissButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
            dismissButton.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            };
            addAndMakeVisible (dismissButton);

            help.setText (juce::String (rows.size())
                              + " session(s) didn't shut down cleanly. The audio is on disk -- "
                                "Recover loads the session, Delete removes it, Skip closes this "
                                "dialog and leaves them for next launch.",
                          juce::dontSendNotification);
            help.setFont (brand::type::caption());
            help.setColour (juce::Label::textColourId, brand::textSecondary);
            help.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (help);

            setSize (640, 440);
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "SESSION RECOVERY");
        }

        void resized() override
        {
            auto body = dialog::bodyBounds (*this).reduced (12);
            help.setBounds (body.removeFromTop (44));
            body.removeFromTop (6);
            auto footer = body.removeFromBottom (34);
            table.setBounds (body);
            dismissButton.setBounds (footer.removeFromRight (90));
            footer.removeFromRight (8);
            deleteButton.setBounds (footer.removeFromRight (110));
            footer.removeFromRight (8);
            recoverButton.setBounds (footer.removeFromRight (160));
        }

        static void launch (juce::Array<juce::File> orphans, OpenCallback onOpen)
        {
            if (orphans.isEmpty()) return;
            auto* content = new SessionRecoveryDialog (std::move (orphans), std::move (onOpen));
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned (content);
            opts.dialogTitle                  = "Recover sessions";
            opts.dialogBackgroundColour       = brand::bgDeep;
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar            = true;
            opts.resizable                    = true;
            opts.launchAsync();
        }

    private:
        static juce::int64 computeDirSize (const juce::File& dir)
        {
            juce::int64 total = 0;
            auto files = dir.findChildFiles (juce::File::findFiles, true);
            for (auto& f : files) total += f.getSize();
            return total;
        }

        static juce::String formatBytes (juce::int64 bytes)
        {
            const double gb = (double) bytes / (1024.0 * 1024.0 * 1024.0);
            if (gb >= 1.0) return juce::String (gb, 2) + " GB";
            const double mb = (double) bytes / (1024.0 * 1024.0);
            if (mb >= 1.0) return juce::String (mb, 1) + " MB";
            const double kb = (double) bytes / 1024.0;
            return juce::String (kb, 1) + " KB";
        }

        static juce::String formatTime (juce::int64 ms)
        {
            return juce::Time (ms).formatted ("%Y-%m-%d  %H:%M");
        }

        const Row* selectedRow() const
        {
            const int row = table.getSelectedRow();
            if (row < 0 || row >= (int) sortedIndices.size()) return nullptr;
            return &rows[(size_t) sortedIndices[(size_t) row]];
        }

        void recoverSelected()
        {
            const auto* r = selectedRow();
            if (r == nullptr) return;
            r->dir.getChildFile ("recording.session").deleteFile();
            const auto dir = r->dir;
            if (openCb) openCb (dir);
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (1);
        }

        void deleteSelected()
        {
            const auto* r = selectedRow();
            if (r == nullptr) return;
            const auto dir  = r->dir;
            const auto name = r->name;
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Delete session?")
                    .withMessage ("This will permanently delete:\n\n"
                                  + dir.getFullPathName()
                                  + "\n\nThe audio in this session will be lost.")
                    .withButton ("Delete")
                    .withButton ("Cancel"),
                [this, dir] (int chosen)
                {
                    if (chosen != 1) return;
                    dir.deleteRecursively();
                    // Drop the row from our local model so the table
                    // updates without needing a relaunch. If we just
                    // emptied the list, close the dialog.
                    int dropAt = -1;
                    for (size_t i = 0; i < rows.size(); ++i)
                        if (rows[i].dir == dir) { dropAt = (int) i; break; }
                    if (dropAt >= 0)
                    {
                        rows.erase (rows.begin() + dropAt);
                        sortedIndices.clear();
                        for (size_t i = 0; i < rows.size(); ++i)
                            sortedIndices.push_back ((int) i);
                        sortIndicesBy (table.getHeader().getSortColumnId(),
                                       table.getHeader().isSortedForwards());
                        table.updateContent();
                        if (rows.empty())
                            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                                dw->exitModalState (0);
                    }
                });
        }

        int getNumRows() override { return (int) sortedIndices.size(); }

        void paintRowBackground (juce::Graphics& g, int /*row*/, int /*w*/, int /*h*/, bool selected) override
        {
            g.fillAll (selected ? brand::controlBgHover : brand::bgPanel);
        }

        void paintCell (juce::Graphics& g, int rowNum, int columnId,
                         int width, int height, bool /*selected*/) override
        {
            if (rowNum < 0 || rowNum >= (int) sortedIndices.size()) return;
            const auto& r = rows[(size_t) sortedIndices[(size_t) rowNum]];
            juce::String text;
            auto fg = brand::textPrimary;
            switch (columnId)
            {
                case 1: text = r.name;                              break;
                case 2: text = juce::String (r.trackCount);
                        if (r.trackCount == 0) fg = brand::textMuted;
                        break;
                case 3: text = formatBytes (r.sizeBytes);
                        if (r.sizeBytes == 0) fg = brand::accentRecord;
                        break;
                case 4: text = formatTime (r.modifiedMs);           break;
            }
            g.setColour (fg);
            g.setFont (brand::type::uiBody());
            g.drawText (text,
                        juce::Rectangle<int> (6, 0, width - 12, height),
                        columnId == 1 ? juce::Justification::centredLeft
                                       : juce::Justification::centredRight,
                        false);
        }

        void sortOrderChanged (int newSortColumnId, bool isForwards) override
        {
            sortIndicesBy (newSortColumnId, isForwards);
            table.updateContent();
        }

        void cellDoubleClicked (int /*row*/, int /*col*/, const juce::MouseEvent&) override
        {
            recoverSelected();
        }

        void sortIndicesBy (int col, bool forwards)
        {
            auto cmp = [this, col, forwards] (int a, int b) -> bool
            {
                const auto& ra = rows[(size_t) a];
                const auto& rb = rows[(size_t) b];
                auto lt = [&]() -> bool
                {
                    switch (col)
                    {
                        case 1: return ra.name.compareNatural (rb.name) < 0;
                        case 2: return ra.trackCount < rb.trackCount;
                        case 3: return ra.sizeBytes  < rb.sizeBytes;
                        case 4: return ra.modifiedMs < rb.modifiedMs;
                    }
                    return false;
                }();
                return forwards ? lt : ! lt;
            };
            std::sort (sortedIndices.begin(), sortedIndices.end(), cmp);
        }

        std::vector<Row>      rows;
        std::vector<int>      sortedIndices;
        juce::TableListBox    table;
        juce::TextButton      recoverButton;
        juce::TextButton      deleteButton;
        juce::TextButton      dismissButton;
        juce::Label           help;
        OpenCallback          openCb;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionRecoveryDialog)
    };
}
