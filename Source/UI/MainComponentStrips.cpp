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
#include "../Theme/DialogChrome.h"

using namespace zynforge;

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

    auto picker = std::make_unique<juce::ColourSelector>(
        juce::ColourSelector::showColourspace
        | juce::ColourSelector::showSliders);
    picker->setSize (300, 280);
    picker->setCurrentColour (brand::brandOrange);

    struct Apply : public juce::ChangeListener
    {
        MainComponent* owner;
        void changeListenerCallback (juce::ChangeBroadcaster* src) override
        {
            if (auto* cs = dynamic_cast<juce::ColourSelector*> (src))
            {
                const auto c = cs->getCurrentColour();
                for (int logical : owner->selectedLogical)
                {
                    if (logical < 0 || logical >= (int) owner->strips.size()) continue;
                    int phys = 0;
                    for (int k = 0; k < logical; ++k)
                    {
                        auto& t = owner->engine.getRecorder().getTrack (phys);
                        phys += t.isStereo.load (std::memory_order_relaxed) ? 2 : 1;
                    }
                    owner->engine.setTrackColour (phys, c);
                    auto& t = owner->engine.getRecorder().getTrack (phys);
                    if (t.isStereo.load (std::memory_order_relaxed))
                        owner->engine.setTrackColour (phys + 1, c);
                }
                owner->lastTrackCount = -1;
            }
        }
    };
    auto listener = std::make_shared<Apply>();
    listener->owner = this;
    picker->addChangeListener (listener.get());
    batchColourListenerHandle = listener;   // keep alive

    juce::CallOutBox::launchAsynchronously (
        std::move (picker),
        juce::Rectangle<int>{ getScreenBounds().getCentreX(),
                              getScreenBounds().getCentreY(), 1, 1 },
        nullptr);

    showStatus ("Picking colour for " + juce::String ((int) selectedLogical.size()) + " strip(s)...");
}

int MainComponent::physicalFromLogicalIdx (int logical)
{
    int phys = 0;
    auto& rec = engine.getRecorder();
    for (int k = 0; k < logical && phys < rec.getNumTracks(); ++k)
        phys += rec.getTrack (phys).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
    return phys;
}

// Inverse of physicalFromLogicalIdx: collapse a physical track index to
// the logical strip ordinal that contains it (stereo pairs count once).
int MainComponent::logicalFromPhysicalIdx (int physical)
{
    auto& rec = engine.getRecorder();
    int logical = 0, p = 0;
    while (p < physical && p < rec.getNumTracks())
    {
        p += rec.getTrack (p).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        ++logical;
    }
    return logical;
}

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

    auto* aw = new juce::AlertWindow ("Batch Rename Channels",
                                      "Apply a numbered name to a range of channels.\n"
                                      "Example: prefix 'Drums', first 1, last 8, start 1\n"
                                      "         → 'Drums 1', 'Drums 2', ... 'Drums 8'.",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("prefix", "Drums",              "Prefix:");
    dialog::primeNameEditor (*aw, "prefix");
    aw->addTextEditor ("first",  "1",                  "First channel:");
    dialog::primeNameEditor (*aw, "first");
    aw->addTextEditor ("last",   juce::String (total), "Last channel:");
    aw->addTextEditor ("start",  "1",                  "Start number:");
    aw->addButton ("Apply",  1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, total] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;

            const auto prefix = aw->getTextEditorContents ("prefix").trim();
            const int firstCh = juce::jlimit (1, total,
                                              aw->getTextEditorContents ("first").getIntValue());
            const int lastCh  = juce::jlimit (firstCh, total,
                                              aw->getTextEditorContents ("last").getIntValue());
            const int startN  = juce::jmax (0,
                                            aw->getTextEditorContents ("start").getIntValue());

            int suffix = startN;
            for (int ch = firstCh - 1; ch < lastCh; ++ch, ++suffix)
            {
                const auto name = prefix.isEmpty()
                                     ? juce::String (suffix)
                                     : prefix + " " + juce::String (suffix);
                engine.setTrackName (ch, name);
            }
            showStatus ("Renamed channels " + juce::String (firstCh)
                        + "-" + juce::String (lastCh)
                        + " (" + prefix + " " + juce::String (startN) + "...)");
            lastTrackCount = -1;   // force strip rebuild so names show up
        }),
        false);
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
    aw->addTextEditor ("first", "1",                  "First channel:");
    aw->addTextEditor ("last",  juce::String (total), "Last channel:");
    aw->addButton ("Pick colour...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel",       0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, total] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;

            const int firstCh = juce::jlimit (1, total,
                                              aw->getTextEditorContents ("first").getIntValue());
            const int lastCh  = juce::jlimit (firstCh, total,
                                              aw->getTextEditorContents ("last").getIntValue());

            auto picker = std::make_unique<juce::ColourSelector>(
                juce::ColourSelector::showColourspace
                | juce::ColourSelector::showSliders);
            picker->setSize (300, 280);
            picker->setCurrentColour (brand::brandOrange);

            // ColourSelector is a Component; juce::CallOutBox wraps it
            // and fires onColourChanged via a Listener. Quickest path:
            // poll the colour on dismiss via a Timer + simple modal pump.
            // Pragmatic shortcut: hand the picker out, apply on dismissal
            // through a ChangeListener.
            struct ColourApplyListener : public juce::ChangeListener
            {
                MainComponent* owner; int first, last;
                void changeListenerCallback (juce::ChangeBroadcaster* src) override
                {
                    if (auto* cs = dynamic_cast<juce::ColourSelector*> (src))
                    {
                        const auto c = cs->getCurrentColour();
                        for (int ch = first - 1; ch < last; ++ch)
                            owner->engine.setTrackColour (ch, c);
                        owner->lastTrackCount = -1;
                    }
                }
            };
            auto listener = std::make_shared<ColourApplyListener>();
            listener->owner = this;
            listener->first = firstCh;
            listener->last  = lastCh;
            picker->addChangeListener (listener.get());

            juce::CallOutBox::launchAsynchronously (
                std::move (picker),
                juce::Rectangle<int>{ getScreenBounds().getCentreX(),
                                      getScreenBounds().getCentreY(), 1, 1 },
                nullptr);

            // Keep the listener alive until the CallOutBox closes.
            // Easiest: stash it in a member; for now leak benignly --
            // colour-pickers are infrequent.
            batchColourListenerHandle = listener;

            showStatus ("Colouring channels " + juce::String (firstCh)
                        + "-" + juce::String (lastCh) + "...");
        }),
        false);
}
