#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "AddTracksDialog.h"
#include "AudioDeviceDialog.h"
#include "Meterbridge.h"
#include "NewSessionDialog.h"
#include "PatchPage.h"
#include "EditToolsBar.h"
#include "SessionRecoveryDialog.h"
#include "MarkerListDialog.h"
#include "SessionInfoDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

MainComponent::MainComponent()
{
    // Every non-UI transport enters through AudioEngine.  Intercept only the
    // daemon-specific record/stop cases; std::nullopt lets ordinary playback
    // and in-process recording retain the engine's default implementation.
    engine.setRemoteTransportHandler ([this] (auto action, juce::String& error)
    { return handleRemoteTransport (action, error); });
    engine.getRemotes().setHost ({ [this] (const juce::String& message) { showStatus (message); },
                                   [this] { return sessionIoBusy.load(); },
                                   [this] (juce::int64 sample)
                                   { if (editPage != nullptr) editPage->scrollToSample (sample); } });
    wireConsoleLink();
    engine.onTakeInterrupted = [this] (bool) { localTakeInterrupted(); };
    engine.onMarkerDropped = [this] (const juce::String& name) { twinLink.noteLocalMarker (name); };
    engine.getMarkers().onDamagedFileKept = [this] (const juce::File& kept)
    { toast.show ("markers.json was damaged and is kept as " + kept.getFileName(), Toast::Kind::Warning); };

    engine.onTracksReordered = [this] (const std::vector<int>& destination)
    {
        clipClipboard = juce::var(); // any copied implicit track index is now stale
        undoManager.clearUndoHistory(); // old per-index snapshots must not undo onto different audio
        rebaselineMixerUndo();
        // reorderTracks bumps the track generation, so the strips rebuild on
        // the next tick even when a block move keeps the count. Refresh EDIT
        // asynchronously: this callback can run inside an EDIT row's own
        // mouseUp, and refresh() destroys and recreates the rows. The rename
        // keeps each row's file PATH, so the path-keyed peaks must be rescanned
        // or a lane shows its previous occupant's audio -- for a MIXER move as
        // much as an EDIT drag.
        if (editPage != nullptr)
            juce::MessageManager::callAsync ([safePage = juce::Component::SafePointer<EditPage> (editPage.get())]
            {
                if (safePage == nullptr) return;
                safePage->refresh();
                safePage->rescanWaveforms();
            });
    };
    setLookAndFeel (&laf);
    // Make the ZynForge chrome the app-wide default so EVERY prompt --
    // including juce::AlertWindow::showAsync(...) boxes that don't set their
    // own LookAndFeel -- renders as a first-party dialog (grey chrome + the
    // forge-mark badge) instead of JUCE's stock warning-triangle alert.
    // Reset to nullptr in the destructor before `laf` is torn down.
    juce::LookAndFeel::setDefaultLookAndFeel (&laf);

    // Restore the persisted clip-nudge step (cycled with N).
    if (auto* props = engine.getAppProps())
        nudgeMs = juce::jlimit (1, 5000, props->getIntValue ("editNudgeMs", nudgeMs));

    // Title text removed from the header -- the macOS window title +
    // app icon already identify the product, no need to repeat it.
    // Mock parity: the wordmark is now PAINTED in MainComponent::paint()
    // (badge + two-tone tracked text), so the plain Label stays hidden.
    titleLabel.setFont (brand::type::headline());
    titleLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setVisible (false);

    statusLabel.setFont (brand::type::uiBody());
    statusLabel.setColour (juce::Label::textColourId, brand::textMuted);
    // Alarm prefixes must stay in view when a long warning is clipped.
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    nextCueLabel.setColour (juce::Label::textColourId, brand::accentVS);
    nextCueLabel.setJustificationType (juce::Justification::centredRight);
    nextCueLabel.setFont (brand::type::uiBody());
    addAndMakeVisible (nextCueLabel);

    // DANTE pill -- shows when the active audio device is Dante Virtual
    // Soundcard so the engineer sees at a glance that they're on the
    // Dante network. Updated from the existing timer.
    danteLabel.setColour (juce::Label::textColourId, brand::featureEngaged);
    danteLabel.setJustificationType (juce::Justification::centredLeft);
    danteLabel.setFont (brand::type::caption());
    addAndMakeVisible (danteLabel);

    // Persistent sample-rate-mismatch banner -- stays lit (record-red) the
    // whole time the device clock disagrees with the session rate, so it
    // can't scroll away like a status message. Empty = hidden.
    srWarnLabel.setColour (juce::Label::textColourId, brand::accentRecord);
    srWarnLabel.setJustificationType (juce::Justification::centredLeft);
    srWarnLabel.setFont (brand::type::ui (12.0f, true));
    addAndMakeVisible (srWarnLabel);

    // Twin Recorder state while the link is on: a flat red block for TWIN LOST.
    twinStatusLabel.setJustificationType (juce::Justification::centred);
    twinStatusLabel.setFont (brand::type::ui (12.0f, true));
    twinStatusLabel.setMinimumHorizontalScale (0.8f);
    addChildComponent (twinStatusLabel);

    addChildComponent (alarmBanner);

    punchPill.setJustificationType (juce::Justification::centred);
    punchPill.setFont (brand::type::ui (12.0f, true));
    punchPill.setMinimumHorizontalScale (0.8f);
    addChildComponent (punchPill);

    sessionLabel.setFont (brand::type::caption());
    sessionLabel.setColour (juce::Label::textColourId, brand::textMuted);
    sessionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (sessionLabel);

    // Transport timecode -- mono so HH:MM:SS doesn't dance
    transportLabel.setFont (brand::type::mono (13.0f, true));
    transportLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    transportLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (transportLabel);

    deviceButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
    deviceButton.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
    deviceButton.onClick = [this] { onDeviceClicked(); };
    addAndMakeVisible (deviceButton);

    // Mock parity: neutral dark pill; keep a subtle red label as the danger cue.
    lockButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
    lockButton.setColour (juce::TextButton::textColourOffId, brand::lift (brand::accentRecord, 0.10f));
    lockButton.onClick = [this] { onLockToggled(); };
    addAndMakeVisible (lockButton);

    backupButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
    backupButton.setColour (juce::TextButton::textColourOffId,   brand::textSecondary);
    backupButton.onClick = [this] { onBackupClicked(); };
    addAndMakeVisible (backupButton);

    patchButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
    patchButton.setColour (juce::TextButton::textColourOffId, brand::textSecondary);
    patchButton.onClick = [this]
    {
        if (auto* w = patchDialog.getComponent())
        {
            delete w;                  // second click closes the non-modal window
            patchDialog = nullptr;
            return;
        }
        openPatchWindow();
    };

    // VSC chip uses accentVS so the 'virtual soundcheck' role has a unique
    // visual identity (was sharing engagedAmber).
    // Mock parity: neutral pill, amber glyph/label keeps the VSC dot identity.
    vscButton.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
    vscButton.setColour (juce::TextButton::textColourOffId, brand::accentVS);
    vscButton.setTooltip ("Virtual Soundcheck -- repatch every strip's OUTPUT to match its INPUT, "
                          "so playback feeds the desk via the same channels the live mics did.");
    vscButton.onClick = [this] { onVscClicked(); };
    addAndMakeVisible (vscButton);
    addAndMakeVisible (patchButton);

    auto styleViewBtn = [] (juce::TextButton& b, bool engaged)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     engaged ? brand::sink (brand::accentStatus, 0.55f)
                             : brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId,
                     engaged ? brand::lift (brand::accentStatus, 0.10f)
                             : brand::textSecondary);
    };
    styleViewBtn (mixViewButton,  true);
    styleViewBtn (editViewButton, false);
    mixViewButton .setTooltip ("Mixer view -- channel strips with faders and meters.");
    editViewButton.setTooltip ("Edit view -- waveforms of the loaded/recorded tracks.");
    mixViewButton .onClick = [this] { switchView (View::Mix);  };
    editViewButton.onClick = [this] { switchView (View::Edit); };
    addAndMakeVisible (mixViewButton);
    addAndMakeVisible (editViewButton);

    // Strip-width preset toggle (XS / S / M / L). Tooltips describe how
    // many channel strips fit on a single screen page at each setting.
    auto styleStripBtn = [] (juce::TextButton& b, const char* tt)
    {
        b.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
        b.setColour (juce::TextButton::buttonOnColourId,  brand::accentStatus);
        b.setColour (juce::TextButton::textColourOffId,   brand::textSecondary);
        b.setColour (juce::TextButton::textColourOnId,    brand::onSignal (brand::accentStatus));
        b.setClickingTogglesState (false);
        b.setTooltip (tt);
    };
    styleStripBtn (stripXsButton, "XS strip width -- 24 strips per page (tightest)");
    styleStripBtn (stripSButton,  "S  strip width -- 16 strips per page");
    styleStripBtn (stripMButton,  "M  strip width -- 12 strips per page (default)");
    styleStripBtn (stripLButton,  "L  strip width -- 8 strips per page (largest)");
    stripXsButton.onClick = [this] { setStripWidthPreset (StripWidth::XS); };
    stripSButton .onClick = [this] { setStripWidthPreset (StripWidth::S);  };
    stripMButton .onClick = [this] { setStripWidthPreset (StripWidth::M);  };
    stripLButton .onClick = [this] { setStripWidthPreset (StripWidth::L);  };
    addAndMakeVisible (stripXsButton);
    addAndMakeVisible (stripSButton);
    addAndMakeVisible (stripMButton);
    addAndMakeVisible (stripLButton);

    // Grid / table view toggle -- 24 faders on a page (12 per row, 2 rows).
    styleStripBtn (gridButton, "Grid view -- 24 channel faders on one page "
                               "(12 per row, 2 rows; scroll for more)");
    gridButton.onClick = [this]
    {
        mixerGridView = ! mixerGridView;
        zynforge::updateSettings (engine.getAppProps(), [this] (juce::PropertiesFile& props)
                                  { props.setValue ("mixerGridView", mixerGridView); });
        lastTrackCount = -1;   // force a strip relayout on the next tick
        resized();
    };
    addAndMakeVisible (gridButton);

    // Restore the engineer's preferred width from prefs.
    if (auto* props = engine.getAppProps())
    {
        const auto saved = props->getValue ("stripWidthPreset", "M");
        if      (saved == "XS") stripWidthPreset = StripWidth::XS;
        else if (saved == "S")  stripWidthPreset = StripWidth::S;
        else if (saved == "L")  stripWidthPreset = StripWidth::L;
        else                    stripWidthPreset = StripWidth::M;
        mixerGridView = props->getBoolValue ("mixerGridView", false);
    }

    addChannelButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
    addChannelButton.setColour (juce::TextButton::textColourOffId, brand::lift (brand::accentStatus, 0.10f));
    addChannelButton.setTooltip ("Set the number of recording channels -- opens a prompt where you type the count (1-256).");
    addChannelButton.onClick = [this]
    {
        if (sessionIoBusy.load())
        {
            showStatus ("Wait for the session operation to finish");
            return;
        }
        if (engine.isRecording())
        {
            showStatus ("Stop recording before changing channel count");
            return;
        }

        juce::Component::SafePointer<MainComponent> self (this);
        zynforge::AddTracksDialog::launch (
            [self, this] (const std::vector<zynforge::AddTracksDialog::Entry>& entries)
        {
            if (self == nullptr || entries.empty()) return;

            const int existing = engine.getRecorder().getNumTracks();
            int firstNew = existing;
            int newTracks = 0;
            for (const auto& e : entries)
                newTracks += e.count * (e.stereo ? 2 : 1);
            const int target = juce::jlimit (0, 256, existing + newTracks);
            engine.setStripCount (target);

            // Apply per-row naming + stereo flags to the just-added strips.
            int cursor = firstNew;
            int totalStereo = 0;
            for (const auto& e : entries)
            {
                for (int i = 0; i < e.count && cursor < target; ++i)
                {
                    const int suffix = e.count > 1 ? i + 1 : 0;
                    const auto base  = e.baseName.isNotEmpty() ? e.baseName : juce::String (cursor + 1);
                    if (e.stereo && cursor + 1 < target)
                    {
                        const auto label = suffix > 0 ? base + " " + juce::String (suffix) : base;
                        engine.setTrackName  (cursor,     label);
                        engine.setTrackName  (cursor + 1, label + " R");
                        engine.setTrackStereo (cursor,     true);
                        engine.setTrackStereo (cursor + 1, false);
                        // Hard-pan the new pair L/R so it reproduces its full
                        // stereo image from the start. Matches import.
                        engine.setTrackPan (cursor,     -1.0f);
                        engine.setTrackPan (cursor + 1,  1.0f);
                        cursor += 2;
                        ++totalStereo;
                    }
                    else
                    {
                        engine.setTrackName  (cursor, suffix > 0 ? base + " " + juce::String (suffix)
                                                                  : base);
                        engine.setTrackStereo (cursor, false);
                        ++cursor;
                    }
                }
            }
            lastTrackCount = -1;   // force rebuild for stereo collapse
            showStatus ("Added " + juce::String (cursor - firstNew) + " track(s)"
                        + (totalStereo > 0 ? " (" + juce::String (totalStereo) + " stereo)"
                                            : juce::String()));
        });
    };
    addAndMakeVisible (addChannelButton);

    metersButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
    metersButton.setColour (juce::TextButton::textColourOffId,   brand::lift (brand::featureEngaged, 0.10f));
    metersButton.onClick = [this]
    {
        if (auto* w = metersDialog.getComponent())
        {
            delete w;                 // second click closes the non-modal window
            metersDialog = nullptr;
            return;
        }
        openMetersWindow();
    };
    metersButton.setTooltip ("Open the floating meterbridge -- drag onto a second display.");
    addAndMakeVisible (metersButton);

    // Header "tab" buttons that open a floating panel light up while that
    // panel is open (and clear when it's closed -- by the button OR by the
    // panel's own close box; syncTabButtonStates() in the 10 Hz timer keeps
    // them in sync). The active wash uses engagedAmber, the app's "this is
    // engaged" accent. Pressing the button again closes the panel.
    for (auto* b : { &deviceButton, &patchButton, &metersButton })
    {
        b->setColour (juce::TextButton::buttonOnColourId, brand::engagedAmber);
        b->setColour (juce::TextButton::textColourOnId,   brand::onSignal (brand::engagedAmber));
    }

    // Tooltips on header controls.
    deviceButton .setTooltip ("Choose the audio device, sample rate and buffer size.");
    lockButton   .setTooltip ("System Lock: disables every other control so a stray click can't kill a take.");
    backupButton .setTooltip ("Backup & Mirror Drives -- pick the drives every track is copied to as you record, and their formats.");
    patchButton  .setTooltip ("Open the patch matrix: route hardware inputs / outputs to channel strips.");
    titleLabel   .setTooltip ("Zynforge Recording -- JUCE 8 multitrack recorder + virtual soundcheck.");

    addAndMakeVisible (bigClock);
    addAndMakeVisible (perfDashboard);
    addAndMakeVisible (toast);

    // Setlist + cue bar. Wires the three engineer actions back into
    // helpers that read/write the session's .zfproj.
    setlistBar.onPick       = [this] (int idx) { jumpToCue (idx); };
    setlistBar.onPrev       = [this] { jumpToCue (juce::jmax (0, currentCueIndex - 1)); };
    setlistBar.onNext       = [this] { jumpToCue (juce::jmin ((int) cues.size() - 1,
                                                              currentCueIndex + 1)); };
    setlistBar.onAddCue     = [this] { addCueAtTransport(); };
    setlistBar.onUpdateCue  = [this] { updateCueAtTransport(); };
    setlistBar.onRecallCue  = [this]
    {
        if (currentCueIndex >= 0) jumpToCue (currentCueIndex);
        else                      showStatus ("Pick a cue first, then Recall");
    };
    setlistBar.onRenameCue  = [this] { if (! sessionLocked) renameCurrentCue(); };
    setlistBar.onMoveCue    = [this] (int dir)
    {
        if (sessionLocked || currentCueIndex < 0 || currentCueIndex >= (int) cues.size()) return;
        const int target = currentCueIndex + dir;
        if (target < 0 || target >= (int) cues.size()) return;
        std::swap (cues[(size_t) currentCueIndex], cues[(size_t) target]);
        currentCueIndex = target;
        setlistBar.setCues (cues, currentCueIndex);
        saveCueChanges ("Cue moved");
    };
    setlistBar.onDeleteCue  = [this]
    {
        if (sessionLocked || currentCueIndex < 0 || currentCueIndex >= (int) cues.size()) return;
        const auto gone = cues[(size_t) currentCueIndex].name;
        cues.erase (cues.begin() + currentCueIndex);
        currentCueIndex = juce::jmin (currentCueIndex, (int) cues.size() - 1);
        setlistBar.setCues (cues, currentCueIndex);
        saveCueChanges ("Deleted cue '" + gone + "'");
    };
    addAndMakeVisible (setlistBar);

    editPage = std::make_unique<zynforge::EditPage> (engine);
    addChildComponent (*editPage);          // hidden by default; switchView toggles

    transportBar = std::make_unique<zynforge::TransportBar> (engine);
    transportBar->onRequestPlay   = [this] { onPlayClicked();   };
    transportBar->onRequestStop   = [this] { onStopClicked();   };
    transportBar->onRequestRecord = [this] { onRecordClicked(); };
    addAndMakeVisible (*transportBar);

    stripsViewport.setViewedComponent (&stripsContainer, false);
    stripsViewport.setScrollBarsShown (false, true);     // h-scroll only
    addAndMakeVisible (stripsViewport);
    addChildComponent (mixerPlaceholder);   // empty-state overlay (manages own visibility)

    // editPage was created above. The rest of its wiring lives here.
    editPage->canMutateTimeline = [this] { return canMutateTimeline(); };
    // Save per-session zoom into .zfproj on every idle change.
    // A zoom click during capture changes only the view. The normal stop-save
    // persists its final value without writing to the active take volume.
    editPage->onZoomChanged = [this] (float)
    {
        if (! engine.isRecording()) requestUILayoutSave();
    };

    // EDIT-view channel selection feeds the SAME shared selection set the
    // MIXER uses, so a click in either view drives Option+R bulk arm and
    // both views highlight together. The EDIT row hands us a physical
    // track index; map it to a logical strip ordinal first.
    editPage->onRowSelect = [this] (int physTrack, bool additive)
    {
        const int logical = logicalFromPhysicalIdx (physTrack);
        if (logical < 0 || logical >= (int) strips.size()) return;
        if (additive)
        {
            if (selectedLogical.count (logical)) selectedLogical.erase (logical);
            else                                 selectedLogical.insert (logical);
        }
        else
        {
            selectedLogical.clear();
            selectedLogical.insert (logical);
        }
        for (int i = 0; i < (int) strips.size(); ++i)
            if (strips[(size_t) i] != nullptr)
                strips[(size_t) i]->setSelected (selectedLogical.count (i) > 0);
        if (editPage != nullptr) editPage->repaint();
    };
    editPage->isTrackSelected = [this] (int physTrack) -> bool
    {
        return selectedLogical.count (logicalFromPhysicalIdx (physTrack)) > 0;
    };
    editPage->isStripSelectionEmpty = [this] { return selectedLogical.empty(); };
    // Option/Alt-drag with the Selector: clear the strip selection so the
    // range is global -- it shades every track and range edits hit all tracks.
    editPage->onRangeSelectAllTracks = [this]
    {
        clearStripSelection();
        if (editPage != nullptr) editPage->repaint();
    };
    editPage->onStatus = [this] (const juce::String& message) { showStatus (message); };
    editPage->consolidateRange = [this] (const std::vector<int>& targets, juce::int64 start, juce::int64 end)
    { consolidateTracks (targets, start, end); };
    // Right-click ▸ Delete Channel from the EDIT row header. Select just
    // this channel (so the highlight matches what's about to go), then run
    // the shared confirm + delete path used by the Track menu + Delete key.
    editPage->onRowDelete = [this] (int physTrack)
    {
        const int logical = logicalFromPhysicalIdx (physTrack);
        if (logical < 0 || logical >= (int) strips.size()) return;
        selectedLogical.clear();
        selectedLogical.insert (logical);
        for (int i = 0; i < (int) strips.size(); ++i)
            if (strips[(size_t) i] != nullptr)
                strips[(size_t) i]->setSelected (selectedLogical.count (i) > 0);
        if (editPage != nullptr) editPage->repaint();
        confirmDeleteChannels ([this] { deleteSelectedStrips(); });
    };
    // Clip-edit undo bridge: a clip drag (trim / move / fade) snapshots
    // the playlist JSON at mouse-down and pushes a single Cmd+Z step at
    // mouse-up. pushClipUndo no-ops when nothing changed.
    editPage->captureClipsForUndo = [this] { return engine.playlistsToJson(); };
    editPage->commitClipUndo = [this] (const juce::var& before, const juce::String& label)
    {
        pushClipUndo (label, before);
    };
    // Re-parent the EDIT-tools palette onto MainComponent so it can
    // sit on its own row above the page.
    if (auto* tb = editPage->getEditToolsBar())
        addAndMakeVisible (*tb);
    switchView (View::Mix);

    masterStrip = std::make_unique<zynforge::MasterStrip> (engine);
    addAndMakeVisible (*masterStrip);

    vcaPanel = std::make_unique<zynforge::VcaPanel> (engine);
    addChildComponent (*vcaPanel);   // hidden by default; VCA button toggles

    vcaToggleButton.setColour (juce::TextButton::buttonColourId,
                                brand::sink (brand::featureEngaged, 0.55f));
    vcaToggleButton.setColour (juce::TextButton::textColourOffId,
                                brand::lift (brand::featureEngaged, 0.10f));
    vcaToggleButton.setTooltip ("Toggle the 8-bus VCA fader panel -- group faders for "
                                 "drums / vocals / etc. Strips opt in via right-click > Assign to VCA.");
    vcaToggleButton.onClick = [this]
    {
        showVcaPanel = ! showVcaPanel;
        if (vcaPanel != nullptr) vcaPanel->setVisible (showVcaPanel);
        resized();
        requestUILayoutSave();
    };
    addAndMakeVisible (vcaToggleButton);

    setWantsKeyboardFocus (true);
    addKeyListener (this);

    rebuildStrips();
    updateTransportLabels();

    if (s_testConstruct)   // headless test: skip timers, dialogs, menu-bar reg
        return;

    startTimerHz (10);  // poll for input-channel count + transport position

    // If the previous run had a session pinned, rehydrate its setlist
    // AND its UI layout (view, strip width, VCA panel, EDIT zoom).
    loadSetlistFromActiveSession();
    juce::Component::SafePointer<MainComponent> startupSelf (this);
    juce::Timer::callAfterDelay (50, [startupSelf]
    { if (startupSelf != nullptr) startupSelf->loadUILayoutFromActiveSession(); });

    // Launch dialogs land in order so first-run + crash-recovery +
    // welcome don't fight for the front. Recovery wins first (data
    // loss is the highest-stakes prompt), then the optional first-
    // run tutorial, then the Welcome dialog. ModalComponentManager
    // serialises the queue for us once we hand each dialog over
    // through its own callAfterDelay.
    const bool firstRun = engine.getAppProps() == nullptr
                        || ! engine.getAppProps()->getBoolValue ("tutorialShown", false);

    // Crash telemetry runs late in the launch-dialog queue (recovery and
    // welcome are higher priority; ModalComponentManager serialises).
    juce::Timer::callAfterDelay (brand::motion::launchDelayMs + 1500,
                                 [startupSelf]
                                 { if (startupSelf != nullptr) startupSelf->scanForCrashReports(); });

    // Capture daemon (Phase 1d/2): restore the flag and reattach -- a take
    // left rolling by a crashed/quit GUI is adopted, not orphaned.
    if (engine.getAppProps() != nullptr
        && engine.getAppProps()->getBoolValue ("useCaptureDaemon", false))
        useCaptureDaemon = true;

    juce::Timer::callAfterDelay (brand::motion::launchDelayMs, [startupSelf, firstRun]
    {
        auto self = startupSelf;
        if (self == nullptr) return;
        // Reattach a daemon take BEFORE the crash-recovery scan: that take's
        // folder still has its recording.session marker, and the dialog would
        // otherwise offer Delete on a session that is still being written.
        if (self->useCaptureDaemon) self->connectCaptureDaemon (true);
        // The tutorial + Welcome/auto-reopen step. It MUST run only after any
        // recovery dialog closes -- running it underneath auto-reopened the
        // orphan being recovered, so "Delete" then nuked the loaded session.
        auto proceed = [self, firstRun]
        {
            if (self == nullptr) return;
            if (self->explicitDocumentOpened) return;
            if (firstRun)
            {
                zynforge::updateSettings (self->engine.getAppProps(), [] (juce::PropertiesFile& p)
                                          { p.setValue ("tutorialShown", true); });
                // Modal dialogs stack rather than queue, so Welcome waits for the tour.
                self->showFirstRunTutorial ([self] { if (self != nullptr) self->showStartupWelcome(); });
            }
            else
            {
                self->showStartupWelcome();
            }
        };

        // If the recovery dialog opens, defer `proceed` to its onClosed hook;
        // otherwise run it after the usual short delay.
        if (! self->offerSessionRecovery (proceed))
            juce::Timer::callAfterDelay (450, proceed);
    });

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (this);
   #endif
}

