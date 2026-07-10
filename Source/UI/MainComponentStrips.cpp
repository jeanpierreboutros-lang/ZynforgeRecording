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
#include "NewSessionDialog.h"
#include "../Audio/ChannelCsv.h"

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

    // Scrollable batch-colour table: one row per channel (number + name + a
    // clickable colour swatch), plus a range bar at the top to colour a
    // contiguous block at once. Colours apply LIVE through the engine, so the
    // engineer can keep colouring as many channels / ranges as they like in
    // one sitting (1-10 one colour, 11-20 another, then tweak individuals).
    // Embedded in the Batch Colour AlertWindow; Cancel reverts to a snapshot.
    class BatchColourTable final : public juce::Component
    {
    public:
        BatchColourTable (AudioEngine& eng, std::function<void()> changed)
            : engine (eng), onChanged (std::move (changed))
        {
            const int n = eng.getRecorder().getNumTracks();

            addAndMakeVisible (rangeLabel);
            rangeLabel.setText ("Colour channels", juce::dontSendNotification);
            rangeLabel.setColour (juce::Label::textColourId, brand::textPrimary);
            rangeLabel.setFont (brand::type::uiBody());

            for (auto* e : { &fromEd, &toEd })
            {
                dialog::styleTextEditor (*e);
                e->setInputRestrictions (4, "0123456789");
                e->setJustification (juce::Justification::centred);
                addAndMakeVisible (*e);
            }
            fromEd.setText ("1",                 juce::dontSendNotification);
            toEd  .setText (juce::String (n),    juce::dontSendNotification);

            addAndMakeVisible (dashLabel);
            dashLabel.setText ("to", juce::dontSendNotification);
            dashLabel.setJustificationType (juce::Justification::centred);
            dashLabel.setColour (juce::Label::textColourId, brand::textSecondary);

            rangeBtn.setButtonText ("Pick colour for range...");
            dialog::stylePrimary (rangeBtn);
            rangeBtn.onClick = [this] { pickForRange(); };
            addAndMakeVisible (rangeBtn);

            for (int i = 0; i < n; ++i)
            {
                auto r = std::make_unique<Row>();
                r->index = i;
                r->num.setText ("In " + juce::String (i + 1), juce::dontSendNotification);
                r->num.setColour (juce::Label::textColourId, brand::textTertiary);
                r->num.setFont (brand::type::uiLabel());
                r->name.setText (eng.getRecorder().getTrack (i).name, juce::dontSendNotification);
                r->name.setColour (juce::Label::textColourId, brand::textPrimary);
                r->name.setFont (brand::type::uiBody());
                applySwatchColour (*r);
                r->swatch.onClick = [this, idx = i] { pickForOne (idx); };
                content.addAndMakeVisible (r->num);
                content.addAndMakeVisible (r->name);
                content.addAndMakeVisible (r->swatch);
                rows.push_back (std::move (r));
            }
            viewport.setViewedComponent (&content, false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
        }

        int preferredHeight() const { return juce::jlimit (220, 540, (int) rows.size() * 31 + 64); }

        void resized() override
        {
            auto b = getLocalBounds();
            auto top = b.removeFromTop (32);
            rangeLabel.setBounds (top.removeFromLeft (108));
            fromEd.setBounds (top.removeFromLeft (46).reduced (1, 2));
            dashLabel.setBounds (top.removeFromLeft (22));
            toEd.setBounds (top.removeFromLeft (46).reduced (1, 2));
            top.removeFromLeft (8);
            rangeBtn.setBounds (top.removeFromLeft (juce::jmin (200, top.getWidth())).reduced (0, 2));
            b.removeFromTop (8);
            viewport.setBounds (b);

            const int rowH = 28, gap = 3, numW = 50, swW = 48, pad = 4;
            const int w = juce::jmax (180, viewport.getWidth() - 14);
            content.setSize (w, (int) rows.size() * (rowH + gap) + pad);
            int y = pad;
            for (auto& r : rows)
            {
                r->num   .setBounds (6, y, numW, rowH);
                r->swatch.setBounds (w - swW - 6, y + 3, swW, rowH - 6);
                r->name  .setBounds (6 + numW + 8, y, w - numW - swW - 28, rowH);
                y += rowH + gap;
            }
        }

    private:
        struct Row { int index { 0 }; juce::Label num, name; juce::TextButton swatch; };

        void applySwatchColour (Row& r)
        {
            const auto argb = engine.getRecorder().getTrack (r.index)
                                 .colourARGB.load (std::memory_order_relaxed);
            r.swatch.setColour (juce::TextButton::buttonColourId,
                                argb != 0 ? juce::Colour (argb) : brand::stripDefaultGrey);
        }

        void setRowColour (int idx, juce::Colour c)
        {
            if (idx < 0 || idx >= (int) rows.size()) return;
            engine.setTrackColour (idx, c);
            rows[(size_t) idx]->swatch.setColour (juce::TextButton::buttonColourId, c);
            rows[(size_t) idx]->swatch.repaint();
            if (onChanged) onChanged();
        }

        void pickForOne (int idx)
        {
            const auto cur = juce::Colour (engine.getRecorder().getTrack (idx)
                                              .colourARGB.load (std::memory_order_relaxed));
            auto picker = std::make_unique<zynforge::StripColourPicker> (
                cur, [this, idx] (juce::Colour c) { setRowColour (idx, c); });
            juce::CallOutBox::launchAsynchronously (std::move (picker),
                rows[(size_t) idx]->swatch.getScreenBounds(), nullptr);
        }

        void pickForRange()
        {
            const int n = (int) rows.size();
            if (n == 0) return;
            const int first = juce::jlimit (1, n, fromEd.getText().getIntValue());
            const int last  = juce::jlimit (first, n, toEd.getText().getIntValue());
            auto picker = std::make_unique<zynforge::StripColourPicker> (
                brand::brandOrange,
                [this, first, last] (juce::Colour c)
                { for (int ch = first - 1; ch < last; ++ch) setRowColour (ch, c); });
            juce::CallOutBox::launchAsynchronously (std::move (picker),
                rangeBtn.getScreenBounds(), nullptr);
        }

        AudioEngine& engine;
        std::function<void()> onChanged;
        juce::Label      rangeLabel, dashLabel;
        juce::TextEditor fromEd, toEd;
        juce::TextButton rangeBtn;
        juce::Viewport   viewport;
        juce::Component  content;
        std::vector<std::unique_ptr<Row>> rows;
    };
}

