#include "MainComponent.h"
#include "../Theme/BrandColors.h"
#include "AudioDeviceDialog.h"
#include "Meterbridge.h"
#include "PatchPage.h"
#include "SessionSettingsDialog.h"

using namespace zynforge;

MainComponent::MainComponent()
{
    setLookAndFeel (&laf);

    titleLabel.setFont (juce::Font (juce::FontOptions().withHeight (16.0f).withStyle ("Bold")));
    titleLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (titleLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f)));
    statusLabel.setColour (juce::Label::textColourId, brand::textMuted);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    sessionLabel.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
    sessionLabel.setColour (juce::Label::textColourId, brand::textMuted);
    sessionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (sessionLabel);

    transportLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold")));
    transportLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    transportLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (transportLabel);

    recordButton.setColour (juce::TextButton::buttonColourId, brand::accentRecord.withAlpha (0.18f));
    recordButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord);
    recordButton.onClick = [this] { onRecordClicked(); };
    addAndMakeVisible (recordButton);

    playButton.setColour (juce::TextButton::buttonColourId, brand::accentPlay.withAlpha (0.18f));
    playButton.setColour (juce::TextButton::textColourOffId, brand::accentPlay);
    playButton.onClick = [this] { onPlayClicked(); };
    addAndMakeVisible (playButton);

    stopButton.onClick = [this] { onStopClicked(); };
    addAndMakeVisible (stopButton);

    loadButton.setColour (juce::TextButton::buttonColourId, brand::accentVS.withAlpha (0.18f));
    loadButton.setColour (juce::TextButton::textColourOffId, brand::accentVS);
    loadButton.onClick = [this] { onFileMenuClicked(); };
    addAndMakeVisible (loadButton);

    deviceButton.onClick = [this] { onDeviceClicked(); };
    addAndMakeVisible (deviceButton);

    formatButton.onClick = [this] { onFormatClicked(); };
    addAndMakeVisible (formatButton);

    preRollButton.onClick = [this] { onPreRollClicked(); };
    addAndMakeVisible (preRollButton);

    lockButton.setColour (juce::TextButton::buttonColourId, brand::accentRecord.withAlpha (0.18f));
    lockButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord);
    lockButton.onClick = [this] { onLockToggled(); };
    addAndMakeVisible (lockButton);

    backupButton.onClick = [this] { onBackupClicked(); };
    addAndMakeVisible (backupButton);

    patchButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus.withAlpha (0.18f));
    patchButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus);
    patchButton.onClick = [this] { zynforge::PatchPage::launch (engine); };
    addAndMakeVisible (patchButton);

    auto styleViewBtn = [] (juce::TextButton& b, bool engaged)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     engaged ? brand::accentStatus.withAlpha (0.32f)
                             : brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId,
                     engaged ? brand::accentStatus : brand::textSecondary);
    };
    styleViewBtn (mixViewButton,  true);
    styleViewBtn (editViewButton, false);
    mixViewButton .setTooltip ("Mixer view — channel strips with faders and meters.");
    editViewButton.setTooltip ("Edit view — waveforms of the loaded/recorded tracks.");
    mixViewButton .onClick = [this] { switchView (View::Mix);  };
    editViewButton.onClick = [this] { switchView (View::Edit); };
    addAndMakeVisible (mixViewButton);
    addAndMakeVisible (editViewButton);

    addChannelButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus.withAlpha (0.18f));
    addChannelButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus);
    addChannelButton.setTooltip ("Set the number of recording channels — opens a prompt where you type the count (1–256).");
    addChannelButton.onClick = [this]
    {
        if (engine.isRecording()) { showStatus ("Stop recording before changing channel count"); return; }
        const int cur = engine.getRecorder().getNumTracks();

        auto* aw = new juce::AlertWindow ("Number of channels",
                                          "How many recording channels do you want?\n"
                                          "(current: " + juce::String (cur) + ", min 0, max 256)\n"
                                          "Stereo creates two linked strips per channel "
                                          "(L + R), so '4 stereo' = 8 strips.",
                                          juce::MessageBoxIconType::QuestionIcon);
        aw->addTextEditor ("count", juce::String (cur), {});

        juce::StringArray modes { "Mono", "Stereo" };
        aw->addComboBox ("mode", modes, "Channel mode");
        if (auto* cb = aw->getComboBoxComponent ("mode"))
            cb->setSelectedItemIndex (0, juce::dontSendNotification);

        aw->addButton ("Apply",  1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true,
            juce::ModalCallbackFunction::create (
                [this, aw] (int result)
                {
                    if (result == 1)
                    {
                        const int requested = aw->getTextEditorContents ("count").getIntValue();
                        const bool stereo = aw->getComboBoxComponent ("mode") != nullptr
                                          && aw->getComboBoxComponent ("mode")->getSelectedItemIndex() == 1;

                        // Stereo mode creates two strips per requested channel.
                        const int rawCount = stereo ? requested * 2 : requested;
                        const int clamped  = juce::jlimit (0, 256, rawCount);
                        engine.setStripCount (clamped);

                        // Mark each L track in a stereo pair so the UI knows
                        // to collapse the L+R pair into one stereo strip.
                        // setTrackStereo also persists the flag to appProps.
                        const int total = engine.getRecorder().getNumTracks();
                        for (int i = 0; i < total; ++i)
                        {
                            const bool isLeftOfPair = stereo && (i % 2 == 0) && (i + 1 < total);
                            engine.setTrackStereo (i, isLeftOfPair);
                        }

                        if (stereo)
                        {
                            // Auto-name pair: only the L track is exposed in
                            // the mixer (the R partner is hidden), so give
                            // the L track a clean pair label.
                            for (int i = 0; i < total; i += 2)
                            {
                                const int pairNum = i / 2 + 1;
                                engine.setTrackName (i, "Stereo " + juce::String (pairNum));
                                if (i + 1 < total)
                                    engine.setTrackName (i + 1, "  (R)");
                            }
                        }

                        showStatus ("Channels: " + juce::String (engine.getRecorder().getNumTracks())
                                    + (stereo ? " (stereo pairs)" : ""));
                    }
                    delete aw;
                }),
            false);
    };
    addAndMakeVisible (addChannelButton);

    metersButton.onClick = [this] { zynforge::Meterbridge::launch (engine); };
    metersButton.setTooltip ("Open the floating meterbridge — drag onto a second display.");
    addAndMakeVisible (metersButton);

    oscButton.onClick = [this]
    {
        // Quick menu: pick a console dialect and start listening on 8000.
        juce::PopupMenu menu;
        menu.addSectionHeader (engine.isOscListening()
                                ? "OSC listening on port " + juce::String (engine.getOscPort())
                                : "OSC idle");
        menu.addSeparator();
        menu.addItem (1, "Listen — Generic /zynforge");
        menu.addItem (2, "Listen — DiGiCo");
        menu.addItem (3, "Listen — Allen & Heath (SQ / Avantis)");
        menu.addItem (4, "Listen — SSL Live");
        menu.addItem (5, "Listen — Yamaha (DM7 / RIVAGE)");
        menu.addSeparator();
        menu.addItem (10, "Stop", engine.isOscListening());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&oscButton),
                            [this] (int chosen)
        {
            if (chosen <= 0) return;
            if (chosen == 10) { engine.stopOsc(); oscButton.setButtonText ("OSC"); showStatus ("OSC stopped"); return; }
            const int dialect = chosen - 1;
            if (engine.startOsc (8000, dialect))
            {
                oscButton.setButtonText ("OSC •");
                showStatus ("OSC listening on 8000 (" +
                            juce::StringArray ({"Generic","DiGiCo","A&H","SSL","Yamaha"})[dialect] + ")");
            }
            else showStatus ("OSC failed to bind port 8000");
        });
    };
    oscButton.setTooltip ("OSC remote: receive transport / scene / marker / channel-name messages from DiGiCo / A&H / SSL / Yamaha consoles or any OSC app.");
    addAndMakeVisible (oscButton);

    // Tooltips on header controls.
    recordButton .setTooltip ("Start / stop multitrack recording.");
    deviceButton .setTooltip ("Choose the audio device, sample rate and buffer size.");
    formatButton .setTooltip ("Capture format: WAV 24, WAV 32-float, or FLAC 24.");
    preRollButton.setTooltip ("Pre-roll: 0 / 5 / 10 / 30 seconds. When RECORD is pressed, this much already-buffered audio is dumped to disk first.");
    lockButton   .setTooltip ("System Lock: disables every other control so a stray click can't kill a take.");
    loadButton   .setTooltip ("FILE menu: Open / Save / Save As / Export.");
    playButton   .setTooltip ("Play the loaded session through the matching output of each strip (virtual soundcheck).");
    stopButton   .setTooltip ("Stop playback and rewind to the start.");
    backupButton .setTooltip ("Pick a second drive — every track is mirrored there as you record.");
    patchButton  .setTooltip ("Open the patch matrix: route hardware inputs / outputs to channel strips.");
    titleLabel   .setTooltip ("Zynforge Recording — JUCE 8 multitrack recorder + virtual soundcheck.");

    refreshFormatButton();
    refreshPreRollButton();

    addAndMakeVisible (bigClock);

    phaseMeter = std::make_unique<zynforge::PhaseMeter> (engine);
    addAndMakeVisible (*phaseMeter);

    timeline = std::make_unique<zynforge::TimelineStrip> (engine);
    addAndMakeVisible (*timeline);

    transportBar = std::make_unique<zynforge::TransportBar> (engine);
    addAndMakeVisible (*transportBar);

    stripsViewport.setViewedComponent (&stripsContainer, false);
    stripsViewport.setScrollBarsShown (false, true);     // h-scroll only
    addAndMakeVisible (stripsViewport);

    editPage = std::make_unique<zynforge::EditPage> (engine);
    addChildComponent (*editPage);   // hidden by default; switchView toggles
    switchView (View::Mix);

    setWantsKeyboardFocus (true);
    addKeyListener (this);

    startTimerHz (10);  // poll for input-channel count + transport position
    rebuildStrips();
    updateTransportLabels();

    juce::Timer::callAfterDelay (250, [this] { offerSessionRecovery(); });

   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (this);
   #endif
}

