#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/DialogChrome.h"

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Sortable marker list. Double-click a row to jump the player to
    // that sample position; right-click for rename / delete. The dialog
    // tracks marker mutations via a 4 Hz timer so adds / renames in the
    // EDIT view show up live.
    class MarkerListDialog final : public juce::Component,
                                    private juce::TableListBoxModel,
                                    private juce::Timer
    {
    public:
        explicit MarkerListDialog (AudioEngine& eng) : engine (eng)
        {
            auto& header = table.getHeader();
            header.addColumn ("#",       1,  40);
            header.addColumn ("Name",    2, 220);
            header.addColumn ("Time",    3, 100);
            header.addColumn ("Sample",  4, 120);

            table.setHeaderHeight (22);
            table.setRowHeight (22);
            table.setColour (juce::ListBox::backgroundColourId, brand::bgPanel);
            table.setModel (this);
            addAndMakeVisible (table);

            setSize (520, 420);
            startTimerHz (4);
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "MARKERS");
        }
        void resized() override
        {
            // Body sits between the chrome title (44 px) and the
            // footer divider (60 px). Reduce 8 px for breathing room.
            table.setBounds (dialog::bodyBounds (*this).reduced (8));
        }

        static void launch (AudioEngine& engine)
        {
            auto* content = new MarkerListDialog (engine);
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned (content);
            opts.dialogTitle                  = "Markers";
            opts.dialogBackgroundColour       = brand::bgDeep;
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar            = true;
            opts.resizable                    = true;
            opts.launchAsync();
        }

    private:
        void timerCallback() override
        {
            const int n = engine.getMarkers().getCount();
            if (n != lastCount) { lastCount = n; table.updateContent(); }
        }

        int getNumRows() override { return engine.getMarkers().getCount(); }

        void paintRowBackground (juce::Graphics& g, int /*r*/, int /*w*/,
                                  int /*h*/, bool selected) override
        {
            g.fillAll (selected ? brand::controlBgHover : brand::bgPanel);
        }

        void paintCell (juce::Graphics& g, int row, int col, int w, int h, bool) override
        {
            const auto& m = engine.getMarkers().getMarker (row);
            const auto sr = engine.getMarkers().getSampleRate();
            juce::String text;
            switch (col)
            {
                case 1: text = juce::String (row + 1); break;
                case 2: text = m.name.isEmpty() ? juce::String ("(unnamed)") : m.name; break;
                case 3: if (sr > 0.0)
                        {
                            const double secs = (double) m.sampleOffset / sr;
                            const int mins  = (int) secs / 60;
                            const double sec= secs - mins * 60;
                            text = juce::String::formatted ("%02d:%06.3f", mins, sec);
                        }
                        break;
                case 4: text = juce::String (m.sampleOffset); break;
            }
            g.setColour (brand::textPrimary);
            g.setFont (brand::fonts::body());
            g.drawText (text, juce::Rectangle<int> (4, 0, w - 8, h),
                        col == 2 ? juce::Justification::centredLeft
                                  : juce::Justification::centredRight,
                        false);
        }

        void cellClicked (int row, int /*col*/, const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                juce::PopupMenu m;
                m.addItem (1, "Rename...");
                m.addItem (2, "Delete");
                m.showMenuAsync (juce::PopupMenu::Options(), [this, row] (int chosen)
                {
                    if (chosen == 1)
                    {
                        auto* aw = new juce::AlertWindow ("Rename marker",
                            "Name:", juce::MessageBoxIconType::NoIcon);
                        aw->addTextEditor ("n", engine.getMarkers().getMarker (row).name, {});
                        if (auto* ed = aw->getTextEditor ("n"))
                        {
                            ed->setSelectAllWhenFocused (true);
                            juce::MessageManager::callAsync (
                                [safe = juce::Component::SafePointer<juce::TextEditor> (ed)]
                                {
                                    if (safe != nullptr) safe->grabKeyboardFocus();
                                });
                        }
                        aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
                        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                        aw->enterModalState (true, juce::ModalCallbackFunction::create (
                            [aw, this, row] (int r)
                        {
                            std::unique_ptr<juce::AlertWindow> own (aw);
                            if (r != 1) return;
                            engine.getMarkers().renameMarker (row, own->getTextEditorContents ("n").trim());
                            engine.getMarkers().save();
                            table.updateContent();
                        }));
                    }
                    else if (chosen == 2)
                    {
                        engine.getMarkers().removeMarker (row);
                        engine.getMarkers().save();
                        table.updateContent();
                    }
                });
            }
        }

        void cellDoubleClicked (int row, int /*col*/, const juce::MouseEvent&) override
        {
            // Jump the player to this marker's sample position.
            const auto& m = engine.getMarkers().getMarker (row);
            engine.getPlayer().setPositionSamples (m.sampleOffset);
        }

        AudioEngine&         engine;
        juce::TableListBox   table;
        int                  lastCount { -1 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerListDialog)
    };
}
