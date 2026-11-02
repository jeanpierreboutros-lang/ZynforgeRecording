// MainComponent's macOS menu-bar surface -- getMenuBarNames,
// getMenuForIndex (the full menu construction), and menuItemSelected
// (the dispatch switch). Extracted from MainComponent.cpp as part of
// the 2026-05-24 god-class split.
//
// menuItemSelected dispatches to ~50 MainComponent member functions
// that still live in MainComponent.cpp (or in the other split TUs);
// all calls work transparently since they're member-function calls
// on the same `this`.

#include "MainComponent.h"
#include "../Audio/VirtualSoundcheck.h"
#include "../Theme/DialogChrome.h"
#include "PatchPage.h"
#include "Meterbridge.h"
#include "MarkerListDialog.h"
#include "TimecodeSyncDialog.h"
#include "ControlSurfacesDialog.h"
#include "DeskMacroDialog.h"
#include "SessionSettingsDialog.h"
#include "../Audio/AtomicFile.h"

using namespace zynforge;

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Track", "Session", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelIndex == 0)  // File
    {
        // Session LOCK also disables these -- LOCK is a "don't touch" safety for
        // a running show, but only the buttons + keyboard were gated, so the
        // menu bar still let a locked show New/Open/Close/Import/Export.
        const bool sessionEditable = ! engine.isRecording() && ! sessionLocked
                                  && ! sessionIoBusy.load();
        // Song Export has its own cancel in the Export menu.
        if (const auto job = sessionJobs.runningJob(); job.isNotEmpty() && ! songExportRunning)
        {
            menu.addItem (981, "Cancel " + job);
            menu.addSeparator();
        }
        menu.addItem (5, "New Session...",         sessionEditable);
        // Opening another session mid-take re-pins the active dir + applies the
        // new session's mixer/routing onto the strips being recorded, then STOP
        // re-pins back -- corrupting the capture. Gate it like New/Import/Close.
        menu.addItem (1, "Open Session...",        sessionEditable);
        menu.addItem (4, "Import Audio Files...",  sessionEditable);
        menu.addSeparator();
        // Save / Save As are always enabled: if there's no active
        // session yet, Save falls through to Save As (picker), and
        // Save As is by definition a picker so it never needs prior
        // state.
        menu.addItem (2, "Save\tCmd+S", ! sessionLocked && ! captureMetadataPending
                      && (! sessionIoBusy.load() || pendingMetadataSaves != 0));
        menu.addItem (3, "Save As...", ! sessionIoBusy.load());
        menu.addItem (951, "Auto-Save & Backup...");
        menu.addSeparator();
        // Close the session back to the Welcome screen WITHOUT quitting --
        // so the engineer can start/open another session between sets.
        menu.addItem (6, "Close Session", (engine.hasSessionFolder() || sessionFolderIsMissing())
                                          && sessionEditable);
        menu.addSeparator();

        juce::PopupMenu exportMenu;
        // Export/bounce read the Track_NN files synchronously on the message
        // thread. During a take those headers aren't finalised (truncated
        // exports) and the read I/O competes with the recorder + freezes the
        // UI, so gate the whole submenu on not-recording. CSV timeline export
        // is metadata-only but gated too for a consistent "not mid-take" rule.
        const bool hasActive = engine.hasSessionFolder();
        const bool canExport = hasActive && sessionEditable;   // also blocked while LOCKED
        const int n = engine.getRecorder().getNumTracks();
        exportMenu.addItem (970, "Export Audio...", canExport && n > 0);
        exportMenu.addSeparator();
        exportMenu.addItem (14, "Export Timeline (markers/cues/tracks) as CSV...", canExport && n > 0);
        exportMenu.addItem (966, "Export Reaper Project...", canExport && n > 0);
        exportMenu.addItem (965, "Export DAWproject...",     canExport && n > 0);
        exportMenu.addItem (962, "Create Verified Show Handoff...", canExport && n > 0);
        exportMenu.addSeparator();
        // The same item stops a running export: nothing else in the menus can
        // reach the job while it holds the session.
        if (songExportRunning)
            exportMenu.addItem (967, "Cancel Song Export");
        else
            exportMenu.addItem (967, "Export Songs...", canExport && n > 0);
        menu.addSubMenu ("Export", exportMenu);
        menu.addSeparator();

        // Session templates -- save the current strip config (count,
        // names, colours, routings, stereo flags) to a reusable
        // template, or start a new session pre-populated from one.
        menu.addItem (71, "Print setlist...", ! cues.empty() && ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (70, "Save Current as Template...",
                      engine.getRecorder().getNumTracks() > 0
                       && ! engine.isRecording());

        juce::PopupMenu templates;
        const auto tList    = listSessionTemplates();
        const auto starred  = getDefaultTemplate();
        // The ids after 200-249 are other commands, so the list stops there.
        // "Delete a template..." shows every one, which is the way back to the rest.
        const int tShown = juce::jmin (tList.size(), kMenuTemplateSlots);
        for (int i = 0; i < tShown; ++i)
        {
            const bool isDefault = (tList[i] == starred);
            const auto label = tList[i].getFileNameWithoutExtension()
                               + (isDefault ? juce::String ("  [default]") : juce::String());
            templates.addItem (200 + i, label);
        }
        if (tList.isEmpty())
            templates.addItem (-1, "(no templates yet)", false);
        else
        {
            if (tList.size() > tShown)
                templates.addItem (-1, juce::String (tList.size() - tShown) + " more not shown: delete some to reach them", false);
            templates.addSeparator();

            // 'Set default template' sub-popup: lists every template + 'None'.
            // The default auto-applies after File ▸ New Session creates the
            // folder, so the engineer starts each show pre-patched.
            juce::PopupMenu setDefault;
            setDefault.addItem (260, "(None)", true, ! starred.existsAsFile());
            for (int i = 0; i < tShown; ++i)
                setDefault.addItem (2000 + i, tList[i].getFileNameWithoutExtension(),
                                    true, tList[i] == starred);
            templates.addSubMenu ("Set default template", setDefault);

            templates.addItem (250, "Delete a template...");
        }
        menu.addSubMenu ("New Session from Template", templates, ! engine.isRecording());
        menu.addItem (7, "New Session from CSV (channel names)...", ! engine.isRecording());

        menu.addSeparator();
        menu.addItem (61, "Backup & Mirror Drives...", ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (99, "Quit ZynForge Recording...");
    }
    else if (topLevelIndex == 1)  // Edit -- clip / audio / timeline editing
    {
        // The Edit menu is now strictly about editing the *audio on the
        // timeline*. Channel / mixer-strip management moved to its own
        // "Track" menu (below), the way a real DAW separates the two.
        const bool canEdit = engine.getPlayer().isLoaded()
                          && ! engine.isRecording()
                          && ! sessionIoBusy.load()
                          && ! sessionLocked;

        menu.addItem (300, "Undo\tCmd+Z",       canEdit && undoManager.canUndo());
        menu.addItem (301, "Redo\tCmd+R",       canEdit && undoManager.canRedo());
        menu.addSeparator();
        menu.addItem (310, "Separate Clip (split at selection / playhead)\tCmd+E",
                      canEdit);
        menu.addItem (315, "Heal Separation\tCmd+H", canEdit);
        menu.addItem (306, "Crop to Loop Range\tCtrl+Cmd+C",
                      canEdit && engine.getPlayer().hasLoopRegion());
        menu.addItem (317, "Consolidate Selection",
                      canEdit && engine.getPlayer().hasLoopRegion());
        menu.addSeparator();
        menu.addItem (311, "Start Range at Playhead\t,",  canEdit);
        menu.addItem (312, "Finish Range at Playhead\t.", canEdit);
        menu.addItem (308, "Set Range to Loop Range", canEdit);
        menu.addSeparator();
        menu.addItem (309, "Toggle Snap\tCtrl+4", ! sessionLocked, snapToMarkers);
        menu.addItem (318, "Snap edits to zero-crossing (click-free)", true, zeroCrossSnap);
        menu.addItem (314, "Punch In/Out Mode",
                      canEdit, engine.isPunchModeOn());
        menu.addSeparator();
        menu.addItem (313, "Remove Last Capture", ! engine.isRecording());
    }
    else if (topLevelIndex == 2)  // Track -- channel / mixer-strip management
    {
        const bool hasSelection  = ! selectedLogical.empty();
        const bool hasClipboard  = stripClipboard.isObject();
        const int  selCount      = (int) selectedLogical.size();
        const int  stripCount    = (int) strips.size();

        menu.addItem (302, "Cut Selected Strips\tCmd+X",  hasSelection);
        menu.addItem (303, "Copy Selected Strips\tCmd+C", hasSelection);
        menu.addItem (304, "Paste Strip Settings\tCmd+V",   hasClipboard && hasSelection);
        menu.addItem (305, "Delete Selected Strips",        hasSelection);
        menu.addSeparator();
        menu.addItem (307, "Solo Selection\tA",   hasSelection);
        menu.addSeparator();
        // Batch ops -- sweep a range of channels in one dialog instead of
        // renaming / recolouring 24 strips by hand.
        menu.addItem (320, "Batch Rename Channels...",  engine.getRecorder().getNumTracks() > 0);
        menu.addItem (322, "Import Channel Names from CSV...", engine.getRecorder().getNumTracks() > 0);
        menu.addItem (321, "Batch Colour Channels...",  engine.getRecorder().getNumTracks() > 0);
        menu.addSeparator();
        juce::PopupMenu sel;
        sel.addItem (335, "Select all strips\tCmd+A",  stripCount > 0);
        sel.addItem (334, "Clear selection\tEsc",      selCount > 0);
        sel.addSeparator();
        sel.addItem (330, "Move selected up",         selCount > 0);
        sel.addItem (331, "Move selected down",       selCount > 0);
        sel.addSeparator();
        sel.addItem (332, "Colour selected...",         selCount > 0);
        sel.addItem (333, "Delete selected",          selCount > 0);
        menu.addSubMenu ("Selection (" + juce::String (selCount) + ")", sel, true);
    }
    else if (topLevelIndex == 3)  // Session
    {
        // The Session menu had grown to ~25 flat items. Keep the primary,
        // frequently-reached actions at the top level and group the rest into
        // submenus. IDs + enable conditions are UNCHANGED -- only the menu
        // structure is reorganised -- so menuItemSelected dispatch and the
        // menu-state signature keep working untouched.
        menu.addItem (54, "Show pre-flight checklist...");
        menu.addSeparator();
        menu.addItem (50, "Patch...");
        menu.addItem (52, zynforge::vsc::hasSavedPatch (engine)
                              ? "Restore Outputs From Before Virtual Soundcheck..."
                              : "Virtual Soundcheck -- repatch outputs to match inputs...");
        menu.addItem (51, "Meterbridge...");
        menu.addSeparator();
        menu.addItem (56, "Add Marker...\tM",
                      engine.hasSessionFolder());
        menu.addItem (53, "Memory Locations...");
        menu.addSeparator();

        // ── Recording & Sync ─────────────────────────────────────────────
        {
            juce::PopupMenu m;
            // Auto-arm: walk the stage, hit each mic, watch strips arm.
            m.addItem (62, "Auto-arm on input detect", true,
                       engine.isAutoArmOnInputDetect());
            m.addSeparator();
            m.addItem (255, "Session Format & Recording...", ! engine.isRecording());
            const int noTrimReference = engine.isTrimFollowEnabled()
                                          ? engine.getTrimFollowUnreferencedCount() : 0;
            m.addItem (256, noTrimReference > 0
                               ? "Trim-Follow (" + juce::String (noTrimReference) + " with no capture reference, held at 0 dB)"
                               : juce::String ("Trim-Follow (console input gain -> soundcheck)"),
                       true, engine.isTrimFollowEnabled());
            m.addItem (991, "Let the Mix Share Outputs With Patched Strips", true,
                       engine.getMixMayShareStripOutputs());
            m.addItem (990, engine.getStreamOutputL() >= 0
                               ? "Stream Bus Output (Out " + juce::String (engine.getStreamOutputL() + 1) + "-"
                                     + juce::String (engine.getStreamOutputR() + 1) + ")..."
                               : juce::String ("Stream Bus Output (off)..."));
            // Transport Sync -- chase MTC / LTC with live status.
            m.addItem (600, engine.getChaseMode() != zynforge::AudioEngine::ChaseMode::Off
                               ? juce::String ("Transport Sync (chasing)...")
                               : juce::String ("Transport Sync (MTC / LTC)..."));
            m.addSeparator();
            m.addItem (958, useCaptureDaemon
                               ? (lostDaemonTakeUnknown()
                                      ? "Capture daemon: NOT ANSWERING (turn off...)"
                                      : captureSupervisor.isDaemonRecording()
                                      ? "Capture daemon: TAKE ROLLING (stop to disable)"
                                      : "Capture daemon: ON (experimental)")
                               : juce::String ("Capture daemon: off (experimental)..."));
            m.addItem (975, "Reconnect Capture Daemon",
                       useCaptureDaemon && ! captureSupervisor.isAttached());
            menu.addSubMenu ("Recording & Sync", m);
        }

        // ── Network & Surfaces ───────────────────────────────────────────
        {
            juce::PopupMenu m;
            // Control Surfaces hub -- OSC + every console/surface protocol.
            m.addItem (720, "Control Surfaces...");

            m.addSeparator();
            m.addItem (968, "Twin Recorder...", true, twinLink.isEnabled());
            menu.addSubMenu ("Network & Surfaces", m);
        }

        // ── Console link (ids 954-957, outside every dispatch range) ──────
        {
            juce::PopupMenu m;
            m.addItem (954, consoleLink.isConnected()
                               ? "Console: disconnect " + consoleLink.getHost() + " (" + consoleLink.healthText() + ")"
                               : juce::String ("Console: connect..."));
            // Repatch + gain items only light up for a desk we drive over OSC.
            // Write-tier items also require a TRUSTWORTHY dialect: either the
            // address model is one we trust (the X32 reference) or the console
            // answered the connect-time handshake. An unconfirmed desk stays
            // read-only, so grey the write items rather than offering an action
            // that would be refused.
            const bool consoleWritable = consoleLink.isConnected() && consoleLink.canWrite();
            const bool consoleRepatch = consoleWritable && consoleLink.getProfile().canRepatch
                                     && ! engine.isRecording() && ! captureMetadataPending;
            const bool consoleGains   = consoleWritable && consoleLink.getProfile().canCaptureGains;
            m.addItem (955, consoleLink.isOnSoundcheckPatch()
                               ? "Console: back to STAGE patch"
                               : "Console: SOUNDCHECK patch (card returns)",
                       consoleRepatch);
            m.addItem (987, "Console: check the desk's patch",
                       consoleLink.isConnected() && consoleLink.getProfile().canRepatch);
            m.addItem (956, "Console: capture head-amp gains", consoleGains);
            m.addItem (957, "Console: restore head-amp gains",
                       consoleGains && ! engine.isRecording() && ! captureMetadataPending);
            m.addSeparator();
            m.addItem (963, "Console: follow scenes to markers (virtual soundcheck)",
                       true, engine.isSceneFollowEnabled());
            m.addItem (964, "Console: desk macro control...",
                       true, engine.getRemotes().isDeskMacroListening());
            m.addItem (969, "Console: Save Console Log...", ! engine.isRecording());
            menu.addSubMenu ("Console", m);
        }

        // ── Analysis & Reports (ids 952/953 outside dispatch ranges) ──────
        {
            juce::PopupMenu m;
            m.addItem (281, "Write soundcheck report",
                       engine.hasSessionFolder() && ! engine.isRecording());
            // Each reads every take file in full: never during a take.
            const bool canAnalyse = engine.hasSessionFolder() && ! engine.isRecording();
            m.addItem (952, "Post-show QC report...",             canAnalyse);
            m.addItem (953, "Detect songs -> markers...",         canAnalyse);
            menu.addSubMenu ("Analysis & Reports", m);
        }

        menu.addSeparator();
        menu.addItem (251, "Session Info...", engine.hasSessionFolder() && ! engine.isTakeOnDisk());
    }
    else if (topLevelIndex == 4)  // Help
    {
        menu.addItem (900, "Keyboard Shortcuts...");
        menu.addItem (903, "User Guide (panel tips)...");
        menu.addItem (901, "Quick Start (replay tutorial)");
        menu.addSeparator();
        menu.addItem (902, "About ZynForge Recording");
    }

    // macOS draws the menu bar itself, so the look-and-feel never sees these.
    for (juce::PopupMenu::MenuItemIterator item (menu, true); item.next();)
        item.getItem().text = zynforge::typeset (item.getItem().text);

    return menu;
}

void MainComponent::refreshMenuStateIfChanged()
{
    // Build a cheap signature of every condition that gates a menu item's
    // enabled state. When it changes, tell the OS to re-query the menu so the
    // greyed/lit states actually update (macOS caches them otherwise).
    auto& player = engine.getPlayer();
    juce::String sig;
    sig << (int) player.isLoaded()
        << '|' << (int) undoManager.canUndo()
        << '|' << (int) undoManager.canRedo()
        << '|' << engine.getRecorder().getNumTracks()
        << '|' << (int) selectedLogical.size()
        << '|' << (int) engine.isRecording()
        << '|' << (int) sessionIoBusy.load()
        << '|' << (int) (pendingMetadataSaves != 0)
        << '|' << (int) captureMetadataPending
        << '|' << (int) sessionLocked
        << '|' << (int) player.hasLoopRegion()
        << '|' << (int) engine.hasSessionFolder()
        << '|' << (int) sessionFolderIsMissing()
        << '|' << (int) engine.isPunchModeOn()
        << '|' << (int) snapToMarkers
        << '|' << (int) cues.empty()
        << '|' << (int) stripClipboard.isObject()
        << '|' << (int) zynforge::vsc::hasSavedPatch (engine)
        << '|' << (int) engine.isTrimFollowEnabled()
        << '|' << engine.getStreamOutputL()
        << '|' << (int) engine.getMixMayShareStripOutputs()
        << '|' << (engine.isTrimFollowEnabled() ? engine.getTrimFollowUnreferencedCount() : 0)
        << '|' << (int) engine.getChaseMode()
        << '|' << engine.getLtcSourceStrip()
        << '|' << (int) twinLink.isEnabled()
        << '|' << (int) consoleLink.isConnected()
        << '|' << (int) consoleLink.getHealth()
        << '|' << (int) consoleLink.getPatch()
        << '|' << (int) consoleLink.isOnSoundcheckPatch()
        << '|' << (int) consoleLink.getProfile().canRepatch
        << '|' << (int) consoleLink.getProfile().canCaptureGains
        // The handshake flips canWrite() asynchronously AFTER connect, so it
        // must be in the signature or the write items stay greyed until some
        // unrelated state change happens to refresh the menu.
        << '|' << (int) consoleLink.canWrite()
        << '|' << (int) engine.isSceneFollowEnabled()
        << '|' << (int) engine.getRemotes().isDeskMacroListening()
        << '|' << (int) useCaptureDaemon
        << '|' << (int) captureSupervisor.isAttached()
        << '|' << (int) (useCaptureDaemon && captureSupervisor.isDaemonRecording())
        << '|' << (int) engine.isTakeOnDisk()
        << '|' << sessionJobs.runningJob()
        << '|' << (int) songExportRunning;

    if (sig != lastMenuStateSig)
    {
        lastMenuStateSig = sig;
        menuItemsChanged();   // re-queries getMenuForIndex -> fresh enabled states
    }
}

void MainComponent::menuItemSelected (int id, int /*topLevelIndex*/)
{
    if (sessionLocked)
    { showStatus ("LOCKED -- click UNLOCK to resume control"); return; }
    if (sessionIoBusy.load())
    {
        const bool safeDuringMetadata = pendingMetadataSaves != 0
            && ((id == 2 && ! captureMetadataPending) || id == 334 || id == 335 || id == 303);
        const bool stopsSongExport = id == 967 && songExportRunning;
        if (! safeDuringMetadata && ! stopsSongExport && id != 981)
        { showStatus ("Wait for the session operation to finish"); return; }
    }
    juce::Logger::writeToLog ("[ZF] menuItemSelected id=" + juce::String (id));

    if (menuCommands.isEmpty()) registerMenuCommands();
    menuCommands.dispatch (id);

}

// ── Menu command table ─────────────────────────────────────────────────────
// Every menu-bar id maps here. Exact ids always win over ranges, ranges may
// not overlap, and MenuDispatchTests fails on any exact id inside a range or
// any built menu item without a handler (MenuCommandTable.h). The former
// if/else chain also carried handlers for ids nothing built any more (OSC
// dialects 110..116 -- the OSC button and Control Surfaces own those; LTC/MTC
// 610..699 -- the Timecode Sync dialog; per-track export 100..199 -- the
// export popup dispatches itself); they are gone.
void MainComponent::registerMenuCommands()
{
    auto& m = menuCommands;
    auto replaceSession = [this] (std::function<void()> next)
    { return [this, next] { confirmSessionReplacement (next); }; };

    // File
    m.add (1,   replaceSession ([this] { onLoadSessionClicked(); }));
    m.add (2,   [this] { onSaveSessionState(); });
    m.add (3,   [this] { onSaveSessionAs(); });
    m.add (4,   [this] { onImportAudioFiles(); });
    m.add (5,   replaceSession ([this] { launchNewSessionDialog(); }));
    m.add (6,   [this] { closeSession(); });
    m.add (7,   replaceSession ([this] { createSessionFromCsv(); }));
    m.add (970, [this] { onExportAudio(); });
    m.add (14,  [this] { exportTimelineCsv(); });
    m.add (962, [this] { exportShowHandoff(); });
    m.add (965, [this] { exportDawProject (projectexport::Format::DawProject); });
    m.add (966, [this] { exportDawProject (projectexport::Format::Reaper); });
    m.add (967, [this]
    {
        if (songExportRunning) { sessionJobs.cancel(); showStatus ("Cancelling song export..."); }
        else                   exportSongs();
    });
    m.add (981, [this]
    {
        const auto job = sessionJobs.runningJob();
        if (job.isEmpty()) return;
        sessionJobs.cancel();
        showStatus ("Cancelling " + job + "...");
    });
    m.add (70,  [this] { promptSaveSessionTemplate(); });
    m.add (71,  [this] { printSetlist(); });
    m.addRange (200, 200 + kMenuTemplateSlots - 1, [this] (int offset)
    {
        const auto list = listSessionTemplates();
        if (offset < list.size())
        {
            const auto selected = list[offset];
            confirmSessionReplacement ([this, selected] { launchNewSessionDialog (selected); });
        }
    });
    m.add (250, [this] { promptDeleteSessionTemplate(); });
    m.add (260, [this] { setDefaultTemplate ({}); });
    m.addRange (2000, 2099, [this] (int offset)
    {
        const auto list = listSessionTemplates();
        if (offset < list.size()) setDefaultTemplate (list[offset]);
    });
    m.add (61,  [this] { onBackupClicked(); });
    m.add (99,  [this] { confirmAndQuit(); });

    // Session
    m.add (50,  [this] { openPatchWindow(); });
    m.add (51,  [this] { openMetersWindow(); });
    m.add (52,  [this] { onVscClicked(); });
    m.add (53,  [this] { zynforge::MarkerListDialog::launch (engine); });
    m.add (54,  [this] { showPreflightChecklist(); });
    m.add (56,  [this] { dropMarkerAndPromptName(); });
    m.add (62,  [this]
    {
        engine.setAutoArmOnInputDetect (! engine.isAutoArmOnInputDetect());
        showStatus (engine.isAutoArmOnInputDetect() ? "Auto-arm on input detect: ON"
                                                    : "Auto-arm on input detect: OFF");
    });
    m.add (251, [this] { showSessionInfo(); });
    m.add (255, [this]
    {
        SessionSettingsDialog::launch (engine,
            [self = juce::Component::SafePointer<MainComponent> (this)] { if (self != nullptr) self->relocateActiveSession(); },
            [self = juce::Component::SafePointer<MainComponent> (this)] (double sr) { if (self != nullptr) self->applySessionSampleRate (sr); });
    });
    m.add (256, [this]
    {
        const bool on = ! engine.isTrimFollowEnabled();
        engine.setTrimFollowEnabled (on);
        const auto notice = on ? engine.getTrimFollowNotice() : juce::String();
        showStatus (notice.isNotEmpty() ? notice
                    : on ? "Trim-Follow ON -- console input-gain moves now track the recorded soundcheck"
                         : "Trim-Follow OFF -- recorded tracks play at their printed level");
    });
    m.add (991, [this]
    {
        const bool allowed = ! engine.getMixMayShareStripOutputs();
        engine.setMixMayShareStripOutputs (allowed);
        showStatus (allowed ? "The mix now sums onto outputs that strips are patched to. Do not use this with console returns"
                            : "The mix stays off outputs that strips are patched to");
    });
    m.add (990, [this] { promptStreamOutputs(); });
    m.add (281, [this] { writeSoundcheckReport(); });
    m.add (600, [this] { zynforge::TimecodeSyncDialog::launch (engine); });
    m.add (720, [this]
    {
        juce::Component::SafePointer<MainComponent> self (this);
        zynforge::ControlSurfacesDialog::launch (engine, [self]
        {
            if (self == nullptr) return;
            auto continuation = self;
            self->confirmSessionReplacement ([continuation]
            { if (continuation != nullptr) continuation->createSessionFromConsole(); });
        },
        [self] { return self != nullptr && self->consoleLink.isConnected()
                     && self->consoleLink.getProfile().canReadNames; });
    });
    m.add (951, [this] { showAutosaveSettings(); });
    m.add (952, [this] { runQcAnalysis(); });
    m.add (953, [this] { detectSongsToMarkers(); });
    m.add (954, [this] { promptConsoleConnect(); });
    m.add (955, [this] { consoleToggleSoundcheck(); });
    m.add (987, [this] { consoleCheckDeskPatch(); });
    m.add (956, [this] { consoleCaptureGains(); });
    m.add (957, [this] { consoleRestoreGains(); });
    m.add (958, [this] { toggleCaptureDaemon(); });
    m.add (975, [this] { reconnectCaptureDaemon(); });
    m.add (963, [this]
    {
        const bool on = ! engine.isSceneFollowEnabled();
        engine.setSceneFollowEnabled (on);
        showStatus (on ? "Scene Follow ON -- outside a take, a console scene jumps to the marker of that name"
                       : "Scene Follow OFF -- console scenes leave the playhead alone");
    });
    m.add (964, [this] { zynforge::DeskMacroDialog::launch (engine); });
    m.add (969, [this] { saveConsoleLog(); });
    m.add (968, [this] { showTwinRecorderDialog(); });

    // Help
    m.add (900, [this] { showKeyboardShortcuts(); });
    m.add (901, [this] { showFirstRunTutorial(); });   // the tour -- NOT showStartupWelcome (which reloads the session + discards unsaved changes)
    m.add (902, [this] { showAboutDialog(); });
    m.add (903, [this] { showUserGuide(); });

    // Edit
    m.add (300, [this] { editUndo(); });
    m.add (301, [this] { editRedo(); });
    m.add (302, [this] { editCutSelected (true); });
    m.add (303, [this] { editCutSelected (false); });
    m.add (304, [this] { editPasteSelected(); });
    m.add (305, [this] { confirmDeleteChannels ([this] { deleteSelectedStrips(); }); });
    m.add (306, [this] { editCropToLoopRange(); });
    m.add (307, [this] { editSoloSelection(); });
    m.add (308, [this] { editSetRangeToLoopRange(); });
    m.add (309, [this] { editToggleSnap(); });
    m.add (310, [this]
    {
        if (engine.getPlayer().hasLoopRegion()) editSeparateAtSelection();
        else                                    editSplitAtPlayhead();
    });
    m.add (311, [this] { editStartRange(); });
    m.add (312, [this] { editFinishRange(); });
    m.add (313, [this] { removeLastCapture(); });
    m.add (314, [this] { togglePunchMode(); });
    m.add (315, [this] { editHealSeparation(); });
    m.add (317, [this] { editConsolidateSelection(); });
    m.add (318, [this]
    {
        zeroCrossSnap = ! zeroCrossSnap;
        showStatus (zeroCrossSnap ? "Zero-crossing snap on (click-free cuts)" : "Zero-crossing snap off");
    });

    // Track
    m.add (320, [this] { showBatchRenameDialog(); });
    m.add (321, [this] { showBatchColourDialog(); });
    m.add (322, [this] { importChannelNamesFromCsv(); });
    m.add (330, [this] { moveSelectedStrips (-1); });
    m.add (331, [this] { moveSelectedStrips ( 1); });
    m.add (332, [this] { colourSelectedStrips(); });
    m.add (333, [this] { confirmDeleteChannels ([this] { deleteSelectedStrips(); }); });
    m.add (334, [this] { clearStripSelection(); });
    m.add (335, [this] { selectAllStrips(); });
}

void MainComponent::saveConsoleLog()
{
    if (engine.isRecording())
    { showStatus ("Stop recording first -- the console log stays in memory until then"); return; }
    if (engine.getRemotes().getOscLogTail (1).isEmpty())
    {
        showStatus ("The console log is empty -- turn on \"Debug: log OSC traffic\" in Control Surfaces first");
        return;
    }

    const auto suggested = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
        .getChildFile ("ZynForge Console Log "
                       + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M") + ".txt");
    chooser = std::make_unique<juce::FileChooser> ("Save Console Log", suggested, "*.txt");
    juce::Component::SafePointer<MainComponent> self (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::warnAboutOverwriting,
        [self] (const juce::FileChooser& fc)
        {
            if (self == nullptr) return;
            const auto file = fc.getResult();
            if (file == juce::File()) return;
            self->writeConsoleLog (file);
        });
}

void MainComponent::writeConsoleLog (const juce::File& file)
{
    // The chooser can sit open while a take starts.
    if (engine.isRecording())
    { showStatus ("Stop recording first -- the console log stays in memory until then"); return; }

    juce::Component::SafePointer<MainComponent> self (this);
    const bool started = sessionJobs.start (
        [self, file, text = engine.getRemotes().formatConsoleLog (consoleLinkSummary())]
        (const std::atomic<bool>&) -> zynforge::SessionJobRunner::Completion
        {
            const bool ok = zynforge::atomicfile::writeText (file, text);
            return [self, file, ok]
            {
                if (self != nullptr)
                    self->showStatus (ok ? "Console log saved: " + file.getFileName()
                                         : "Could not write the console log to " + file.getFullPathName());
            };
        });
    if (! started) showStatus ("Wait for the session file operation to finish");
}

juce::String MainComponent::consoleLinkSummary() const
{
    const auto& profile = consoleLink.getProfile();
    if (! consoleLink.isConnected()) return "Console link: not connected";
    return "Console link: " + profile.displayName + " at " + consoleLink.getHost()
         + (consoleLink.isDialectConfirmed() ? ", dialect confirmed by the desk"
                                             : ", dialect not confirmed by the desk")
         + ", " + consoleLink.healthText();
}

int MainComponent::oscListenPort() const
{
    if (auto* p = engine.getAppProps())
        return juce::jlimit (1, 65535, p->getIntValue ("oscListenPort", 8000));
    return 8000;
}

