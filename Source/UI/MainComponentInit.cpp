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
#include "../Audio/MidiClockOut.h"
#include "../Audio/NoiseAnalyzer.h"
#include "NoiseReportDialog.h"
#include "SessionRecoveryDialog.h"
#include "MarkerListDialog.h"
#include "ClickSettingsDialog.h"
#include "../Audio/SpectralClassifier.h"
#include "SessionPropertiesDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

MainComponent::MainComponent()
{
    setLookAndFeel (&laf);

    // Restore the persisted clip-nudge step (cycled with N).
    if (auto* props = engine.getAppProps())
        nudgeMs = juce::jlimit (1, 5000, props->getIntValue ("editNudgeMs", nudgeMs));

    // Title text removed from the header -- the macOS window title +
    // app icon already identify the product, no need to repeat it.
    titleLabel.setFont (brand::type::headline());
    titleLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setVisible (false);

    statusLabel.setFont (brand::type::uiBody());
    statusLabel.setColour (juce::Label::textColourId, brand::textMuted);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    midiStatusLabel.setColour (juce::Label::textColourId, brand::featureEngaged);
    midiStatusLabel.setJustificationType (juce::Justification::centredLeft);
    midiStatusLabel.setFont (brand::type::caption());
    addAndMakeVisible (midiStatusLabel);

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

    sessionLabel.setFont (brand::type::caption());
    sessionLabel.setColour (juce::Label::textColourId, brand::textMuted);
    sessionLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (sessionLabel);

    // Transport timecode -- mono so HH:MM:SS doesn't dance
    transportLabel.setFont (brand::type::mono (13.0f, true));
    transportLabel.setColour (juce::Label::textColourId, brand::textPrimary);
    transportLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (transportLabel);

    recordButton.setColour (juce::TextButton::buttonColourId, brand::accentRecord.darker (0.55f));
    recordButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord.brighter (0.10f));
    recordButton.onClick = [this] { onRecordClicked(); };
    // The transport-bar red-circle button now owns the RECORD action;
    // keep this button alive (status text updates still reference it)
    // but don't paint it in the header.
    recordButton.setVisible (false);

    playButton.setColour (juce::TextButton::buttonColourId, brand::accentPlay.withAlpha (brand::alpha::subtle));
    playButton.setColour (juce::TextButton::textColourOffId, brand::accentPlay);
    playButton.onClick = [this] { onPlayClicked(); };
    addAndMakeVisible (playButton);

    stopButton.onClick = [this] { onStopClicked(); };
    addAndMakeVisible (stopButton);

    loadButton.setColour (juce::TextButton::buttonColourId, brand::accentVS.withAlpha (brand::alpha::subtle));
    loadButton.setColour (juce::TextButton::textColourOffId, brand::accentVS);
    loadButton.onClick = [this] { onFileMenuClicked(); };
    addAndMakeVisible (loadButton);

    deviceButton.setColour (juce::TextButton::buttonColourId,    brand::bgElevated);
    deviceButton.setColour (juce::TextButton::textColourOffId,   brand::textPrimary);
    deviceButton.onClick = [this] { onDeviceClicked(); };
    addAndMakeVisible (deviceButton);

    formatButton.onClick = [this] { onFormatClicked(); };
    addAndMakeVisible (formatButton);

    preRollButton.onClick = [this] { onPreRollClicked(); };
    addAndMakeVisible (preRollButton);

    lockButton.setColour (juce::TextButton::buttonColourId, brand::accentRecord.darker (0.55f));
    lockButton.setColour (juce::TextButton::textColourOffId, brand::accentRecord.brighter (0.10f));
    lockButton.onClick = [this] { onLockToggled(); };
    addAndMakeVisible (lockButton);

    backupButton.setColour (juce::TextButton::buttonColourId,    brand::accentVS.darker (0.55f));
    backupButton.setColour (juce::TextButton::textColourOffId,   brand::accentVS.brighter (0.10f));
    backupButton.onClick = [this] { onBackupClicked(); };
    addAndMakeVisible (backupButton);

    patchButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus.darker (0.55f));
    patchButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus.brighter (0.10f));
    patchButton.onClick = [this]
    {
        if (auto* w = patchDialog.getComponent())
        {
            delete w;                  // second click closes the non-modal window
            patchDialog = nullptr;
            return;
        }
        patchDialog = zynforge::PatchPage::launch (engine);
    };

    // VSC chip uses the dedicated brand::signalVsc() colour so the
    // 'virtual soundcheck' role has a unique visual identity (was
    // sharing engagedAmber, now ties to accentVS).
    vscButton.setColour (juce::TextButton::buttonColourId,  brand::signalVsc().darker (0.55f));
    vscButton.setColour (juce::TextButton::textColourOffId, brand::signalVsc().brighter (0.10f));
    vscButton.setTooltip ("Virtual Soundcheck -- repatch every strip's OUTPUT to match its INPUT, "
                          "so playback feeds the desk via the same channels the live mics did.");
    vscButton.onClick = [this] { onVscClicked(); };
    addAndMakeVisible (vscButton);
    addAndMakeVisible (patchButton);

    auto styleViewBtn = [] (juce::TextButton& b, bool engaged)
    {
        b.setColour (juce::TextButton::buttonColourId,
                     engaged ? brand::accentStatus.darker (0.55f)
                             : brand::bgElevated);
        b.setColour (juce::TextButton::textColourOffId,
                     engaged ? brand::accentStatus.brighter (0.10f)
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

    // Restore the engineer's preferred width from prefs.
    if (auto* props = engine.getAppProps())
    {
        const auto saved = props->getValue ("stripWidthPreset", "M");
        if      (saved == "XS") stripWidthPreset = StripWidth::XS;
        else if (saved == "S")  stripWidthPreset = StripWidth::S;
        else if (saved == "L")  stripWidthPreset = StripWidth::L;
        else                    stripWidthPreset = StripWidth::M;
    }

    addChannelButton.setColour (juce::TextButton::buttonColourId, brand::accentStatus.darker (0.55f));
    addChannelButton.setColour (juce::TextButton::textColourOffId, brand::accentStatus.brighter (0.10f));
    addChannelButton.setTooltip ("Set the number of recording channels -- opens a prompt where you type the count (1-256).");
    addChannelButton.onClick = [this]
    {
        if (engine.isRecording())
        {
            showStatus ("Stop recording before changing channel count");
            return;
        }

        zynforge::AddTracksDialog::launch (
            [this] (const std::vector<zynforge::AddTracksDialog::Entry>& entries)
        {
            if (entries.empty()) return;

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
                    const auto defaultName = e.isBus ? juce::String ("Bus ") + juce::String (cursor + 1)
                                                      : juce::String (cursor + 1);
                    const auto base  = e.baseName.isNotEmpty() ? e.baseName : defaultName;
                    if (e.stereo && cursor + 1 < target)
                    {
                        const auto label = suffix > 0 ? base + " " + juce::String (suffix) : base;
                        engine.setTrackName  (cursor,     label);
                        engine.setTrackName  (cursor + 1, label + " R");
                        engine.setTrackStereo (cursor,     true);
                        engine.setTrackStereo (cursor + 1, false);
                        engine.setTrackIsBus  (cursor,     e.isBus);
                        engine.setTrackIsBus  (cursor + 1, e.isBus);
                        // Hard-pan the new pair L/R so it reproduces its full
                        // stereo image from the start (buses excepted -- a
                        // stereo bus sums centred). Matches import.
                        if (! e.isBus)
                        {
                            engine.setTrackPan (cursor,     -1.0f);
                            engine.setTrackPan (cursor + 1,  1.0f);
                        }
                        cursor += 2;
                        ++totalStereo;
                    }
                    else
                    {
                        engine.setTrackName  (cursor, suffix > 0 ? base + " " + juce::String (suffix)
                                                                  : base);
                        engine.setTrackStereo (cursor, false);
                        engine.setTrackIsBus  (cursor, e.isBus);
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

    metersButton.setColour (juce::TextButton::buttonColourId,    brand::featureEngaged.darker (0.55f));
    metersButton.setColour (juce::TextButton::textColourOffId,   brand::featureEngaged.brighter (0.10f));
    metersButton.onClick = [this]
    {
        if (auto* w = metersDialog.getComponent())
        {
            delete w;                 // second click closes the non-modal window
            metersDialog = nullptr;
            return;
        }
        metersDialog = zynforge::Meterbridge::launch (engine);
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

    oscButton.onClick = [this]
    {
        // Quick menu: pick a console dialect and start listening on 8000.
        juce::PopupMenu menu;
        menu.addSectionHeader (engine.isOscListening()
                                ? "OSC listening on port " + juce::String (engine.getOscPort())
                                : "OSC idle");
        menu.addSeparator();
        menu.addItem (1, "Listen -- Generic /zynforge");
        menu.addItem (2, "Listen -- DiGiCo");
        menu.addItem (3, "Listen -- Allen & Heath (SQ / Avantis)");
        menu.addItem (4, "Listen -- SSL Live");
        menu.addItem (5, "Listen -- Yamaha (DM7 / RIVAGE)");
        menu.addSeparator();
        menu.addItem (10, "Stop", engine.isOscListening());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&oscButton),
                            [this] (int chosen)
        {
            if (chosen <= 0) return;
            if (chosen == 10) { engine.stopOsc(); oscButton.setButtonText ("OSC"); showStatus ("OSC stopped"); return; }
            const int dialect = chosen - 1;
            const int port = oscListenPort();
            if (engine.startOsc (port, dialect))
            {
                oscButton.setButtonText ("OSC *");
                showStatus ("OSC listening on " + juce::String (port) + " (" +
                            juce::StringArray ({"Generic","DiGiCo","A&H","SSL","Yamaha"})[dialect] + ")");
            }
            else showStatus ("OSC failed to bind port " + juce::String (port));
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
    backupButton .setTooltip ("Pick a second drive -- every track is mirrored there as you record.");
    patchButton  .setTooltip ("Open the patch matrix: route hardware inputs / outputs to channel strips.");
    titleLabel   .setTooltip ("Zynforge Recording -- JUCE 8 multitrack recorder + virtual soundcheck.");

    refreshFormatButton();
    refreshPreRollButton();

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
    setlistBar.onRenameCue  = [this] { renameCurrentCue(); };
    setlistBar.onMoveCue    = [this] (int dir)
    {
        if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size()) return;
        const int target = currentCueIndex + dir;
        if (target < 0 || target >= (int) cues.size()) return;
        std::swap (cues[(size_t) currentCueIndex], cues[(size_t) target]);
        currentCueIndex = target;
        setlistBar.setCues (cues, currentCueIndex);
        saveSetlistToActiveSession();
        showStatus ("Cue moved");
    };
    setlistBar.onDeleteCue  = [this]
    {
        if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size()) return;
        const auto gone = cues[(size_t) currentCueIndex].name;
        cues.erase (cues.begin() + currentCueIndex);
        currentCueIndex = juce::jmin (currentCueIndex, (int) cues.size() - 1);
        setlistBar.setCues (cues, currentCueIndex);
        saveSetlistToActiveSession();
        showStatus ("Deleted cue '" + gone + "'");
    };
    addAndMakeVisible (setlistBar);

    // EDIT page owns the automation toolbar (mirrors getEditToolsBar).
    // Create the page here so the toolbar exists before we wire its
    // callbacks, then re-parent the toolbar onto MainComponent for the
    // top-row layout. The page's other wiring stays below.
    editPage = std::make_unique<zynforge::EditPage> (engine);
    addChildComponent (*editPage);          // hidden by default; switchView toggles
    automationToolbar = editPage->getAutomationToolbar();
    addAndMakeVisible (*automationToolbar);

    // Automation toolbar -- visible only when the EDIT view is active.
    // Tool / parameter changes are exposed to the EDIT rows via the
    // engine + a simple poll (TrackRow::resized + paint pick the
    // current laneMode from the toolbar's param choice).
    automationToolbar->setVisible (false);
    automationToolbar->onToolChanged  = [this] (zynforge::AutomationToolbar::Tool)
        { if (editPage != nullptr) editPage->repaint(); };
    automationToolbar->onParamChanged = [this] (zynforge::AutomationToolbar::Param)
    {
        if (editPage != nullptr)
        {
            // Toolbar param choice drives every row's lane content --
            // engineer doesn't have to flip the per-row VIEW menu first.
            editPage->applyToolbarParamToAllRows();
            editPage->repaint();
        }
    };
    automationToolbar->onClearAll = [this]
    {
        showStatus ("Automation clear is recognised -- point storage lands in the next pass");
    };

    // editPage doesn't exist yet here -- the real wiring happens below
    // after `editPage = std::make_unique<EditPage>(...)`.

    // onClearAll wipes the active parameter across every track.
    automationToolbar->onClearAll = [this]
    {
        const auto p = automationToolbar->getParam();
        const auto engineParam =
            p == zynforge::AutomationToolbar::Param::Volume ? zynforge::AudioEngine::AutomationParam::Volume
          : p == zynforge::AutomationToolbar::Param::Pan    ? zynforge::AudioEngine::AutomationParam::Pan
                                                            : zynforge::AudioEngine::AutomationParam::Mute;
        // Wrap in an undoable transaction so a stray click can be
        // recovered with Cmd+Z. AutomationSnapshotAction captures
        // before/after, undo restores the full automation table.
        runAutomationEdit ("Clear automation lane",
                           [this, engineParam] { engine.clearAutomation (engineParam); });
        showStatus ("Cleared automation points for the active parameter (Cmd+Z to undo)");
    };

    // WRITE-mode dropdown -- forward to engine's write-mode state.
    // While the mode is anything other than Off AND playback is
    // rolling, every fader / pan / mute move drops a new automation
    // point at the current playhead (thinned to ~50 ms resolution by
    // writeAutomationPointThinned in writeAutoIfPlaying below).
    automationToolbar->onWriteModeChanged = [this] (zynforge::AutomationToolbar::WriteMode wm)
    {
        using EM = zynforge::AudioEngine::AutomationWriteMode;
        engine.setAutomationWriteMode ((EM) (int) wm);
        const char* label = "Off";
        switch (wm)
        {
            case zynforge::AutomationToolbar::WriteMode::Off:   label = "Off";   break;
            case zynforge::AutomationToolbar::WriteMode::Touch: label = "Touch"; break;
            case zynforge::AutomationToolbar::WriteMode::Latch: label = "Latch"; break;
            case zynforge::AutomationToolbar::WriteMode::Write: label = "Write"; break;
        }
        showStatus (juce::String ("Automation WRITE: ") + label);
    };

    automationToolbar->onTrimModeChanged = [this] (bool trimOn)
    {
        engine.setAutomationTrimMode (trimOn);
        showStatus (trimOn ? "Automation TRIM armed (fader moves nudge the per-track trim, not the lane shape)"
                           : "Automation TRIM off");
    };

    // SUSPEND -- engine ignores every lane at read time. Useful for
    // auditioning a raw fader pass without overwriting automation.
    automationToolbar->onSuspendChanged = [this] (bool on)
    {
        engine.setAutomationReadSuspended (on);
        showStatus (on ? "Automation SUSPEND on -- playback ignores every lane (raw faders only)"
                       : "Automation SUSPEND off -- playback follows stored automation again");
    };

    // PUNCH -- writes only fire inside the engine's punch range
    // (the EDIT page's selection on the time ruler, when one
    // exists). Outside the range, writes are no-ops.
    automationToolbar->onPunchChanged = [this] (bool on)
    {
        engine.setAutomationPunchEnabled (on);
        if (on)
        {
            const auto in  = engine.getAutomationPunchIn();
            const auto out = engine.getAutomationPunchOut();
            if (in < 0 || out <= in)
                showStatus ("PUNCH on -- no range set yet; drag a selection on the EDIT timeline to define one");
            else
                showStatus ("PUNCH on -- automation writes will only fire inside the selected range");
        }
        else
        {
            showStatus ("PUNCH off -- automation writes fire anywhere on the timeline");
        }
    };

    tempoBar.setBpm (engine.getSessionTempoBpm());
    tempoBar.onBpmChanged = [this] (float bpm)
    {
        engine.setSessionTempoBpm (bpm);
        tempoBar.setBpm (engine.getSessionTempoBpm());   // clamp echo

        // If a click track has already been created in this session,
        // regenerate it so the metronome stays locked to the new tempo.
        if (clickTrackIndex >= 0)
            generateOrRefreshClickTrack();
    };
    tempoBar.onCreateClickTrack = [this]
    {
        // CLICK button now opens the Click II dialog. The real-time
        // click engine runs in the audio thread so tempo / voice /
        // subdivision changes take effect on the next beat without
        // ever stopping playback.
        auto& cl = engine.getClickEngine();
        zynforge::ClickSettings init;
        init.on              = cl.isEnabled();
        init.click1.volumeDb = 0.0f;     // engine doesn't currently round-trip these values back
        init.click1.voice    = zynforge::ClickSettings::Voice::Click;
        init.click1.sub      = zynforge::ClickSettings::Subdivision::Quarter;
        init.click2.volumeDb = -3.0f;
        init.click2.voice    = zynforge::ClickSettings::Voice::Click;
        init.click2.sub      = zynforge::ClickSettings::Subdivision::Quarter;

        zynforge::ClickSettingsDialog::launch (init, engine.getSessionTempoBpm(),
            [this] (const zynforge::ClickSettings& s)
            {
                auto& c = engine.getClickEngine();
                c.setEnabled    (s.on);
                c.setVolume1Db  (s.click1.volumeDb);
                c.setVolume2Db  (s.click2.volumeDb);
                c.setVoice1     ((zynforge::ClickEngine::Voice) s.click1.voice);
                c.setVoice2     ((zynforge::ClickEngine::Voice) s.click2.voice);
                c.setSub1       ((zynforge::ClickEngine::Subdivision) s.click1.sub);
                c.setSub2       ((zynforge::ClickEngine::Subdivision) s.click2.sub);
            },
            [this]
            {
                // 'Generate click track' button -- render an audio file
                // of the click for offline workflows (mixdown / export).
                generateOrRefreshClickTrack();
            });
    };
    addAndMakeVisible (tempoBar);


    // TimelineStrip exists for marker bookkeeping but is no longer
    // painted in either MIX or EDIT -- the empty "Load a session..."
    // band added visual noise without working state. Keep the
    // instance for marker APIs that reference it; just don't show it.
    timeline = std::make_unique<zynforge::TimelineStrip> (engine);

    transportBar = std::make_unique<zynforge::TransportBar> (engine);
    transportBar->onRequestPlay   = [this] { onPlayClicked();   };
    transportBar->onRequestStop   = [this] { onStopClicked();   };
    transportBar->onRequestRecord = [this] { onRecordClicked(); };
    addAndMakeVisible (*transportBar);

    stripsViewport.setViewedComponent (&stripsContainer, false);
    stripsViewport.setScrollBarsShown (false, true);     // h-scroll only
    addAndMakeVisible (stripsViewport);
    addChildComponent (mixerPlaceholder);   // empty-state overlay (manages own visibility)

    // editPage was created above (it owns the automation toolbar). The
    // rest of its wiring lives here.
    // Forward the undo wrapper so EditPage TrackRows can push
    // per-point Add / Delete edits through the same undo stack the
    // Clear All button uses. Goes through setAutomationEditWrapper
    // so it propagates into TrackList + every TrackRow.
    editPage->setAutomationEditWrapper (
        [this] (const juce::String& label, std::function<void()> fn)
        {
            runAutomationEdit (label, std::move (fn));
        });
    editPage->automationDragBegin = [this] { beginAutomationTransaction(); };
    editPage->automationDragEnd   = [this] (const juce::String& label)
                                    { endAutomationTransaction (label); };
    // Autosave per-session zoom into .zfproj on every change.
    editPage->onZoomChanged = [this] (float) { saveUILayoutToActiveSession(); };

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
    // share the same 28 px row as the automation toolbar. (The earlier
    // if-block tried this too but ran before editPage existed.)
    if (auto* tb = editPage->getEditToolsBar())
        addAndMakeVisible (*tb);
    switchView (View::Mix);

    masterStrip = std::make_unique<zynforge::MasterStrip> (engine);
    addAndMakeVisible (*masterStrip);

    vcaPanel = std::make_unique<zynforge::VcaPanel> (engine);
    addChildComponent (*vcaPanel);   // hidden by default; VCA button toggles

    vcaToggleButton.setColour (juce::TextButton::buttonColourId,
                                brand::featureEngaged.darker (0.55f));
    vcaToggleButton.setColour (juce::TextButton::textColourOffId,
                                brand::featureEngaged.brighter (0.10f));
    vcaToggleButton.setTooltip ("Toggle the 8-bus VCA fader panel -- group faders for "
                                 "drums / vocals / etc. Strips opt in via right-click > Assign to VCA.");
    vcaToggleButton.onClick = [this]
    {
        showVcaPanel = ! showVcaPanel;
        if (vcaPanel != nullptr) vcaPanel->setVisible (showVcaPanel);
        resized();
        saveUILayoutToActiveSession();
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
    juce::Timer::callAfterDelay (50, [this] { loadUILayoutFromActiveSession(); });

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
                                 [this] { scanForCrashReports(); });

    // Capture daemon (Phase 1d/2): restore the flag and reattach -- a take
    // left rolling by a crashed/quit GUI is adopted, not orphaned.
    if (engine.getAppProps() != nullptr
        && engine.getAppProps()->getBoolValue ("useCaptureDaemon", false))
    {
        useCaptureDaemon = true;
        juce::Timer::callAfterDelay (brand::motion::launchDelayMs + 800,
                                     [this] { connectCaptureDaemon (true); });
    }

    juce::Timer::callAfterDelay (brand::motion::launchDelayMs, [this, firstRun]
    {
        offerSessionRecovery();
        juce::Timer::callAfterDelay (450, [this, firstRun]
        {
            if (firstRun)
            {
                showFirstRunTutorial();
                if (auto* p = engine.getAppProps())
                {
                    p->setValue ("tutorialShown", true);
                    p->saveIfNeeded();
                }
                juce::Timer::callAfterDelay (350, [this] { showStartupWelcome(); });
            }
            else
            {
                showStartupWelcome();
            }
        });
    });

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

void MainComponent::setStripWidthPreset (StripWidth p)
{
    if (p == stripWidthPreset) return;
    stripWidthPreset = p;
    if (auto* props = engine.getAppProps())
    {
        const auto code = (p == StripWidth::XS) ? "XS"
                        : (p == StripWidth::S)  ? "S"
                        : (p == StripWidth::L)  ? "L" : "M";
        props->setValue ("stripWidthPreset", code);
        props->saveIfNeeded();
    }
    // Reflect the active preset in the button toggle visuals + relayout
    // the strip viewport so the engineer sees the new density right away.
    stripXsButton.setToggleState (p == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (p == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (p == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (p == StripWidth::L,  juce::dontSendNotification);
    resized();
    saveUILayoutToActiveSession();
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
    saveUILayoutToActiveSession();

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

    // Automation toolbar is part of the EDIT-view chrome.
    automationToolbar->setVisible (! mix);
    if (editPage != nullptr)
        if (auto* tb = editPage->getEditToolsBar())
            tb->setVisible (! mix);
    updateMixerPlaceholder();   // only shows in MIX view + when empty

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

    // Single layout pass after every visibility / colour change above
    // (was two -- the redundant re-flow is gone).
    resized();
}

void MainComponent::rebuildStrips()
{
    const juce::ScopedValueSetter<bool> guard (rebuildingStrips, true);
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
            // If the engineer has multiple strips selected (Shift /
            // Cmd-click), apply the colour to every one of them so
            // 'select 8 strips, set colour' works as expected. The
            // single-strip path still works because mouseDown selects
            // this strip first; selectedLogical will contain at least
            // this one when the colour picker opens via right-click.
            if (selectedLogical.size() > 1)
            {
                for (int logical : selectedLogical)
                {
                    if (logical < 0 || logical >= (int) strips.size()) continue;
                    const auto& s = strips[(size_t) logical];
                    const int phys = s->getStripIndex();
                    engine.setTrackColour (phys, chosen);
                    if (s->isStereo()) engine.setTrackColour (phys + 1, chosen);
                }
            }
            else
            {
                engine.setTrackColour (i, chosen);
                if (step == 2) engine.setTrackColour (i + 1, chosen);
            }
        };
        auto nameCb   = [this, i] (juce::String chosen) { engine.setTrackName   (i, chosen); };
        // Helper: routes fader / pan moves into one of three sinks
        // depending on toolbar state:
        //   WRITE  - drop a new automation point at the playhead
        //   TRIM   - nudge the per-track trim atomic by (new - prev)
        //   neither- direct only (engine.setTrack* already ran)
        auto writeAutoIfPlaying = [this] (int trackIdx,
                                          zynforge::AudioEngine::AutomationParam p,
                                          float value)
        {
            if (engine.isAutomationTrimming())
            {
                // Trim: store the absolute fader value as the trim
                // delta from the lane value at the playhead. Result:
                // moving the fader to -3 dB while the lane reads 0
                // sets trim = -3, riding the automation down 3 dB.
                const auto pos       = engine.getPlayer().getPositionSamples();
                const float laneVal  = engine.getAutomation (trackIdx, p).empty()
                                       ? 0.0f
                                       : engine.automationValueAt (trackIdx, p, pos, 0.0f);
                engine.setAutomationTrim (trackIdx, p, value - laneVal);
                return;
            }
            if (! engine.isAutomationWriting()) return;
            const auto pos = engine.getPlayer().getPositionSamples();
            // Punch gate -- when PUNCH is on, only write inside the
            // engine's punch range. Outside the range, do nothing.
            if (engine.isAutomationPunchEnabled()
                && ! engine.isInsideAutomationPunchRange (pos))
                return;
            // Throttle WRITE-mode drops to ~50 ms apart so a 60 Hz
            // fader.onValueChange stream doesn't blow up the lane.
            const auto sr = (juce::int64) juce::jmax (1.0, engine.getPlayer().getSampleRate());
            const auto minGap = sr / 20;        // 50 ms
            engine.writeAutomationPointThinned (trackIdx, p, pos, value, minGap);
        };

        // Pro Tools-style Edit Group broadcast. When strip `i` is
        // in an edit group, the gain / pan change is mirrored onto
        // every other strip in the same group. Returns the list of
        // peers (excluding self) so each callback can decide
        // whether to also write automation for them.
        auto editGroupPeers = [this] (int src) -> std::vector<int>
        {
            const int g = engine.getTrackEditGroup (src);
            if (g < 0) return {};
            auto all = engine.getStripsInEditGroup (g);
            all.erase (std::remove (all.begin(), all.end(), src), all.end());
            return all;
        };

        auto gainCb   = [this, i, step, writeAutoIfPlaying, editGroupPeers] (float dB)
        {
            engine.setTrackGainDb (i, dB);
            writeAutoIfPlaying (i, zynforge::AudioEngine::AutomationParam::Volume, dB);
            if (step == 2)
            {
                engine.setTrackGainDb (i + 1, dB);
                writeAutoIfPlaying (i + 1, zynforge::AudioEngine::AutomationParam::Volume, dB);
            }
            for (int peer : editGroupPeers (i))
            {
                engine.setTrackGainDb (peer, dB);
                writeAutoIfPlaying (peer, zynforge::AudioEngine::AutomationParam::Volume, dB);
            }
        };
        // L pan persists to track i. For a stereo strip, the R pan
        // travels through its own panRCb (below) -- the two sides are
        // INDEPENDENT, not mirrored, so the engineer can pan the L
        // channel hard-left and R hard-right (or whatever they want).
        auto panCb  = [this, i, writeAutoIfPlaying, editGroupPeers] (float pan)
        {
            engine.setTrackPan (i, pan);
            writeAutoIfPlaying (i, zynforge::AudioEngine::AutomationParam::Pan, pan);
            for (int peer : editGroupPeers (i))
            {
                engine.setTrackPan (peer, pan);
                writeAutoIfPlaying (peer, zynforge::AudioEngine::AutomationParam::Pan, pan);
            }
        };
        auto panRCb = [this, i, writeAutoIfPlaying] (float pan)
        {
            engine.setTrackPan (i + 1, pan);
            writeAutoIfPlaying (i + 1, zynforge::AudioEngine::AutomationParam::Pan, pan);
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

        // Read-mode display follow: while playback rolls, the MIXER fader /
        // pan show the AUTOMATED value at the playhead so they track the EDIT
        // automation curve (the views stay linked). Stopped -> manual value.
        // Volume automation lives on the L track of a stereo pair, so the
        // stereo strip fader reads track i; each pan knob reads its own
        // channel's lane.
        s->displayGainDb = [this, i] (float manualDb) -> float
        {
            if (! engine.isPlaying()) return manualDb;
            return engine.automationValueAt (i, zynforge::AudioEngine::AutomationParam::Volume,
                                             engine.getPlayer().getPositionSamples(), manualDb);
        };
        s->displayPanL = [this, i] (float manualPan) -> float
        {
            if (! engine.isPlaying()) return manualPan;
            return engine.automationValueAt (i, zynforge::AudioEngine::AutomationParam::Pan,
                                             engine.getPlayer().getPositionSamples(), manualPan);
        };
        if (step == 2)
            s->displayPanR = [this, i] (float manualPan) -> float
            {
                if (! engine.isPlaying()) return manualPan;
                return engine.automationValueAt (i + 1, zynforge::AudioEngine::AutomationParam::Pan,
                                                 engine.getPlayer().getPositionSamples(), manualPan);
            };

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
            // Newly linked stereo -> hard-pan L/R for the full image; unlinked
            // back to mono -> recentre both halves so a mono strip isn't left
            // stuck hard-right. (Buses sum centred, so skip them.)
            if (! engine.getRecorder().getTrack (i).isBus.load())
            {
                engine.setTrackPan (i,     ! wasStereo ? -1.0f : 0.0f);
                engine.setTrackPan (i + 1, ! wasStereo ?  1.0f : 0.0f);
            }
            // Trigger a full rebuild on the next timer tick.
            lastTrackCount = -1;
        };
        auto linkOtherCb  = [this, i] (int other) { juce::ignoreUnused (i, other); };
        s->setMenuCallbacks (std::move (deleteCb),
                             std::move (addCb),
                             std::move (linkStereoCb),
                             std::move (linkOtherCb));

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

        // Aux send wiring -- feed the strip the live bus list so its
        // 'Send to bus' submenu populates, and persist whatever the
        // engineer picks for send slot 0. Unity post-fader default.
        s->getBusList = [this]() -> std::vector<std::pair<int, juce::String>>
        {
            std::vector<std::pair<int, juce::String>> out;
            for (int t = 0; t < engine.getRecorder().getNumTracks(); ++t)
            {
                auto& ts = engine.getRecorder().getTrack (t);
                if (ts.isBus.load (std::memory_order_relaxed))
                    out.emplace_back (t, ts.name.isEmpty() ? juce::String ("Bus ") + juce::String (t + 1)
                                                              : ts.name);
            }
            return out;
        };
        s->onSendTargetChanged = [this, i] (int target)
        {
            engine.setTrackSend (i, 0, target, 0.0f /* dB */, true /* post */);
        };

        // Per-track Automation Safe lock. When on, the engine
        // refuses every write to this track (Add, Remove, Paste,
        // WRITE-mode drops, TRIM offsets). The strip mirrors via
        // setAutomationLed in the slow poll below.
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
        s->onAfterArmedToggle    = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::armed);   };
        s->onAfterMonitorToggle  = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::monitor); };
        s->onAfterMuteToggle     = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::muted);   };
        s->onAfterSoloToggle     = [this, broadcastToggle] (int src) { broadcastToggle (src, &zynforge::TrackState::soloed);  };

        s->onAutomationSafeChanged = [this, i] (bool safeOn)
        {
            engine.setTrackAutomationSafe (i, safeOn);
            if (safeOn) showStatus (juce::String ("Track ") + juce::String (i + 1)
                                    + ": Automation Safe -- writes blocked");
            else        showStatus (juce::String ("Track ") + juce::String (i + 1)
                                    + ": Automation Safe off");
        };

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
        stripsContainer.addAndMakeVisible (*s);
        strips.push_back (std::move (s));

        i += step;
    }
    lastTrackCount = n;
    lastTrackGen   = recorder.getTrackGeneration();
    resized();
    updateMixerPlaceholder();
}
