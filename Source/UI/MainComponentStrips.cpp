// Multi-strip-selection methods on MainComponent. Extracted from
// MainComponent.cpp as part of the 2026-05-25 god-class split (part 6).
//
// 'selectedLogical' is the set of logical strip indices currently
// selected via shift/cmd-click. For stereo pairs, both halves move /
// recolour / delete together because the strip list iterates logical
// rows, not raw track indices.
//
// Includes: clearStripSelection, selectAllStrips, deleteSelectedStrips,
// colourSelectedStrips, physicalFromLogicalIdx, moveSelectedStrips,
// showBatchRenameDialog, showBatchColourDialog.

#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "StripColourPicker.h"

using namespace zynforge;

namespace
{
    // A name field that treats Tab / Shift+Tab as "jump to the next /
    // previous channel" instead of ordinary focus traversal, so the
    // engineer can rename a whole input list keyboard-only: type, Tab,
    // type, Tab... The callbacks are wired by the RenameTable owner.
    class NameEditor final : public juce::TextEditor
    {
    public:
        std::function<void()> onTab, onShiftTab;

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key.getKeyCode() == juce::KeyPress::tabKey)
            {
                if (key.getModifiers().isShiftDown()) { if (onShiftTab) onShiftTab(); }
                else                                  { if (onTab)      onTab();      }
                return true;
            }
            return juce::TextEditor::keyPressed (key);
        }
    };

    // Scrollable per-channel rename table: a row per channel showing its
    // input number + an editable name field. applyTo() writes every name
    // back through the engine (MIXER / EDIT then pick them up via their
    // own polling). Embedded in the Rename Channels AlertWindow.
    class RenameTable final : public juce::Component
    {
    public:
        explicit RenameTable (AudioEngine& eng)
        {
            const int n = eng.getRecorder().getNumTracks();
            for (int i = 0; i < n; ++i)
            {
                auto r = std::make_unique<Row>();
                r->index = i;
                r->num.setText ("In " + juce::String (i + 1), juce::dontSendNotification);
                r->num.setColour (juce::Label::textColourId, brand::textTertiary);
                r->num.setFont (brand::type::uiLabel());
                r->num.setJustificationType (juce::Justification::centredLeft);
                r->editor.setText (eng.getRecorder().getTrack (i).name, juce::dontSendNotification);
                r->editor.setFont (brand::type::uiBody());
                dialog::styleTextEditor (r->editor);
                r->editor.setSelectAllWhenFocused (true);
                content.addAndMakeVisible (r->num);
                content.addAndMakeVisible (r->editor);
                rows.push_back (std::move (r));
            }

            // Wire Tab / Shift+Tab to walk the list (wrapping at the ends),
            // scrolling the viewport so the focused field stays visible.
            for (int i = 0; i < (int) rows.size(); ++i)
            {
                rows[(size_t) i]->editor.onTab      = [this, i] { focusRow (i + 1); };
                rows[(size_t) i]->editor.onShiftTab = [this, i] { focusRow (i - 1); };
            }

            viewport.setViewedComponent (&content, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
        }

        void resized() override
        {
            viewport.setBounds (getLocalBounds());
            const int rowH = 28, gap = 3, numW = 52, pad = 4;
            const int w = juce::jmax (120, viewport.getWidth() - 14);
            content.setSize (w, (int) rows.size() * (rowH + gap) + pad);
            int y = pad;
            for (auto& r : rows)
            {
                r->num   .setBounds (6, y, numW, rowH);
                r->editor.setBounds (6 + numW + 8, y, w - numW - 24, rowH);
                y += rowH + gap;
            }
        }

        void applyTo (AudioEngine& eng)
        {
            for (auto& r : rows)
                eng.setTrackName (r->index, r->editor.getText().trim());
        }

        int preferredHeight() const
        {
            return juce::jlimit (140, 460, (int) rows.size() * 31 + 8);
        }

        // Move keyboard focus to row `target` (wrapping past either end),
        // scrolling it into view first so the field isn't off-screen.
        void focusRow (int target)
        {
            const int n = (int) rows.size();
            if (n == 0) return;
            target = ((target % n) + n) % n;   // wrap both directions
            auto& ed = rows[(size_t) target]->editor;
            const int rowH = 28, gap = 3, pad = 4;
            const int top    = pad + target * (rowH + gap);
            const int bottom = top + rowH;
            const int viewTop = viewport.getViewPositionY();
            const int viewH   = viewport.getMaximumVisibleHeight();
            if (top < viewTop)                 viewport.setViewPosition (0, top - pad);
            else if (bottom > viewTop + viewH) viewport.setViewPosition (0, bottom - viewH + pad);
            ed.grabKeyboardFocus();
        }

    private:
        struct Row { int index { 0 }; juce::Label num; NameEditor editor; };
        juce::Viewport viewport;
        juce::Component content;
        std::vector<std::unique_ptr<Row>> rows;
    };
}

