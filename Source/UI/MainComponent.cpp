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
#include "SessionSettingsDialog.h"
#include "SessionProjPath.h"

using namespace zynforge;

MainComponent::MainComponent()
{
    setLookAndFeel (&laf);

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
            w->closeButtonPressed();   // second click closes the open dialog
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
            w->closeButtonPressed();
            metersDialog = nullptr;
            return;
        }
        metersDialog = zynforge::Meterbridge::launch (engine);
    };
    metersButton.setTooltip ("Open the floating meterbridge -- drag onto a second display.");
    addAndMakeVisible (metersButton);

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
    backupButton .setTooltip ("Pick a second drive -- every track is mirrored there as you record.");
    patchButton  .setTooltip ("Open the patch matrix: route hardware inputs / outputs to channel strips.");
    titleLabel   .setTooltip ("Zynforge Recording -- JUCE 8 multitrack recorder + virtual soundcheck.");

    refreshFormatButton();
    refreshPreRollButton();

    addAndMakeVisible (bigClock);
    addAndMakeVisible (perfDashboard);
    addAndMakeVisible (toast);

    peakTally = std::make_unique<zynforge::PeakTally> (engine);
    addAndMakeVisible (*peakTally);

    // Setlist + cue bar. Wires the three engineer actions back into
    // helpers that read/write the session's .zfproj.
    setlistBar.onPick       = [this] (int idx) { jumpToCue (idx); };
    setlistBar.onPrev       = [this] { jumpToCue (juce::jmax (0, currentCueIndex - 1)); };
    setlistBar.onNext       = [this] { jumpToCue (juce::jmin ((int) cues.size() - 1,
                                                              currentCueIndex + 1)); };
    setlistBar.onAddCue     = [this] { addCueAtTransport(); };
    setlistBar.onUpdateCue  = [this] { updateCueAtTransport(); };
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

    // Automation toolbar -- visible only when the EDIT view is active.
    // Tool / parameter changes are exposed to the EDIT rows via the
    // engine + a simple poll (TrackRow::resized + paint pick the
    // current laneMode from the toolbar's param choice).
    automationToolbar.setVisible (false);
    automationToolbar.onToolChanged  = [this] (zynforge::AutomationToolbar::Tool)
        { if (editPage != nullptr) editPage->repaint(); };
    automationToolbar.onParamChanged = [this] (zynforge::AutomationToolbar::Param)
    {
        if (editPage != nullptr)
        {
            // Toolbar param choice drives every row's lane content --
            // engineer doesn't have to flip the per-row VIEW menu first.
            editPage->applyToolbarParamToAllRows();
            editPage->repaint();
        }
    };
    automationToolbar.onClearAll = [this]
    {
        showStatus ("Automation clear is recognised -- point storage lands in the next pass");
    };
    addAndMakeVisible (automationToolbar);

    // editPage doesn't exist yet here -- the real wiring happens below
    // after `editPage = std::make_unique<EditPage>(...)`.

    // onClearAll wipes the active parameter across every track.
    automationToolbar.onClearAll = [this]
    {
        const auto p = automationToolbar.getParam();
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
    automationToolbar.onWriteModeChanged = [this] (zynforge::AutomationToolbar::WriteMode wm)
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

    automationToolbar.onTrimModeChanged = [this] (bool trimOn)
    {
        engine.setAutomationTrimMode (trimOn);
        showStatus (trimOn ? "Automation TRIM armed (fader moves nudge the per-track trim, not the lane shape)"
                           : "Automation TRIM off");
    };

    // SUSPEND -- engine ignores every lane at read time. Useful for
    // auditioning a raw fader pass without overwriting automation.
    automationToolbar.onSuspendChanged = [this] (bool on)
    {
        engine.setAutomationReadSuspended (on);
        showStatus (on ? "Automation SUSPEND on -- playback ignores every lane (raw faders only)"
                       : "Automation SUSPEND off -- playback follows stored automation again");
    };

    // PUNCH -- writes only fire inside the engine's punch range
    // (the EDIT page's selection on the time ruler, when one
    // exists). Outside the range, writes are no-ops.
    automationToolbar.onPunchChanged = [this] (bool on)
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

    editPage = std::make_unique<zynforge::EditPage> (engine);
    addChildComponent (*editPage);   // hidden by default; switchView toggles
    // Wire the toolbar AFTER the page exists. The handlers above
    // captured 'this' so they will still see editPage when they fire.
    editPage->setAutomationToolbar (&automationToolbar);
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
                                 "drums / vocals / etc. Strips opt in via right-click ▸ Assign to VCA.");
    vcaToggleButton.onClick = [this]
    {
        showVcaPanel = ! showVcaPanel;
        if (vcaPanel != nullptr) vcaPanel->setVisible (showVcaPanel);
        resized();
    };
    addAndMakeVisible (vcaToggleButton);

    setWantsKeyboardFocus (true);
    addKeyListener (this);

    startTimerHz (10);  // poll for input-channel count + transport position
    rebuildStrips();
    updateTransportLabels();

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

    // Automation toolbar is part of the EDIT-view chrome.
    automationToolbar.setVisible (! mix);
    if (editPage != nullptr)
        if (auto* tb = editPage->getEditToolsBar())
            tb->setVisible (! mix);
    resized();   // re-flow the layout for the new state

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

        // VCA assignment from the strip's right-click menu -- the
        // ChannelStrip sets state.vcaGroup directly for immediate
        // audio-thread effect; this callback persists the choice
        // through appProps so a relaunch restores it.
        s->onVcaGroupChanged = [this, i] (int vcaIdx)
        {
            engine.setTrackVcaGroup (i, vcaIdx);
        };

        s->onEditGroupChanged = [this, i] (int groupId)
        {
            engine.setTrackEditGroup (i, groupId);
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

        s->setAvailableInputs  (numIns);
        s->setAvailableOutputs (numOuts);
        stripsContainer.addAndMakeVisible (*s);
        strips.push_back (std::move (s));

        i += step;
    }
    lastTrackCount = n;
    resized();
}

