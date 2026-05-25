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
#include "../Theme/DialogChrome.h"
#include "PatchPage.h"
#include "Meterbridge.h"
#include "MarkerListDialog.h"
#include "ClickSettingsDialog.h"
#include "SessionSettingsDialog.h"
#include "MirrorDrivesDialog.h"

using namespace zynforge;

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Session", "Help" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelIndex == 0)  // File
    {
        menu.addItem (5, "New Session...",         ! engine.isRecording());
        menu.addItem (1, "Open Session...");
        menu.addItem (4, "Import Audio Files...",  ! engine.isRecording());
        menu.addSeparator();
        // Save / Save As are always enabled: if there's no active
        // session yet, Save falls through to Save As (picker), and
        // Save As is by definition a picker so it never needs prior
        // state.
        menu.addItem (2, "Save Session State");
        menu.addItem (3, "Save Session As...");
        menu.addSeparator();

        juce::PopupMenu exportMenu;
        const bool hasActive = engine.getActiveSessionDir().isDirectory();
        exportMenu.addItem (10, "Export All Tracks...", hasActive);

        juce::PopupMenu indiv;
        const int n = engine.getRecorder().getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const auto& t  = engine.getRecorder().getTrack (i);
            const auto nm  = t.name.isNotEmpty() ? t.name
                                                  : juce::String (i + 1);
            indiv.addItem (100 + i, nm, hasActive);
        }
        exportMenu.addSubMenu ("Export Individual Track", indiv, hasActive && n > 0);
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
                setDefault.addItem (261 + i, tList[i].getFileNameWithoutExtension(),
                                    true, tList[i] == starred);
            templates.addSubMenu ("Set default template", setDefault);

            templates.addItem (250, "Delete a template...");
        }
        menu.addSubMenu ("New Session from Template", templates, ! engine.isRecording());

        menu.addSeparator();
        menu.addItem (60, "Choose Backup Folder...", ! engine.isRecording());
        menu.addItem (61, "Mirror Drives...",        ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (99, "Quit Zynforge Recording...");
    }
    else if (topLevelIndex == 1)  // Edit
    {
        // Edit menu. Shortcut hints appear after a tab character -- macOS
        // pulls those out and right-aligns them in the native menu.
        const bool hasContext = engine.getPlayer().isLoaded() || engine.isRecording();

        const bool hasSelection  = ! selectedLogical.empty();
        const bool hasClipboard  = stripClipboard.isObject();
        const bool playerLoaded  = engine.getPlayer().isLoaded();

        menu.addItem (300, "Undo\tCmd+Z",       undoManager.canUndo());
        menu.addItem (301, "Redo\tCmd+R",       undoManager.canRedo());
        menu.addSeparator();
        menu.addItem (302, "Cut Selected Strip(s)\tCmd+X",  hasSelection);
        menu.addItem (303, "Copy Selected Strip(s)\tCmd+C", hasSelection);
        menu.addItem (304, "Paste Strip Settings\tCmd+V",   hasClipboard && hasSelection);
        menu.addItem (305, "Delete Selected Strips",        hasSelection);
        menu.addItem (306, "Crop to Loop Range\tCtrl+Cmd+C",
                      playerLoaded && engine.getPlayer().hasLoopRegion());
        menu.addItem (307, "Solo Selection\tA",   hasSelection);
        menu.addSeparator();
        menu.addItem (308, "Set Range to Loop Range", playerLoaded);
        menu.addSeparator();
        menu.addItem (309, "Toggle Snap\t4", true, snapToMarkers);
        menu.addSeparator();
        menu.addItem (310, "Split / Separate at Playhead\tS",
                      playerLoaded);
        menu.addSeparator();
        menu.addItem (311, "Start Range at Playhead\t,",  playerLoaded);
        menu.addItem (312, "Finish Range at Playhead\t.", playerLoaded);
        menu.addSeparator();
        menu.addItem (313, "Remove Last Capture", ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (314, "Punch In/Out Mode",
                      playerLoaded, engine.isPunchModeOn());
        menu.addSeparator();
        // Batch ops -- let the engineer sweep a range of channels in one
        // dialog instead of renaming/recolouring 24 strips by hand.
        menu.addItem (320, "Batch Rename Channels...",  engine.getRecorder().getNumTracks() > 0);
        menu.addItem (321, "Batch Colour Channels...",  engine.getRecorder().getNumTracks() > 0);
        menu.addSeparator();
        const int selCount = (int) selectedLogical.size();
        const int stripCount = (int) strips.size();
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
    else if (topLevelIndex == 2)  // Session
    {
        menu.addItem (54, "Show pre-flight checklist...");
        menu.addSeparator();
        menu.addItem (50, "Patch...");
        menu.addItem (52, "Virtual Soundcheck -- repatch outputs ↔ inputs");
        menu.addItem (51, "Meterbridge...");
        menu.addItem (56, "Add Marker...\tM",
                      engine.getActiveSessionDir().isDirectory());
        menu.addItem (53, "Memory Locations...");
        menu.addSeparator();
        // Auto-arm toggle. Convenience for first-time pre-show
        // channel discovery (walk the stage, hit each mic, watch the
        // strips arm themselves). Marked with a check when on.
        menu.addItem (62, "Auto-arm on input detect", true,
                      engine.isAutoArmOnInputDetect());
        menu.addSeparator();
        menu.addItem (55, engine.getNDIBridge().isEnabled()
                              ? juce::String ("Stop NDI broadcast")
                              : juce::String ("Start NDI broadcast..."));

        // OSC submenu with the five dialects.
        juce::PopupMenu oscMenu;
        oscMenu.addItem (110, "OSC Generic /zynforge");
        oscMenu.addItem (111, "OSC DiGiCo");
        oscMenu.addItem (112, "OSC Allen & Heath");
        oscMenu.addItem (113, "OSC SSL Live");
        oscMenu.addItem (114, "OSC Yamaha");
        oscMenu.addSeparator();
        oscMenu.addItem (115, "Stop OSC", engine.isOscListening());
        menu.addSubMenu ("OSC", oscMenu);

        // MIDI clock master -- drive outboard synths / drum machines /
        // DAW slaves from the session tempo (24 PPQN). Submenu lists
        // every installed MIDI output; pick one to enable, or "Stop"
        // to disable. Currently-active device is checkmarked.
        juce::PopupMenu midiMenu;
        const auto outs = zynforge::MidiClockOut::listOutputs();
        const auto active = engine.getMidiClockOut().getOutputDeviceName();
        const bool clockOn = engine.getMidiClockOut().isEnabled();
        midiMenu.addItem (400, "Stop MIDI clock", clockOn, ! clockOn);
        midiMenu.addSeparator();
        for (int i = 0; i < outs.size() && i < 30; ++i)
            midiMenu.addItem (401 + i, outs[i], true, clockOn && outs[i] == active);
        menu.addSubMenu ("MIDI clock out", midiMenu);

        menu.addSeparator();
        menu.addItem (260, "Upload session to cloud...", ! engine.isRecording());
        menu.addItem (261, "Configure cloud upload command...");
        menu.addSeparator();
        const bool compRunning = engine.isCompanionServerRunning();
        menu.addItem (270, compRunning
                            ? juce::String ("Stop companion (port " + juce::String (engine.getCompanionServerPort()) + ")")
                            : juce::String ("Start companion server on :9000..."));
        menu.addSeparator();
        // Session Settings = format / sample-rate / bit-depth (editable).
        // Session Info     = name / notes / read-only config summary.
        // The old "Settings" + "Properties" labels were too close;
        // engineers couldn't tell which dialog owned what.
        menu.addItem (250, "Session Format && Recording...", ! engine.isRecording());
        menu.addItem (251, "Session Info && Notes...",       engine.getActiveSessionDir().isDirectory());
        menu.addSeparator();
        menu.addItem (280, "Spectral auto-name strips",
                      engine.getRecorder().getNumTracks() > 0);
        menu.addItem (281, "Write soundcheck report",
                      engine.getActiveSessionDir().isDirectory());
        menu.addItem (282, "Analyse for noise / hum / bumps...",
                      engine.getActiveSessionDir().isDirectory());
        menu.addSeparator();
        menu.addItem (290, sessionMirror.isMirroring()
                              ? "Stop mirroring " + sessionMirror.getPrimary()
                              : juce::String ("Mirror primary host..."));
    }
    else if (topLevelIndex == 3)  // Help
    {
        menu.addItem (900, "Keyboard Shortcuts...");
        menu.addItem (903, "User Guide (panel tips)...");
        menu.addItem (901, "Quick Start (replay tutorial)");
        menu.addSeparator();
        menu.addItem (902, "About Zynforge Recording");
    }

    return menu;
}

void MainComponent::menuItemSelected (int id, int /*topLevelIndex*/)
{
    juce::Logger::writeToLog ("[ZF] menuItemSelected id=" + juce::String (id));

    if (id == 1)         onLoadSessionClicked();
    else if (id == 2)    onSaveSessionState();
    else if (id == 3)    onSaveSessionAs();
    else if (id == 4)    onImportAudioFiles();
    else if (id == 5)    launchNewSessionDialog();
    else if (id == 10)   onExportAllTracks();
    else if (id == 70)   promptSaveSessionTemplate();
    else if (id == 71)   printSetlist();
    else if (id >= 200 && id < 250)
    {
        const auto list = listSessionTemplates();
        const int idx = id - 200;
        if (idx >= 0 && idx < list.size()) applySessionTemplate (list[idx]);
    }
    else if (id == 250)  promptDeleteSessionTemplate();
    else if (id == 260)  setDefaultTemplate ({});
    else if (id >= 261 && id < 290)
    {
        const auto list = listSessionTemplates();
        const int idx = id - 261;
        if (idx >= 0 && idx < list.size()) setDefaultTemplate (list[idx]);
    }
    else if (id == 900)  showKeyboardShortcuts();
    else if (id == 901)  showStartupWelcome();
    else if (id == 902)  showAboutDialog();
    else if (id == 903)  showUserGuide();
    else if (id == 99)   confirmAndQuit();
    // Track-export sub-menu uses ids 100..199. Tightened from the
    // previous open-ended `>= 100` which was swallowing 110..115
    // (OSC dialects), 200..230 (settings menu), and 250 (Session
    // Settings) -- sending all of them to onExportIndividualTrack.
    // OSC dialect picks under Session ▶ OSC ▶ -- must be checked BEFORE
    // the export range below, otherwise IDs 110..115 fall through to
    // onExportIndividualTrack(10..15) and silently no-op.
    else if (id >= 110 && id <= 114)
    {
        const int dialect = id - 110;
        if (engine.startOsc (8000, dialect))
            showStatus ("OSC listening on 8000 (" +
                        juce::StringArray ({"Generic","DiGiCo","A&H","SSL","Yamaha"})[dialect] + ")");
        else
            showStatus ("OSC failed to bind UDP 8000 -- port already in use?");
    }
    else if (id == 115)  { engine.stopOsc(); showStatus ("OSC stopped"); }
    else if (id == 400)
    {
        engine.getMidiClockOut().setEnabled (false);
        showStatus ("MIDI clock stopped");
    }
    else if (id >= 401 && id < 431)
    {
        const auto outs = zynforge::MidiClockOut::listOutputs();
        const int idx = id - 401;
        if (idx >= 0 && idx < outs.size())
        {
            engine.getMidiClockOut().setOutputDevice (outs[idx]);
            engine.getMidiClockOut().setTempoBpm (engine.getSessionTempoBpm());
            engine.getMidiClockOut().setEnabled (true);
            showStatus ("MIDI clock → " + outs[idx]);
        }
    }
    else if (id == 260)
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
        const auto cmd = tmpl.replace ("{SESSION}", sessionDir.getFullPathName().quoted(), false);
        juce::ChildProcess cp;
        if (cp.start (cmd))
        {
            showStatus ("Cloud upload started: " + sessionDir.getFileName());
        }
        else
        {
            showStatus ("Cloud upload failed to launch -- check the configured command");
        }
    }
    else if (id == 270)
    {
        if (engine.isCompanionServerRunning())
        {
            engine.stopCompanionServer();
            showStatus ("Companion server stopped");
        }
        else if (engine.startCompanionServer (9000))
        {
            const auto url = engine.getCompanionAccessUrl();
            // Show + copy the full URL (with access token) so the
            // engineer can paste it on their phone without typing
            // the 32-hex token. Companion is loopback-only by
            // default; LAN exposure is a separate menu item.
            juce::SystemClipboard::copyTextToClipboard (url);
            showStatus ("Companion on (loopback-only) -- URL copied to clipboard");
        }
        else
        {
            showStatus ("Companion failed to bind port 9000 -- try a different port");
        }
    }
    else if (id == 261)
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
        const auto current = engine.getAppProps() != nullptr
                              ? engine.getAppProps()->getValue ("cloudUploadCommand")
                              : juce::String();
        aw->addTextEditor ("cmd", current, {});
        dialog::primeNameEditor (*aw, "cmd");
        aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true,
            juce::ModalCallbackFunction::create (
                [this, aw] (int result)
                {
                    if (result == 1 && engine.getAppProps() != nullptr)
                    {
                        const auto v = aw->getTextEditorContents ("cmd").trim();
                        engine.getAppProps()->setValue ("cloudUploadCommand", v);
                        engine.getAppProps()->saveIfNeeded();
                        showStatus (v.isEmpty() ? juce::String ("Cloud upload command cleared")
                                                : juce::String ("Cloud upload command saved"));
                    }
                    delete aw;
                }),
            false);
    }
    else if (id >= 100 && id < 200) onExportIndividualTrack (id - 100);
    else if (id == 50)   zynforge::PatchPage::launch (engine);
    else if (id == 51)   zynforge::Meterbridge::launch (engine);
    else if (id == 52)   onVscClicked();
    else if (id == 53)   zynforge::MarkerListDialog::launch (engine);
    else if (id == 54)   showPreflightChecklist();
    else if (id == 56)   dropMarkerAndPromptName();
    else if (id == 55)
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
    else if (id == 251) showSessionProperties();
    else if (id == 280) runSpectralAutoName();
    else if (id == 281) writeSoundcheckReport();
    else if (id == 282) runNoiseAnalysis();
    else if (id == 290) promptMirrorHost();
    else if (id == 250)
    {
        struct StubContent final : public juce::Component
        {
            explicit StubContent (AudioEngine& e) : eng (e) { rebuild(); setSize (380, 230); }

            void rebuild()
            {
                using F = zynforge::CaptureFormat;
                const auto cur = eng.getRecorder().getCaptureFormat();
                const double sr = eng.getDeviceManager().getAudioDeviceSetup().sampleRate;

                fmtL.setText ("Audio Format", juce::dontSendNotification);
                rateL.setText ("Sample Rate", juce::dontSendNotification);
                bitsL.setText ("Bit Depth", juce::dontSendNotification);
                for (auto* l : { &fmtL, &rateL, &bitsL })
                    { l->setColour (juce::Label::textColourId, brand::textPrimary); addAndMakeVisible (*l); }

                fmtBox.addItem ("WAV",  1);
                fmtBox.addItem ("AIFF", 2);
                fmtBox.addItem ("FLAC", 3);
                int container = (cur == F::Wav16 || cur == F::Wav24 || cur == F::Wav32Float) ? 1
                              : (cur == F::Aiff16 || cur == F::Aiff24 || cur == F::Aiff32Float) ? 2 : 3;
                fmtBox.setSelectedId (container, juce::dontSendNotification);
                fmtBox.onChange = [this] { refreshBits(); };
                addAndMakeVisible (fmtBox);

                rateBox.addItem ("44.1 kHz", 1);
                rateBox.addItem ("48 kHz",   2);
                rateBox.addItem ("96 kHz",   3);
                rateBox.addItem ("192 kHz",  4);
                rateBox.setSelectedId (juce::approximatelyEqual (sr, 44100.0)  ? 1
                                     : juce::approximatelyEqual (sr, 96000.0)  ? 3
                                     : juce::approximatelyEqual (sr, 192000.0) ? 4 : 2,
                                     juce::dontSendNotification);
                addAndMakeVisible (rateBox);

                addAndMakeVisible (bitsBox);
                refreshBits();
                bitsBox.setSelectedId ((cur == F::Wav16 || cur == F::Aiff16 || cur == F::Flac16) ? 1
                                     : (cur == F::Wav32Float || cur == F::Aiff32Float) ? 3 : 2,
                                     juce::dontSendNotification);

                applyB.setButtonText ("Apply");
                cancelB.setButtonText ("Cancel");
                applyB.onClick  = [this] { apply(); };
                cancelB.onClick = [this] { close(); };
                addAndMakeVisible (applyB);
                addAndMakeVisible (cancelB);
            }

            void refreshBits()
            {
                const int prev = bitsBox.getSelectedId();
                bitsBox.clear (juce::dontSendNotification);
                bitsBox.addItem ("16-bit PCM", 1);
                bitsBox.addItem ("24-bit PCM", 2);
                if (fmtBox.getSelectedId() != 3) bitsBox.addItem ("32-bit float", 3);
                if (prev > 0) bitsBox.setSelectedId (prev, juce::dontSendNotification);
                if (bitsBox.getSelectedId() == 0)
                    bitsBox.setSelectedId (2, juce::dontSendNotification);
            }

            void apply()
            {
                if (eng.isRecording()) { close(); return; }
                using F = zynforge::CaptureFormat;
                F f = F::Wav24;
                const int c = fmtBox.getSelectedId(), b = bitsBox.getSelectedId();
                if      (c == 1) f = b == 1 ? F::Wav16 : b == 2 ? F::Wav24 : F::Wav32Float;
                else if (c == 2) f = b == 1 ? F::Aiff16 : b == 2 ? F::Aiff24 : F::Aiff32Float;
                else             f = b == 1 ? F::Flac16 : F::Flac24;
                eng.getRecorder().setCaptureFormat (f);

                double sr = 48000.0;
                switch (rateBox.getSelectedId())
                { case 1: sr = 44100.0; break; case 2: sr = 48000.0; break;
                  case 3: sr = 96000.0; break; case 4: sr = 192000.0; break; }
                auto setup = eng.getDeviceManager().getAudioDeviceSetup();
                setup.sampleRate = sr;
                eng.getDeviceManager().setAudioDeviceSetup (setup, true);
                close();
            }

            void close()
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (16);
                const int rowH = 30, labelW = 120;
                auto row = [&] (juce::Label& l, juce::ComboBox& b)
                {
                    auto rr = r.removeFromTop (rowH);
                    l.setBounds (rr.removeFromLeft (labelW));
                    b.setBounds (rr);
                    r.removeFromTop (brand::space::sm);
                };
                row (fmtL, fmtBox); row (rateL, rateBox); row (bitsL, bitsBox);
                r.removeFromTop (brand::space::lg);
                auto br = r.removeFromBottom (32);
                applyB .setBounds (br.removeFromRight (110));
                br.removeFromRight (brand::space::md);
                cancelB.setBounds (br.removeFromRight (110));
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.08f, 0.14f));
                g.fillRect (r);
            }

            AudioEngine& eng;
            juce::Label fmtL, rateL, bitsL;
            juce::ComboBox fmtBox, rateBox, bitsBox;
            juce::TextButton applyB, cancelB;
        };

        auto* content = new StubContent (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content);
        opts.dialogTitle                  = "Session Settings";
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        if (auto* dw = opts.launchAsync())
        {
            dw->setAlwaysOnTop (true);
            dw->toFront (true);
        }
    }
    else if (id == 60)   onBackupClicked();
    else if (id == 61)   MirrorDrivesDialog::launch (engine);
    else if (id == 62)
    {
        engine.setAutoArmOnInputDetect (! engine.isAutoArmOnInputDetect());
        showStatus (engine.isAutoArmOnInputDetect()
                      ? "Auto-arm on input detect: ON"
                      : "Auto-arm on input detect: OFF");
    }
    // Edit menu
    else if (id == 300)  editUndo();
    else if (id == 301)  editRedo();
    else if (id == 302)  editCutSelected (true);
    else if (id == 303)  editCutSelected (false);
    else if (id == 304)  editPasteSelected();
    else if (id == 305)  deleteSelectedStrips();
    else if (id == 306)  editCropToLoopRange();
    else if (id == 307)  editSoloSelection();
    else if (id == 308)  editSetRangeToLoopRange();
    else if (id == 309)  editToggleSnap();
    else if (id == 310)  editSplitAtPlayhead();
    else if (id == 311)  editStartRange();
    else if (id == 312)  editFinishRange();
    else if (id == 313)  removeLastCapture();
    else if (id == 314)  togglePunchMode();
    else if (id == 320)  showBatchRenameDialog();
    else if (id == 321)  showBatchColourDialog();
    else if (id == 330)  moveSelectedStrips (-1);
    else if (id == 331)  moveSelectedStrips ( 1);
    else if (id == 332)  colourSelectedStrips();
    else if (id == 333)  deleteSelectedStrips();
    else if (id == 334)  clearStripSelection();
    else if (id == 335)  selectAllStrips();
}