void MainComponent::clearStripSelection()
{
    if (selectedLogical.empty()) return;
    selectedLogical.clear();
    for (auto& s : strips) s->setSelected (false);
    showStatus ("Selection cleared");
}

void MainComponent::selectAllStrips()
{
    if (strips.empty()) return;
    selectedLogical.clear();
    for (int i = 0; i < (int) strips.size(); ++i)
    {
        selectedLogical.insert (i);
        if (strips[(size_t) i] != nullptr) strips[(size_t) i]->setSelected (true);
    }
    showStatus (juce::String ((int) strips.size()) + " strip(s) selected");
}

void MainComponent::deleteSelectedStrips()
{
    if (selectedLogical.empty() || engine.isRecording()) return;

    // Delete from the highest index down so earlier indices stay valid.
    std::vector<int> sorted (selectedLogical.begin(), selectedLogical.end());
    std::sort (sorted.rbegin(), sorted.rend());

    int removed = 0;
    for (int logical : sorted)
    {
        if (logical < 0 || logical >= (int) strips.size()) continue;
        // The strip's deleteCb already knows how to remove the right
        // number of underlying tracks (1 for mono, 2 for stereo). We
        // simulate that by walking through the engine's index map.
        // Since the strip list rebuilds on the next tick, we just call
        // engine.removeStripAt for each logical entry -- for stereo
        // pairs we call it twice at the same physical index because
        // the second physical track shifts down.
        // To find the physical index of a logical row, sum mono+stereo
        // strip widths up to that point.
        int phys = 0;
        for (int k = 0; k < logical; ++k)
        {
            auto& t = engine.getRecorder().getTrack (phys);
            phys += t.isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        }
        if (phys >= engine.getRecorder().getNumTracks()) continue;
        const bool wasStereo = engine.getRecorder().getTrack (phys)
                                    .isStereo.load (std::memory_order_relaxed);
        engine.removeStripAt (phys);
        if (wasStereo) engine.removeStripAt (phys);
        ++removed;
    }
    selectedLogical.clear();
    lastTrackCount = -1;
    showStatus ("Deleted " + juce::String (removed) + " selected strip(s)");
}

void MainComponent::colourSelectedStrips()
{
    if (selectedLogical.empty()) return;

    // Use the app's gradient StripColourPicker -- the same hue×shade palette
    // the per-strip swatch + EDIT recolour use -- not the OS colourspace
    // selector, so "Colour selected..." matches the rest of the app.
    const int first = *selectedLogical.begin();
    juce::Colour current = brand::brandOrange;
    if (first >= 0 && first < (int) strips.size() && strips[(size_t) first] != nullptr)
        current = strips[(size_t) first]->getResolvedColour();

    const std::set<int> sel = selectedLogical;   // snapshot for the callback
    auto picker = std::make_unique<zynforge::StripColourPicker> (
        current,
        [this, sel] (juce::Colour chosen)
        {
            for (int logical : sel)
            {
                if (logical < 0 || logical >= (int) strips.size()) continue;
                const int phys = physicalFromLogicalIdx (logical);
                if (phys < 0 || phys >= engine.getRecorder().getNumTracks()) continue;
                engine.setTrackColour (phys, chosen);
                auto& t = engine.getRecorder().getTrack (phys);
                if (t.isStereo.load (std::memory_order_relaxed))
                    engine.setTrackColour (phys + 1, chosen);
            }
            lastTrackCount = -1;
        });

    juce::CallOutBox::launchAsynchronously (
        std::move (picker),
        juce::Rectangle<int>{ getScreenBounds().getCentreX(),
                              getScreenBounds().getCentreY(), 1, 1 },
        nullptr);

    showStatus ("Picking colour for " + juce::String ((int) selectedLogical.size()) + " strip(s)...");
}

// Stereo logical<->physical mapping now lives on AudioEngine (it's a pure
// function of the recorder's per-track stereo flags). These thin forwarders
// keep the existing UI call sites working.
int MainComponent::physicalFromLogicalIdx (int logical)  { return engine.physicalFromLogical (logical); }
int MainComponent::logicalFromPhysicalIdx (int physical) { return engine.logicalFromPhysical (physical); }