void MainComponent::onRecordClicked()
{
    if (engine.isRecording())
    {
        engine.stopRecording();
        statusLabel.setText ("Idle", juce::dontSendNotification);
        recordButton.setButtonText ("RECORD");
        return;
    }

    // Pre-flight: surface why recording wouldn't capture anything BEFORE
    // we open writers + flip the engine into recording state.
    auto& recorder = engine.getRecorder();
    const int numTracks = recorder.getNumTracks();
    if (numTracks == 0)
    {
        statusLabel.setText ("Add a channel with +CH before recording.", juce::dontSendNotification);
        return;
    }

    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    if (device == nullptr)
    {
        statusLabel.setText ("No audio device open -- pick one with DEVICE.", juce::dontSendNotification);
        return;
    }
    const int numInputs = device->getActiveInputChannels().countNumberOfSetBits();
    if (numInputs == 0)
    {
        statusLabel.setText ("Device has 0 active inputs -- open DEVICE and enable inputs.",
                             juce::dontSendNotification);
        return;
    }

    // Count armed tracks AND auto-fix any strip whose inputRouting points
    // outside the device's actual input count -- fall back to a sequential
    // wrap so engineer doesn't have to open PATCH to start recording.
    int armed = 0;
    int autoRouted = 0;
    for (int i = 0; i < numTracks; ++i)
    {
        auto& t = recorder.getTrack (i);
        if (t.armed.load (std::memory_order_relaxed)) ++armed;
        const int dev = t.inputRouting.load (std::memory_order_relaxed);
        if (dev < 0 || dev >= numInputs)
        {
            const int target = numInputs > 0 ? (i % numInputs) : 0;
            engine.setTrackInputRouting (i, target);
            ++autoRouted;
        }
    }
    if (armed == 0)
    {
        statusLabel.setText ("Arm at least one track (R button) before recording.",
                             juce::dontSendNotification);
        return;
    }

    const auto dir = makeNewSessionDir();

    // Disk-space pre-flight -- abort if the drive can't hold at least
    // 30 minutes at current SR × armed-track count × bit depth (assume
    // 24-bit if unknown). Surface the actual free GB so the engineer
    // knows what to clear.
    {
        const double sr = device->getCurrentSampleRate();
        const int    bytesPerSample = 3;   // 24-bit ≈ worst-case PCM
        const juce::int64 bytesPerSec    = (juce::int64) (sr * armed * bytesPerSample);
        const juce::int64 wantBytes      = bytesPerSec * 60 * 30;  // 30 min headroom
        const juce::int64 freeBytes      = dir.getParentDirectory().getBytesFreeOnVolume();
        if (freeBytes > 0 && freeBytes < wantBytes)
        {
            const double freeGB = freeBytes / 1.0e9;
            statusLabel.setText ("Drive has only "
                                 + juce::String (freeGB, 1)
                                 + " GB free -- < 30 min headroom. Clear space before recording.",
                                 juce::dontSendNotification);
            return;
        }
    }

    if (engine.startRecording (dir))
    {
        auto msg = juce::String ("Recording ") + juce::String (armed) + "/"
                 + juce::String (numTracks) + " tracks → " + dir.getFileName();
        if (autoRouted > 0)
            msg << " (auto-routed " << autoRouted << ")";
        statusLabel.setText (msg, juce::dontSendNotification);
        recordButton.setButtonText ("STOP");
    }
    else
    {
        statusLabel.setText ("Failed to start recording -- could not open writer files.",
                             juce::dontSendNotification);
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
        // Pro Tools-style: if the engineer has placed the edit cursor,
        // playback starts there instead of from wherever the player
        // happened to be paused. Without a cursor (-1) the player
        // resumes from its current position as before.
        const auto cursor = engine.getEditCursorSample();
        if (cursor >= 0)
            player.setPositionSamples (cursor);

        engine.startPlayback();
        playButton.setButtonText ("PAUSE");
        statusLabel.setText ("Playing → " + player.getSessionName(), juce::dontSendNotification);
    }
}

void MainComponent::onStopClicked()
{
    // STOP-while-recording is a take-killing action. A fat-fingered
    // keystroke or stray touch must not end a live recording. First
    // tap arms (flash + toast); a second tap within 2 s fires for
    // real. Any other state -- playback, idle -- stops immediately
    // because there's nothing irreversible to protect.
    auto& player = engine.getPlayer();

    if (engine.isRecording())
    {
        const auto now = juce::Time::getMillisecondCounter();
        constexpr juce::uint32 kArmWindowMs = 2000;

        if (stopArmedAtMs == 0 || (now - stopArmedAtMs) > kArmWindowMs)
        {
            // First tap -- arm. Don't actually stop yet.
            stopArmedAtMs = now;
            toast.show ("Tap STOP again to end the recording", Toast::Kind::Warning);
            statusLabel.setText ("STOP armed -- tap again within 2 s to stop recording",
                                 juce::dontSendNotification);
            return;
        }
        // Second tap within window -- disarm and fall through to stop.
        stopArmedAtMs = 0;
    }
    else
    {
        stopArmedAtMs = 0;
    }

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
    showStatus (sessionLocked ? "LOCKED -- click UNLOCK to resume control"
                              : engine.isRecording()  ? "Recording"
                              : engine.isPlaying()    ? "Playing"
                                                      : "Idle");
}

void MainComponent::onVscClicked()
{
    // Virtual Soundcheck: copy each strip's input routing to its output
    // routing, so playback feeds the same hardware channel the live
    // input would have used. The desk sees the recorded tracks on the
    // same snake / network channels it'd see the live mics on.
    auto& rec = engine.getRecorder();
    const int n = rec.getNumTracks();
    int repatched = 0;
    for (int i = 0; i < n; ++i)
    {
        const int inDev = rec.getTrack (i).inputRouting.load (std::memory_order_relaxed);
        // -2 means 'identity default' -- resolve to i so the output ends
        // up explicitly set to the strip's identity input.
        const int target = (inDev == -2) ? i : inDev;
        engine.setTrackOutputRouting (i, target);
        ++repatched;
    }
    showStatus (repatched > 0
                ? "Virtual Soundcheck -- " + juce::String (repatched)
                  + " strip(s) repatched: outputs now match inputs"
                : juce::String ("No strips to repatch"));
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

void MainComponent::showFirstRunTutorial()
{
    // Sequential walkthrough -- each AlertWindow chains the next via
    // its modal callback. Plain dialogs (not arrow callouts) so the
    // tutorial keeps working even if the layout shifts later.
    struct Step { juce::String title; juce::String body; };
    static const std::vector<Step> steps = {
        { "Welcome to Zynforge Recording",
          "Live multitrack recording + virtual soundcheck.\n\n"
          "This 5-step tour shows you how to capture your first session. "
          "You can replay this any time from Help > Quick Start." },
        { "Step 1 of 5 -- Add channels",
          "Click the green + CH button in the top-right header to add channels. "
          "Pick a count (e.g. 8 for a basic drum kit) and tick Stereo for pairs. "
          "Channels appear in the mixer left-to-right." },
        { "Step 2 of 5 -- Arm tracks",
          "On each channel strip, click the red R button to ARM it for recording. "
          "Click the green I button to also monitor (hear) it through your outputs.\n\n"
          "Tip: Shift-click multiple strips to select them, then bulk-arm / delete / "
          "colour them via Edit > Selection. Esc clears the selection." },
        { "Step 3 of 5 -- Record + play back",
          "Press the red record button in the transport bar (centre-bottom of "
          "the header) to start recording. Press it again to stop.\n\n"
          "When you stop, the session auto-loads for playback. Press play "
          "(green triangle) or just SPACEBAR to hear what you captured." },
        { "Step 4 of 5 -- Cue list for shows",
          "For playback shows, build a setlist with the cue bar at the top. "
          "Each cue snapshots fader / pan / routing--recall via cue "
          "buttons OR number keys 1-""9. Soft-takeover ramps prevent clicks." },
        { "Step 5 of 5 -- Help is always one menu away",
          "Help > Keyboard Shortcuts shows every shortcut.\n"
          "Help > Quick Start replays this tour.\n\n"
          "You're ready. Press OK to start your first session." }
    };

    juce::Component::SafePointer<MainComponent> self (this);
    std::shared_ptr<std::function<void (int)>> runner = std::make_shared<std::function<void (int)>>();
    *runner = [self, runner] (int idx)
    {
        if (self == nullptr || idx >= (int) steps.size()) return;
        const auto& s = steps[(size_t) idx];
        auto* aw = new juce::AlertWindow (s.title, s.body, juce::MessageBoxIconType::NoIcon);
        const bool last = (idx + 1 >= (int) steps.size());
        aw->addButton (last ? "OK" : "Next", 1, juce::KeyPress (juce::KeyPress::returnKey));
        if (! last) aw->addButton ("Skip tour", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create (
            [aw, runner, idx, last] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result == 1 && ! last && runner) (*runner) (idx + 1);
        }));
    };
    (*runner) (0);
}