MainComponent::~MainComponent()
{
   #if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu (nullptr);
   #endif
    removeKeyListener (this);
    setLookAndFeel (nullptr);
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Session" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int topLevelIndex, const juce::String&)
{
    juce::PopupMenu menu;

    if (topLevelIndex == 0)  // File
    {
        menu.addItem (1, "Open Session…");
        menu.addItem (4, "Import Audio Files…",  ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (2, "Save Session State",  engine.getActiveSessionDir().isDirectory());
        menu.addItem (3, "Save Session As…",   engine.getActiveSessionDir().isDirectory());
        menu.addSeparator();

        juce::PopupMenu exportMenu;
        const bool hasActive = engine.getActiveSessionDir().isDirectory();
        exportMenu.addItem (10, "Export All Tracks…", hasActive);

        juce::PopupMenu indiv;
        const int n = engine.getRecorder().getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const auto& t  = engine.getRecorder().getTrack (i);
            const auto nm  = t.name.isNotEmpty() ? t.name
                                                  : juce::String ("In " + juce::String (i + 1));
            indiv.addItem (100 + i, nm, hasActive);
        }
        exportMenu.addSubMenu ("Export Individual Track", indiv, hasActive && n > 0);
        menu.addSubMenu ("Export", exportMenu);
        menu.addSeparator();
        menu.addItem (60, "Choose Backup Folder…", ! engine.isRecording());
        menu.addSeparator();
        menu.addItem (99, "Quit Zynforge Recording…");
    }
    else if (topLevelIndex == 1)  // Edit
    {
        // Edit menu. Shortcut hints appear after a tab character — macOS
        // pulls those out and right-aligns them in the native menu.
        const bool hasContext = engine.getPlayer().isLoaded() || engine.isRecording();

        menu.addItem (300, "Undo\tCmd+Z",       false);
        menu.addItem (301, "Redo\tCmd+R",       false);
        menu.addSeparator();
        menu.addItem (302, "Cut\tCmd+X",        false);
        menu.addItem (303, "Copy\tCmd+C",       false);
        menu.addItem (304, "Paste\tCmd+V",      false);
        menu.addItem (305, "Delete",             false);
        menu.addItem (306, "Crop\tCtrl+Cmd+C", false);
        menu.addItem (307, "Solo Selection\tA",   true);
        menu.addSeparator();
        menu.addItem (308, "Set Range to Loop Range",
                      engine.getPlayer().hasLoopRegion());
        menu.addSeparator();
        menu.addItem (309, "Toggle Snap\t4", true, snapToMarkers);
        menu.addSeparator();
        menu.addItem (310, "Split/Separate\tS", hasContext);
        menu.addSeparator();
        menu.addItem (311, "Start Range\t,",  hasContext);
        menu.addItem (312, "Finish Range\t.", hasContext);
        menu.addSeparator();
        menu.addItem (313, "Remove Last Capture", ! engine.isRecording());
    }
    else if (topLevelIndex == 2)  // Session
    {
        menu.addItem (50, "Patch…");
        menu.addItem (51, "Meterbridge…");

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

        menu.addSeparator();
        menu.addItem (260, "Upload session to cloud…", ! engine.isRecording());
        menu.addItem (261, "Configure cloud upload command…");
        menu.addSeparator();
        const bool compRunning = engine.isCompanionServerRunning();
        menu.addItem (270, compRunning
                            ? juce::String ("Stop companion (port " + juce::String (engine.getCompanionServerPort()) + ")")
                            : juce::String ("Start companion server on :9000…"));
        menu.addSeparator();
        menu.addItem (250, "Session Settings…", ! engine.isRecording());
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
    else if (id == 10)   onExportAllTracks();
    else if (id == 99)   confirmAndQuit();
    // Track-export sub-menu uses ids 100..199. Tightened from the
    // previous open-ended `>= 100` which was swallowing 110..115
    // (OSC dialects), 200..230 (settings menu), and 250 (Session
    // Settings) — sending all of them to onExportIndividualTrack.
    // OSC dialect picks under Session ▶ OSC ▶ — must be checked BEFORE
    // the export range below, otherwise IDs 110..115 fall through to
    // onExportIndividualTrack(10..15) and silently no-op.
    else if (id >= 110 && id <= 114)
    {
        const int dialect = id - 110;
        if (engine.startOsc (8000, dialect))
            showStatus ("OSC listening on 8000 (" +
                        juce::StringArray ({"Generic","DiGiCo","A&H","SSL","Yamaha"})[dialect] + ")");
        else
            showStatus ("OSC failed to bind UDP 8000 — port already in use?");
    }
    else if (id == 115)  { engine.stopOsc(); showStatus ("OSC stopped"); }
    else if (id == 260)
    {
        // Run the configured cloud-upload command with {SESSION} expanded.
        const auto sessionDir = engine.getActiveSessionDir();
        if (! sessionDir.isDirectory()) { showStatus ("No session active — load or record first"); return; }
        auto* props = engine.getAppProps();
        const auto tmpl = props != nullptr ? props->getValue ("cloudUploadCommand") : juce::String();
        if (tmpl.isEmpty())
        {
            showStatus ("No upload command configured — pick \"Configure cloud upload command…\" first");
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
            showStatus ("Cloud upload failed to launch — check the configured command");
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
            showStatus ("Companion server on http://localhost:9000 — open it from any browser / iPad");
        }
        else
        {
            showStatus ("Companion failed to bind port 9000 — try a different port");
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
                                          juce::MessageBoxIconType::QuestionIcon);
        const auto current = engine.getAppProps() != nullptr
                              ? engine.getAppProps()->getValue ("cloudUploadCommand")
                              : juce::String();
        aw->addTextEditor ("cmd", current, {});
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
                    r.removeFromTop (6);
                };
                row (fmtL, fmtBox); row (rateL, rateBox); row (bitsL, bitsBox);
                r.removeFromTop (10);
                auto br = r.removeFromBottom (32);
                applyB .setBounds (br.removeFromRight (110));
                br.removeFromRight (8);
                cancelB.setBounds (br.removeFromRight (110));
            }

            void paint (juce::Graphics& g) override { g.fillAll (brand::bgPanel); }

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
    // Edit menu
    else if (id == 307)  soloSelection();
    else if (id == 308)  setRangeToLoopRange();
    else if (id == 309)  toggleSnap();
    else if (id == 310)  splitSeparate();
    else if (id == 311)  startRange();
    else if (id == 312)  finishRange();
    else if (id == 313)  removeLastCapture();
}

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

    if (c == 'm')
    {
        const int n = engine.dropMarkerAtCurrentPosition();
        showStatus (n >= 0 ? "Marker " + juce::String (n) + " dropped"
                            : "No active session — can't drop marker");
        return true;
    }
    if (c == 'a') { soloSelection();        return true; }
    if (c == 's') { splitSeparate();        return true; }
    if (c == ',') { startRange();           return true; }
    if (c == '.') { finishRange();          return true; }
    if (c == '4') { toggleSnap();           return true; }
    return false;
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

    // Reflect active state in the button tinting.
    auto colour = [] (juce::TextButton& b, bool engaged)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     engaged ? brand::accentStatus.withAlpha (0.32f)
                             : brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId,
                     engaged ? brand::accentStatus : brand::textSecondary);
    };
    colour (mixViewButton,  mix);
    colour (editViewButton, ! mix);

    resized();
}

