#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/DialogChrome.h"

#include "../Audio/NoiseAnalyzer.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

#include <vector>

namespace zynforge
{
    // Sortable post-analysis table -- replaces the AlertWindow text
    // dump so the engineer can click column headers to sort by
    // worst hum / most bumps / highest noise floor and find the
    // problem tracks at a glance.
    //
    // Columns: Track | Hum dB | Hz | Bumps | Noise floor dBFS | Crest dB
    class NoiseReportDialog final : public juce::Component,
                                     private juce::TableListBoxModel
    {
    public:
        explicit NoiseReportDialog (std::vector<NoiseFinding> findings)
            : data (std::move (findings))
        {
            sortedIndices.resize (data.size());
            for (size_t i = 0; i < data.size(); ++i)
                sortedIndices[i] = (int) i;

            auto& header = table.getHeader();
            header.addColumn ("Track",           1, 160);
            header.addColumn ("Hum dB",          2,  70);
            header.addColumn ("Hz",              3,  50);
            header.addColumn ("Above floor",     4,  90);
            header.addColumn ("Bumps",           5,  60);
            header.addColumn ("Noise dBFS",      6,  90);
            header.addColumn ("Crest dB",        7,  80);
            header.setSortColumnId (4, false);     // worst hum first
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
            dialog::paintChrome (g, *this, "NOISE REPORT");
        }

        void resized() override
        {
            table.setBounds (dialog::bodyBounds (*this).reduced (8));
        }

        // Helper -- launch as an async modal dialog.
        static void launch (std::vector<NoiseFinding> findings)
        {
            auto* content = new NoiseReportDialog (std::move (findings));
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned (content);
            opts.dialogTitle                  = "Noise analysis";
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
            const auto& f = data[(size_t) sortedIndices[(size_t) rowNum]];

            juce::String text;
            juce::Colour fg = brand::textPrimary;
            switch (columnId)
            {
                case 1: text = f.trackName; break;
                case 2: text = juce::String (f.humDb, 1); break;
                case 3: text = f.humFundamentalHz > 0 ? juce::String (f.humFundamentalHz) : "--"; break;
                case 4: text = f.humDbAboveFloor > 0.0f
                                ? juce::String (f.humDbAboveFloor, 1)
                                : "--";
                        if (f.humDbAboveFloor > 12.0f) fg = brand::accentRecord;
                        else if (f.humDbAboveFloor > 6.0f) fg = brand::accentSolo;
                        break;
                case 5: text = juce::String (f.bumpCount);
                        if (f.bumpCount > 0) fg = brand::accentSolo;
                        if (f.bumpCount > 5) fg = brand::accentRecord;
                        break;
                case 6: text = juce::String (f.noiseFloorDbFS, 1);
                        if (f.noiseFloorDbFS > -50.0f) fg = brand::accentRecord;
                        else if (f.noiseFloorDbFS > -60.0f) fg = brand::accentSolo;
                        break;
                case 7: text = juce::String (f.crestFactor, 1); break;
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
            auto cmp = [this, col, forwards] (int a, int b) -> bool
            {
                const auto& fa = data[(size_t) a];
                const auto& fb = data[(size_t) b];
                auto lt = [&]() -> bool
                {
                    switch (col)
                    {
                        case 1: return fa.trackName.compareNatural (fb.trackName) < 0;
                        case 2: return fa.humDb < fb.humDb;
                        case 3: return fa.humFundamentalHz < fb.humFundamentalHz;
                        case 4: return fa.humDbAboveFloor < fb.humDbAboveFloor;
                        case 5: return fa.bumpCount < fb.bumpCount;
                        case 6: return fa.noiseFloorDbFS < fb.noiseFloorDbFS;
                        case 7: return fa.crestFactor < fb.crestFactor;
                    }
                    return false;
                }();
                return forwards ? lt : ! lt;
            };
            std::sort (sortedIndices.begin(), sortedIndices.end(), cmp);
        }

        std::vector<NoiseFinding> data;
        std::vector<int>          sortedIndices;
        juce::TableListBox        table;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoiseReportDialog)
    };
}