MainComponent::~MainComponent()
{
    // Prevent a transport callback already queued by a remote from
    // entering a partially destructed MainComponent. AudioEngine invalidates
    // its own queued-callback handle at the start of its later destruction.
    engine.setRemoteTransportHandler ({});
    engine.getRemotes().setHost ({});
    engine.onMarkerDropped = nullptr;
    engine.onTakeInterrupted = nullptr;
    engine.getMarkers().onDamagedFileKept = nullptr;
    zynforge::AudioDeviceDialog::closeSelfOwned();
    delete twinDialog.getComponent();
    twinLink.stop();

    // Cancel + join any in-flight session job (bounce, export, copy) now,
    // explicitly, before anything else in this destructor tears down state a
    // job might read. The runner's own destructor would also join, but later.
    sessionJobs.cancelAndJoin();
    metadataWriter.finish(); // drains accepted immutable saves before engine/UI teardown

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    removeKeyListener (this);
    // Clear the app-wide default BEFORE `laf` (a member) is destroyed, so no
    // lingering AlertWindow can dereference a dangling LookAndFeel on shutdown.
    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &laf)
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void MainComponent::setStripWidthPreset (StripWidth p)
{
    if (p == stripWidthPreset) return;
    stripWidthPreset = p;
    zynforge::updateSettings (engine.getAppProps(), [p] (juce::PropertiesFile& props)
    {
        const auto code = (p == StripWidth::XS) ? "XS"
                        : (p == StripWidth::S)  ? "S"
                        : (p == StripWidth::L)  ? "L" : "M";
        props.setValue ("stripWidthPreset", code);
    });
    // Reflect the active preset in the button toggle visuals + relayout
    // the strip viewport so the engineer sees the new density right away.
    stripXsButton.setToggleState (p == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (p == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (p == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (p == StripWidth::L,  juce::dontSendNotification);
    resized();
    requestUILayoutSave();
}

void MainComponent::switchView (View v)
{
    currentView = v;
    const bool mix = (v == View::Mix);

    stripsViewport.setVisible (mix);
    if (editPage != nullptr)
    {
        editPage->setVisible (! mix);
        if (! mix) editPage->refresh();   // rescan session dir on entry
    }
    requestUILayoutSave();

    // Pull keyboard focus back to MainComponent (the app's KeyListener) on
    // every view change. Without this, focus can sit on whatever button or
    // combo the engineer last touched, and EDIT-view keys -- Delete to clear
    // a range, the S/T/R/G/F/B tool keys, etc. -- silently never arrive.
    // Deferred so it runs after the visibility / layout pass settles.
    juce::Component::SafePointer<MainComponent> self (this);
    juce::MessageManager::callAsync ([self]
    {
        if (self != nullptr && self->isShowing())
            self->grabKeyboardFocus();
    });

    if (editPage != nullptr)
        if (auto* tb = editPage->getEditToolsBar())
            tb->setVisible (! mix);
    updateMixerPlaceholder();   // only shows in MIX view + when empty

    // Reflect active state in the button tinting.
    auto colour = [] (juce::TextButton& b, bool engaged)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     engaged ? brand::accentStatus.withAlpha (zynforge::brand::alpha::wash)
                             : brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId,
                     engaged ? brand::accentStatus : brand::textSecondary);
    };
    colour (mixViewButton,  mix);
    colour (editViewButton, ! mix);

    // Single layout pass after every visibility / colour change above
    // (was two -- the redundant re-flow is gone).
    resized();
}

// Every session swap and layout change comes through rebuildStrips. The strip
// selection holds positions in ONE layout of ONE session: left alone, a range
// edit, Delete or a paste hits whatever now sits at the old index.
void MainComponent::syncStripSelectionScope()
{
    const int layout = engine.getRecorder().getTrackGeneration();
    const auto session = engine.getActiveSessionDir();
    if (layout != selectionLayoutGen || session != selectionSession)
    {
        selectedLogical.clear();
        selectionLayoutGen = layout;
        selectionSession   = session;
    }
}

void MainComponent::rebuildStrips()
{
    const juce::ScopedValueSetter<bool> guard (rebuildingStrips, true);
    syncStripSelectionScope();
    auto& recorder = engine.getRecorder();
    const int n = recorder.getNumTracks();

    strips.clear();
    strips.reserve ((std::size_t) n);
    const int numIns  = engine.getCurrentDeviceInputCount();
    const int numOuts = engine.getCurrentDeviceOutputCount();

    // Iterate logical strips: a stereo L track owns its R partner and only
    // creates a single ChannelStrip representing the pair.
    int i = 0;
    while (i < n)
    {
        auto& tL = recorder.getTrack (i);
        const bool stereo = tL.isStereo.load() && (i + 1 < n);
        TrackState* tR = stereo ? &recorder.getTrack (i + 1) : nullptr;
        const int  step = stereo ? 2 : 1;

        juce::Component::SafePointer<MainComponent> colourOwner (this);
        auto colourCb = [colourOwner, i, step] (juce::Colour chosen)
        {
            if (colourOwner == nullptr) return;
            // If the engineer has multiple strips selected (Shift /
            // Cmd-click), apply the colour to every one of them so
            // 'select 8 strips, set colour' works as expected. The
            // single-strip path still works because mouseDown selects
            // this strip first; selectedLogical will contain at least
            // this one when the colour picker opens via right-click.
            if (colourOwner->selectedLogical.size() > 1)
            {
                for (int logical : colourOwner->selectedLogical)
                {
                    if (logical < 0 || logical >= (int) colourOwner->strips.size()) continue;
                    const auto& s = colourOwner->strips[(size_t) logical];
                    const int phys = s->getStripIndex();
                    colourOwner->engine.setTrackColour (phys, chosen);
                    if (s->isStereo()) colourOwner->engine.setTrackColour (phys + 1, chosen);
                }
            }
            else
            {
                colourOwner->engine.setTrackColour (i, chosen);
                if (step == 2) colourOwner->engine.setTrackColour (i + 1, chosen);
            }
        };
        // A stereo pair is ONE logical strip, so the name mirrors onto the R
        // half like gain / mute / solo / colour already do. Without this the R
        // track kept its old name in session_mix.json and showed it the moment
        // the pair was unlinked.
        auto nameCb   = [this, i, step] (juce::String chosen)
        {
            engine.setTrackName (i, chosen);
            if (step == 2 && i + 1 < engine.getRecorder().getNumTracks())
                engine.setTrackName (i + 1, chosen.isEmpty() ? juce::String()
                                                             : chosen + " R");
        };
        // Pro Tools-style Edit Group broadcast. When strip `i` is
        // in an edit group, the gain / pan change is mirrored onto
        // every other strip in the same group. Returns the list of
        // peers (excluding self).
        auto editGroupPeers = [this] (int src) -> std::vector<int>
        {
            const int g = engine.getTrackEditGroup (src);
            if (g < 0) return {};
            auto all = engine.getStripsInEditGroup (g);
            all.erase (std::remove (all.begin(), all.end(), src), all.end());
            return all;
        };

        auto gainCb   = [this, i, step, editGroupPeers] (float dB)
        {
            engine.setTrackGainDb (i, dB);
            if (step == 2)
                engine.setTrackGainDb (i + 1, dB);
            for (int peer : editGroupPeers (i))
                engine.setTrackGainDb (peer, dB);
        };
        // L pan persists to track i. For a stereo strip, the R pan
        // travels through its own panRCb (below) -- the two sides are
        // INDEPENDENT, not mirrored, so the engineer can pan the L
        // channel hard-left and R hard-right (or whatever they want).
        auto panCb  = [this, i, editGroupPeers] (float pan)
        {
            engine.setTrackPan (i, pan);
            for (int peer : editGroupPeers (i))
                engine.setTrackPan (peer, pan);
        };
        auto panRCb = [this, i] (float pan)
        {
            engine.setTrackPan (i + 1, pan);
        };

        // Stereo routing: L → device[N], R → device[N+1].
        auto inCb = [this, i, step] (int dev)
        {
            engine.setTrackLinkedRouting (i, dev);
            if (step == 2)
                engine.setTrackLinkedRouting (i + 1, (dev < 0) ? -1 : dev + 1);
        };
        auto outCb = [this, i, step] (int dev)
        {
            engine.setTrackLinkedRouting (i, dev);
            if (step == 2)
                engine.setTrackLinkedRouting (i + 1, (dev < 0) ? -1 : dev + 1);
        };

        auto s = std::make_unique<ChannelStrip> (i, tL,
                                                 std::move (colourCb),
                                                 std::move (nameCb),
                                                 std::move (gainCb),
                                                 std::move (panCb),
                                                 std::move (inCb),
                                                 std::move (outCb),
                                                 tR,
                                                 (step == 2) ? std::move (panRCb)
                                                             : ChannelStrip::FloatCallback{});

        // Right-click menu wiring.
        auto deleteCb     = [this, i, step]
        {
            if (engine.isRecording() || sessionIoBusy.load() || sessionLocked)
            {
                // The strip already stopped its meter timer (ChannelStrip
                // invalidates before calling this). Rebuild it, or it stays
                // frozen for the rest of the show.
                lastTrackCount = -1;
                showStatus (sessionLocked         ? "LOCKED -- click UNLOCK to resume control"
                            : engine.isRecording() ? "Stop recording before removing a strip"
                                                   : "Wait for the session operation to finish");
                return;
            }
            std::vector<int> order;
            for (int t = 0; t < engine.getRecorder().getNumTracks(); ++t)
                if (t < i || t >= i + step) order.push_back (t);
            condemnAllStrips();
            const bool ok = engine.reorderTracks (order);
            lastTrackCount = -1;
            showStatus (ok ? "Strip removed; audio retained in Removed Tracks"
                        : order.empty() ? "A saved session keeps at least one strip; nothing was removed"
                        : "Could not remove strip safely");
        };
        auto addCb        = [this] { if (! sessionLocked) engine.addOneStrip(); };
        auto linkStereoCb = [this, i]
        {
            if (i + 1 < engine.getRecorder().getNumTracks())
                setStereoLink (i, ! engine.getRecorder().getTrack (i).isStereo.load());
        };
        s->setMenuCallbacks (std::move (deleteCb),
                             std::move (addCb),
                             std::move (linkStereoCb));

        // VCA assignment from the strip's right-click menu -- the
        // ChannelStrip sets state.vcaGroup directly for immediate
        // audio-thread effect; this callback persists the choice
        // through appProps so a relaunch restores it. For stereo
        // pairs both halves must persist, otherwise R loses its
        // assignment on relaunch even though it ran with L in-memory.
        s->onVcaGroupChanged = [this, i, step] (int vcaIdx)
        {
            engine.setTrackVcaGroup (i, vcaIdx);
            if (step == 2) engine.setTrackVcaGroup (i + 1, vcaIdx);
        };

        s->onEditGroupChanged = [this, i, step] (int groupId)
        {
            engine.setTrackEditGroup (i, groupId);
            if (step == 2) engine.setTrackEditGroup (i + 1, groupId);
            showStatus (groupId < 0
                          ? juce::String ("Strip ") + juce::String (i + 1) + " unlinked"
                          : juce::String ("Strip ") + juce::String (i + 1)
                              + " -> Edit Group " + juce::String (groupId + 1));
        };

        s->onStreamSendChanged = [this, i] (bool on)
        {
            engine.setTrackStream (i, on);
            if (on && engine.getStreamOutputL() < 0 && ! engine.getRecordStereoMix())
                showStatus ("STREAM send is on, but the stream bus has no output: pick one in "
                            "Session > Recording & Sync > Stream Bus Output, or record StereoMix");
        };

        // Edit-group broadcast for the four toggles. The strip
        // mutates its own state immediately; we mirror to every
        // peer in the same edit group so a single click on, say,
        // SnareTop's MUTE mutes the whole drum kit when it's
        // grouped. No-op when the strip isn't grouped.
        auto broadcastToggle = [this] (int src,
                                       auto stateField)
        {
            const int g = engine.getTrackEditGroup (src);
            if (g < 0) return;
            const auto srcVal =
                (engine.getRecorder().getTrack (src).*stateField).load (std::memory_order_relaxed);
            for (int peer : engine.getStripsInEditGroup (g))
            {
                if (peer == src) continue;
                (engine.getRecorder().getTrack (peer).*stateField)
                    .store (srcVal, std::memory_order_relaxed);
            }
        };
        s->canChangeArm = [this] { return ! engine.isRecording(); };
        s->onAfterArmedToggle    = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::armed);   };
        s->onAfterMonitorToggle  = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::monitor); };
        s->onAfterMuteToggle     = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::muted);   };
        s->onAfterSoloToggle     = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::soloed);  };

        // Hook shift/cmd-click selection. The toggle handler is keyed
        // on the LOGICAL strip index (i.e. stereo pairs count as one).
        const int logicalIdx = (int) strips.size();
        s->onToggleSelection = [this, logicalIdx] (bool additive)
        {
            if (additive)
            {
                // Shift / Cmd-click: toggle this strip in/out, keep
                // existing selection.
                if (selectedLogical.count (logicalIdx) > 0)
                    selectedLogical.erase (logicalIdx);
                else
                    selectedLogical.insert (logicalIdx);
            }
            else
            {
                // Plain click: make this strip the sole selection.
                // Clicking the only-selected strip a second time
                // clears the selection entirely (toggle-off).
                const bool wasOnlySelected = selectedLogical.size() == 1
                                          && selectedLogical.count (logicalIdx) > 0;
                selectedLogical.clear();
                if (! wasOnlySelected) selectedLogical.insert (logicalIdx);
            }
            for (size_t k = 0; k < strips.size(); ++k)
                strips[k]->setSelected (selectedLogical.count ((int) k) > 0);
            showStatus (juce::String ((int) selectedLogical.size()) + " strip(s) selected");
        };
        s->setSelected (selectedLogical.count (logicalIdx) > 0);

        // Tab / Shift+Tab in a strip's name field hops to the next /
        // previous strip (wrapping), scrolls it into view, and opens its
        // editor -- so a whole input list renames keyboard-only.
        s->onTabRename = [this] (int fromStripIndex, bool shift)
        {
            const int count = (int) strips.size();
            if (count == 0) return;
            int pos = -1;
            for (int k = 0; k < count; ++k)
                if (strips[(size_t) k] != nullptr
                    && strips[(size_t) k]->getStripIndex() == fromStripIndex) { pos = k; break; }
            if (pos < 0) return;
            const int target = ((pos + (shift ? -1 : 1)) % count + count) % count;
            if (auto* st = strips[(size_t) target].get())
            {
                const auto inView = stripsViewport.getLocalArea (&stripsContainer, st->getBounds());
                if (inView.getX() < 0)
                    stripsViewport.setViewPosition (st->getX(), 0);
                else if (inView.getRight() > stripsViewport.getWidth())
                    stripsViewport.setViewPosition (
                        st->getRight() - stripsViewport.getMaximumVisibleWidth(), 0);
                st->beginRename();
            }
        };

        s->setAvailableInputs  (numIns);
        s->setAvailableOutputs (numOuts);
        // Any rebuild (GRID, a job finishing, a device change) makes new
        // strips, and LOCK must hold for those too.
        s->setEnabled (! sessionLocked);
        stripsContainer.addAndMakeVisible (*s);
        strips.push_back (std::move (s));

        i += step;
    }
    lastTrackCount = n;
    lastTrackGen   = recorder.getTrackGeneration();
    resized();
    updateMixerPlaceholder();
}