void MainComponent::rebuildStrips()
{
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

        auto colourCb = [this, i, step] (juce::Colour chosen)
        {
            engine.setTrackColour (i, chosen);
            if (step == 2) engine.setTrackColour (i + 1, chosen);
        };
        auto nameCb   = [this, i] (juce::String chosen) { engine.setTrackName   (i, chosen); };
        auto gainCb   = [this, i, step] (float dB)
        {
            engine.setTrackGainDb (i, dB);
            if (step == 2) engine.setTrackGainDb (i + 1, dB);
        };
        auto panCb    = [this, i, step] (float pan)
        {
            engine.setTrackPan (i, pan);
            if (step == 2) engine.setTrackPan (i + 1, pan);
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
                                                 tR);

        // Right-click menu wiring.
        auto deleteCb     = [this, i, step]
        {
            // Stereo strip: delete both halves of the pair.
            engine.removeStripAt (i);
            if (step == 2) engine.removeStripAt (i);   // same idx after shift
        };
        auto addCb        = [this] { engine.addOneStrip(); };
        auto linkStereoCb = [this, i]
        {
            const int total = engine.getRecorder().getNumTracks();
            if (i + 1 >= total) return;
            const bool wasStereo = engine.getRecorder().getTrack (i).isStereo.load();
            engine.setTrackStereo (i, ! wasStereo);
            // Trigger a full rebuild on the next timer tick.
            lastTrackCount = -1;
        };
        auto linkOtherCb  = [this, i] (int other) { juce::ignoreUnused (i, other); };
        s->setMenuCallbacks (std::move (deleteCb),
                             std::move (addCb),
                             std::move (linkStereoCb),
                             std::move (linkOtherCb));

        s->setAvailableInputs  (numIns);
        s->setAvailableOutputs (numOuts);
        stripsContainer.addAndMakeVisible (*s);
        strips.push_back (std::move (s));

        i += step;
    }
    lastTrackCount = n;
    resized();
}