void MainComponent::showKeyboardShortcuts()
{
    const juce::String body =
        "TRANSPORT\n"
        "    Space        Play / pause (stops recording if armed)\n"
        "    M             Drop marker at playhead\n"
        "\n"
        "CUES\n"
        "    1 - 9         Jump to cue 1 - 9\n"
        "\n"
        "EDIT\n"
        "    A             Solo selected strips\n"
        "    S             Split clip at playhead\n"
        "    ,              Set range start at playhead\n"
        "    .              Set range end at playhead\n"
        "    4             Toggle snap mode\n"
        "    Cmd + Z         Undo last clip edit\n"
        "    Cmd + R         Redo\n"
        "    Cmd + X         Cut selected strips\n"
        "    Cmd + C         Copy selected strips\n"
        "    Cmd + V         Paste strips\n"
        "\n"
        "MIXER\n"
        "    Shift + click strip\n"
        "                    Toggle multi-selection (additive)\n"
        "    Cmd + A         Select every strip\n"
        "    Esc           Clear strip selection\n"
        "    Right-click strip\n"
        "                    Rename / Colour / Stereo / Stream send / VCA assign / Output-mute\n"
        "\n"
        "EDIT VIEW\n"
        "    Right-click row\n"
        "                    Row size + Take swap (comp playlists)\n"
        "    Right-click clip\n"
        "                    Mute / Lock / Duplicate / Delete / Gain / Fade in & out\n"
        "    Drag swatch (left edge)\n"
        "                    Reorder strip (paused only)";

    auto* aw = new juce::AlertWindow ("Keyboard shortcuts",
                                      body,
                                      juce::MessageBoxIconType::NoIcon);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw] (int) { std::unique_ptr<juce::AlertWindow> dispose (aw); }));
}

void MainComponent::showUserGuide()
{
    const juce::String body =
        "MIXER VIEW\n"
        "  Channel strips run left-to-right; each has R / I / M / S "
        "buttons, a pan knob, and a fader. Right-click for rename, "
        "colour, stereo link, stream send, VCA assign, output mute.\n\n"
        "EDIT VIEW\n"
        "  Waveform of each Track_NN.wav. Edit tools palette top-right: "
        "Smart / Selector / Trim / Grabber / Fade / Scrubber. "
        "Right-click a row for size + take swap; right-click a clip for "
        "mute / lock / duplicate / delete / gain / fades.\n\n"
        "PATCH PAGE (Session menu)\n"
        "  INPUT / OUTPUT tabs -- rows are hardware channels, columns are "
        "your strips. Click a dot to route; drag diagonally for "
        "incremental patching.\n\n"
        "CUE LIST (top bar)\n"
        "  + Cue captures current fader / pan / routing / mute / arm + "
        "playback position + tempo. Recall via cue buttons OR number "
        "keys 1-""9. 250 ms soft-takeover prevents clicks.\n\n"
        "VCA PANEL (toggle with the VCA button)\n"
        "  8 group faders. Assign strips via right-click > Assign to VCA. "
        "VCA fader sums into each assigned strip's gain. VCA mute / solo "
        "follows console (solo-in-place) convention.\n\n"
        "TAKE SWAP (EDIT row right-click)\n"
        "  Each track holds N named takes (clip lists). Capture the "
        "current state as a new take mid-comp; switch between takes "
        "without losing edits.\n\n"
        "DEVICE / FORMAT / BACKUP\n"
        "  DEVICE opens the audio interface picker. FILE > New Session "
        "configures format + sample rate. BACKUP picks a second drive "
        "where every track mirrors as you record.\n\n"
        "MIDI CLOCK\n"
        "  Session > MIDI clock out picks an output. Tempo drives "
        "24 PPQN; play / pause / stop send midi-start / continue / stop.\n\n"
        "NOISE ANALYSIS\n"
        "  Session > Analyse for noise scans every WAV for "
        "50 / 60 Hz hum, mic bumps, and high noise floor.\n\n"
        "DANTE\n"
        "  Native Dante requires Audinate's paid SDK + NDA. Practical "
        "path: install Audinate's free Dante Virtual Soundcard (DVS) "
        "from audinate.com. DVS exposes up to 64\xC3\x97""64 Dante channels "
        "as a Core Audio device--just pick it in DEVICE and route "
        "strips to its channels via PATCH. ZynForge auto-detects DVS "
        "and shows a 'DANTE' badge in the status bar.\n\n"
        "NETWORK AUDIO (NDI)\n"
        "  Session > NDI broadcast pushes the master mix onto your "
        "LAN as an NDI Audio source. Any NDI receiver (NDI Tools, OBS, "
        "TouchDesigner) on the same network can monitor your live "
        "stream. Requires NDI runtime from ndi.tv--free.";

    auto* aw = new juce::AlertWindow ("Zynforge user guide", body,
                                      juce::MessageBoxIconType::NoIcon);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw] (int) { std::unique_ptr<juce::AlertWindow> dispose (aw); }));
}