void MainComponent::clearStripSelection()
{
    if (selectedLogical.empty()) return;
    selectedLogical.clear();
    for (auto& s : strips) s->setSelected (false);
    // The EDIT row headers read the selection in paint() -- repaint them too,
    // or the highlight lingers until a mouse move dirties the rows.
    if (editPage != nullptr) editPage->repaintLanes();
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
    // Cmd+A from the EDIT view: repaint the rows so their headers show selected
    // immediately, not only after the mouse moves over a channel.
    if (editPage != nullptr) editPage->repaintLanes();
    showStatus (juce::String ((int) strips.size()) + " strip(s) selected");
}

void MainComponent::condemnAllStrips()
{
    // Stop every strip's strip/meter/spectrum timers so none samples a
    // TrackState that a recorder-vector shrink (New Session, close, opening a
    // smaller session, a console-session rebuild) is about to free. The strips
    // are rebuilt on the 10 Hz timer AFTER the free, so without this there is a
    // ~100 ms freed-memory read window (the meter-lifetime fix covered the
    // per-strip delete paths but not the whole-set shrinks).
    for (auto& s : strips)
        if (s != nullptr) s->invalidate();
}

void MainComponent::deleteSelectedStrips()
{
    if (selectedLogical.empty() || engine.isRecording()) return;

    // removeStripAt frees TrackStates + shifts indices as it goes; condemn all
    // strips first (the single-delete path does the same via invalidate()).
    condemnAllStrips();

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

    // Snapshot every colour so Cancel can revert (the table applies live).
    std::vector<juce::uint32> snapshot;
    snapshot.reserve ((size_t) total);
    for (int i = 0; i < total; ++i)
        snapshot.push_back (engine.getRecorder().getTrack (i)
                               .colourARGB.load (std::memory_order_relaxed));

    auto* aw = new juce::AlertWindow ("Batch Colour Channels",
                                      "Click a channel's swatch to recolour it, or colour a whole "
                                      "range at once. Changes apply live -- keep going for as many "
                                      "channels as you like, then Done (or Cancel to revert).",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome (not JUCE-default navy)

    auto* table = new BatchColourTable (engine, [this] { lastTrackCount = -1; });
    table->setSize (460, table->preferredHeight());
    aw->addCustomComponent (table);
    aw->addButton ("Done",   1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, table, snapshot, total] (int result)
        {
            std::unique_ptr<juce::AlertWindow>     disposeAw (aw);
            std::unique_ptr<BatchColourTable>      disposeTable (table);
            if (result != 1)   // Cancel -> restore the snapshot
            {
                for (int i = 0; i < total && i < (int) snapshot.size(); ++i)
                    engine.setTrackColour (i, juce::Colour (snapshot[(size_t) i]));
                lastTrackCount = -1;
                showStatus ("Batch colour cancelled");
                return;
            }
            lastTrackCount = -1;
            showStatus ("Channel colours updated");
        }),
        false);
}