void MainComponent::timerCallback()
{
    const int n = engine.getRecorder().getNumTracks();
    if (n != lastTrackCount)
        rebuildStrips();

    // Keep each strip's input/output combos in sync with engine state —
    // the PATCH page can mutate routing behind the strip's back. Also
    // refresh name + colour so changes made from the EDIT view show up.
    for (auto& s : strips)
        if (s != nullptr)
        {
            s->refreshRoutingSelection();
            s->refreshAppearance();
        }

    updateTransportLabels();

    const bool playing = engine.isPlaying();
    if (! playing && playButton.getButtonText() == "PAUSE")
        playButton.setButtonText ("PLAY");

    // Drive BigClockPanel
    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();
    auto& markers  = engine.getMarkers();

    const double deviceSR = [this]() -> double
    {
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            return d->getCurrentSampleRate();
        return 48000.0;
    }();

    BigClockPanel::Mode m = BigClockPanel::Mode::Idle;
    juce::int64 elapsed = 0;
    double      timerSR = deviceSR;

    if (engine.isRecording())
    {
        m = BigClockPanel::Mode::Recording;
        elapsed = recorder.getSamplesSinceStart();
    }
    else if (engine.isPlaying())
    {
        m = BigClockPanel::Mode::Playing;
        elapsed = player.getPositionSamples();
        timerSR = player.getSampleRate();
    }

    bigClock.setMode (m);
    bigClock.setElapsed (elapsed, timerSR);
    bigClock.setMarkers (markers.getCount());

    const bool rec = engine.isRecording();
    formatButton .setEnabled (! rec);
    preRollButton.setEnabled (! rec);

    // Disk-health calc
    const auto sessRoot = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                              .getChildFile ("Zynforge Sessions");
    const auto bytesFree = sessRoot.exists() ? sessRoot.getBytesFreeOnVolume()
                                              : juce::File ("/").getBytesFreeOnVolume();
    const double freeGB = (double) bytesFree / (1024.0 * 1024.0 * 1024.0);

    const int    bitDepth     = 24;
    const int    bytesPerSamp = bitDepth / 8;
    const int    channels     = juce::jmax (1, recorder.getNumTracks());
    const double bytesPerSec  = deviceSR * bytesPerSamp * channels;
    const double remainingSec = bytesPerSec > 0 ? (double) bytesFree / bytesPerSec : 0.0;

    bigClock.setDiskInfo (freeGB,
                          recorder.getLastWriteMs(),
                          recorder.getMissedSamples(),
                          remainingSec);
}

static juce::String samplesToTimecode (juce::int64 samples, double sr)
{
    if (sr <= 0.0) return "00:00";
    const auto seconds = (juce::int64) (samples / sr);
    return juce::String::formatted ("%02lld:%02lld", seconds / 60, seconds % 60);
}

void MainComponent::updateTransportLabels()
{
    auto& player = engine.getPlayer();
    const auto sr  = player.getSampleRate();
    const auto pos = samplesToTimecode (player.getPositionSamples(), sr);
    const auto tot = samplesToTimecode (player.getTotalLengthSamples(), sr);
    transportLabel.setText (pos + " / " + tot, juce::dontSendNotification);

    if (player.isLoaded())
    {
        const auto name = player.getSessionName();
        const auto tracks = juce::String (player.getNumTracks());
        sessionLabel.setText ("Session: " + name + " (" + tracks + " tr)",
                              juce::dontSendNotification);
    }
    else
    {
        sessionLabel.setText ("No session loaded", juce::dontSendNotification);
    }
}

void MainComponent::onRecordClicked()
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        statusLabel.setText ("Idle", juce::dontSendNotification);
        recordButton.setButtonText ("RECORD");
    }
    else
    {
        const auto dir = makeNewSessionDir();
        if (engine.startRecording (dir))
        {
            statusLabel.setText ("Recording → " + dir.getFileName(), juce::dontSendNotification);
            recordButton.setButtonText ("STOP");
        }
        else
        {
            statusLabel.setText ("Failed to start recording", juce::dontSendNotification);
        }
    }
}

void MainComponent::onPlayClicked()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded())
    {
        statusLabel.setText ("Load a session first", juce::dontSendNotification);
        return;
    }

    if (player.isPlaying())
    {
        engine.stopPlayback();
        playButton.setButtonText ("PLAY");
        statusLabel.setText ("Paused", juce::dontSendNotification);
    }
    else
    {
        engine.startPlayback();
        playButton.setButtonText ("PAUSE");
        statusLabel.setText ("Playing → " + player.getSessionName(), juce::dontSendNotification);
    }
}

void MainComponent::onStopClicked()
{
    auto& player = engine.getPlayer();
    if (engine.isRecording()) engine.stopRecording();
    engine.stopPlayback();
    player.rewind();
    playButton.setButtonText ("PLAY");
    recordButton.setButtonText ("RECORD");
    statusLabel.setText (player.isLoaded() ? "Stopped" : "Idle", juce::dontSendNotification);
}

void MainComponent::onFormatClicked()
{
    if (engine.isRecording()) return;

    auto& recorder = engine.getRecorder();
    using F = zynforge::CaptureFormat;
    const auto cur = recorder.getCaptureFormat();
    const auto next = cur == F::Wav24      ? F::Wav32Float
                    : cur == F::Wav32Float ? F::Flac24
                                            : F::Wav24;
    recorder.setCaptureFormat (next);
    refreshFormatButton();
}

void MainComponent::onPreRollClicked()
{
    if (engine.isRecording()) return;

    auto& recorder = engine.getRecorder();
    const int cur = recorder.getPreRollSeconds();
    const int next = cur == 0  ? 5
                   : cur == 5  ? 10
                   : cur == 10 ? 30
                               : 0;
    recorder.setPreRollSeconds (next);
    refreshPreRollButton();
}

