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
#include "../Network/CloudUpload.h"
#include "../Theme/DialogChrome.h"
#include "PatchPage.h"
#include "Meterbridge.h"
#include "MarkerListDialog.h"
#include "ClickSettingsDialog.h"
#include "MirrorDrivesDialog.h"
#include "TimecodeSyncDialog.h"
#include "ControlSurfacesDialog.h"
#include "SessionSettingsDialog.h"

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
        menu.addItem (6, "Close Session", engine.getActiveSessionDir().isDirectory()
                                          && sessionEditable);
        menu.addSeparator();

        juce::PopupMenu exportMenu;
        // Export/bounce read the Track_NN files synchronously on the message
        // thread. During a take those headers aren't finalised (truncated
        // exports) and the read I/O competes with the recorder + freezes the
        // UI, so gate the whole submenu on not-recording. CSV timeline export
        // is metadata-only but gated too for a consistent "not mid-take" rule.
        const bool hasActive = engine.getActiveSessionDir().isDirectory();
        const bool canExport = hasActive && sessionEditable;   // also blocked while LOCKED
        exportMenu.addItem (10, "Export All Tracks...", canExport);

        const int n = engine.getRecorder().getNumTracks();
        // Opens a tick-box picker of the session's tracks → format → folder.
        exportMenu.addItem (11, "Export Individual Tracks...", canExport && n > 0);
        exportMenu.addSeparator();
        exportMenu.addItem (12, "Bounce Edited Tracks (Stems)...", canExport && n > 0);
        exportMenu.addItem (13, "Bounce Stereo Mix...",            canExport && n > 0);
        exportMenu.addSeparator();
        exportMenu.addItem (14, "Export Timeline (markers/cues/tracks) as CSV...", canExport && n > 0);
        exportMenu.addItem (962, "Create Verified Show Handoff...", canExport && n > 0);
        menu.addSubMenu ("Export", exportMenu);
        menu.addSeparator();

        // Session templates -- save the current strip config (count,
        // names, colours, routings, stereo flags) to a reusable
        // template, or start a new session pre-populated from one.
        menu.addItem (71, "Print setlist...", ! cues.empty());
        menu.addSeparator();
        menu.addItem (70, "Save Current as Template...",
                      engine.getRecorder().getNumTracks() > 0
                       && ! engine.isRecording());

        juce::PopupMenu templates;
        const auto tList    = listSessionTemplates();
        const auto starred  = getDefaultTemplate();
        for (int i = 0; i < tList.size(); ++i)
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
            templates.addSeparator();

            // 'Set default template' sub-popup: lists every template + 'None'.
            // The default auto-applies after File ▸ New Session creates the
            // folder, so the engineer starts each show pre-patched.
            juce::PopupMenu setDefault;
            setDefault.addItem (260, "(None)", true, ! starred.existsAsFile());
            for (int i = 0; i < tList.size(); ++i)
                setDefault.addItem (2000 + i, tList[i].getFileNameWithoutExtension(),
                                    true, tList[i] == starred);
            templates.addSubMenu ("Set default template", setDefault);

            templates.addItem (250, "Delete a template...");
        }
        menu.addSubMenu ("New Session from Template", templates, ! engine.isRecording());
        menu.addItem (7, "New Session from CSV (channel names)...", ! engine.isRecording());

        menu.addSeparator();
        menu.addItem (60, "Choose Backup Folder...", ! engine.isRecording());
        menu.addItem (61, "Mirror Drives...",        ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (99, "Quit Zynforge Recording...");
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
        menu.addItem (316, "Strip Silence", canEdit);
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

        menu.addItem (302, "Cut Selected Strip(s)\tCmd+X",  hasSelection);
        menu.addItem (303, "Copy Selected Strip(s)\tCmd+C", hasSelection);
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
        menu.addItem (52, "Virtual Soundcheck -- repatch outputs to match inputs");
        menu.addItem (51, "Meterbridge...");
        menu.addSeparator();
        menu.addItem (56, "Add Marker...\tM",
                      engine.getActiveSessionDir().isDirectory());
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
            m.addItem (256, "Trim-Follow (console input gain -> soundcheck)",
                       true, engine.isTrimFollowEnabled());
            // Transport Sync -- chase MTC / LTC with live status.
            m.addItem (600, engine.getChaseMode() != zynforge::AudioEngine::ChaseMode::Off
                               ? juce::String ("Transport Sync (chasing)...")
                               : juce::String ("Transport Sync (MTC / LTC)..."));
            m.addSeparator();
            m.addItem (958, useCaptureDaemon
                               ? (captureSupervisor.isDaemonRecording()
                                      ? "Capture daemon: TAKE ROLLING (stop to disable)"
                                      : "Capture daemon: ON (experimental)")
                               : juce::String ("Capture daemon: off (experimental)..."));
            menu.addSubMenu ("Recording & Sync", m);
        }

        // ── Network & Surfaces ───────────────────────────────────────────
        {
            juce::PopupMenu m;
            m.addItem (55, engine.getNDIBridge().isEnabled()
                               ? juce::String ("Stop NDI broadcast")
                               : juce::String ("Start NDI broadcast..."));
            // Control Surfaces hub -- OSC + every console/surface protocol.
            m.addItem (720, "Control Surfaces...");

            // MIDI clock master -- drive outboard gear from session tempo.
            juce::PopupMenu midiMenu;
            const auto outs    = zynforge::MidiClockOut::listOutputs();
            const auto active  = engine.getMidiClockOut().getOutputDeviceName();
            const bool clockOn = engine.getMidiClockOut().isEnabled();
            midiMenu.addItem (400, "Stop MIDI clock", clockOn, ! clockOn);
            midiMenu.addSeparator();
            for (int i = 0; i < outs.size() && i < 30; ++i)
                midiMenu.addItem (401 + i, outs[i], true, clockOn && outs[i] == active);
            m.addSubMenu ("MIDI clock out", midiMenu);

            m.addSeparator();
            const bool compRunning = engine.getRemotes().isCompanionServerRunning();
            // id 950 -- outside every menuItemSelected dispatch range.
            m.addItem (950, compRunning
                               ? juce::String ("Stop companion (port " + juce::String (engine.getRemotes().getCompanionServerPort()) + ")")
                               : juce::String ("Start companion server on :9000..."));
            m.addItem (961, "Copy read-only confidence URL", compRunning);
            menu.addSubMenu ("Network & Surfaces", m);
        }

        // ── Console link (ids 954-957, outside every dispatch range) ──────
        {
            juce::PopupMenu m;
            m.addItem (954, consoleLink.isConnected()
                               ? "Console: disconnect " + consoleLink.getHost()
                               : juce::String ("Console: connect..."));
            // Repatch + gain items only light up for a desk we drive over OSC.
            // Write-tier items also require a TRUSTWORTHY dialect: either the
            // address model is one we trust (the X32 reference) or the console
            // answered the connect-time handshake. An unconfirmed desk stays
            // read-only, so grey the write items rather than offering an action
            // that would be refused.
            const bool consoleWritable = consoleLink.isConnected() && consoleLink.canWrite();
            const bool consoleRepatch = consoleWritable && consoleLink.getProfile().canRepatch;
            const bool consoleGains   = consoleWritable && consoleLink.getProfile().canCaptureGains;
            m.addItem (955, (consoleLink.getPatch() == zynforge::ConsoleLink::Patch::Soundcheck
                            || (consoleLink.getPatch() == zynforge::ConsoleLink::Patch::Unknown && consoleLink.hasStagePatch()))
                               ? "Console: back to STAGE patch"
                               : "Console: SOUNDCHECK patch (card returns)",
                       consoleRepatch);
            m.addItem (956, "Console: capture head-amp gains", consoleGains);
            m.addItem (957, "Console: restore head-amp gains", consoleGains);
            menu.addSubMenu ("Console", m);
        }

        // ── Cloud & Mirror ───────────────────────────────────────────────
        {
            juce::PopupMenu m;
            // ids 959/960 -- outside every menuItemSelected dispatch range.
            // (260/261 collided with the File template-default submenu, which
            //  meant Upload silently ran setDefaultTemplate({}) instead.)
            m.addItem (959, "Upload session to cloud...", ! engine.isRecording());
            m.addItem (960, "Configure cloud upload command...");
            m.addSeparator();
            m.addItem (290, sessionMirror.isMirroring()
                               ? "Stop mirroring " + sessionMirror.getPrimary()
                               : juce::String ("Mirror primary host..."));
            menu.addSubMenu ("Cloud & Mirror", m);
        }

        // ── Analysis & Reports (ids 952/953 outside dispatch ranges) ──────
        {
            juce::PopupMenu m;
            m.addItem (280, "Spectral auto-name strips",
                       engine.getRecorder().getNumTracks() > 0);
            m.addItem (281, "Write soundcheck report",
                       engine.getActiveSessionDir().isDirectory());
            // Each reads every take file in full: never during a take.
            const bool canAnalyse = engine.getActiveSessionDir().isDirectory() && ! engine.isRecording();
            m.addItem (282, "Analyse for noise / hum / bumps...", canAnalyse);
            m.addItem (952, "Post-show QC report...",             canAnalyse);
            m.addItem (953, "Detect songs -> markers...",         canAnalyse);
            menu.addSubMenu ("Analysis & Reports", m);
        }

        menu.addSeparator();
        menu.addItem (251, "Session Info & Notes...", engine.getActiveSessionDir().isDirectory());
    }
    else if (topLevelIndex == 4)  // Help
    {
        menu.addItem (900, "Keyboard Shortcuts...");
        menu.addItem (903, "User Guide (panel tips)...");
        menu.addItem (901, "Quick Start (replay tutorial)");
        menu.addSeparator();
        menu.addItem (902, "About Zynforge Recording");
    }

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
        << '|' << (int) engine.getActiveSessionDir().isDirectory()
        << '|' << (int) engine.isPunchModeOn()
        << '|' << (int) snapToMarkers
        << '|' << (int) cues.empty()
        << '|' << (int) stripClipboard.isObject()
        << '|' << (int) engine.isTrimFollowEnabled()
        << '|' << (int) engine.getChaseMode()
        << '|' << engine.getLtcSourceStrip()
        << '|' << (int) consoleLink.isConnected()
        << '|' << (int) consoleLink.getPatch()
        << '|' << (int) consoleLink.getProfile().canRepatch
        << '|' << (int) consoleLink.getProfile().canCaptureGains
        // The handshake flips canWrite() asynchronously AFTER connect, so it
        // must be in the signature or the write items stay greyed until some
        // unrelated state change happens to refresh the menu.
        << '|' << (int) consoleLink.canWrite()
        << '|' << (int) useCaptureDaemon
        << '|' << (int) engine.getRemotes().isCompanionServerRunning()
        << '|' << (int) captureSupervisor.isAttached()
        << '|' << (int) (useCaptureDaemon && captureSupervisor.isDaemonRecording());

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
        if (! safeDuringMetadata)
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
    m.add (10,  [this] { onExportAllTracks(); });
    m.add (11,  [this] { onExportIndividualTracks(); });
    m.add (12,  [this] { onBounceStems(); });
    m.add (13,  [this] { onBounceStereoMix(); });
    m.add (14,  [this] { exportTimelineCsv(); });
    m.add (962, [this] { exportShowHandoff(); });
    m.add (70,  [this] { promptSaveSessionTemplate(); });
    m.add (71,  [this] { printSetlist(); });
    m.addRange (200, 249, [this] (int offset)
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
    m.add (60,  [this] { onBackupClicked(); });
    m.add (61,  [this] { MirrorDrivesDialog::launch (engine); });
    m.add (99,  [this] { confirmAndQuit(); });

    // Session
    m.add (50,  [this] { zynforge::PatchPage::launch (engine); });
    m.add (51,  [this] { zynforge::Meterbridge::launch (engine); });
    m.add (52,  [this] { onVscClicked(); });
    m.add (53,  [this] { zynforge::MarkerListDialog::launch (engine); });
    m.add (54,  [this] { showPreflightChecklist(); });
    m.add (55,  [this] { toggleNdiBroadcast(); });
    m.add (56,  [this] { dropMarkerAndPromptName(); });
    m.add (62,  [this]
    {
        engine.setAutoArmOnInputDetect (! engine.isAutoArmOnInputDetect());
        showStatus (engine.isAutoArmOnInputDetect() ? "Auto-arm on input detect: ON"
                                                    : "Auto-arm on input detect: OFF");
    });
    m.add (251, [this] { showSessionProperties(); });
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
        showStatus (on ? "Trim-Follow ON -- console input-gain moves now track the recorded soundcheck"
                       : "Trim-Follow OFF -- recorded tracks play at their printed level");
    });
    m.add (280, [this] { runSpectralAutoName(); });
    m.add (281, [this] { writeSoundcheckReport(); });
    m.add (282, [this] { runNoiseAnalysis(); });
    m.add (290, [this] { promptMirrorHost(); });
    m.add (400, [this]
    {
        engine.getMidiClockOut().setEnabled (false);
        showStatus ("MIDI clock stopped");
    });
    m.addRange (401, 430, [this] (int offset)
    {
        const auto outs = zynforge::MidiClockOut::listOutputs();
        const int idx = offset;
        if (idx >= 0 && idx < outs.size())
        {
            engine.getMidiClockOut().setOutputDevice (outs[idx]);
            engine.getMidiClockOut().setTempoBpm (engine.getSessionTempoBpm());
            engine.getMidiClockOut().setEnabled (true);
            showStatus ("MIDI clock -> " + outs[idx]);
        }
    });
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
        });
    });
    m.add (950, [this] { toggleCompanionServer(); });
    m.add (951, [this] { showAutosaveSettings(); });
    m.add (952, [this] { runQcAnalysis(); });
    m.add (953, [this] { detectSongsToMarkers(); });
    m.add (954, [this] { promptConsoleConnect(); });
    m.add (955, [this] { consoleToggleSoundcheck(); });
    m.add (956, [this] { consoleCaptureGains(); });
    m.add (957, [this] { consoleRestoreGains(); });
    m.add (958, [this] { toggleCaptureDaemon(); });
    m.add (959, [this] { runCloudUpload(); });
    m.add (960, [this] { editCloudUploadCommand(); });
    m.add (961, [this] { copyConfidenceUrl(); });

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
    m.add (316, [this] { editStripSilence(); });
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