void MainComponent::importChannelNamesFromCsv()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Import channel names from CSV", getSessionsRoot(), "*.csv;*.txt");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const auto names = zynforge::channelcsv::parseNames (f.loadFileAsString());
            if (names.isEmpty()) { showStatus ("No channel names found in " + f.getFileName()); return; }

            const int n = engine.getRecorder().getNumTracks();
            int applied = 0;
            for (int i = 0; i < names.size() && i < n; ++i)
            { engine.setTrackName (i, names[i]); ++applied; }
            lastTrackCount = -1;
            showStatus ("Imported " + juce::String (applied) + " channel name(s) from "
                        + f.getFileName()
                        + (names.size() > n ? "  (" + juce::String (names.size() - n)
                                              + " extra ignored)" : juce::String()));
        });
}

void MainComponent::createSessionFromCsv()
{
    if (engine.isRecording()) { showStatus ("Stop recording before creating a session"); return; }

    chooser = std::make_unique<juce::FileChooser> (
        "New session from CSV", getSessionsRoot(), "*.csv;*.txt");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (! f.existsAsFile()) return;
            const auto names = zynforge::channelcsv::parseNames (f.loadFileAsString());
            if (names.isEmpty()) { showStatus ("No channel names found in " + f.getFileName()); return; }

            zynforge::NewSessionDialog::Result r;
            r.name          = f.getFileNameWithoutExtension();
            r.location      = getSessionsRoot();
            r.captureFormat = engine.getRecorder().getCaptureFormat();
            r.sampleRate    = pendingSampleRate;
            r.interleaved   = true;
            r.ioSettings    = "Last Used";
            const auto dir  = createSessionFolderStructure (r);

            engine.setActiveSessionDir (dir);
            engine.clearAllStripOverrides();
            engine.setStripCount (names.size());
            for (int i = 0; i < names.size(); ++i)
                engine.setTrackName (i, names[i]);
            lastTrackCount = -1;
            loadSetlistFromActiveSession();
            loadUILayoutFromActiveSession();
            saveSessionStateTo (dir);   // persist the names + strip count now

            showStatus ("New session '" + r.name + "' with " + juce::String (names.size())
                        + " channels from " + f.getFileName());
        });
}

void MainComponent::createSessionFromConsole()
{
    if (engine.isRecording()) { showStatus ("Stop recording before creating a session"); return; }

    auto* p = engine.getAppProps();
    const int  dialect = p ? p->getIntValue ("cs_console_dialect", 1) : 1;
    const auto key     = p ? p->getValue ("cs_console_key", "digico") : juce::String ("digico");
    const auto ip = p ? p->getValue ("cs_" + key + "_ip").trim() : juce::String();
    const int  rx = p ? p->getIntValue ("cs_" + key + "_port", 8001) : 8001;
    if (ip.isEmpty())
    { showStatus ("Set the Console IP first (Control Surfaces -> console -> Console IP)."); return; }

    // Listen on the chosen console's dialect so replies are parsed, capture
    // incoming names, then nudge the console to report them.
    if (! engine.isOscListening() || engine.getOscDialect() != dialect)
        engine.startOsc (oscListenPort(), dialect);
    engine.setConsoleNameCapture (true);
    if (! engine.requestConsoleChannelNames (ip, rx, dialect))
    {
        engine.setConsoleNameCapture (false);
        showStatus ("Couldn't reach console at " + ip + ":" + juce::String (rx));
        return;
    }
    showStatus ("Requesting channel names from " + ip + " -- building session in ~3 s...");

    juce::Component::SafePointer<MainComponent> self (this);
    juce::Timer::callAfterDelay (3000, [self]
    {
        if (self == nullptr) return;
        self->engine.setConsoleNameCapture (false);
        const auto names = self->engine.takeCapturedConsoleNames();
        if (names.empty())
        {
            self->showStatus ("No channel names received -- check the desk is sending OSC to this Mac.");
            return;
        }
        const int maxCh = names.rbegin()->first;   // highest 1-based channel

        zynforge::NewSessionDialog::Result r;
        r.name          = "DiGiCo " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M");
        r.location      = self->getSessionsRoot();
        r.captureFormat = self->engine.getRecorder().getCaptureFormat();
        r.sampleRate    = self->pendingSampleRate;
        r.interleaved   = true;
        r.ioSettings    = "Last Used";
        const auto dir  = self->createSessionFolderStructure (r);

        self->engine.setActiveSessionDir (dir);
        self->engine.clearAllStripOverrides();
        self->condemnAllStrips();   // console-session rebuild resizes the recorder vector; stop strip timers first
        self->engine.setStripCount (maxCh);
        for (const auto& kv : names)
        {
            const int i = kv.first - 1;
            if (i >= 0 && i < maxCh) self->engine.setTrackName (i, kv.second);
        }
        self->lastTrackCount = -1;
        self->loadSetlistFromActiveSession();
        self->loadUILayoutFromActiveSession();
        self->saveSessionStateTo (dir);
        self->showStatus ("Session created from console: " + juce::String ((int) names.size())
                          + " named channel(s)");
    });
}