void MainComponent::refreshFormatButton()
{
    using F = zynforge::CaptureFormat;
    juce::String label;
    switch (engine.getRecorder().getCaptureFormat())
    {
        case F::Wav16:       label = "WAV 16";   break;
        case F::Wav24:       label = "WAV 24";   break;
        case F::Wav32Float:  label = "WAV 32F";  break;
        case F::Aiff16:      label = "AIFF 16";  break;
        case F::Aiff24:      label = "AIFF 24";  break;
        case F::Aiff32Float: label = "AIFF 32F"; break;
        case F::Flac16:      label = "FLAC 16";  break;
        case F::Flac24:      label = "FLAC 24";  break;
    }
    formatButton.setButtonText (label);
}

void MainComponent::refreshPreRollButton()
{
    const int s = engine.getRecorder().getPreRollSeconds();
    preRollButton.setButtonText ("PRE " + juce::String (s) + "s");
}

void MainComponent::onLockToggled()
{
    sessionLocked = ! sessionLocked;
    applyLockState();
}

void MainComponent::applyLockState()
{
    const bool e = ! sessionLocked;

    recordButton .setEnabled (e);
    deviceButton .setEnabled (e);
    formatButton .setEnabled (e && ! engine.isRecording());
    preRollButton.setEnabled (e && ! engine.isRecording());
    loadButton   .setEnabled (e);
    playButton   .setEnabled (e);
    stopButton   .setEnabled (e);
    backupButton .setEnabled (e);
    patchButton  .setEnabled (e);
    metersButton .setEnabled (e);
    oscButton    .setEnabled (e);
    addChannelButton.setEnabled (e && ! engine.isRecording());

    for (auto& s : strips) if (s != nullptr) s->setEnabled (e);

    lockButton.setButtonText (sessionLocked ? "UNLOCK" : "LOCK");
    showStatus (sessionLocked ? "LOCKED — click UNLOCK to resume control"
                              : engine.isRecording()  ? "Recording"
                              : engine.isPlaying()    ? "Playing"
                                                      : "Idle");
}

void MainComponent::onBackupClicked()
{
    if (engine.isRecording())
    {
        showStatus ("Stop recording to change the backup folder");
        return;
    }

    chooser = std::make_unique<juce::FileChooser> (
        "Pick a backup folder (recordings are mirrored here)",
        engine.getBackupDirectory().exists() ? engine.getBackupDirectory()
                                              : getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        auto dir = fc.getResult();
        if (dir.getFullPathName().isEmpty()) return;
        engine.setBackupDirectory (dir);
        backupButton.setButtonText ("BACKUP ✓");
        showStatus ("Backup folder → " + dir.getFileName());
    });
}

void MainComponent::offerSessionRecovery()
{
    const auto incomplete = zynforge::AudioEngine::findIncompleteSessions (getSessionsRoot());
    if (incomplete.isEmpty()) return;

    juce::PopupMenu menu;
    menu.addSectionHeader (juce::String (incomplete.size()) +
                           " session(s) didn't stop cleanly");
    for (int i = 0; i < incomplete.size(); ++i)
        menu.addItem (i + 1, incomplete[i].getFileName());
    menu.addSeparator();
    menu.addItem (1000, "Dismiss");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&statusLabel),
                        [this, incomplete] (int chosen)
    {
        if (chosen <= 0 || chosen >= 1000) return;
        const auto& dir = incomplete[chosen - 1];
        // Clear the marker; the WAV files are intact thanks to periodic
        // header flush. Load the session so the engineer can inspect it.
        dir.getChildFile ("recording.session").deleteFile();
        engine.loadSession (dir);
        showStatus ("Recovered: " + dir.getFileName());
        updateTransportLabels();
    });
}

void MainComponent::onFileMenuClicked()
{
    const auto activeDir   = engine.getActiveSessionDir();
    const bool hasActive   = activeDir.isDirectory();

    juce::PopupMenu menu;
    menu.addItem (1, "Open Session…");
    menu.addItem (4, "Import Audio Files…",   ! engine.isRecording());
    menu.addSeparator();
    menu.addItem (2, "Save Session State",      hasActive);
    menu.addItem (3, "Save Session As…",   hasActive);
    menu.addSeparator();

    juce::PopupMenu exportMenu;
    exportMenu.addItem (10, "Export All Tracks…", hasActive);

    juce::PopupMenu individualMenu;
    const int trackCount = engine.getRecorder().getNumTracks();
    for (int i = 0; i < trackCount; ++i)
    {
        const auto& t   = engine.getRecorder().getTrack (i);
        const auto name = t.name.isNotEmpty() ? t.name
                                              : juce::String ("In " + juce::String (i + 1));
        individualMenu.addItem (100 + i, name, hasActive);
    }
    exportMenu.addSubMenu ("Export Individual Track", individualMenu, hasActive && trackCount > 0);
    menu.addSubMenu ("Export", exportMenu);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&loadButton),
                        [this] (int chosen)
    {
        if (chosen == 0) return;
        if (chosen == 1)           onLoadSessionClicked();
        else if (chosen == 2)      onSaveSessionState();
        else if (chosen == 3)      onSaveSessionAs();
        else if (chosen == 4)      onImportAudioFiles();
        else if (chosen == 10)     onExportAllTracks();
        else if (chosen >= 100)    onExportIndividualTrack (chosen - 100);
    });
}

void MainComponent::showStatus (const juce::String& msg)
{
    statusLabel.setText (msg, juce::dontSendNotification);
}

void MainComponent::soloSelection()
{
    // Live-recorder interpretation: if any track is soloed, clear all
    // solos. Otherwise solo every track that's currently armed.
    auto& rec = engine.getRecorder();
    bool anySolo = false;
    for (int i = 0; i < rec.getNumTracks(); ++i)
        if (rec.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

    if (anySolo)
    {
        for (int i = 0; i < rec.getNumTracks(); ++i)
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);
        showStatus ("Cleared all solos");
    }
    else
    {
        int n = 0;
        for (int i = 0; i < rec.getNumTracks(); ++i)
            if (rec.getTrack (i).armed.load (std::memory_order_relaxed))
            { rec.getTrack (i).soloed.store (true, std::memory_order_relaxed); ++n; }
        showStatus ("Soloed " + juce::String (n) + " armed track" + (n == 1 ? "" : "s"));
    }
}