void MainComponent::copyConfidenceUrl()
{
    const auto url = engine.getRemotes().getCompanionAccessUrl().replace ("/?t=", "/confidence?t=");
    if (engine.getRemotes().isCompanionServerRunning() && url.isNotEmpty())
    {
        juce::SystemClipboard::copyTextToClipboard (url);
        showStatus ("Read-only confidence URL copied. Use a TLS tunnel for another device.");
    }
    else
        showStatus ("Start the companion server before copying its confidence URL");
}

void MainComponent::runCloudUpload()
{
    // Run the configured cloud-upload command with {SESSION} expanded.
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory()) { showStatus ("No session active -- load or record first"); return; }
    auto* props = engine.getAppProps();
    const auto tmpl = props != nullptr ? props->getValue ("cloudUploadCommand") : juce::String();
    if (tmpl.isEmpty())
    {
        showStatus ("No upload command configured -- pick \"Configure cloud upload command...\" first");
        return;
    }
    if (zynforge::cloud::launchUpload (tmpl, sessionDir))
    {
        showStatus ("Cloud upload started: " + sessionDir.getFileName());
    }
    else
    {
        showStatus ("Cloud upload failed to launch -- check the configured command");
    }
}

void MainComponent::toggleCompanionServer()
{
    if (engine.getRemotes().isCompanionServerRunning())
    {
        engine.getRemotes().stopCompanionServer();
        showStatus ("Companion server stopped");
    }
    else if (engine.getRemotes().startCompanionServer (9000))
    {
        const auto url = engine.getRemotes().getCompanionAccessUrl();
        // Show + copy the full URL (with access token) so the
        // engineer can paste it on their phone without typing
        // the 32-hex token. Companion is loopback-only by
        // default; for phone / off-machine access put a tunnel in
        // front of loopback (Tailscale / Cloudflare / SSH -L) -- it
        // gives real TLS, unlike serving plaintext HTTP over Wi-Fi.
        // See README "Companion server -- Security & secure remote access".
        juce::SystemClipboard::copyTextToClipboard (url);
        if (engine.getRemotes().isCompanionExposedOnLan())
            showStatus ("! Companion EXPOSED ON LAN (plaintext) -- use a tunnel for remote access, not raw Wi-Fi");
        else
            showStatus ("Companion on (loopback-only) -- URL copied. For phone access, tunnel to 127.0.0.1:9000 (see README)");
    }
    else
    {
        showStatus ("Companion failed to bind port 9000 -- try a different port");
    }
}