void MainComponent::showAboutDialog()
{
    juce::AlertWindow::showMessageBoxAsync (
        juce::MessageBoxIconType::NoIcon,
        "Zynforge Recording",
        "Live multitrack recording + virtual soundcheck.\n\n"
        "JUCE 8 * C++20 * macOS Universal\n"
        "\xC2\xA9 Zynforge",
        "OK");
}

void MainComponent::showStartupWelcome()
{
    // Pro Tools-style two-pane welcome: side rail with New / Open,
    // right pane with the matching form. ZynForge palette throughout
    // -- no native OS chrome inside the dialog body.
    const auto defaultRoot = getSessionsRoot();
    const auto curFormat   = engine.getRecorder().getCaptureFormat();
    const double curSr     = pendingSampleRate;

    juce::Component::SafePointer<MainComponent> self (this);

    auto onCreate = [self] (const zynforge::WelcomeDialog::NewResult& r)
    {
        if (self == nullptr) return;
        // The WelcomeDialog::NewResult mirrors NewSessionDialog::Result
        // 1:1 in field types, so reuse the same builder by hand-rolling
        // a NewSessionDialog::Result and forwarding.
        zynforge::NewSessionDialog::Result n;
        n.name          = r.name;
        n.location      = r.location;
        n.captureFormat = r.captureFormat;
        n.sampleRate    = r.sampleRate;
        n.interleaved   = r.interleaved;
        n.ioSettings    = r.ioSettings;
        const auto sessionFolder = self->createSessionFolderStructure (n);

        if (auto* p = self->engine.getAppProps())
        {
            p->setValue ("sessionsRoot", r.location.getFullPathName());
            p->saveIfNeeded();
        }
        self->engine.getRecorder().setCaptureFormat (r.captureFormat);
        self->engine.setActiveSessionDir (sessionFolder);

        // New session = clean slate. Wipe any per-strip persistence
        // from a previous show AND reset the strip count to zero so
        // the engineer dials in the channels they want via +CH /
        // clicking the empty EDIT pane. Matches the legacy
        // launchNewSessionDialog path.
        self->engine.clearAllStripOverrides();
        self->engine.setStripCount (0);
        self->lastTrackCount = -1;

        self->refreshFormatButton();
        self->showStatus ("Session created: " + sessionFolder.getFileName()
                          + " -- add channels with +CH");
    };

    auto onOpen = [self] (const juce::File& sessionDir)
    {
        if (self == nullptr) return;
        const int n = self->engine.loadSession (sessionDir);
        self->showStatus (n > 0
                          ? "Loaded: " + sessionDir.getFileName()
                          : "Failed to load " + sessionDir.getFileName());
        if (n > 0) self->warnIfSampleRateMismatch();
    };

    zynforge::WelcomeDialog::launch (defaultRoot, curSr, curFormat,
                                     std::move (onCreate),
                                     std::move (onOpen));
}

void MainComponent::launchNewSessionDialog()
{
    const auto defaultRoot = getSessionsRoot();
    const auto curFormat   = engine.getRecorder().getCaptureFormat();
    const double curSr     = pendingSampleRate;

    juce::Component::SafePointer<MainComponent> self (this);
    zynforge::NewSessionDialog::launch (defaultRoot, curSr, curFormat,
        [self] (const zynforge::NewSessionDialog::Result& r)
    {
        if (self == nullptr) return;

        // Build the named session folder right away (Pro Tools-style):
        //   <Local Storage>/<Name>/
        //       <Name>.zfproj
        //       Audio Files/
        //       Bounced Files/
        //       Session File Backups/
        //       WaveCache.wfm
        // Subsequent record / save / export operations all live inside
        // this folder.
        const auto sessionFolder = self->createSessionFolderStructure (r);

        // Persist the new sessions root (used by makeNewSessionDir +
        // every future file dialog) and remember the chosen format
        // so the dB readout / capture matches.
        if (auto* props = self->engine.getAppProps())
        {
            props->setValue ("sessionsRoot",      r.location.getFullPathName());
            props->setValue ("sessionName",       r.name);
            props->setValue ("interleavedFlag",   r.interleaved);
            props->setValue ("ioPreset",          r.ioSettings);
            props->saveIfNeeded();
        }

        // Pin this folder as the active session so Save / Save As light
        // up immediately (engine.getActiveSessionDir() now reports it
        // even before any recording or playback has started).
        self->engine.setActiveSessionDir (sessionFolder);
        self->loadSetlistFromActiveSession();
        self->loadUILayoutFromActiveSession();

        self->engine.getRecorder().setCaptureFormat (r.captureFormat);
        self->pendingSampleRate = r.sampleRate;
        self->refreshFormatButton();

        // Apply the chosen sample rate to the device immediately.
        auto setup = self->engine.getDeviceManager().getAudioDeviceSetup();
        if (! juce::approximatelyEqual (setup.sampleRate, r.sampleRate))
        {
            setup.sampleRate = r.sampleRate;
            self->engine.getDeviceManager().setAudioDeviceSetup (setup, true);
        }

        // Start clean: clear every per-strip persisted override and
        // reset the strip count so the engineer dials in their own
        // channels with +CH.
        self->engine.resetAllStripState();
        self->engine.setStripCount (0);
        self->lastTrackCount = -1;

        self->showStatus ("New session '" + r.name + "' -- add channels with +CH and arm REC to capture");
    });
}

void MainComponent::offerSessionRecovery()
{
    const auto incomplete = zynforge::AudioEngine::findIncompleteSessions (getSessionsRoot());
    if (incomplete.isEmpty()) return;

    // Proper modal dialog (DialogChrome-styled) with a sortable
    // table + Recover / Delete / Skip actions. Replaces the popup
    // menu that engineers tended to miss when it fired behind the
    // welcome dialog or while they were already mid-click on
    // something else.
    juce::Component::SafePointer<MainComponent> self (this);
    SessionRecoveryDialog::launch (incomplete,
        [self] (const juce::File& dir)
        {
            if (self == nullptr) return;
            self->engine.loadSession (dir);
            self->showStatus ("Recovered: " + dir.getFileName());
            self->updateTransportLabels();
            self->lastTrackCount = -1;
        });
}