void MainComponent::setRangeToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("No loop region set"); return; }
    auto& m = engine.getMarkers();
    m.drop (player.getLoopStart(), "Range In");
    m.drop (player.getLoopEnd(),   "Range Out");
    showStatus ("Range markers placed at loop boundaries");
}

void MainComponent::toggleSnap()
{
    snapToMarkers = ! snapToMarkers;
    showStatus (snapToMarkers ? "Snap to markers: ON" : "Snap to markers: OFF");
}

namespace
{
    juce::int64 currentPlayheadSamples (zynforge::AudioEngine& eng)
    {
        if (eng.isRecording()) return eng.getRecorder().getSamplesSinceStart();
        if (eng.getPlayer().isLoaded()) return eng.getPlayer().getPositionSamples();
        return 0;
    }
}

void MainComponent::splitSeparate()
{
    const auto pos = currentPlayheadSamples (engine);
    engine.getMarkers().drop (pos, "Split");
    showStatus ("Split marker dropped");
}

void MainComponent::startRange()
{
    engine.getMarkers().drop (currentPlayheadSamples (engine), "Range In");
    showStatus ("Range In marker dropped");
}

void MainComponent::finishRange()
{
    engine.getMarkers().drop (currentPlayheadSamples (engine), "Range Out");
    showStatus ("Range Out marker dropped");
}

void MainComponent::removeLastCapture()
{
    const auto root = getSessionsRoot();
    if (! root.isDirectory()) { showStatus ("No sessions to remove"); return; }

    auto dirs = root.findChildFiles (juce::File::findDirectories, false, "Session_*");
    if (dirs.isEmpty()) { showStatus ("No sessions to remove"); return; }

    // Find the most-recently-modified session folder.
    dirs.sort();   // alphabetical; our naming is ISO-style so this is chronological
    const auto target = dirs.getLast();

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Remove last capture?")
            .withMessage ("Permanently delete\n\n" + target.getFullPathName() + "\n\nThis cannot be undone.")
            .withButton ("Delete")
            .withButton ("Cancel"),
        [this, target] (int result)
    {
        if (result != 0) return;
        if (target.deleteRecursively())
            showStatus ("Removed: " + target.getFileName());
        else
            showStatus ("Couldn't remove that folder");
    });
}

void MainComponent::applySessionSettings()
{
    if (engine.isRecording()) { showStatus ("Stop recording before applying settings"); return; }

    using F = zynforge::CaptureFormat;
    F fmt;
    if (pendingContainer == 0)
        fmt = pendingBitDepth == 16 ? F::Wav16  : pendingBitDepth == 24 ? F::Wav24  : F::Wav32Float;
    else if (pendingContainer == 1)
        fmt = pendingBitDepth == 16 ? F::Aiff16 : pendingBitDepth == 24 ? F::Aiff24 : F::Aiff32Float;
    else
        fmt = pendingBitDepth == 16 ? F::Flac16 : F::Flac24;   // FLAC has no 32-float

    engine.getRecorder().setCaptureFormat (fmt);
    refreshFormatButton();

    auto setup = engine.getDeviceManager().getAudioDeviceSetup();
    setup.sampleRate = pendingSampleRate;
    const auto err = engine.getDeviceManager().setAudioDeviceSetup (setup, true);
    if (err.isEmpty())
        showStatus ("Session settings applied (" + formatButton.getButtonText()
                     + " @ " + juce::String (pendingSampleRate / 1000.0, 1) + " kHz)");
    else
        showStatus ("Sample rate change failed: " + err);
}

void MainComponent::confirmAndQuit()
{
    const bool recording = engine.isRecording();
    const auto activeDir = engine.getActiveSessionDir();
    const bool hasActiveSession = activeDir.isDirectory();

    juce::String title, message;
    juce::String b1, b2, b3;  // primary (positive), alt (Don't save), cancel

    if (recording)
    {
        title   = "Recording is still rolling";
        message = "A recording is in progress.\n"
                  "Stop the recording cleanly and quit?";
        b1 = "Stop & Quit";
        b2 = juce::String();   // no alt
        b3 = "Cancel";
    }
    else if (hasActiveSession)
    {
        title   = "Quit Zynforge Recording?";
        message = "Save session state for \"" + activeDir.getFileName() + "\" before quitting?";
        b1 = "Save & Quit";
        b2 = "Don't Save";
        b3 = "Cancel";
    }
    else
    {
        title   = "Quit Zynforge Recording?";
        message = "Any unsaved app state will be lost.";
        b1 = "Quit";
        b2 = juce::String();
        b3 = "Cancel";
    }

    auto options = juce::MessageBoxOptions()
                     .withIconType (juce::MessageBoxIconType::QuestionIcon)
                     .withTitle (title)
                     .withMessage (message)
                     .withButton (b1)
                     .withAssociatedComponent (this);
    if (b2.isNotEmpty()) options = options.withButton (b2);
    options = options.withButton (b3);

    juce::AlertWindow::showAsync (options,
        [this, recording, hasActiveSession, b2nonEmpty = b2.isNotEmpty(), activeDir]
        (int result)
    {
        // JUCE numbers buttons from 1 in the order they're added.
        // Result 0 = first (primary), 1 = second, 2 = third.
        // With 2 buttons (primary + cancel): cancel = 1.
        // With 3 buttons (save + don't save + cancel): cancel = 2.

        const int cancelIndex = b2nonEmpty ? 2 : 1;
        if (result == cancelIndex) return;

        if (recording)
        {
            // Single primary action: stop and quit.
            engine.stopRecording();
        }
        else if (hasActiveSession)
        {
            if (result == 0)        // Save & Quit
                saveSessionStateTo (activeDir);
            // result == 1 → Don't Save → fall through
        }

        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    });
}

bool MainComponent::saveSessionStateTo (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;

    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("captureFormat",  (int) recorder.getCaptureFormat());
    root->setProperty ("preRollSeconds", recorder.getPreRollSeconds());
    root->setProperty ("phaseLeft",      engine.getPhaseLeftChannel());
    root->setProperty ("phaseRight",     engine.getPhaseRightChannel());

    if (player.hasLoopRegion())
    {
        root->setProperty ("loopStart", (juce::int64) player.getLoopStart());
        root->setProperty ("loopEnd",   (juce::int64) player.getLoopEnd());
    }

    juce::Array<juce::var> trackArr;
    for (int i = 0; i < recorder.getNumTracks(); ++i)
    {
        juce::DynamicObject::Ptr t (new juce::DynamicObject());
        t->setProperty ("index", i);
        t->setProperty ("name",  recorder.getTrack (i).name);
        t->setProperty ("colourARGB",
                        (int) recorder.getTrack (i).colourARGB.load (std::memory_order_relaxed));
        trackArr.add (juce::var (t.get()));
    }
    root->setProperty ("tracks", trackArr);

    const auto json = juce::JSON::toString (juce::var (root.get()), true);
    return dir.getChildFile ("session_settings.json").replaceWithText (json);
}

