#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/DialogChrome.h"

#include "../Audio/QcAnalyzer.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <cmath>
#include <vector>

namespace zynforge
{
    // Post-show QC table -- sortable like NoiseReportDialog so the
    // engineer clicks "Clips" and the problem channels float to the top.
    //
    // Columns: Track | Peak dBFS | LUFS-I | Clips | First clip | Noise dBFS
    class QcReportDialog final : public juce::Component,
                                 private juce::TableListBoxModel
    {
    public:
        explicit QcReportDialog (std::vector<qc::TrackQc> results)
            : data (std::move (results))
        {
            sortedIndices.resize (data.size());
            for (size_t i = 0; i < data.size(); ++i)
                sortedIndices[i] = (int) i;

            auto& header = table.getHeader();
            header.addColumn ("Track",       1, 160);
            header.addColumn ("Peak dBFS",   2,  80);
            header.addColumn ("LUFS-I",      3,  70);
            header.addColumn ("Clips",       4,  60);
            header.addColumn ("First clip",  5, 100);
            header.addColumn ("Noise dBFS",  6,  90);
            header.setSortColumnId (4, false);     // most clipping first
            sortIndicesBy (4, false);

            table.setHeaderHeight (22);
            table.setRowHeight (22);
            table.setColour (juce::ListBox::backgroundColourId, brand::bgPanel);
            table.setModel (this);
            addAndMakeVisible (table);

            setSize (620, 380);
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "POST-SHOW QC");
        }

        void resized() override
        {
            table.setBounds (dialog::bodyBounds (*this).reduced (8));
        }

        static void launch (std::vector<qc::TrackQc> results)
        {
            auto* content = new QcReportDialog (std::move (results));
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned (content);
            opts.dialogTitle                  = "Post-show QC";
            opts.dialogBackgroundColour       = brand::bgDeep;
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar            = true;
            opts.resizable                    = true;
            opts.launchAsync();
        }

    private:
        // TableListBoxModel
        int getNumRows() override { return (int) sortedIndices.size(); }

        void paintRowBackground (juce::Graphics& g, int /*rowNum*/,
                                  int /*w*/, int /*h*/, bool selected) override
        {
            g.fillAll (selected ? brand::controlBgHover : brand::bgPanel);
        }

        void paintCell (juce::Graphics& g, int rowNum, int columnId,
                         int width, int height, bool /*selected*/) override
        {
            if (rowNum < 0 || rowNum >= (int) sortedIndices.size()) return;
            const auto& q = data[(size_t) sortedIndices[(size_t) rowNum]];

            juce::String text;
            juce::Colour fg = brand::textPrimary;
            switch (columnId)
            {
                case 1: text = q.name; break;
                case 2: text = juce::String (q.peakDb, 1);
                        if (q.peakDb > -0.1f)      fg = brand::accentRecord;
                        else if (q.peakDb > -3.0f) fg = brand::accentSolo;
                        break;
                case 3: text = q.integratedLufs > -119.0f ? juce::String (q.integratedLufs, 1) : "--";
                        break;
                case 4: text = juce::String (q.clipEventCount);
                        if (q.clipEventCount > 0)  fg = brand::accentSolo;
                        if (q.clipEventCount > 10) fg = brand::accentRecord;
                        break;
                case 5: text = q.clipEvents.empty()
                                 ? juce::String ("--")
                                 : qc::timecode (q.clipEvents.front().startSample, q.sampleRate);
                        break;
                case 6: text = juce::String (q.noiseFloorDb, 1);
                        if (q.noiseFloorDb > -50.0f)      fg = brand::accentRecord;
                        else if (q.noiseFloorDb > -60.0f) fg = brand::accentSolo;
                        break;
            }

            g.setColour (fg);
            g.setFont (brand::type::uiBody());
            g.drawText (text, juce::Rectangle<int> (4, 0, width - 8, height),
                        columnId == 1 ? juce::Justification::centredLeft
                                       : juce::Justification::centredRight,
                        false);
        }

        void sortOrderChanged (int newSortColumnId, bool isForwards) override
        {
            sortIndicesBy (newSortColumnId, isForwards);
            table.updateContent();
        }

        void sortIndicesBy (int col, bool forwards)
        {
            auto cmp = [this, col, forwards] (int ia, int ib) -> bool
            {
                const int a = forwards ? ia : ib;   // descending = swapped
                const int b = forwards ? ib : ia;   // operands, never !lt
                const auto& qa = data[(size_t) a];
                const auto& qb = data[(size_t) b];
                // NaN violates strict-weak-ordering and crashes introsort
                // (SIGBUS at the first real gig's QC run) -- map non-finite
                // to a floor so the sort is total no matter the data.
                auto num = [] (float v) { return std::isfinite (v) ? v : -1.0e9f; };
                auto lt = [&]() -> bool
                {
                    switch (col)
                    {
                        case 1: return qa.name.compareNatural (qb.name) < 0;
                        case 2: return num (qa.peakDb) < num (qb.peakDb);
                        case 3: return num (qa.integratedLufs) < num (qb.integratedLufs);
                        case 4: return qa.clipEventCount < qb.clipEventCount;
                        case 5: return (qa.clipEvents.empty() ? -1 : (juce::int64) qa.clipEvents.front().startSample)
                                     < (qb.clipEvents.empty() ? -1 : (juce::int64) qb.clipEvents.front().startSample);
                        case 6: return num (qa.noiseFloorDb) < num (qb.noiseFloorDb);
                    }
                    return false;
                }();
                return lt;
            };
            std::sort (sortedIndices.begin(), sortedIndices.end(), cmp);
        }

        std::vector<qc::TrackQc> data;
        std::vector<int>         sortedIndices;
        juce::TableListBox       table;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QcReportDialog)
    };
}