void MainComponent::onFileMenuClicked()
{
    const auto activeDir   = engine.getActiveSessionDir();
    const bool hasActive   = activeDir.isDirectory();

    juce::PopupMenu menu;
    menu.addItem (1, "Open Session...");
    menu.addItem (4, "Import Audio Files...",   ! engine.isRecording());
    menu.addSeparator();
    menu.addItem (2, "Save Session State",      hasActive);
    menu.addItem (3, "Save Session As...",   hasActive);
    menu.addSeparator();

    juce::PopupMenu exportMenu;
    exportMenu.addItem (10, "Export All Tracks...", hasActive);

    juce::PopupMenu individualMenu;
    const int trackCount = engine.getRecorder().getNumTracks();
    for (int i = 0; i < trackCount; ++i)
    {
        const auto& t   = engine.getRecorder().getTrack (i);
        const auto name = t.name.isNotEmpty() ? t.name
                                              : juce::String (i + 1);
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
    // Also surface the message as a non-modal toast so the engineer
    // catches feedback even when their eyes are on the strips, not
    // the footer. Pick the Kind from the message tone -- anything
    // containing "Stop", "can't", "fail" reads as a warning;
    // everything else as info.
    if (msg.isNotEmpty())
    {
        const auto kind = (msg.containsIgnoreCase ("can't")
                           || msg.containsIgnoreCase ("fail")
                           || msg.containsIgnoreCase ("stop record"))
                              ? Toast::Kind::Warning
                              : Toast::Kind::Info;
        toast.show (msg, kind);
    }
}


void MainComponent::generateOrRefreshClickTrack()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before generating a click");
        return;
    }

    auto audioFiles = sessionDir.getChildFile ("Audio Files");
    audioFiles.createDirectory();

    auto& recorder = engine.getRecorder();
    if (clickTrackIndex < 0)
    {
        // First press in this session -- append a fresh track at the end.
        clickTrackIndex = recorder.getNumTracks();
        engine.setStripCount (clickTrackIndex + 1);
        engine.setTrackName  (clickTrackIndex, "Click");
    }

    const double sr      = juce::jmax (8000.0, [this]
    {
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
            return d->getCurrentSampleRate();
        return 48000.0;
    }());
    // Match the session's playback length, falling back to 4 hours when
    // the session is empty so the engineer always has more than enough
    // click for a show.
    auto& player = engine.getPlayer();
    juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
    if (totalSamples <= 0)
        // 1-hour fallback when there's nothing loaded yet -- plenty for
        // any single show, 4× faster to render than the old 4-hour cap.
        totalSamples = (juce::int64) (sr * 60.0 * 60.0);

    // Render the offline click using the same per-voice tone presets
    // + subdivision factors the real-time engine uses, so the file
    // matches what the engineer was hearing live before they hit
    // Generate.
    auto& cl = engine.getClickEngine();
    const auto preset1 = ClickEngine::getVoicePreset (cl.getVoice1());
    const auto preset2 = ClickEngine::getVoicePreset (cl.getVoice2());
    const auto sub1    = cl.getSub1();
    const auto sub2    = cl.getSub2();
    const double f1    = ClickEngine::subFactor (sub1);
    const double f2    = ClickEngine::subFactor (sub2);
    const float lin1   = juce::Decibels::decibelsToGain (cl.getVol1Db());
    const float lin2   = juce::Decibels::decibelsToGain (cl.getVol2Db());

    const auto& tempoMap = engine.getTempoMap();
    auto bpmAtSample = [&] (juce::int64 sample) -> float
    {
        float bpm = engine.getSessionTempoBpm();
        for (const auto& tc : tempoMap)
        {
            if (tc.samplePos <= sample) bpm = tc.bpm;
            else break;
        }
        return bpm;
    };

    const auto trackName = juce::String::formatted ("Track_%02d", clickTrackIndex + 1);
    auto dest = audioFiles.getChildFile (trackName + ".wav");
    dest.deleteFile();

    if (auto* out = dest.createOutputStream().release())
    {
        juce::WavAudioFormat wav;
        juce::StringPairArray meta;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (out, sr, 1, 24, meta, 0));
        if (writer == nullptr) { delete out; showStatus ("Click write failed"); return; }

        // Larger render chunks → fewer disk writes (8× the old size).
        constexpr int kChunk = 32768;
        juce::AudioBuffer<float> buf (1, kChunk);

        // Per-voice burst envelope state. We render a damped sine for
        // each active burst, decaying over ~50 ms.
        struct Burst { double phase=0, tSec=0, freq=1000, decay=60, gain=0; bool active=false; };
        Burst v1Burst, v2Burst;

        // Beat-scheduling counters, identical to ClickEngine's runtime.
        double samplesUntil1 = 0.0;
        double samplesUntil2 = 0.0;
        int    beat1Counter  = 0;
        int    beat2Counter  = 0;

        // Hoist tempo / per-click sample counts out of the sample loop.
        // bpmAtSample() does a tempoMap scan; running it once per sample
        // at 60M+ samples was the main reason Generate felt slow. We
        // recompute when the map says the tempo changes (rare) and
        // otherwise leave the cached values alone.
        const bool tempoIsConstant = tempoMap.empty();
        float  cachedBpm           = engine.getSessionTempoBpm();
        double samplesPerQuarter   = 60.0 * sr / juce::jmax (20.0f, cachedBpm);
        double samplesPerClick1    = (f1 > 0.0) ? (samplesPerQuarter / f1) : 0.0;
        double samplesPerClick2    = (f2 > 0.0) ? (samplesPerQuarter / f2) : 0.0;

        juce::int64 written = 0;
        while (written < totalSamples)
        {
            const int thisChunk = (int) juce::jmin ((juce::int64) kChunk, totalSamples - written);
            buf.clear();
            auto* dst = buf.getWritePointer (0);

            // If the session has tempo changes, refresh the cached
            // per-click sample counts once per chunk (samples per block,
            // not per sample). For a typical session with no tempo map
            // this branch never executes.
            if (! tempoIsConstant)
            {
                const float bpm = bpmAtSample (written);
                if (bpm != cachedBpm)
                {
                    cachedBpm        = bpm;
                    samplesPerQuarter = 60.0 * sr / juce::jmax (20.0f, cachedBpm);
                    samplesPerClick1  = (f1 > 0.0) ? (samplesPerQuarter / f1) : 0.0;
                    samplesPerClick2  = (f2 > 0.0) ? (samplesPerQuarter / f2) : 0.0;
                }
            }

            for (int i = 0; i < thisChunk; ++i)
            {

                if (samplesPerClick1 > 0.0)
                {
                    samplesUntil1 -= 1.0;
                    if (samplesUntil1 <= 0.0)
                    {
                        samplesUntil1 += samplesPerClick1;
                        if ((beat1Counter % 4) == 0)
                        {
                            v1Burst = { 0.0, 0.0, preset1.freq, preset1.decay, lin1, true };
                        }
                        ++beat1Counter;
                    }
                }
                if (samplesPerClick2 > 0.0)
                {
                    samplesUntil2 -= 1.0;
                    if (samplesUntil2 <= 0.0)
                    {
                        samplesUntil2 += samplesPerClick2;
                        const bool downAligned =
                            (sub2 == ClickEngine::Subdivision::Quarter) && (beat2Counter % 4) == 0;
                        if (! downAligned)
                            v2Burst = { 0.0, 0.0, preset2.freq, preset2.decay, lin2, true };
                        ++beat2Counter;
                    }
                }

                auto renderBurst = [sr] (Burst& b) -> float
                {
                    if (! b.active) return 0.0f;
                    const double dt   = 1.0 / sr;
                    const double env  = std::exp (-b.tSec * b.decay);
                    const double samp = std::sin (b.phase) * env * b.gain;
                    b.phase += juce::MathConstants<double>::twoPi * b.freq * dt;
                    b.tSec  += dt;
                    if (env < 0.001) b.active = false;
                    return (float) samp;
                };

                dst[i] = renderBurst (v1Burst) + renderBurst (v2Burst);
            }

            writer->writeFromFloatArrays (buf.getArrayOfReadPointers(), 1, thisChunk);
            written += thisChunk;
        }
    }

    // Default the click track to hardware output 1 so it's audible
    // without further routing -- the strip's OUT combo still exposes
    // every available device output so the engineer can re-patch it
    // to a dedicated cue bus (headphones, drummer's IEM, etc.).
    if (auto* dev = engine.getDeviceManager().getCurrentAudioDevice())
    {
        const int outs = dev->getActiveOutputChannels().countNumberOfSetBits();
        const int outCh = juce::jlimit (0, juce::jmax (0, outs - 1), 0);
        engine.setTrackOutputRouting (clickTrackIndex, outCh);
    }
    engine.setTrackInputRouting (clickTrackIndex, -1);   // no input -- playback only

    engine.loadSession (sessionDir);
    lastTrackCount = -1;

    showStatus ("Click track generated at "
                + juce::String (engine.getSessionTempoBpm(), 1) + " BPM "
                + "(routable to any output via its strip)");

    // Light up the click-beat overlay on every other EDIT row so the
    // engineer can see the metronome pulse against each track's audio.
    if (editPage != nullptr)
        editPage->setClickTrackPresent (true, clickTrackIndex);
}