int MainComponent::exportTracksTo (const juce::File& destDir,
                                   const std::vector<int>& channelIndices,
                                   const zynforge::ExportOptions& opts)
{
    auto sourceDir = engine.getActiveSessionDir();
    if (! sourceDir.isDirectory() || ! destDir.isDirectory()) return 0;

    destDir.createDirectory();

    auto allFiles = sourceDir.findChildFiles (juce::File::findFiles, false, "Track_*");

    auto matchesIndex = [] (const juce::File& f, int index1Based) -> bool
    {
        const auto base = f.getFileNameWithoutExtension();
        const auto suffix = juce::String::formatted ("Track_%02d", index1Based);
        return base == suffix;
    };

    zynforge::TrackExporter exporter;
    int succeeded = 0;
    juce::String firstError;

    for (int i : channelIndices)
    {
        for (auto& src : allFiles)
        {
            if (! matchesIndex (src, i + 1)) continue;

            auto& trackState = engine.getRecorder().getTrack (i);
            const auto safeName = trackState.name.replaceCharacter ('/', '_')
                                                  .replaceCharacter ('\\', '_');
            const auto baseName = juce::String::formatted ("Track_%02d - ", i + 1) + safeName;
            const auto destStem = destDir.getChildFile (baseName);

            juce::String err;
            if (exporter.exportTrack (src, destStem, opts, err))
                ++succeeded;
            else if (firstError.isEmpty())
                firstError = err;
            break;
        }
    }

    if (succeeded == 0 && firstError.isNotEmpty())
        showStatus ("Export failed: " + firstError);

    return succeeded;
}

void MainComponent::onSaveSessionState()
{
    const auto dir = engine.getActiveSessionDir();
    if (! dir.isDirectory()) { showStatus ("No active session"); return; }
    if (saveSessionStateTo (dir))
        showStatus ("Saved session state → " + dir.getFileName());
    else
        showStatus ("Save failed");
}

void MainComponent::onImportAudioFiles()
{
    if (engine.isRecording()) { showStatus ("Stop recording before importing"); return; }

    // Accept anything the JUCE basic format manager + FLAC can read.
    // (WavAudioFormat, AiffAudioFormat, FlacAudioFormat, OggVorbisAudioFormat,
    //  MP3AudioFormat — read-only — when JUCE_USE_MP3AUDIOFORMAT is on.)
    const juce::String filters = "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.caf";

    chooser = std::make_unique<juce::FileChooser> (
        "Pick audio files to import as a new session",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        filters);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectMultipleItems,
        [this] (const juce::FileChooser& fc)
    {
        const auto picks = fc.getResults();
        if (picks.isEmpty()) return;

        // Convert each picked file into a Track_NN.wav inside a fresh
        // session dir. The session is then loaded for VSC playback.
        auto sessionDir = makeNewSessionDir();
        sessionDir.createDirectory();

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();   // WAV, AIFF, FLAC, OGG, MP3 (read-only)

        int imported = 0;
        int failed   = 0;
        for (int i = 0; i < picks.size(); ++i)
        {
            const auto& src = picks.getReference (i);
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src));
            if (reader == nullptr) { ++failed; continue; }

            const auto dst = sessionDir.getChildFile (
                "Track_" + juce::String (i + 1).paddedLeft ('0', 2) + ".wav");
            dst.deleteFile();

            // Write as 24-bit WAV at the source sample rate so the
            // SessionPlayer's BufferingAudioReader path picks it up like
            // any other recorded track.
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> out (dst.createOutputStream());
            if (out == nullptr) { ++failed; continue; }

            juce::StringPairArray meta;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (out.get(), reader->sampleRate,
                                     juce::jmax (1, (int) reader->numChannels),
                                     24, meta, 0));
            if (writer == nullptr) { ++failed; continue; }
            out.release();   // writer now owns the stream

            if (! writer->writeFromAudioReader (*reader, 0, reader->lengthInSamples))
                ++failed;
            else
                ++imported;
        }

        if (imported == 0)
        {
            showStatus ("Import failed — no readable audio files");
            return;
        }

        // Load the freshly-built session for virtual soundcheck playback.
        const int loaded = engine.loadSession (sessionDir);
        showStatus ("Imported " + juce::String (imported)
                    + (failed > 0 ? " (skipped " + juce::String (failed) + ")"
                                  : juce::String())
                    + " — loaded " + juce::String (loaded) + " for playback");
    });
}

void MainComponent::onSaveSessionAs()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    chooser = std::make_unique<juce::FileChooser> (
        "Save session copy in…",
        getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::saveMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this, source] (const juce::FileChooser& fc)
    {
        auto dest = fc.getResult();
        if (dest.getFullPathName().isEmpty()) return;

        if (! dest.exists()) dest.createDirectory();
        if (! dest.isDirectory()) { showStatus ("Save As destination invalid"); return; }

        // Copy every file inside the source session dir (audio + markers + settings).
        if (! source.copyDirectoryTo (dest))
        {
            showStatus ("Save As failed");
            return;
        }

        // Refresh the per-session state JSON in the new location.
        saveSessionStateTo (dest);
        showStatus ("Saved As → " + dest.getFileName());
    });
}

void MainComponent::onExportAllTracks()
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    zynforge::ExportDialog::launch ("Export all tracks",
        [this] (std::optional<zynforge::ExportOptions> opts)
    {
        if (! opts.has_value()) return;
        const auto chosenOpts = *opts;

        chooser = std::make_unique<juce::FileChooser> (
            "Export all tracks to…", getSessionsRoot(), "");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags,
            [this, chosenOpts] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            std::vector<int> all;
            for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i) all.push_back (i);

            showStatus ("Exporting " + juce::String ((int) all.size()) + " tracks…");
            const int n = exportTracksTo (dest, all, chosenOpts);
            showStatus ("Exported " + juce::String (n) + " tracks → " + dest.getFileName());
        });
    });
}