void MainComponent::moveSelectedStrips (int delta)
{
    if (selectedLogical.empty() || engine.isRecording()) return;

    recordUndoSnapshot ("Move selection");

    // Order the move: up (delta < 0) sweeps low-to-high; down sweeps
    // high-to-low -- so we never trample a target slot mid-sweep.
    std::vector<int> sorted (selectedLogical.begin(), selectedLogical.end());
    if (delta < 0) std::sort (sorted.begin(),  sorted.end());
    else           std::sort (sorted.rbegin(), sorted.rend());

    std::set<int> newSelection;
    int moved = 0;
    for (int logical : sorted)
    {
        const int target = logical + delta;
        if (target < 0 || target >= (int) strips.size()) { newSelection.insert (logical); continue; }

        const int physA = physicalFromLogicalIdx (logical);
        const int physB = physicalFromLogicalIdx (target);

        auto& rec = engine.getRecorder();
        const bool stereoA = (physA < rec.getNumTracks())
                              && rec.getTrack (physA).isStereo.load (std::memory_order_relaxed);
        const bool stereoB = (physB < rec.getNumTracks())
                              && rec.getTrack (physB).isStereo.load (std::memory_order_relaxed);

        // Swap each physical-track pair. Mono-mono / stereo-stereo
        // moves swap the matching halves; mono-stereo asymmetric
        // pairs fall back to swapping only the first half and let
        // the engineer adjust (rare in practice).
        engine.swapTracks (physA, physB);
        if (stereoA && stereoB
            && physA + 1 < rec.getNumTracks()
            && physB + 1 < rec.getNumTracks())
        {
            engine.swapTracks (physA + 1, physB + 1);
        }
        newSelection.insert (target);
        ++moved;
    }
    selectedLogical = std::move (newSelection);
    lastTrackCount = -1;
    showStatus ("Moved " + juce::String (moved) + " strip(s) "
                + (delta < 0 ? "up" : "down"));
}

void MainComponent::showBatchRenameDialog()
{
    const int total = engine.getRecorder().getNumTracks();
    if (total <= 0) { showStatus ("No channels to rename"); return; }

    auto* aw = new juce::AlertWindow ("Rename Channels",
                                      "Edit any channel name, then Apply. Changes show "
                                      "immediately in the MIXER and EDIT views.",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome (not JUCE-default navy)

    auto* table = new RenameTable (engine);
    table->setSize (440, table->preferredHeight());
    aw->addCustomComponent (table);   // AlertWindow lays it out but doesn't own it

    aw->addButton ("Apply",  1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, table] (int result)
        {
            std::unique_ptr<juce::AlertWindow> disposeAw (aw);
            std::unique_ptr<RenameTable>       disposeTable (table);
            if (result != 1) return;

            table->applyTo (engine);
            lastTrackCount = -1;   // force a strip rebuild so names refresh
            showStatus ("Channel names updated");
        }),
        false);

    // Land in the first name field so the engineer can type → Tab →
    // type straight down the input list without reaching for the mouse.
    juce::Component::SafePointer<RenameTable> safeTable (table);
    juce::MessageManager::callAsync ([safeTable]
    {
        if (safeTable != nullptr) safeTable->focusRow (0);
    });
}

void MainComponent::showBatchColourDialog()
{
    const int total = engine.getRecorder().getNumTracks();
    if (total <= 0) { showStatus ("No channels to colour"); return; }

    // Two-step: range picker, then colour picker. Keeps the AlertWindow
    // simple (numeric inputs only) and reuses StripColourPicker.
    auto* aw = new juce::AlertWindow ("Batch Colour Channels",
                                      "Apply a colour to a contiguous range of channels.",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome (not JUCE-default navy)
    aw->addTextEditor ("first", "1",                  "First channel:");
    aw->addTextEditor ("last",  juce::String (total), "Last channel:");
    aw->addButton ("Pick colour...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel",       0, juce::KeyPress (juce::KeyPress::escapeKey));
    dialog::primeNameEditor (*aw, "first");   // focus first field; Enter advances to Pick colour

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, total] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;

            const int firstCh = juce::jlimit (1, total,
                                              aw->getTextEditorContents ("first").getIntValue());
            const int lastCh  = juce::jlimit (firstCh, total,
                                              aw->getTextEditorContents ("last").getIntValue());

            // App's gradient StripColourPicker (same palette as per-strip +
            // EDIT recolour + "Colour selected..."), not the OS selector.
            auto picker = std::make_unique<zynforge::StripColourPicker> (
                brand::brandOrange,
                [this, firstCh, lastCh] (juce::Colour chosen)
                {
                    for (int ch = firstCh - 1; ch < lastCh; ++ch)
                        engine.setTrackColour (ch, chosen);
                    lastTrackCount = -1;
                });

            juce::CallOutBox::launchAsynchronously (
                std::move (picker),
                juce::Rectangle<int>{ getScreenBounds().getCentreX(),
                                      getScreenBounds().getCentreY(), 1, 1 },
                nullptr);

            showStatus ("Colouring channels " + juce::String (firstCh)
                        + "-" + juce::String (lastCh) + "...");
        }),
        false);
}