void MainComponent::togglePunchMode()
{
    const bool on = ! engine.isPunchModeOn();
    engine.setPunchModeOn (on);
    if (on)
    {
        // Default: punch-arm every currently-armed track. Engineer can
        // narrow by un-arming individuals via the track's right-click
        // menu later.
        auto& rec = engine.getRecorder();
        for (int i = 0; i < rec.getNumTracks(); ++i)
            engine.setTrackPunchArmed (i,
                rec.getTrack (i).armed.load (std::memory_order_relaxed));
        showStatus ("Punch mode ON -- set the loop region, then press PLAY");
    }
    else
    {
        if (engine.isRecording()) engine.stopRecording();
        showStatus ("Punch mode OFF");
    }
}

void MainComponent::servicePunch()
{
    auto& player = engine.getPlayer();
    if (! player.isLoaded()) return;
    const auto pos    = player.getPositionSamples();
    const auto inside = (pos >= player.getLoopStart() && pos < player.getLoopEnd());

    if (inside && ! wasInsidePunch && player.isPlaying())
    {
        // Crossed into the punch window -- drop into record on every
        // punch-armed track, leave the rest playing back as normal.
        if (! engine.isRecording())
        {
            // Save each strip's pre-punch arm state, then force-arm
            // only the punch-armed ones for the duration of the punch.
            auto& rec = engine.getRecorder();
            for (int i = 0; i < rec.getNumTracks(); ++i)
                rec.getTrack (i).armed.store (engine.isTrackPunchArmed (i),
                                              std::memory_order_relaxed);
            engine.startRecording (makeNewSessionDir());
        }
    }
    else if (! inside && wasInsidePunch && engine.isRecording())
    {
        // Crossed out -- stop recording cleanly.
        engine.stopRecording();
    }
    wasInsidePunch = inside;
}

void MainComponent::runNoiseAnalysis()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or record a session before analysing");
        return;
    }

    showStatus ("Analysing tracks for noise / hum / bumps...");

    // Snapshot track names so the worker doesn't touch engine state
    // from the background thread.
    juce::StringArray names;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        names.add (engine.getRecorder().getTrack (i).name);

    juce::Component::SafePointer<MainComponent> self (this);
    juce::Thread::launch ([self, sessionDir, names]
    {
        const auto findings = zynforge::NoiseAnalyzer::analyseSession (
            sessionDir,
            [&names] (int idx) -> juce::String
            {
                return (idx >= 0 && idx < names.size())
                    ? names[idx]
                    : juce::String ("Track ") + juce::String (idx + 1);
            });

        // Build a one-line summary + a popup with per-track detail.
        int humCount = 0, bumpCount = 0;
        for (const auto& f : findings)
        {
            if (f.humFundamentalHz > 0) ++humCount;
            if (f.bumpCount > 0) ++bumpCount;
        }
        juce::String summary = juce::String (findings.size()) + " track"
            + (findings.size() == 1 ? juce::String() : juce::String ("s"))
            + " analysed -- " + juce::String (humCount) + " with hum, "
            + juce::String (bumpCount) + " with mic bumps. Report saved to noise_report.json.";

        juce::MessageManager::callAsync ([self, summary, findings]
        {
            if (self == nullptr) return;
            self->showStatus (summary);
            // Sortable table replaces the text dump -- engineer clicks
            // column headers to sort by worst hum / most bumps /
            // highest noise floor.
            zynforge::NoiseReportDialog::launch (findings);
        });
    });
}

void MainComponent::writeSoundcheckReport()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session before writing a soundcheck report");
        return;
    }

    auto& rec = engine.getRecorder();
    const double sr = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
        ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
        : 48000.0;

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("generatedAt", juce::Time::getCurrentTime().toISO8601 (true));
    root->setProperty ("sampleRate",  sr);
    root->setProperty ("bpm",         engine.getSessionTempoBpm());

    juce::Array<juce::var> arr;
    for (int i = 0; i < rec.getNumTracks(); ++i)
    {
        const auto& t = rec.getTrack (i);
        const auto r  = SpectralClassifier::classify (t, sr);

        juce::DynamicObject::Ptr o (new juce::DynamicObject());
        o->setProperty ("track",        i + 1);
        o->setProperty ("name",         t.name);
        o->setProperty ("guess",        r.name);
        o->setProperty ("peak",         (double) t.peak.load());
        o->setProperty ("rms",          (double) t.rms .load());
        o->setProperty ("clipCount",    t.clipCount.load());
        o->setProperty ("bandSub",      (double) r.bands.sub);
        o->setProperty ("bandLow",      (double) r.bands.low);
        o->setProperty ("bandMid",      (double) r.bands.mid);
        o->setProperty ("bandHigh",     (double) r.bands.high);
        o->setProperty ("bandAir",      (double) r.bands.air);
        o->setProperty ("stereo",       t.isStereo.load());
        arr.add (juce::var (o.get()));
    }
    root->setProperty ("strips", juce::var (arr));

    const auto reportFile = sessionDir.getChildFile ("soundcheck.report.json");
    reportFile.replaceWithText (juce::JSON::toString (juce::var (root.get())));
    showStatus ("Soundcheck report → " + reportFile.getFileName());
}

