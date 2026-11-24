#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Theme/DialogChrome.h"
#include "FloatingLaunch.h"

#include "../Audio/AudioEngine.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    // Sortable marker list. Double-click a row to jump the player to
    // that marker; right-click for rename / delete. The dialog draws from a
    // copy of the markers it last showed and a 4 Hz timer refreshes it on any
    // change (an add, a move, a rename, a re-sort), not only a new count, so
    // edits in the EDIT view show up live. A row acts on the marker it shows,
    // by identity: an index goes stale as soon as the list re-sorts. It
    // closes when a take starts behind it, and
    // refuses Rename / Delete while one rolls: a rename prompt would stand
    // in front of STOP.
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
            table.setColour (juce::ListBox::backgroundColourId, brand::bgStrip);
            table.setModel (this);
            addAndMakeVisible (table);

            setSize (520, 420);
            refreshRows();
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

        static juce::DialogWindow* launch (AudioEngine& engine)
        {
            auto* content = new MarkerListDialog (engine);
            juce::DialogWindow::LaunchOptions opts;
            opts.content.setOwned (content);
            opts.dialogTitle                  = "Markers";
            opts.dialogBackgroundColour       = brand::bgDeep;
            opts.escapeKeyTriggersCloseButton = true;
            opts.useNativeTitleBar            = true;
            opts.resizable                    = true;
            return launchClosesForTake (opts);
        }

        // What the right-click menu does with item `chosen` (1 Rename, 2 Delete)
        // for the marker with this runtimeId. Split out of cellClicked's async
        // callback so the suite can drive it without a popup (test seam).
        void applyMenuChoice (int chosen, const juce::String& markerId)
        {
            // Asked when the item is chosen, not when the list opened: a take
            // can start from a desk or a remote while the list is up.
            if (engine.isRecording()) return;
            juce::Component::SafePointer<MarkerListDialog> safeSelf (this);
            const int row = engine.getMarkers().indexOfId (markerId);
            if (row < 0) return;
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
                closesForTake (aw);
                aw->enterModalState (true, juce::ModalCallbackFunction::create (
                    [aw, safeSelf, markerId] (int r)
                {
                    std::unique_ptr<juce::AlertWindow> own (aw);
                    // The dialog can be closed while the rename box is
                    // open -- re-check before touching any member.
                    if (r != 1 || safeSelf == nullptr || safeSelf->engine.isRecording()) return;
                    const int target = safeSelf->engine.getMarkers().indexOfId (markerId);
                    if (target < 0) return;
                    safeSelf->engine.getMarkers().renameMarker (
                        target, own->getTextEditorContents ("n").trim());
                    safeSelf->engine.getMarkers().save();
                    safeSelf->refreshRows();
                }));
            }
            else if (chosen == 2)
            {
                engine.getMarkers().removeMarker (row);
                engine.getMarkers().save();
                refreshRows();
            }
        }

    private:
        // Same markers, in the same order, at the same places and names, at
        // the same rate as the rows on screen.
        bool showsCurrentMarkers() const
        {
            const auto& markers = engine.getMarkers();
            const auto& all = markers.getAll();
            if (all.size() != shown.size() || markers.getSampleRate() != shownRate) return false;
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].runtimeId != shown[i].runtimeId || all[i].sampleOffset != shown[i].sampleOffset
                    || all[i].name != shown[i].name)
                    return false;
            return true;
        }

        void refreshRows()
        {
            // The selection stays on its marker when the rows re-sort.
            const auto* selected = shownAt (table.getSelectedRow());
            const auto selectedId = selected != nullptr ? selected->runtimeId : juce::String();
            shown     = engine.getMarkers().getAll();
            shownRate = engine.getMarkers().getSampleRate();
            table.updateContent();
            const int row = rowOf (selectedId);
            if (row >= 0) table.selectRow (row, true, true);
            else          table.deselectAllRows();
            // updateContent repaints only rows whose index changed: a row that
            // now shows another marker, time or name must redraw too.
            table.repaint();
        }

        const Marker* shownAt (int row) const noexcept
        {
            return juce::isPositiveAndBelow (row, (int) shown.size()) ? &shown[(size_t) row] : nullptr;
        }

        int rowOf (const juce::String& runtimeId) const
        {
            if (runtimeId.isEmpty()) return -1;
            for (size_t i = 0; i < shown.size(); ++i)
                if (shown[i].runtimeId == runtimeId) return (int) i;
            return -1;
        }

        void timerCallback() override
        {
            if (! showsCurrentMarkers()) refreshRows();
        }

        int getNumRows() override { return (int) shown.size(); }

        void paintRowBackground (juce::Graphics& g, int /*r*/, int /*w*/,
                                  int /*h*/, bool selected) override
        {
            g.fillAll (selected ? brand::controlBgHover : brand::bgStrip);
        }

        void paintCell (juce::Graphics& g, int row, int col, int w, int h, bool) override
        {
            const auto* shownMarker = shownAt (row);
            if (shownMarker == nullptr) return;
            const auto& m = *shownMarker;
            const auto sr = shownRate;
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
            g.setFont (brand::type::uiBody());
            g.drawText (text, juce::Rectangle<int> (4, 0, w - 8, h),
                        col == 2 ? juce::Justification::centredLeft
                                  : juce::Justification::centredRight,
                        false);
        }

        void cellClicked (int row, int /*col*/, const juce::MouseEvent& e) override
        {
            const auto* shownMarker = shownAt (row);
            if (shownMarker == nullptr) return;
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                juce::PopupMenu m;
                const bool idle = ! engine.isRecording();
                m.addItem (1, idle ? "Rename..." : "Rename... (after the take)", idle);
                m.addItem (2, idle ? "Delete"    : "Delete (after the take)",    idle);
                // SafePointer, not raw `this`: showMenuAsync and the rename
                // modal below both OUTLIVE this click, and the dialog can be
                // closed while either is open -- dereferencing `engine`/`table`
                // afterwards is a use-after-free. Every other dialog in the app
                // already uses this pattern; this was the one that didn't.
                juce::Component::SafePointer<MarkerListDialog> safeSelf (this);
                const auto markerId = shownMarker->runtimeId;
                m.showMenuAsync (juce::PopupMenu::Options(), [safeSelf, markerId] (int chosen)
                {
                    if (safeSelf != nullptr) safeSelf->applyMenuChoice (chosen, markerId);
                });
            }
        }

        void cellDoubleClicked (int row, int /*col*/, const juce::MouseEvent&) override
        {
            // Jump the player to the marker this row shows, where it is now.
            const auto* shownMarker = shownAt (row);
            if (shownMarker == nullptr) return;
            auto& markers = engine.getMarkers();
            const int index = markers.indexOfId (shownMarker->runtimeId);
            if (index < 0) { refreshRows(); return; }   // deleted since the list last refreshed
            engine.getPlayer().setPositionSamples (markers.getMarker (index).sampleOffset);
        }

        AudioEngine&         engine;
        juce::TableListBox   table;
        // The rows on screen, as of the last refresh.
        std::vector<Marker>  shown;
        double               shownRate { 0.0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MarkerListDialog)
    };
}