void MainComponent::onExportIndividualTrack (int channelIndex)
{
    const auto source = engine.getActiveSessionDir();
    if (! source.isDirectory()) { showStatus ("No active session"); return; }

    zynforge::ExportDialog::launch ("Export track",
        [this, channelIndex] (std::optional<zynforge::ExportOptions> opts)
    {
        if (! opts.has_value()) return;
        const auto chosenOpts = *opts;

        chooser = std::make_unique<juce::FileChooser> (
            "Export track to…", getSessionsRoot(), "");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags,
            [this, channelIndex, chosenOpts] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            showStatus ("Exporting track " + juce::String (channelIndex + 1) + "…");
            const int n = exportTracksTo (dest, { channelIndex }, chosenOpts);
            showStatus (n > 0
                        ? "Exported track " + juce::String (channelIndex + 1)
                           + " → " + dest.getFileName()
                        : "Export failed");
        });
    });
}

void MainComponent::onLoadSessionClicked()
{
    chooser = std::make_unique<juce::FileChooser> (
        "Choose a session folder",
        getSessionsRoot(),
        "");

    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;

    chooser->launchAsync (flags, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (! dir.isDirectory()) return;

        engine.stopPlayback();
        const int n = engine.loadSession (dir);
        if (n > 0)
            statusLabel.setText ("Loaded " + juce::String (n) + " tracks", juce::dontSendNotification);
        else
            statusLabel.setText ("No Track_*.wav found in folder", juce::dontSendNotification);
        playButton.setButtonText ("PLAY");
        updateTransportLabels();
    });
}

juce::File MainComponent::getSessionsRoot() const
{
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile ("Zynforge Sessions");
}

void MainComponent::onDeviceClicked()
{
    zynforge::AudioDeviceDialog::launch (engine);
}

juce::File MainComponent::makeNewSessionDir() const
{
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    return getSessionsRoot().getChildFile ("Session_" + stamp);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (brand::bgDeep);

    auto header = getLocalBounds().removeFromTop (44 + 40).toFloat();
    g.setColour (brand::bgPanel);
    g.fillRect (header);
    g.setColour (brand::edge);
    g.drawHorizontalLine ((int) header.getBottom() - 1,
                          header.getX(), header.getRight());
    g.drawHorizontalLine (44,
                          header.getX(), header.getRight());
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    // Row 1 — title + status + LOCK + + CH + DEVICE + RECORD
    // FMT / PRE moved into Session Settings.
    auto row1 = r.removeFromTop (44).reduced (12, 8);
    titleLabel   .setBounds (row1.removeFromLeft (220));
    recordButton .setBounds (row1.removeFromRight (104).reduced (0, 2));
    row1.removeFromRight (6);
    deviceButton .setBounds (row1.removeFromRight (118).reduced (0, 2));
    row1.removeFromRight (6);
    addChannelButton.setBounds (row1.removeFromRight (70).reduced (0, 2));
    row1.removeFromRight (6);
    lockButton   .setBounds (row1.removeFromRight (76).reduced (0, 2));
    statusLabel  .setBounds (row1);

    // FMT and PRE buttons are kept alive (still wired) but not laid out.
    formatButton .setBounds ({});
    preRollButton.setBounds ({});

    // Row 2 — Transport bar (taller, wider) | transport label | session label | BACKUP / PATCH / METERS / OSC
    auto row2 = r.removeFromTop (52).reduced (12, 4);

    // Six-button transport bar — bigger, since the FILE button moved to
    // the macOS menu bar.
    if (transportBar != nullptr)
        transportBar->setBounds (row2.removeFromLeft (340).reduced (0, 2));
    row2.removeFromLeft (16);

    // The old FILE TextButton is no longer shown; the File menu lives in
    // the macOS system menu bar. Keep loadButton out of the layout.
    loadButton.setBounds ({});

    // PATCH + MIX / EDIT view toggle are the quick-access in-app buttons.
    patchButton    .setBounds (row2.removeFromRight (90).reduced (0, 2));
    row2.removeFromRight (8);
    editViewButton .setBounds (row2.removeFromRight (60).reduced (0, 2));
    row2.removeFromRight (4);
    mixViewButton  .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (10);
    transportLabel.setBounds (row2.removeFromLeft (140));
    sessionLabel  .setBounds (row2);

    // Hidden duplicates of menu-bar items.
    backupButton .setBounds ({});
    metersButton .setBounds ({});
    oscButton    .setBounds ({});
    playButton   .setBounds ({});
    stopButton   .setBounds ({});

    // Big clock banner with a phase meter docked on its right edge
    auto clockRow = r.removeFromTop (96).reduced (12, 6);
    if (phaseMeter != nullptr)
        phaseMeter->setBounds (clockRow.removeFromRight (200).reduced (4, 4));
    bigClock.setBounds (clockRow);

    // Timeline strip
    if (timeline != nullptr)
        timeline->setBounds (r.removeFromTop (52).reduced (12, 4));

    // Strips area — width adapts so 12 strips always fit on one page,
    // matching the Live app convention. Beyond 12 strips, the viewport
    // scrolls horizontally and the strip width stays at the same 12-fit
    // value so panning reveals additional banks without changing scale.
    constexpr int kStripsPerPage = 12;
    constexpr int kMinStripW     = 90;     // floor so 12 stays usable
    constexpr int kMaxStripW     = 160;
    const int margin = 12;
    const int gap    = 6;
    const int total  = (int) strips.size();

    auto viewportArea = r.reduced (margin);
    stripsViewport.setBounds (viewportArea);
    if (editPage != nullptr)
        editPage->setBounds (viewportArea);

    const int pageW  = viewportArea.getWidth();
    const int stripW = juce::jlimit (kMinStripW, kMaxStripW,
                                     (pageW - (kStripsPerPage - 1) * gap) / kStripsPerPage);

    const int containerW = total > 0
                            ? total * stripW + (total - 1) * gap
                            : pageW;
    stripsContainer.setSize (juce::jmax (containerW, pageW),
                              viewportArea.getHeight());

    int x = 0;
    for (int i = 0; i < total; ++i)
    {
        strips[(std::size_t) i]->setBounds (x, 0, stripW,
                                            stripsContainer.getHeight());
        x += stripW + gap;
    }
}