void MainComponent::showSessionProperties()
{
    const auto sessionDir = engine.getActiveSessionDir();
    if (! sessionDir.isDirectory())
    {
        showStatus ("Open or create a session first");
        return;
    }

    // Find the .zfproj inside the active session folder. If there's
    // more than one (shouldn't happen), pick the first match.
    juce::File proj;
    for (auto& f : sessionDir.findChildFiles (juce::File::findFiles, false, "*.zfproj"))
    {
        proj = f;
        break;
    }
    if (! proj.existsAsFile())
        proj = sessionDir.getChildFile (sessionDir.getFileName() + ".zfproj");

    juce::var parsed;
    if (proj.existsAsFile())
        parsed = juce::JSON::parse (proj);
    juce::DynamicObject::Ptr obj;
    if (parsed.isObject())
        obj = parsed.getDynamicObject();
    else
        obj = new juce::DynamicObject();

    auto readString = [&] (const juce::Identifier& k, const juce::String& fallback = {}) -> juce::String
    {
        if (obj == nullptr) return fallback;
        const auto v = obj->getProperty (k);
        if (v.isString()) return v.toString();
        return fallback;
    };

    // Convert the persisted captureFormat int back to a readable label.
    auto formatLabel = [] (int code) -> juce::String
    {
        using F = CaptureFormat;
        switch ((F) code)
        {
            case F::Wav16:        return "WAV 16-bit";
            case F::Wav24:        return "WAV 24-bit";
            case F::Wav32Float:   return "WAV 32-bit float";
            case F::Aiff16:       return "AIFF 16-bit";
            case F::Aiff24:       return "AIFF 24-bit";
            case F::Aiff32Float:  return "AIFF 32-bit float";
            case F::Flac16:       return "FLAC 16-bit";
            case F::Flac24:       return "FLAC 24-bit";
        }
        return "--";
    };

    SessionPropertiesDialog::Fields fields;
    fields.name        = readString ("name", sessionDir.getFileName());
    fields.artist      = readString ("artist");
    fields.venue       = readString ("venue");
    fields.fohEngineer = readString ("fohEngineer");
    fields.date        = readString ("date",
                                     juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
    fields.notes       = readString ("notes");
    {
        const auto sr = obj ? (double) obj->getProperty ("sampleRate") : 0.0;
        fields.sampleRate = sr > 0.0
            ? juce::String (sr / 1000.0, 1) + " kHz"
            : juce::String ("--");
    }
    {
        const int fmt = obj ? (int) obj->getProperty ("captureFormat") : (int) CaptureFormat::Wav24;
        fields.captureFormat = formatLabel (fmt);
        fields.bitDepth = (fmt == (int) CaptureFormat::Wav16 || fmt == (int) CaptureFormat::Aiff16 || fmt == (int) CaptureFormat::Flac16) ? "16-bit"
                        : (fmt == (int) CaptureFormat::Wav32Float || fmt == (int) CaptureFormat::Aiff32Float)              ? "32-bit float"
                                                                                                                          : "24-bit";
    }

    SessionPropertiesDialog::launch (fields,
        [this, proj, sessionDir, obj] (const SessionPropertiesDialog::Fields& edited)
        {
            // Merge back into the existing JSON (preserves sampleRate /
            // captureFormat / createdAt that the dialog doesn't edit).
            juce::DynamicObject::Ptr merged = obj;
            if (merged == nullptr) merged = new juce::DynamicObject();
            merged->setProperty ("zynforgeSession", true);
            merged->setProperty ("name",            edited.name);
            merged->setProperty ("artist",          edited.artist);
            merged->setProperty ("venue",           edited.venue);
            merged->setProperty ("fohEngineer",     edited.fohEngineer);
            merged->setProperty ("date",            edited.date);
            merged->setProperty ("notes",           edited.notes);
            merged->setProperty ("updatedAt",
                                 juce::Time::getCurrentTime().toISO8601 (true));

            proj.replaceWithText (juce::JSON::toString (juce::var (merged.get())));
            showStatus ("Saved session properties -- " + sessionDir.getFileName());
        });
}

// ─── Multi-selection helpers ──────────────────────────────────────
//
// 'selectedLogical' is the set of logical strip indices currently
// selected via shift/cmd-click. For stereo pairs, both halves move /
// recolour / delete together because the strip list iterates logical
// rows, not raw track indices.

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
            .withIconType (juce::MessageBoxIconType::NoIcon)
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

    // Recording in progress is its own conversation -- two buttons,
    // primary "Stop & Quit" / Cancel. No Save question because the
    // engineer is mid-take and saving doesn't make sense yet.
    if (recording)
    {
        auto options = juce::MessageBoxOptions()
                         .withIconType (juce::MessageBoxIconType::NoIcon)
                         .withTitle ("Recording is still rolling")
                         .withMessage ("A recording is in progress.\n"
                                       "Stop the recording cleanly and quit?")
                         .withButton ("Stop & Quit")
                         .withButton ("Cancel")
                         .withAssociatedComponent (this);

        juce::AlertWindow::showAsync (options, [this] (int result)
        {
            if (result == 1) return;        // Cancel
            engine.stopRecording();
            if (auto* app = juce::JUCEApplication::getInstance())
                app->quit();
        });
        return;
    }

    // No active session -- nothing to save. Two buttons.
    if (! hasActiveSession)
    {
        auto options = juce::MessageBoxOptions()
                         .withIconType (juce::MessageBoxIconType::NoIcon)
                         .withTitle ("Quit Zynforge Recording?")
                         .withMessage ("No active session. Any unsaved app state will be lost.")
                         .withButton ("Quit")
                         .withButton ("Cancel")
                         .withAssociatedComponent (this);

        juce::AlertWindow::showAsync (options, [] (int result)
        {
            if (result == 1) return;        // Cancel
            if (auto* app = juce::JUCEApplication::getInstance())
                app->quit();
        });
        return;
    }

    // Pro Tools-style three-button save-on-quit dialog. Button order
    // matches the Pro Tools convention the engineer expects:
    //   [Don't Save]  [Cancel]  [Save]
    //
    // Using AlertWindow::addButton with explicit return values is
    // critical here. MessageBoxOptions' index-based return mapping
    // proved unreliable -- in earlier builds Cancel was triggering
    // 'Don't Save' behaviour and vice versa because juce::AlertWindow
    // doesn't number index-added buttons the way the docs suggest on
    // every platform. Explicit IDs leave no room for confusion.
    constexpr int kDontSave = 1;
    constexpr int kCancel   = 2;
    constexpr int kSave     = 3;

    auto* aw = new juce::AlertWindow ("Save changes to \"" + activeDir.getFileName()
                                       + "\" before closing?",
                                      "Your session state, take history, cues and "
                                      "markers will be written to the session folder.",
                                      juce::MessageBoxIconType::NoIcon,
                                      this);
    aw->addButton ("Don't Save", kDontSave);
    aw->addButton ("Cancel",     kCancel, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->addButton ("Save",       kSave,   juce::KeyPress (juce::KeyPress::returnKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw, activeDir] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);

            if (result == kCancel)
                return;                              // abort the quit, stay in app

            if (result == kSave)
                saveSessionStateTo (activeDir);      // silent write; no picker

            // kDontSave (or any unexpected value) -- fall through to quit.
            if (auto* app = juce::JUCEApplication::getInstance())
                app->quit();
        }),
        false);
}