void MainComponent::editCloudUploadCommand()
{
    // Edit the upload command template.
    auto* aw = new juce::AlertWindow ("Cloud upload command",
                                      "Shell command to upload a session. Use {SESSION} as a "
                                      "placeholder for the session directory's absolute path. "
                                      "Examples:\n"
                                      "  rclone copy {SESSION} myremote:zynforge-sessions/\n"
                                      "  aws s3 sync {SESSION} s3://my-bucket/sessions/\n"
                                      "  rsync -a {SESSION} engineer@studio:/sessions/",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    const auto current = engine.getAppProps() != nullptr
                          ? engine.getAppProps()->getValue ("cloudUploadCommand")
                          : juce::String();
    aw->addTextEditor ("cmd", current, {});
    dialog::primeNameEditor (*aw, "cmd");
    aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [self, aw] (int result)
            {
                if (result == 1 && self != nullptr && self->engine.getAppProps() != nullptr)
                {
                    const auto v = aw->getTextEditorContents ("cmd").trim();
                    self->engine.getAppProps()->setValue ("cloudUploadCommand", v);
                    self->engine.getAppProps()->saveIfNeeded();
                    self->showStatus (v.isEmpty() ? juce::String ("Cloud upload command cleared")
                                            : juce::String ("Cloud upload command saved"));
                }
                delete aw;
            }),
        false);
}

void MainComponent::toggleNdiBroadcast()
{
    auto& bridge = engine.getNDIBridge();
    if (bridge.isEnabled())
    {
        bridge.stop();
        showStatus ("NDI broadcast stopped");
    }
    else
    {
        const auto runtime = zynforge::NDIBridge::findRuntimePath();
        if (runtime.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (
                juce::MessageBoxIconType::NoIcon,
                "NDI runtime not found",
                "Install the free NDI runtime from ndi.tv (the 'NDI Tools' bundle "
                "ships with libndi.dylib in /usr/local/lib). Re-launch the app "
                "after installing to broadcast over NDI.", "OK");
            return;
        }
        auto sr = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
            ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
            : 48000.0;
        const auto host = juce::SystemStats::getComputerName();
        if (bridge.start ("Zynforge -- " + host, sr))
            showStatus ("NDI broadcasting as 'Zynforge -- " + host + "'");
        else
            showStatus ("NDI start failed -- check ndi.tv runtime install");
    }
}



int MainComponent::oscListenPort() const
{
    if (auto* p = engine.getAppProps())
        return juce::jlimit (1, 65535, p->getIntValue ("oscListenPort", 8000));
    return 8000;
}