void MainComponent::showPreflightChecklist()
{
    // Build a one-screen status report -- green checks for what's OK,
    // amber warnings for risks, red items for blockers. Engineer
    // reads this before downbeat to verify the rig.
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    const int numInputs  = device != nullptr
        ? device->getActiveInputChannels().countNumberOfSetBits()  : 0;
    const int numOutputs = device != nullptr
        ? device->getActiveOutputChannels().countNumberOfSetBits() : 0;
    const double sr      = device != nullptr ? device->getCurrentSampleRate()      : 0.0;
    const int blockSize  = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;

    const int total = engine.getRecorder().getNumTracks();
    int armed = 0;
    for (int i = 0; i < total; ++i)
        if (engine.getRecorder().getTrack (i).armed.load (std::memory_order_relaxed)) ++armed;

    const auto sess   = engine.getActiveSessionDir();
    const bool hasSes = sess.isDirectory();
    const auto root   = sess.getParentDirectory();
    const double freeGB = (root.exists() ? root.getBytesFreeOnVolume() : 0) / 1.0e9;
    const auto fmt    = engine.getRecorder().getCaptureFormat();
    const juce::String fmtStr =
          fmt == zynforge::CaptureFormat::Wav24      ? "WAV/24"
        : fmt == zynforge::CaptureFormat::Wav16      ? "WAV/16"
        : fmt == zynforge::CaptureFormat::Wav32Float ? "WAV/32f"
        : fmt == zynforge::CaptureFormat::Aiff24     ? "AIFF/24"
        : fmt == zynforge::CaptureFormat::Aiff16     ? "AIFF/16"
        : fmt == zynforge::CaptureFormat::Aiff32Float? "AIFF/32f"
        : fmt == zynforge::CaptureFormat::Flac24     ? "FLAC/24"
        : fmt == zynforge::CaptureFormat::Flac16     ? "FLAC/16"
                                                      : "?";

    // 30-minute headroom estimate at the active configuration.
    const juce::int64 bytesPerSec = (juce::int64) (sr * juce::jmax (1, armed) * 3);
    const double minHeadroom = bytesPerSec > 0 ? (freeGB * 1.0e9 / (double) bytesPerSec) / 60.0
                                                : 0.0;

    auto chk = [] (bool ok, bool warn = false) -> juce::String
    {
        return ok ? juce::String ("[OK]  ")    // ✓
              : warn ? juce::String ("[!]  ") // ⚠
                      : juce::String ("[X]  ");// ✗
    };

    juce::String body;
    body << chk (device != nullptr)
         << "Audio device: " << (device != nullptr ? device->getName() : juce::String ("(none)"))
         << "\n"
         << chk (sr > 0.0)
         << "Sample rate:  " << juce::String ((int) sr) << " Hz, buffer " << blockSize << " samples\n"
         << chk (numInputs > 0)
         << "Inputs:       " << numInputs << " active\n"
         << chk (numOutputs > 0)
         << "Outputs:      " << numOutputs << " active\n"
         << "\n"
         << chk (total > 0) << "Channel strips: " << total << "\n"
         << chk (armed > 0) << "Armed for record: " << armed << " / " << total << "\n"
         << chk (hasSes) << "Active session: "
                          << (hasSes ? sess.getFileName() : juce::String ("(none)")) << "\n"
         << chk (! cues.empty(), cues.empty())
                          << "Setlist cues: " << (int) cues.size() << "\n"
         << chk (engine.getRecorder().isBackupActive())
                          << "Backup writer: "
                          << (engine.getRecorder().isBackupActive() ? juce::String ("active")
                                                                     : juce::String ("not configured"))
                          << "\n"
         << "\n"
         << chk (freeGB > 5.0, freeGB > 1.0)
         << "Disk free:    " << juce::String (freeGB, 1) << " GB ("
         << juce::String (minHeadroom, 0) << " min headroom at " << fmtStr << ")\n"
         << chk (! engine.isRecording())
         << "Status:       " << (engine.isRecording() ? juce::String ("RECORDING ROLLING")
                                                       : engine.getPlayer().isPlaying()
                                                           ? juce::String ("Playing back")
                                                           : juce::String ("Idle"))
         << "\n";

    if (engine.getMidiClockOut().isEnabled())
        body << "[OK]  MIDI clock master: " << engine.getMidiClockOut().getOutputDeviceName() << "\n";

    if (device != nullptr)
    {
        const auto dn = device->getName();
        if (dn.containsIgnoreCase ("Dante") || dn.containsIgnoreCase ("DVS"))
            body << "[OK]  Dante network active (" << dn << ")\n";
    }

    auto* aw = new juce::AlertWindow ("Pre-flight checklist",
                                      body, juce::MessageBoxIconType::NoIcon);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw] (int) { std::unique_ptr<juce::AlertWindow> dispose (aw); }));
}

void MainComponent::onDeviceClicked()
{
    if (auto* w = deviceDialog.getComponent())
    {
        w->closeButtonPressed();
        deviceDialog = nullptr;
        return;
    }
    deviceDialog = zynforge::AudioDeviceDialog::launch (engine);
}


