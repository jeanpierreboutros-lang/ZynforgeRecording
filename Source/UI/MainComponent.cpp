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
    midiStatusLabel.setFont (brand::fonts::small());
    addAndMakeVisible (midiStatusLabel);

    nextCueLabel.setColour (juce::Label::textColourId, brand::accentVS);
    nextCueLabel.setJustificationType (juce::Justification::centredRight);
    nextCueLabel.setFont (brand::fonts::body());
    addAndMakeVisible (nextCueLabel);

    // DANTE pill -- shows when the active audio device is Dante Virtual
    // Soundcard so the engineer sees at a glance that they're on the
    // Dante network. Updated from the existing timer.
    danteLabel.setColour (juce::Label::textColourId, brand::featureEngaged);
    danteLabel.setJustificationType (juce::Justification::centredLeft);
    danteLabel.setFont (brand::fonts::small());
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

    juce::Timer::callAfterDelay (250, [this] { offerSessionRecovery(); });
    // If the previous run had a session pinned, rehydrate its setlist
    // AND its UI layout (view, strip width, VCA panel, EDIT zoom).
    loadSetlistFromActiveSession();
    juce::Timer::callAfterDelay (50, [this] { loadUILayoutFromActiveSession(); });

    // First-launch tutorial -- gated by appProps so it only fires once.
    // Engineers can replay it from Help ▸ Quick Start.
    const bool firstRun = engine.getAppProps() == nullptr
                        || ! engine.getAppProps()->getBoolValue ("tutorialShown", false);
    if (firstRun)
    {
        juce::Timer::callAfterDelay (500, [this]
        {
            showFirstRunTutorial();
            if (auto* p = engine.getAppProps())
            {
                p->setValue ("tutorialShown", true);
                p->saveIfNeeded();
            }
            // After the tutorial, also surface the Welcome dialog so
            // first-launch users immediately see the New / Open flow
            // rather than landing on an empty mixer.
            juce::Timer::callAfterDelay (350, [this] { showStartupWelcome(); });
        });
    }
    else
    {
        juce::Timer::callAfterDelay (350, [this] { showStartupWelcome(); });
    }

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
        const auto tList = listSessionTemplates();
        for (int i = 0; i < tList.size(); ++i)
            templates.addItem (200 + i, tList[i].getFileNameWithoutExtension());
        if (tList.isEmpty())
            templates.addItem (-1, "(no templates yet)", false);
        else
        {
            templates.addSeparator();
            templates.addItem (250, "Delete a template...");
        }
        menu.addSubMenu ("New Session from Template", templates, ! engine.isRecording());

        menu.addSeparator();
        menu.addItem (60, "Choose Backup Folder...", ! engine.isRecording());
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
        menu.addItem (250, "Session Settings...", ! engine.isRecording());
        menu.addItem (251, "Properties...",       engine.getActiveSessionDir().isDirectory());
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
            showStatus ("Companion server on http://localhost:9000 -- open it from any browser / iPad");
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

bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    // Space: universal play / pause toggle. Always wins so engineers
    // can hit space from anywhere in the app without thinking.
    // Escape clears the multi-strip selection so the engineer can
    // bail out of a half-built bulk action without clicking each
    // selected strip again.
    if (key == juce::KeyPress::escapeKey && ! selectedLogical.empty())
    {
        clearStripSelection();
        return true;
    }

    // Cmd+A -- select every strip. Skips when the system focus is on a
    // text editor (so name-rename Cmd+A still selects the text).
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0)
        && juce::Component::getCurrentlyFocusedComponent() != nullptr
        && dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) == nullptr)
    {
        selectAllStrips();
        return true;
    }

    if (key == juce::KeyPress::spaceKey)
    {
        // Universal play / stop / pause toggle. Priority:
        //   1. If recording → stop the recording
        //   2. Else if playing → pause playback
        //   3. Else if a session is loaded → start playback
        //   4. Otherwise let the engineer know nothing's loaded
        auto& player = engine.getPlayer();
        if (engine.isRecording())
        {
            onStopClicked();
        }
        else if (player.isPlaying())
        {
            engine.stopPlayback();
            playButton.setButtonText ("PLAY");
            showStatus ("Stopped");
        }
        else if (player.isLoaded())
        {
            engine.startPlayback();
            playButton.setButtonText ("PAUSE");
            showStatus ("Playing");
        }
        else
        {
            showStatus ("Load or record a session first -- nothing to play");
        }
        return true;
    }

    // Number keys 1..9 jump to cue 1..9 if a setlist is loaded -- turns
    // the cue list into a real performance tool. Shift+number stores a
    // new cue at the playhead (quick-build setlist).
    {
        const int code = key.getKeyCode();
        if (code >= '1' && code <= '9' && ! key.getModifiers().isAnyModifierKeyDown())
        {
            const int target = code - '1';
            if (target < (int) cues.size())
            {
                jumpToCue (target);
                return true;
            }
        }
    }

    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

    if (c == 'm')
    {
        // Pro Tools-style marker drop: place the marker immediately at
        // the current position, then pop a naming dialog. If the
        // engineer Cancels, the marker keeps its auto-name 'Marker N'
        // (matches Pro Tools Memory Locations).
        dropMarkerAndPromptName();
        return true;
    }

    // Pro Tools-style numeric jump: pressing 1..9 jumps to memory
    // location 1..9 by sample position. Skips when a text editor
    // has focus (engineer is typing) -- JUCE handles that elsewhere.
    if (c >= '1' && c <= '9' && ! key.getModifiers().isCommandDown())
    {
        const int targetIdx = (c - '1');
        const auto& list = engine.getMarkers().getAll();
        if (targetIdx < (int) list.size())
        {
            engine.getPlayer().setPositionSamples (list[(size_t) targetIdx].sampleOffset);
            showStatus ("Jumped to marker " + juce::String (targetIdx + 1)
                        + ": " + list[(size_t) targetIdx].name);
            return true;
        }
    }
    if (c == 'a') { editSoloSelection();    return true; }
    if (c == 's') { editSplitAtPlayhead();  return true; }
    if (c == ',') { editStartRange();       return true; }
    if (c == '.') { editFinishRange();      return true; }
    if (c == '4') { editToggleSnap();       return true; }
    // Cmd+Z / Cmd+R / Cmd+X / Cmd+C / Cmd+V keyboard shortcuts.
    if (key.getModifiers().isCommandDown())
    {
        if (c == 'z') { editUndo();          return true; }
        if (c == 'r') { editRedo();          return true; }
        if (c == 'x') { editCutSelected (true);  return true; }
        if (c == 'c') { editCutSelected (false); return true; }
        if (c == 'v') { editPasteSelected(); return true; }
    }
    return false;
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

        auto gainCb   = [this, i, step, writeAutoIfPlaying] (float dB)
        {
            engine.setTrackGainDb (i, dB);
            writeAutoIfPlaying (i, zynforge::AudioEngine::AutomationParam::Volume, dB);
            if (step == 2)
            {
                engine.setTrackGainDb (i + 1, dB);
                writeAutoIfPlaying (i + 1, zynforge::AudioEngine::AutomationParam::Volume, dB);
            }
        };
        // L pan persists to track i. For a stereo strip, the R pan
        // travels through its own panRCb (below) -- the two sides are
        // INDEPENDENT, not mirrored, so the engineer can pan the L
        // channel hard-left and R hard-right (or whatever they want).
        auto panCb  = [this, i, writeAutoIfPlaying] (float pan)
        {
            engine.setTrackPan (i, pan);
            writeAutoIfPlaying (i, zynforge::AudioEngine::AutomationParam::Pan, pan);
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

void MainComponent::timerCallback()
{
    const int n = engine.getRecorder().getNumTracks();
    if (n != lastTrackCount)
        rebuildStrips();

    // MIDI clock status pill -- visible reassurance that the engineer's
    // outboard sync is alive. Empty string when disabled hides it.
    {
        const auto& clock = engine.getMidiClockOut();
        const auto label = clock.isEnabled()
            ? juce::String ("MIDI * ") + clock.getOutputDeviceName()
            : juce::String();
        if (midiStatusLabel.getText() != label)
            midiStatusLabel.setText (label, juce::dontSendNotification);
    }

    // DANTE detection -- flag the active audio device when its name
    // contains 'Dante' / 'DVS' (Audinate's Dante Virtual Soundcard).
    {
        juce::String txt;
        if (auto* d = engine.getDeviceManager().getCurrentAudioDevice())
        {
            const auto n = d->getName();
            if (n.containsIgnoreCase ("Dante") || n.containsIgnoreCase ("DVS"))
                txt = "DANTE * " + n;
        }
        if (danteLabel.getText() != txt)
            danteLabel.setText (txt, juce::dontSendNotification);
    }

    // LCD countdown to next cue -- only when a setlist exists AND the
    // player is rolling. Pre-show or scrub state shows blank so the
    // chip doesn't lie about a still session.
    {
        juce::String txt;
        if (engine.getPlayer().isPlaying() && ! cues.empty())
        {
            const auto pos = engine.getPlayer().getPositionSamples();
            const auto sr  = engine.getPlayer().getSampleRate();
            // Find the next cue with samplePos > pos.
            const zynforge::SetlistBar::Cue* next = nullptr;
            for (const auto& c : cues)
                if (c.samplePos > pos) { next = &c; break; }
            if (next != nullptr && sr > 0.0)
            {
                const double secs = (double) (next->samplePos - pos) / sr;
                const int totalSec = juce::jmax (0, (int) secs);
                const int mins  = totalSec / 60;
                const int secsR = totalSec % 60;
                txt = juce::String::formatted ("Next: %s in %d:%02d",
                                                next->name.toRawUTF8(), mins, secsR);
            }
        }
        if (nextCueLabel.getText() != txt)
            nextCueLabel.setText (txt, juce::dontSendNotification);
    }

    // Tick the cue-fade ramp if one is in flight -- interpolates gain
    // and pan from the live mix toward the cue's target snapshot.
    if (cueRamp.active) updateCueRamp();

    // Punch in/out -- when the playhead enters the loop region we
    // automatically start recording on every track that has the
    // punch-arm bit set; when it leaves we stop.
    if (engine.isPunchModeOn() && engine.getPlayer().hasLoopRegion())
        servicePunch();

    // Keep each strip's input/output combos in sync with engine state --
    // the PATCH page can mutate routing behind the strip's back. Also
    // refresh name + colour so changes made from the EDIT view show up,
    // and push the per-track automation LED state (WRITE-armed +
    // Safe-locked) so the strip header reads true.
    const bool autoWriting = engine.isAutomationWriting();
    for (size_t k = 0; k < strips.size(); ++k)
    {
        auto& s = strips[k];
        if (s == nullptr) continue;
        s->refreshRoutingSelection();
        s->refreshAppearance();
        const int trackIdx = s->getStripIndex();
        const bool safe = engine.isTrackAutomationSafe (trackIdx);
        s->setAutomationLed (autoWriting && ! safe, safe);
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

    // Surface the brand-orange armed-ready border when at least one
    // strip is record-armed AND we're not already rolling -- the
    // engineer sees, before pressing RECORD, that the next press
    // will print to disk.
    bool anyArmed = false;
    for (int i = 0, n = recorder.getNumTracks(); i < n; ++i)
    {
        if (recorder.getTrack (i).armed.load (std::memory_order_relaxed))
        {
            anyArmed = true;
            break;
        }
    }
    bigClock.setArmedReady (anyArmed && ! engine.isRecording() && ! engine.isPlaying());

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

    // CPU / disk / buffer dashboard -- driven directly from engine atomics
    // so the read is lock-free even at 256 channels under load.
    perfDashboard.setMetrics (engine.getAudioLoadPct(),
                              engine.getDiskMBPerSec(),
                              engine.getRingFillPct(),
                              recorder.getMissedSamples());
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

namespace
{
    juce::int64 currentPlayheadSamples (zynforge::AudioEngine& eng)
    {
        if (eng.isRecording()) return eng.getRecorder().getSamplesSinceStart();
        if (eng.getPlayer().isLoaded()) return eng.getPlayer().getPositionSamples();
        return 0;
    }

    // Serialise the strip's state (name, colour, gain, pan, routing,
    // mute/solo/mon/arm) into a JSON object. Used by Cut / Copy / Paste.
    juce::var snapshotStrip (zynforge::AudioEngine& eng, int physical)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks())
            return {};
        auto& t = eng.getRecorder().getTrack (physical);
        auto obj = new juce::DynamicObject();
        obj->setProperty ("name",     t.name);
        obj->setProperty ("colour",   (int) t.colourARGB.load (std::memory_order_relaxed));
        obj->setProperty ("gainDb",   (double) t.gainDb     .load (std::memory_order_relaxed));
        obj->setProperty ("pan",      (double) t.pan        .load (std::memory_order_relaxed));
        obj->setProperty ("in",       t.inputRouting .load (std::memory_order_relaxed));
        obj->setProperty ("out",      t.outputRouting.load (std::memory_order_relaxed));
        obj->setProperty ("mute",     t.muted   .load (std::memory_order_relaxed));
        obj->setProperty ("solo",     t.soloed  .load (std::memory_order_relaxed));
        obj->setProperty ("mon",      t.monitor .load (std::memory_order_relaxed));
        obj->setProperty ("rec",      t.armed   .load (std::memory_order_relaxed));
        return juce::var (obj);
    }

    void restoreStrip (zynforge::AudioEngine& eng, int physical, const juce::var& v)
    {
        if (physical < 0 || physical >= eng.getRecorder().getNumTracks()) return;
        auto* obj = v.getDynamicObject();
        if (obj == nullptr) return;
        auto& t = eng.getRecorder().getTrack (physical);
        eng.setTrackName  (physical, obj->getProperty ("name").toString());
        eng.setTrackColour(physical, juce::Colour ((juce::uint32) (int) obj->getProperty ("colour")));
        eng.setTrackGainDb(physical, (float) (double) obj->getProperty ("gainDb"));
        eng.setTrackPan   (physical, (float) (double) obj->getProperty ("pan"));
        eng.setTrackInputRouting  (physical, (int) obj->getProperty ("in"));
        eng.setTrackOutputRouting (physical, (int) obj->getProperty ("out"));
        t.muted   .store ((bool) obj->getProperty ("mute"), std::memory_order_relaxed);
        t.soloed  .store ((bool) obj->getProperty ("solo"), std::memory_order_relaxed);
        t.monitor .store ((bool) obj->getProperty ("mon"),  std::memory_order_relaxed);
        t.armed   .store ((bool) obj->getProperty ("rec"),  std::memory_order_relaxed);
    }

    int physicalFromLogical (zynforge::AudioEngine& eng, int logical)
    {
        int phys = 0;
        auto& rec = eng.getRecorder();
        for (int k = 0; k < logical && phys < rec.getNumTracks(); ++k)
            phys += rec.getTrack (phys).isStereo.load (std::memory_order_relaxed) ? 2 : 1;
        return phys;
    }
}

// ─── Undo / redo ─────────────────────────────────────────────────────
// Wrapper UndoableAction that captures + restores a full mixer-state
// snapshot. This is coarse but always-correct for any strip-level edit.
namespace
{
    // Snapshot the engine's automation lanes before / after a user
    // edit. On undo() the engine is reverted to the 'before' var;
    // on redo() it's pushed forward to the 'after' var. Wraps every
    // EditPage automation mouseDown / drag / context-menu mutation.
    struct AutomationSnapshotAction final : public juce::UndoableAction
    {
        AutomationSnapshotAction (zynforge::AudioEngine& e,
                                  juce::var beforeIn, juce::var afterIn)
            : engine (e), before (std::move (beforeIn)), after (std::move (afterIn)) {}

        bool perform() override { engine.loadAutomationFromJson (after);  return true; }
        bool undo()    override { engine.loadAutomationFromJson (before); return true; }

        zynforge::AudioEngine& engine;
        juce::var before, after;
    };

    struct MixerSnapshotAction final : public juce::UndoableAction
    {
        MixerSnapshotAction (zynforge::AudioEngine& e, juce::var before)
            : eng (e), beforeState (std::move (before)) {}

        bool perform() override
        {
            // First invocation: capture the 'after' state so redo works.
            if (! afterCaptured)
            {
                afterState = captureAll();
                afterCaptured = true;
            }
            applyAll (afterState);
            return true;
        }

        bool undo() override
        {
            applyAll (beforeState);
            return true;
        }

        juce::var captureAll() const
        {
            juce::Array<juce::var> arr;
            for (int i = 0; i < eng.getRecorder().getNumTracks(); ++i)
                arr.add (snapshotStrip (eng, i));
            return juce::var (arr);
        }

        void applyAll (const juce::var& v) const
        {
            if (auto* arr = v.getArray())
                for (int i = 0; i < arr->size(); ++i)
                    restoreStrip (eng, i, (*arr)[i]);
        }

        zynforge::AudioEngine& eng;
        juce::var beforeState;
        juce::var afterState;
        bool      afterCaptured { false };
    };
}

void MainComponent::recordUndoSnapshot (const juce::String& label)
{
    juce::Array<juce::var> arr;
    for (int i = 0; i < engine.getRecorder().getNumTracks(); ++i)
        arr.add (snapshotStrip (engine, i));
    undoManager.beginNewTransaction (label);
    undoManager.perform (new MixerSnapshotAction (engine, juce::var (arr)));
}

void MainComponent::editUndo()
{
    if (! undoManager.canUndo()) { showStatus ("Nothing to undo"); return; }
    undoManager.undo();
    lastTrackCount = -1;
    showStatus ("Undo: " + undoManager.getUndoDescription());
}

void MainComponent::editRedo()
{
    if (! undoManager.canRedo()) { showStatus ("Nothing to redo"); return; }
    undoManager.redo();
    lastTrackCount = -1;
    showStatus ("Redo: " + undoManager.getRedoDescription());
}

// ─── Cut / copy / paste / delete on the strip selection ──────────────
void MainComponent::editCutSelected (bool cut)
{
    if (selectedLogical.empty()) { showStatus ("No strip selected"); return; }
    juce::Array<juce::var> arr;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        arr.add (snapshotStrip (engine, phys));
    }
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("strips", juce::var (arr));
    stripClipboard = juce::var (obj);
    showStatus (juce::String (cut ? "Cut " : "Copied ") + juce::String (arr.size())
                + " strip setting(s)");
    if (cut) deleteSelectedStrips();
}

void MainComponent::editPasteSelected()
{
    if (! stripClipboard.isObject()) { showStatus ("Clipboard is empty"); return; }
    auto* obj = stripClipboard.getDynamicObject();
    auto* arr = obj ? obj->getProperty ("strips").getArray() : nullptr;
    if (arr == nullptr || arr->isEmpty()) { showStatus ("Clipboard is empty"); return; }
    if (selectedLogical.empty()) { showStatus ("Pick a target strip first"); return; }

    recordUndoSnapshot ("Paste strip settings");

    // Round-robin: paste clipboard entries onto the selected logical
    // strips. If selection > clipboard, wraps the clipboard.
    int i = 0;
    for (int logical : selectedLogical)
    {
        const int phys = physicalFromLogical (engine, logical);
        restoreStrip (engine, phys, (*arr)[i % arr->size()]);
        ++i;
    }
    lastTrackCount = -1;
    showStatus ("Pasted strip settings onto " + juce::String ((int) selectedLogical.size())
                + " strip(s)");
}

// ─── The rest of the Edit menu ───────────────────────────────────────
void MainComponent::editSoloSelection()
{
    auto& rec = engine.getRecorder();

    // If the engineer has a selection, solo exactly those logical strips.
    if (! selectedLogical.empty())
    {
        recordUndoSnapshot ("Solo selection");
        for (int i = 0; i < rec.getNumTracks(); ++i)
            rec.getTrack (i).soloed.store (false, std::memory_order_relaxed);
        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogical (engine, logical);
            if (phys < rec.getNumTracks())
            {
                rec.getTrack (phys).soloed.store (true, std::memory_order_relaxed);
                if (rec.getTrack (phys).isStereo.load (std::memory_order_relaxed)
                    && phys + 1 < rec.getNumTracks())
                    rec.getTrack (phys + 1).soloed.store (true, std::memory_order_relaxed);
            }
        }
        showStatus ("Soloed " + juce::String ((int) selectedLogical.size()) + " strip(s)");
        return;
    }

    // No selection: toggle -- if any solo on, clear all; else solo all armed.
    bool anySolo = false;
    for (int i = 0; i < rec.getNumTracks(); ++i)
        if (rec.getTrack (i).soloed.load (std::memory_order_relaxed))
            { anySolo = true; break; }

    recordUndoSnapshot (anySolo ? "Clear solos" : "Solo armed");
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

void MainComponent::editCropToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion()) { showStatus ("No loop region to crop to"); return; }

    // Drop a 'Range In' + 'Range Out' marker pair at the loop boundaries.
    // True audio crop (rewriting Track_NN.wav) is a separate workflow we
    // route through Save Session As for now.
    auto& m = engine.getMarkers();
    m.drop (player.getLoopStart(), "Crop In");
    m.drop (player.getLoopEnd(),   "Crop Out");
    showStatus ("Crop markers placed -- use Save Session As to commit the trim");
}

void MainComponent::editSetRangeToLoopRange()
{
    auto& player = engine.getPlayer();
    if (! player.hasLoopRegion())
    {
        // Engineer hit it without a loop set -- start one at the playhead.
        const auto pos = currentPlayheadSamples (engine);
        player.setLoopRegion (pos, pos + (juce::int64) (player.getSampleRate() * 2.0));
        showStatus ("No loop region set -- defaulted to 2 s at the playhead");
        return;
    }
    auto& m = engine.getMarkers();
    m.drop (player.getLoopStart(), "Range In");
    m.drop (player.getLoopEnd(),   "Range Out");
    showStatus ("Range markers placed at loop boundaries");
}

void MainComponent::editToggleSnap()
{
    snapToMarkers = ! snapToMarkers;
    showStatus (snapToMarkers ? "Snap to markers: ON" : "Snap to markers: OFF");
}

void MainComponent::dropMarkerAndPromptName()
{
    // 1. Drop the marker at the current transport position. Auto-names
    //    it 'Marker N' where N is the new total count. Returns -1 if
    //    no session is active.
    const int newCount = engine.dropMarkerAtCurrentPosition();
    if (newCount < 0)
    {
        showStatus ("No active session -- create or open one before dropping markers");
        return;
    }

    const int rowIndex = newCount - 1;
    const auto defaultName = "Marker " + juce::String (newCount);

    // 2. Apply the default name straight away so the marker is
    //    immediately visible in the timeline + Memory Locations list
    //    with a sensible label, even if the engineer cancels the
    //    rename dialog.
    engine.getMarkers().renameMarker (rowIndex, defaultName);
    engine.getMarkers().save();
    showStatus ("Marker " + juce::String (newCount) + " dropped");

    // 3. Open the rename dialog with the default name selected, so
    //    the engineer can immediately type a meaningful name. Pro
    //    Tools' Memory Locations does exactly this on Enter.
    auto* aw = new juce::AlertWindow ("New marker",
                                      "Name this memory location:",
                                      juce::MessageBoxIconType::NoIcon,
                                      this);
    aw->addTextEditor ("markerName", defaultName, {});
    if (auto* ed = aw->getTextEditor ("markerName"))
    {
        ed->setSelectAllWhenFocused (true);
        // Defer focus grab until after AlertWindow finishes its own
        // layout pass -- otherwise the focus call is swallowed and
        // the engineer still has to click the field before typing.
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::TextEditor> (ed)]
        {
            if (safe != nullptr) safe->grabKeyboardFocus();
        });
    }
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, rowIndex, defaultName] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (self == nullptr) return;
            if (result != 1) return;            // Cancel keeps the auto-name

            const auto typed = dispose->getTextEditorContents ("markerName").trim();
            if (typed.isEmpty() || typed == defaultName) return;

            self->engine.getMarkers().renameMarker (rowIndex, typed);
            self->engine.getMarkers().save();
            self->showStatus ("Marker " + juce::String (rowIndex + 1)
                              + " renamed to: " + typed);
        }),
        false);
}

void MainComponent::showMarkersDialog()
{
    zynforge::MarkerListDialog::launch (engine);
}

void MainComponent::runAutomationEdit (const juce::String& label,
                                       std::function<void()> mutate)
{
    if (! mutate) return;
    const auto before = engine.automationToJson();
    mutate();
    const auto after  = engine.automationToJson();

    undoManager.beginNewTransaction (label);
    undoManager.perform (new AutomationSnapshotAction (engine, before, after));
    if (editPage != nullptr) editPage->repaint();
}

void MainComponent::beginAutomationTransaction()
{
    // Re-entry guard. If a drag fires mouseDown again before mouseUp
    // (shouldn't happen, but JUCE event ordering occasionally bunches
    // things up under stress), the earlier snapshot wins.
    if (automationTransactionOpen) return;
    pendingAutomationBefore = engine.automationToJson();
    automationTransactionOpen = true;
}

void MainComponent::endAutomationTransaction (const juce::String& label)
{
    if (! automationTransactionOpen) return;
    automationTransactionOpen = false;
    const auto after = engine.automationToJson();
    undoManager.beginNewTransaction (label);
    undoManager.perform (new AutomationSnapshotAction (engine,
                                                       pendingAutomationBefore,
                                                       after));
    pendingAutomationBefore = juce::var();
    if (editPage != nullptr) editPage->repaint();
}

void MainComponent::editSplitAtPlayhead()
{
    const auto pos = currentPlayheadSamples (engine);
    engine.getMarkers().drop (pos, "Split");

    // Real clip-level split -- only act on the selected strips. If
    // nothing is selected, the action stays as 'just drop a marker' so
    // it's safe to bind to the S hotkey by default.
    int splits = 0;
    if (! selectedLogical.empty())
    {
        recordUndoSnapshot ("Split clips at playhead");
        for (int logical : selectedLogical)
        {
            const int phys = physicalFromLogicalIdx (logical);
            if (engine.splitTrackAtPlayhead (phys)) ++splits;
        }
    }
    if (editPage != nullptr) editPage->repaint();
    showStatus ((splits > 0
                    ? "Split " + juce::String (splits) + " clip(s) and dropped marker"
                    : juce::String ("Split marker dropped"))
                + " at "
                + juce::String ((double) pos
                                / juce::jmax (1.0, engine.getPlayer().getSampleRate()), 2) + " s");
}

void MainComponent::editStartRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto end = player.hasLoopRegion() ? player.getLoopEnd()
                                            : pos + (juce::int64) (player.getSampleRate() * 2.0);
    player.setLoopRegion (pos, end);
    engine.getMarkers().drop (pos, "Range In");
    showStatus ("Range In set at playhead");
}

void MainComponent::editFinishRange()
{
    const auto pos = currentPlayheadSamples (engine);
    auto& player = engine.getPlayer();
    const auto start = player.hasLoopRegion() ? player.getLoopStart() : juce::int64 (0);
    player.setLoopRegion (start, pos);
    engine.getMarkers().drop (pos, "Range Out");
    showStatus ("Range Out set at playhead");
}

// ─── Setlist plumbing ────────────────────────────────────────────────
//
// Cues live as a JSON array under the session's .zfproj document:
//   "setlist": [ {"name": "Intro", "samplePos": 0}, ... ]
// Loaded on session swap, persisted on every add / update so a crash
// mid-show doesn't lose what the engineer just dialled in.

static juce::File findSessionProj (const juce::File& dir)
{
    if (! dir.isDirectory()) return {};
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.zfproj"))
        return f;
    return dir.getChildFile (dir.getFileName() + ".zfproj");
}

static zynforge::SetlistBar::StripSnapshot snapshotStrip (zynforge::TrackState& t)
{
    zynforge::SetlistBar::StripSnapshot s;
    s.stripId      = t.stripId;
    s.gainDb       = t.gainDb       .load (std::memory_order_relaxed);
    s.pan          = t.pan          .load (std::memory_order_relaxed);
    s.inputRouting = t.inputRouting .load (std::memory_order_relaxed);
    s.outputRouting= t.outputRouting.load (std::memory_order_relaxed);
    s.muted        = t.muted        .load (std::memory_order_relaxed);
    s.soloed       = t.soloed       .load (std::memory_order_relaxed);
    s.monitor      = t.monitor      .load (std::memory_order_relaxed);
    s.armed        = t.armed        .load (std::memory_order_relaxed);
    return s;
}

void MainComponent::loadSetlistFromActiveSession()
{
    cues.clear();
    currentCueIndex = -1;

    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (proj);
        if (auto* obj = parsed.getDynamicObject())
        {
            const auto v = obj->getProperty ("setlist");
            if (auto* arr = v.getArray())
            {
                for (const auto& item : *arr)
                {
                    if (auto* c = item.getDynamicObject())
                    {
                        zynforge::SetlistBar::Cue cue;
                        cue.name      = c->getProperty ("name").toString();
                        cue.samplePos = (juce::int64) (double) c->getProperty ("samplePos");
                        cue.tempoBpm   = (float) (double) c->getProperty ("tempoBpm");
                        cue.transition = (zynforge::SetlistBar::Transition)
                                            (int) c->getProperty ("transition");
                        cue.fadeBeats  = (float) (double) c->getProperty ("fadeBeats");

                        // Deserialize per-strip snapshot if present
                        // (cues saved by older builds without snapshots
                        // just leave the strips vector empty -- recall
                        // then skips strip restore).
                        const auto sv = c->getProperty ("strips");
                        if (auto* sa = sv.getArray())
                        {
                            for (const auto& sitem : *sa)
                            {
                                if (auto* so = sitem.getDynamicObject())
                                {
                                    zynforge::SetlistBar::StripSnapshot s;
                                    s.stripId      = so->getProperty ("uid").toString();
                                    s.gainDb       = (float) (double) so->getProperty ("gainDb");
                                    s.pan          = (float) (double) so->getProperty ("pan");
                                    s.inputRouting = (int)            so->getProperty ("in");
                                    s.outputRouting= (int)            so->getProperty ("out");
                                    s.muted        = (bool)           so->getProperty ("mute");
                                    s.soloed       = (bool)           so->getProperty ("solo");
                                    s.monitor      = (bool)           so->getProperty ("mon");
                                    s.armed        = (bool)           so->getProperty ("rec");
                                    cue.strips.push_back (s);
                                }
                            }
                        }
                        // Per-cue tempo curve (optional -- older cues
                        // without one just play at the single tempoBpm).
                        const auto cv = c->getProperty ("tempoCurve");
                        if (auto* ca = cv.getArray())
                        {
                            for (const auto& citem : *ca)
                            {
                                if (auto* co = citem.getDynamicObject())
                                {
                                    zynforge::SetlistBar::TempoPoint p;
                                    p.offsetSamples = (juce::int64) (double) co->getProperty ("off");
                                    p.bpm           = (float)        (double) co->getProperty ("bpm");
                                    cue.tempoCurve.push_back (p);
                                }
                            }
                        }
                        cues.push_back (std::move (cue));
                    }
                }
            }
        }
    }
    setlistBar.setCues (cues, currentCueIndex);

    // Restore comp playlists (Takes) from the .zfproj if present.
    // seedDefaultClips ran earlier and populated trackPlaylists with
    // Take 1; loadPlaylistsFromJson now overwrites that with whatever
    // the engineer had saved. Re-parse the .zfproj here -- the earlier
    // 'obj' was scoped to the setlist deserialise block above.
    {
        const auto proj = findSessionProj (engine.getActiveSessionDir());
        if (proj != juce::File{})
        {
            const auto parsed = juce::JSON::parse (proj);
            if (auto* root = parsed.getDynamicObject())
            {
                const auto pls = root->getProperty ("playlists");
                if (pls.isArray()) engine.loadPlaylistsFromJson (pls);
                // Automation lanes (formatVersion >= 3). Older saves
                // have no automation key, so the if-check is enough
                // to keep them loadable.
                const auto autom = root->getProperty ("automation");
                if (autom.isArray()) engine.loadAutomationFromJson (autom);
            }
        }
    }
}

void MainComponent::saveSetlistToActiveSession() const
{
    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj == juce::File{}) return;

    // Preserve the existing .zfproj fields (createdAt, sampleRate, etc.)
    // when we rewrite -- only the 'setlist' key is touched.
    juce::DynamicObject::Ptr obj;
    const auto parsed = juce::JSON::parse (proj);
    if (parsed.isObject()) obj = parsed.getDynamicObject();
    if (obj == nullptr)    obj = new juce::DynamicObject();

    juce::Array<juce::var> arr;
    for (const auto& c : cues)
    {
        juce::DynamicObject::Ptr entry (new juce::DynamicObject());
        entry->setProperty ("name",      c.name);
        entry->setProperty ("samplePos", (double) c.samplePos);
        entry->setProperty ("tempoBpm",    (double) c.tempoBpm);
        entry->setProperty ("transition",  (int) c.transition);
        entry->setProperty ("fadeBeats",   (double) c.fadeBeats);

        juce::Array<juce::var> stripArr;
        for (const auto& s : c.strips)
        {
            juce::DynamicObject::Ptr st (new juce::DynamicObject());
            st->setProperty ("uid",    s.stripId);
            st->setProperty ("gainDb", (double) s.gainDb);
            st->setProperty ("pan",    (double) s.pan);
            st->setProperty ("in",     s.inputRouting);
            st->setProperty ("out",    s.outputRouting);
            st->setProperty ("mute",   s.muted);
            st->setProperty ("solo",   s.soloed);
            st->setProperty ("mon",    s.monitor);
            st->setProperty ("rec",    s.armed);
            stripArr.add (juce::var (st.get()));
        }
        entry->setProperty ("strips", juce::var (stripArr));

        // Per-cue tempo curve -- offsets + bpm pairs. Empty when the
        // cue is a single-BPM cue (older format / no curve set).
        if (! c.tempoCurve.empty())
        {
            juce::Array<juce::var> curveArr;
            for (const auto& p : c.tempoCurve)
            {
                juce::DynamicObject::Ptr cp (new juce::DynamicObject());
                cp->setProperty ("off", (double) p.offsetSamples);
                cp->setProperty ("bpm", (double) p.bpm);
                curveArr.add (juce::var (cp.get()));
            }
            entry->setProperty ("tempoCurve", juce::var (curveArr));
        }
        arr.add (juce::var (entry.get()));
    }
    obj->setProperty ("setlist",   juce::var (arr));
    // Persist comp playlists (Takes) -- RAM-only before this fix
    // meant every alternate take was lost on app quit.
    obj->setProperty ("playlists",  engine.playlistsToJson());
    // Persist per-track automation lanes (volume/pan/mute points +
    // curve types). Without this, every point the engineer drew was
    // lost on app quit. Same data-loss class as the Takes bug fix.
    obj->setProperty ("automation", engine.automationToJson());
    // .zfproj schema version: 2 introduced playlists + stable strip
    // IDs. v3 adds automation lanes. Older saves read transparently.
    obj->setProperty ("formatVersion", 3);
    obj->setProperty ("updatedAt",     juce::Time::getCurrentTime().toISO8601 (true));

    proj.replaceWithText (juce::JSON::toString (juce::var (obj.get())));

    // Drop a timestamped backup copy into Session File Backups/ so a
    // misclicked cue / accidental delete is recoverable from the show.
    // Keep the most recent ~10 backups; older snapshots get pruned.
    const auto backupsDir = engine.getActiveSessionDir().getChildFile ("Session File Backups");
    if (backupsDir.createDirectory().wasOk())
    {
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
        const auto bk = backupsDir.getChildFile (proj.getFileNameWithoutExtension()
                                                  + "_" + stamp + ".zfproj");
        proj.copyFileTo (bk);

        // Prune -- keep only the 10 most recent.
        auto snaps = backupsDir.findChildFiles (juce::File::findFiles, false, "*.zfproj");
        if (snaps.size() > 10)
        {
            snaps.sort();   // alphabetical = chronological because of the stamp
            for (int i = 0; i < snaps.size() - 10; ++i)
                snaps[i].deleteFile();
        }
    }
}

void MainComponent::jumpToCue (int index)
{
    if (index < 0 || index >= (int) cues.size()) return;
    currentCueIndex = index;
    setlistBar.setCues (cues, currentCueIndex);

    const auto& cue = cues[(size_t) index];

    auto& player = engine.getPlayer();
    player.setPositionSamples (cue.samplePos);

    // Restore the cue's tempo if it has one (older cues stored 0 →
    // skip so the engineer doesn't get yanked to 0 BPM).
    if (cue.tempoBpm > 0.0f)
    {
        const float oldBpm = engine.getSessionTempoBpm();
        engine.setSessionTempoBpm (cue.tempoBpm);
        tempoBar.setBpm (engine.getSessionTempoBpm());
        // Click track is tempo-locked -- regenerate on tempo change so
        // the metronome lines up with the recalled cue.
        if (clickTrackIndex >= 0 && std::abs (oldBpm - cue.tempoBpm) > 0.05f)
            generateOrRefreshClickTrack();
    }

    // Per-cue tempo curve -- install the cue's tempo map (offsets
    // translated to absolute sample positions starting at cue.samplePos)
    // into the engine's tempo map. The audio thread already walks the
    // map each block and pushes the resulting BPM to ClickEngine and
    // MidiClockOut, so accelerandos / ritardandos within the song are
    // honoured automatically.
    if (! cue.tempoCurve.empty())
    {
        std::vector<zynforge::AudioEngine::TempoChange> map;
        map.reserve (cue.tempoCurve.size());
        for (const auto& p : cue.tempoCurve)
            map.push_back ({ cue.samplePos + p.offsetSamples, p.bpm });
        engine.setTempoMap (std::move (map));
    }

    // Fade transition? -- start a ramp instead of instant restore.
    if (cue.transition == zynforge::SetlistBar::Transition::Fade && cue.fadeBeats > 0.0f)
    {
        startCueRampTo (cue);
        updateTransportLabels();
        showStatus ("Cue " + juce::String (index + 1) + " -- fading mix over "
                    + juce::String (cue.fadeBeats, 1) + " beats");
        return;
    }

    // Recall the mixer state captured with this cue: gain / pan /
    // input + output routing / mute / solo / mon / arm -- for every
    // strip the cue knows about. Strips beyond the snapshot length
    // (added after the cue was saved) are left untouched.
    auto& rec = engine.getRecorder();
    const int total = rec.getNumTracks();
    // 250 ms soft-takeover -- short enough to feel snappy on a cue
    // jump, long enough to avoid an audible zipper / click when
    // gain or pan jumps by 6+ dB.
    constexpr double kCueRecallSeconds = 0.25;

    // Stable-ID lookup: pre-v2 cues (no stripId) fall back to array
    // index. v2+ cues match by UUID so reordering strips no longer
    // silently scrambles the cue.
    auto resolveTarget = [&] (const zynforge::SetlistBar::StripSnapshot& s,
                               int fallbackIdx) -> int
    {
        if (s.stripId.isNotEmpty())
        {
            for (int j = 0; j < total; ++j)
                if (rec.getTrack (j).stripId == s.stripId)
                    return j;
            return -1;   // strip was deleted since the cue was saved
        }
        return fallbackIdx < total ? fallbackIdx : -1;
    };

    const int n = (int) cue.strips.size();
    for (int i = 0; i < n; ++i)
    {
        const auto& s = cue.strips[(size_t) i];
        const int target = resolveTarget (s, i);
        if (target < 0) continue;

        engine.setTrackGainDbRamped (target, s.gainDb, kCueRecallSeconds);
        engine.setTrackPanRamped    (target, s.pan,    kCueRecallSeconds);
        engine.setTrackInputRouting (target, s.inputRouting);
        engine.setTrackOutputRouting(target, s.outputRouting);

        auto& t = rec.getTrack (target);
        t.muted  .store (s.muted,   std::memory_order_relaxed);
        t.soloed .store (s.soloed,  std::memory_order_relaxed);
        t.monitor.store (s.monitor, std::memory_order_relaxed);
        t.armed  .store (s.armed,   std::memory_order_relaxed);
    }
    lastTrackCount = -1;   // force strip refresh so combos + buttons redraw

    updateTransportLabels();
    showStatus ("Cue " + juce::String (index + 1) + " -- " + cue.name
                + (n > 0 ? " (mix recalled)" : ""));
}

void MainComponent::promptCueName (const juce::String& title,
                                   const juce::String& initial,
                                   std::function<void (const juce::String&)> onAccept)
{
    auto* aw = new juce::AlertWindow (title,
                                      "Cue name:",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("cueName", initial, juce::String{});
    dialog::primeNameEditor (*aw, "cueName");
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([aw, accept = std::move (onAccept)] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;
            const auto name = aw->getTextEditorContents ("cueName").trim();
            if (accept) accept (name);
        }),
        false);
}

void MainComponent::addCueAtTransport()
{
    if (! engine.getActiveSessionDir().isDirectory())
    {
        showStatus ("Open or create a session before adding cues");
        return;
    }
    auto& player = engine.getPlayer();
    const auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
    const auto suggested = "Song " + juce::String ((int) cues.size() + 1);

    promptCueName ("Add Cue", suggested, [this, pos, &player] (const juce::String& name)
    {
        zynforge::SetlistBar::Cue c;
        c.name      = name.isEmpty()
                         ? juce::String ("Song " + juce::String ((int) cues.size() + 1))
                         : name;
        c.samplePos = pos;
        c.tempoBpm  = engine.getSessionTempoBpm();

        // Snapshot the current mixer state so recalling this cue later
        // restores fader, pan, mute/solo/mon/arm, and routing for every
        // strip.
        auto& rec = engine.getRecorder();
        const int total = rec.getNumTracks();
        c.strips.reserve ((size_t) total);
        for (int i = 0; i < total; ++i)
            c.strips.push_back (snapshotStrip (rec.getTrack (i)));

        cues.push_back (std::move (c));
        currentCueIndex = (int) cues.size() - 1;
        setlistBar.setCues (cues, currentCueIndex);
        saveSetlistToActiveSession();
        showStatus ("Added cue '" + cues.back().name + "' at "
                    + juce::String ((double) pos / juce::jmax (1.0, player.getSampleRate()), 2)
                    + " s (mix snapshot captured)");
    });
}

void MainComponent::renameCurrentCue()
{
    if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size())
    {
        showStatus ("Pick a cue first, then right-click to rename");
        return;
    }
    const auto idx     = currentCueIndex;
    const auto current = cues[(size_t) idx].name;

    promptCueName ("Rename Cue", current, [this, idx] (const juce::String& name)
    {
        if (name.isEmpty()) return;
        if (idx < 0 || idx >= (int) cues.size()) return;
        cues[(size_t) idx].name = name;
        setlistBar.setCues (cues, currentCueIndex);
        saveSetlistToActiveSession();
        showStatus ("Renamed cue → '" + name + "'");
    });
}

void MainComponent::updateCueAtTransport()
{
    if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size())
    {
        showStatus ("Pick a cue first, then Update to move it");
        return;
    }
    auto& player = engine.getPlayer();
    const auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
    auto& cue = cues[(size_t) currentCueIndex];
    cue.samplePos = pos;
    cue.tempoBpm  = engine.getSessionTempoBpm();

    // Re-capture every strip's state. Update means 'whatever the mixer
    // looks like NOW is what this cue should recall to in the future'.
    auto& rec = engine.getRecorder();
    const int total = rec.getNumTracks();
    cue.strips.clear();
    cue.strips.reserve ((size_t) total);
    for (int i = 0; i < total; ++i)
        cue.strips.push_back (snapshotStrip (rec.getTrack (i)));

    setlistBar.setCues (cues, currentCueIndex);
    saveSetlistToActiveSession();
    showStatus ("Cue '" + cue.name + "' updated -- "
                + juce::String ((double) pos / juce::jmax (1.0, player.getSampleRate()), 2) + " s, "
                + juce::String (total) + " strip mix snapshotted");
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

void MainComponent::startCueRampTo (const zynforge::SetlistBar::Cue& cue)
{
    auto& rec = engine.getRecorder();
    const int n = rec.getNumTracks();
    cueRamp.gainStartTarget.clear();
    cueRamp.panStartTarget .clear();
    cueRamp.gainStartTarget.reserve ((size_t) n);
    cueRamp.panStartTarget .reserve ((size_t) n);

    auto& player = engine.getPlayer();
    player.setPositionSamples (cue.samplePos);

    for (int i = 0; i < n; ++i)
    {
        const auto curG = rec.getTrack (i).gainDb.load (std::memory_order_relaxed);
        const auto curP = rec.getTrack (i).pan   .load (std::memory_order_relaxed);
        float tgtG = curG, tgtP = curP;
        if (i < (int) cue.strips.size())
        {
            tgtG = cue.strips[(size_t) i].gainDb;
            tgtP = cue.strips[(size_t) i].pan;
        }
        cueRamp.gainStartTarget.push_back ({ curG, tgtG });
        cueRamp.panStartTarget .push_back ({ curP, tgtP });
    }

    // Other state (mute / solo / mon / arm / routing / tempo) snaps
    // immediately -- only continuous parameters interpolate.
    if (cue.tempoBpm > 0.0f) { engine.setSessionTempoBpm (cue.tempoBpm); tempoBar.setBpm (cue.tempoBpm); }
    for (int i = 0; i < (int) cue.strips.size() && i < n; ++i)
    {
        const auto& s = cue.strips[(size_t) i];
        engine.setTrackInputRouting (i, s.inputRouting);
        engine.setTrackOutputRouting(i, s.outputRouting);
        auto& t = rec.getTrack (i);
        t.muted  .store (s.muted,   std::memory_order_relaxed);
        t.soloed .store (s.soloed,  std::memory_order_relaxed);
        t.monitor.store (s.monitor, std::memory_order_relaxed);
        t.armed  .store (s.armed,   std::memory_order_relaxed);
    }

    const float bpm = engine.getSessionTempoBpm();
    cueRamp.durationMs = (cue.fadeBeats * 60000.0) / juce::jmax (20.0f, bpm);
    cueRamp.startMs    = juce::Time::getMillisecondCounterHiRes();
    cueRamp.active     = true;
    lastTrackCount = -1;
}

void MainComponent::updateCueRamp()
{
    if (! cueRamp.active) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double t   = juce::jlimit (0.0, 1.0,
                                     (now - cueRamp.startMs) / juce::jmax (1.0, cueRamp.durationMs));
    auto& rec = engine.getRecorder();
    const int n = juce::jmin ((int) cueRamp.gainStartTarget.size(), rec.getNumTracks());
    for (int i = 0; i < n; ++i)
    {
        const auto [gA, gB] = cueRamp.gainStartTarget[(size_t) i];
        const auto [pA, pB] = cueRamp.panStartTarget [(size_t) i];
        engine.setTrackGainDb (i, (float) (gA + (gB - gA) * t));
        engine.setTrackPan    (i, (float) (pA + (pB - pA) * t));
    }
    if (t >= 1.0)
        cueRamp.active = false;
}

void MainComponent::runSpectralAutoName()
{
    auto& rec = engine.getRecorder();
    const int n = rec.getNumTracks();
    if (n <= 0) { showStatus ("No tracks to classify"); return; }

    const double sr = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
        ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
        : 48000.0;

    int hits = 0;
    for (int i = 0; i < n; ++i)
    {
        const auto r = SpectralClassifier::classify (rec.getTrack (i), sr);
        if (r.name != "other")
        {
            engine.setTrackName (i, r.name + " " + juce::String (i + 1));
            ++hits;
        }
    }
    lastTrackCount = -1;
    showStatus ("Spectral auto-name: confident guesses on "
                + juce::String (hits) + " of " + juce::String (n) + " strip(s)");
}

void MainComponent::promptMirrorHost()
{
    if (sessionMirror.isMirroring())
    {
        sessionMirror.stop();
        showStatus ("Mirror stopped");
        return;
    }

    auto* aw = new juce::AlertWindow ("Mirror primary host",
                                      "Enter the primary Mac's address (host:port). "
                                      "The primary must have its companion server running.",
                                      juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("addr", "192.168.1.42:9000", "Primary host:");
    dialog::primeNameEditor (*aw, "addr");
    aw->addButton ("Start", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, aw] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1) return;
            const auto addr = aw->getTextEditorContents ("addr").trim();
            const auto colon = addr.indexOfChar (':');
            const auto host = (colon > 0) ? addr.substring (0, colon) : addr;
            const auto port = (colon > 0) ? addr.substring (colon + 1).getIntValue() : 9000;
            sessionMirror.start (host, port);
            showStatus ("Mirroring " + host + ":" + juce::String (port));
        }), false);
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

bool MainComponent::saveSessionStateTo (const juce::File& dir)
{
    if (! dir.isDirectory()) return false;

    auto& recorder = engine.getRecorder();
    auto& player   = engine.getPlayer();

    juce::DynamicObject::Ptr root (new juce::DynamicObject());
    root->setProperty ("captureFormat",  (int) recorder.getCaptureFormat());
    root->setProperty ("preRollSeconds", recorder.getPreRollSeconds());

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
    const bool wroteSettings = dir.getChildFile ("session_settings.json").replaceWithText (json);

    // Also persist the UI layout into the session's .zfproj so reopening
    // the show brings back the engineer's view choice, strip width,
    // VCA-panel visibility, and EDIT zoom. Reads in
    // loadUILayoutFromActiveSession().
    {
        const auto proj = findSessionProj (dir);
        if (proj != juce::File{})
        {
            juce::DynamicObject::Ptr obj;
            const auto parsed = juce::JSON::parse (proj);
            if (parsed.isObject()) obj = parsed.getDynamicObject();
            if (obj == nullptr)    obj = new juce::DynamicObject();

            juce::DynamicObject::Ptr ui (new juce::DynamicObject());
            ui->setProperty ("view",         currentView == View::Mix ? "Mix" : "Edit");
            ui->setProperty ("stripWidth",
                              stripWidthPreset == StripWidth::XS ? "XS"
                            : stripWidthPreset == StripWidth::S  ? "S"
                            : stripWidthPreset == StripWidth::L  ? "L"
                                                                 : "M");
            ui->setProperty ("vcaPanel",     showVcaPanel);
            ui->setProperty ("editZoom",     editPage != nullptr ? (double) editPage->getZoom() : 1.0);
            obj->setProperty ("ui", juce::var (ui.get()));
            obj->setProperty ("updatedAt", juce::Time::getCurrentTime().toISO8601 (true));
            proj.replaceWithText (juce::JSON::toString (juce::var (obj.get())));
        }
    }
    return wroteSettings;
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
    if (dir.isDirectory())
    {
        if (saveSessionStateTo (dir))
            showStatus ("Saved session state → " + dir.getFileName());
        else
            showStatus ("Save failed");
        return;
    }
    // No active session yet -- behave like Save As so the engineer
    // can still capture the current strip / format / routing config
    // to a brand new folder.
    showStatus ("No active session -- pick a destination...");
    onSaveSessionAs();
}

void MainComponent::onImportAudioFiles()
{
    if (engine.isRecording()) { showStatus ("Stop recording before importing"); return; }

    // Accept anything the JUCE basic format manager + FLAC can read.
    // (WavAudioFormat, AiffAudioFormat, FlacAudioFormat, OggVorbisAudioFormat,
    //  MP3AudioFormat -- read-only -- when JUCE_USE_MP3AUDIOFORMAT is on.)
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

        // Convert each picked file into one or two Track_NN.wav files
        // (mono per track) inside the active session's Audio Files/ dir.
        // Stereo source files become a stereo PAIR -- two consecutive
        // mono WAVs whose L track gets isStereo=true so the UI collapses
        // them into one strip. The session is then loaded for VSC playback.
        auto sessionDir = makeNewSessionDir();
        sessionDir.createDirectory();

        // Pro Tools-style: imported audio lives under Audio Files/.
        // makeNewSessionDir() either returns the engineer-named session
        // (from appProps) or freshly auto-stamps one -- either way we
        // want Track files inside the subfolder, not loose at the root.
        auto audioFilesDir = sessionDir.getChildFile ("Audio Files");
        audioFilesDir.createDirectory();
        // Also seed the rest of the Pro Tools-style layout so loose
        // imports look like a real session if the engineer hadn't
        // already created one via File ▸ New Session....
        sessionDir.getChildFile ("Bounced Files")       .createDirectory();
        sessionDir.getChildFile ("Session File Backups").createDirectory();
        engine.setActiveSessionDir (sessionDir);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        // Per imported file: remember (start_strip_index, was_stereo).
        struct ImportRecord { int trackIndex; bool stereo; juce::String name; };
        std::vector<ImportRecord> records;
        // Start *after* the existing strips so multi-file import APPENDS
        // to the session instead of overwriting Track_01.wav onwards.
        int nextTrack = engine.getRecorder().getNumTracks();
        int failed    = 0;

        auto writeMono = [] (juce::AudioFormatReader& reader, int channelIndex,
                             const juce::File& dst, double sr) -> bool
        {
            dst.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> out (dst.createOutputStream());
            if (out == nullptr) return false;
            juce::StringPairArray meta;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (out.get(), sr, 1, 24, meta, 0));
            if (writer == nullptr) return false;
            out.release();

            // Stream samples in chunks so we don't allocate a huge buffer.
            constexpr int chunk = 16384;
            juce::AudioBuffer<float> buf ((int) reader.numChannels, chunk);
            juce::int64 pos = 0;
            while (pos < reader.lengthInSamples)
            {
                const int n = (int) juce::jmin ((juce::int64) chunk,
                                                reader.lengthInSamples - pos);
                if (! reader.read (&buf, 0, n, pos, true, true)) return false;
                const float* mono[1] = { buf.getReadPointer (channelIndex) };
                if (! writer->writeFromFloatArrays (mono, 1, n)) return false;
                pos += n;
            }
            return true;
        };

        for (int i = 0; i < picks.size(); ++i)
        {
            const auto& src = picks.getReference (i);
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (src));
            if (reader == nullptr) { ++failed; continue; }

            const bool isStereoFile = (reader->numChannels >= 2);
            const auto baseName = src.getFileNameWithoutExtension();

            const int lTrack = nextTrack;
            const auto lDst = audioFilesDir.getChildFile (
                "Track_" + juce::String (lTrack + 1).paddedLeft ('0', 2) + ".wav");
            const bool lOk = writeMono (*reader, 0, lDst, reader->sampleRate);

            bool rOk = true;
            if (isStereoFile)
            {
                const int rTrack = nextTrack + 1;
                const auto rDst = audioFilesDir.getChildFile (
                    "Track_" + juce::String (rTrack + 1).paddedLeft ('0', 2) + ".wav");
                rOk = writeMono (*reader, 1, rDst, reader->sampleRate);
            }

            if (! lOk || ! rOk) { ++failed; continue; }

            records.push_back ({ lTrack, isStereoFile, baseName });
            nextTrack += isStereoFile ? 2 : 1;
        }

        if (records.empty())
        {
            showStatus ("Import failed -- no readable audio files");
            return;
        }

        // Resize the mixer to fit every imported strip.
        if (nextTrack > engine.getRecorder().getNumTracks())
            engine.setStripCount (nextTrack);

        // Apply stereo pair flags + names + routing to each imported strip.
        for (auto& rec : records)
        {
            engine.setTrackName    (rec.trackIndex, rec.name);
            engine.setTrackStereo  (rec.trackIndex, rec.stereo);
            if (rec.stereo)
            {
                engine.setTrackName  (rec.trackIndex + 1, rec.name + " R");
                engine.setTrackStereo (rec.trackIndex + 1, false);

                // Route the stereo pair to a sensible pair of inputs +
                // outputs so the strip's combos read 'In 1-2' / 'Out 1-2'
                // rather than (unrouted). L gets the even slot, R gets
                // the next one up.
                engine.setTrackLinkedRouting (rec.trackIndex,     rec.trackIndex);
                engine.setTrackLinkedRouting (rec.trackIndex + 1, rec.trackIndex + 1);
            }
            else
            {
                engine.setTrackLinkedRouting (rec.trackIndex, rec.trackIndex);
            }
        }

        // setTrackStereo doesn't change the track count, so the mixer
        // wouldn't otherwise rebuild -- force the next timer tick to
        // re-iterate logical strips (collapse stereo pairs into one).
        lastTrackCount = -1;

        const int loaded = engine.loadSession (sessionDir);
        const int stereoCount = (int) std::count_if (records.begin(), records.end(),
                                                     [] (const ImportRecord& r) { return r.stereo; });
        showStatus ("Imported " + juce::String ((int) records.size())
                    + " file(s), " + juce::String (stereoCount) + " stereo, "
                    + juce::String ((int) records.size() - stereoCount) + " mono"
                    + (failed > 0 ? " (skipped " + juce::String (failed) + ")"
                                  : juce::String())
                    + " -- loaded " + juce::String (loaded) + " for playback");
    });
}

void MainComponent::onSaveSessionAs()
{
    // Source may or may not exist yet:
    //  * If the engineer made a session via File ▸ New Session... or
    //    loaded one with Open Session..., getActiveSessionDir() points
    //    at it and Save As clones the whole folder to the new spot.
    //  * If there's no active session, Save As still works as a
    //    'save current mixer state to a new folder' flow -- it just
    //    skips the directory copy.
    const auto source = engine.getActiveSessionDir();

    chooser = std::make_unique<juce::FileChooser> (
        "Save session copy in...",
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

        // Seed the Pro Tools-style subfolder layout in the new spot.
        dest.getChildFile ("Audio Files")         .createDirectory();
        dest.getChildFile ("Bounced Files")       .createDirectory();
        dest.getChildFile ("Session File Backups").createDirectory();

        if (source.isDirectory() && source != dest)
        {
            // Copy every file inside the source session dir (audio + markers + settings).
            if (! source.copyDirectoryTo (dest))
            {
                showStatus ("Save As failed");
                return;
            }
        }

        // Refresh the per-session state JSON in the new location and
        // pin it as the new active session so the next Save lands here.
        saveSessionStateTo (dest);
        engine.setActiveSessionDir (dest);
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

        const auto activeSession = engine.getActiveSessionDir();
        const auto bouncedDir    = activeSession.isDirectory()
                                       ? activeSession.getChildFile ("Bounced Files")
                                       : getSessionsRoot();
        bouncedDir.createDirectory();
        chooser = std::make_unique<juce::FileChooser> (
            "Export all tracks to...", bouncedDir, "");

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

            showStatus ("Exporting " + juce::String ((int) all.size()) + " tracks...");
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

        const auto activeSession = engine.getActiveSessionDir();
        const auto bouncedDir    = activeSession.isDirectory()
                                       ? activeSession.getChildFile ("Bounced Files")
                                       : getSessionsRoot();
        bouncedDir.createDirectory();
        chooser = std::make_unique<juce::FileChooser> (
            "Export track to...", bouncedDir, "");

        const auto flags = juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectDirectories;

        chooser->launchAsync (flags,
            [this, channelIndex, chosenOpts] (const juce::FileChooser& fc)
        {
            auto dest = fc.getResult();
            if (dest.getFullPathName().isEmpty()) return;
            if (! dest.exists()) dest.createDirectory();

            showStatus ("Exporting track " + juce::String (channelIndex + 1) + "...");
            const int n = exportTracksTo (dest, { channelIndex }, chosenOpts);
            showStatus (n > 0
                        ? "Exported track " + juce::String (channelIndex + 1)
                           + " → " + dest.getFileName()
                        : "Export failed");
        });
    });
}

void MainComponent::warnIfSampleRateMismatch()
{
    auto* dev = engine.getDeviceManager().getCurrentAudioDevice();
    const double sessSR = engine.getPlayer().getSampleRate();
    const double devSR  = dev != nullptr ? dev->getCurrentSampleRate() : 0.0;
    if (sessSR <= 0.0 || devSR <= 0.0) return;
    if (std::abs (sessSR - devSR) < 0.5) return;

    juce::AlertWindow::showMessageBoxAsync (
        juce::MessageBoxIconType::NoIcon,
        "Sample-rate mismatch",
        "This session was recorded at " + juce::String ((int) sessSR)
            + " Hz but your audio device is set to " + juce::String ((int) devSR)
            + " Hz.\n\nPlayback will be pitched + sped up / slowed down. "
              "Open DEVICE and switch to "
            + juce::String ((int) sessSR) + " Hz for clean playback.",
        "OK");
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
        engine.setActiveSessionDir (dir);   // pin so Save / Save As stay lit
        loadSetlistFromActiveSession();
        loadUILayoutFromActiveSession();
        const int n = engine.loadSession (dir);
        if (n > 0)
        {
            statusLabel.setText ("Loaded " + juce::String (n) + " tracks", juce::dontSendNotification);
            warnIfSampleRateMismatch();
        }
        else
        {
            statusLabel.setText ("No Track_*.wav found in folder", juce::dontSendNotification);
        }
        playButton.setButtonText ("PLAY");
        updateTransportLabels();
    });
}

juce::File MainComponent::getSessionsRoot() const
{
    // Engineer can override via New Session... dialog ("Local Storage:")
    // -- stored in appProps as 'sessionsRoot'. Falls back to the canonical
    // ~/Music/Zynforge Sessions when unset.
    if (auto* props = engine.getAppProps())
    {
        const auto override_ = props->getValue ("sessionsRoot", {});
        if (override_.isNotEmpty())
        {
            juce::File f (override_);
            if (f.isDirectory() || f.createDirectory().wasOk())
                return f;
        }
    }
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile ("Zynforge Sessions");
}

// ── Session templates ───────────────────────────────────────────────
// A template captures the engineer's per-strip layout -- count,
// names, colours, stereo pairs, input + output routings -- and
// nothing else (sample rate / device live on the audio device).
// Persisted as JSON under
//   ~/Library/Application Support/Zynforge Recording/Templates/<name>.zftemplate
// Picking "New Session from Template" applies the template to a
// fresh session.
juce::File MainComponent::templatesDir() const
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("Zynforge Recording")
                    .getChildFile ("Templates");
    base.createDirectory();
    return base;
}

juce::Array<juce::File> MainComponent::listSessionTemplates() const
{
    return templatesDir().findChildFiles (juce::File::findFiles, false, "*.zftemplate");
}

void MainComponent::promptSaveSessionTemplate()
{
    auto* aw = new juce::AlertWindow ("Save session template",
                                       "Name this template:",
                                       juce::MessageBoxIconType::NoIcon);
    aw->addTextEditor ("name", "", {});
    aw->addButton ("Save",   1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create (
        [aw, self] (int r)
    {
        std::unique_ptr<juce::AlertWindow> dispose (aw);
        if (r != 1 || self == nullptr) return;
        const auto name = aw->getTextEditorContents ("name").trim();
        if (name.isEmpty()) return;

        juce::DynamicObject::Ptr obj (new juce::DynamicObject());
        obj->setProperty ("name",       name);
        obj->setProperty ("createdAt",  juce::Time::getCurrentTime().toISO8601 (true));
        obj->setProperty ("trackCount", self->engine.getRecorder().getNumTracks());

        juce::Array<juce::var> strips;
        for (int i = 0; i < self->engine.getRecorder().getNumTracks(); ++i)
        {
            auto& t = self->engine.getRecorder().getTrack (i);
            juce::DynamicObject::Ptr s (new juce::DynamicObject());
            s->setProperty ("name",    t.name);
            s->setProperty ("colour",  (juce::int64) t.colourARGB.load (std::memory_order_relaxed));
            s->setProperty ("inRoute", t.inputRouting .load (std::memory_order_relaxed));
            s->setProperty ("outRoute",t.outputRouting.load (std::memory_order_relaxed));
            s->setProperty ("stereo",  t.isStereo.load (std::memory_order_relaxed));
            s->setProperty ("gainDb",  (double) t.gainDb.load (std::memory_order_relaxed));
            s->setProperty ("pan",     (double) t.pan   .load (std::memory_order_relaxed));
            strips.add (juce::var (s.get()));
        }
        obj->setProperty ("strips", juce::var (strips));

        const auto safeName = name.replaceCharacters ("/:\\?*<>|\"", "         ").trim();
        const auto out = self->templatesDir().getChildFile (safeName + ".zftemplate");
        out.replaceWithText (juce::JSON::toString (juce::var (obj.get())));
        self->showStatus ("Template saved → " + out.getFileName());
    }));
}

void MainComponent::applySessionTemplate (const juce::File& templateFile)
{
    if (engine.isRecording()) { showStatus ("Stop recording first"); return; }

    const auto parsed = juce::JSON::parse (templateFile);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) { showStatus ("Failed to read template"); return; }

    const int n = (int) obj->getProperty ("trackCount");
    if (n <= 0) { showStatus ("Template has no strips"); return; }

    engine.resetAllStripState();
    engine.setStripCount (n);

    if (auto* arr = obj->getProperty ("strips").getArray())
    {
        for (int i = 0; i < arr->size() && i < n; ++i)
        {
            auto* s = (*arr)[i].getDynamicObject();
            if (s == nullptr) continue;
            const auto nm = s->getProperty ("name").toString();
            if (nm.isNotEmpty()) engine.setTrackName (i, nm);
            const auto col = (juce::uint32) (juce::int64) s->getProperty ("colour");
            if (col != 0) engine.setTrackColour (i, juce::Colour (col));
            engine.setTrackInputRouting  (i, (int) s->getProperty ("inRoute"));
            engine.setTrackOutputRouting (i, (int) s->getProperty ("outRoute"));
            engine.setTrackStereo (i, (bool) s->getProperty ("stereo"));
            engine.setTrackGainDb (i, (float) (double) s->getProperty ("gainDb"));
            engine.setTrackPan    (i, (float) (double) s->getProperty ("pan"));
        }
    }
    lastTrackCount = -1;
    showStatus ("Applied template: " + templateFile.getFileNameWithoutExtension());
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

void MainComponent::loadUILayoutFromActiveSession()
{
    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj == juce::File{}) return;

    const auto parsed = juce::JSON::parse (proj);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr) return;

    const auto uiVar = obj->getProperty ("ui");
    auto* ui = uiVar.getDynamicObject();
    if (ui == nullptr) return;

    // View -- must call switchView (not just write currentView) so the
    // page visibility + automation toolbar flip correctly.
    const auto viewStr = ui->getProperty ("view").toString();
    if (viewStr.isNotEmpty())
        switchView (viewStr == "Edit" ? View::Edit : View::Mix);

    // Strip width preset.
    const auto sw = ui->getProperty ("stripWidth").toString();
    if      (sw == "XS") setStripWidthPreset (StripWidth::XS);
    else if (sw == "S")  setStripWidthPreset (StripWidth::S);
    else if (sw == "L")  setStripWidthPreset (StripWidth::L);
    else if (sw == "M")  setStripWidthPreset (StripWidth::M);

    // VCA panel visibility.
    showVcaPanel = (bool) ui->getProperty ("vcaPanel");
    if (vcaPanel != nullptr) vcaPanel->setVisible (showVcaPanel);

    // EDIT zoom level.
    if (editPage != nullptr)
        editPage->setZoom ((float) (double) ui->getProperty ("editZoom"));

    resized();
}

void MainComponent::printSetlist()
{
    if (cues.empty()) { showStatus ("No cues to print"); return; }

    const auto dir = engine.getActiveSessionDir();
    const auto target = dir.isDirectory()
        ? dir.getChildFile ("setlist.html")
        : juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
              .getChildFile ("zynforge_setlist.html");

    const auto sr = engine.getPlayer().getSampleRate();
    const auto sessName = dir.isDirectory() ? dir.getFileName() : juce::String ("Session");

    juce::String html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
         << sessName << " setlist</title>"
         << "<style>"
         << "body{font:14pt -apple-system,Helvetica;background:#fff;color:#000;margin:24pt}"
         << "h1{font-size:22pt;margin:0 0 4pt 0}"
         << "h2{font-size:11pt;color:#888;font-weight:normal;margin:0 0 18pt 0}"
         << "table{border-collapse:collapse;width:100%}"
         << "th{text-align:left;padding:6pt 8pt;background:#eee;border-bottom:1pt solid #888;font-size:10pt}"
         << "td{padding:6pt 8pt;border-bottom:1pt solid #ccc}"
         << ".n{text-align:right;color:#555;font-variant-numeric:tabular-nums}"
         << ".cue{font-weight:600}"
         << ".bpm{color:#555}"
         << "@media print{body{margin:12pt}}"
         << "</style></head><body>"
         << "<h1>" << sessName << "</h1>"
         << "<h2>Setlist - " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M")
         << " * " << (int) cues.size() << " cue" << (cues.size() == 1 ? "" : "s")
         << "</h2>"
         << "<table><thead><tr><th class=\"n\">#</th><th>Cue</th><th>Time</th><th class=\"n\">BPM</th><th class=\"n\">Strips</th></tr></thead><tbody>";

    for (size_t i = 0; i < cues.size(); ++i)
    {
        const auto& c = cues[i];
        juce::String timeStr;
        if (sr > 0.0)
        {
            const double secs = (double) c.samplePos / sr;
            const int mins = (int) secs / 60;
            const double sec = secs - mins * 60;
            timeStr = juce::String::formatted ("%02d:%06.3f", mins, sec);
        }
        html << "<tr><td class=\"n\">" << (int) (i + 1) << "</td>"
             << "<td class=\"cue\">" << c.name.replace ("&", "&amp;").replace ("<", "&lt;") << "</td>"
             << "<td>" << timeStr << "</td>"
             << "<td class=\"n bpm\">" << (c.tempoBpm > 0.0f ? juce::String (c.tempoBpm, 1) : juce::String (" -"))
             << "</td>"
             << "<td class=\"n\">" << (int) c.strips.size() << "</td></tr>";
    }
    html << "</tbody></table></body></html>";

    target.replaceWithText (html);
    target.startAsProcess();    // open in default browser
    showStatus ("Setlist -> " + target.getFileName() + " (printable)");
}

void MainComponent::promptDeleteSessionTemplate()
{
    const auto list = listSessionTemplates();
    if (list.isEmpty()) return;

    juce::PopupMenu m;
    for (int i = 0; i < list.size(); ++i)
        m.addItem (i + 1, "Delete: " + list[i].getFileNameWithoutExtension());
    juce::Component::SafePointer<MainComponent> self (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [self, list] (int chosen)
    {
        if (chosen <= 0 || self == nullptr) return;
        const int idx = chosen - 1;
        if (idx < list.size())
        {
            list[idx].deleteFile();
            self->showStatus ("Template deleted: " + list[idx].getFileNameWithoutExtension());
        }
    });
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

juce::File MainComponent::makeNewSessionDir() const
{
    // If the engineer has created a named session via File ▸ New Session...,
    // every RECORD lands inside that folder (Audio Files/ subdirectory is
    // handled by MultitrackRecorder::startRecording). Otherwise we fall
    // back to the legacy auto-stamped folder so a bare RECORD click still
    // produces a session.
    if (auto* props = engine.getAppProps())
    {
        const auto saved = props->getValue ("activeSessionDir", {});
        if (saved.isNotEmpty())
        {
            juce::File f (saved);
            if (f.isDirectory() || f.createDirectory().wasOk())
                return f;
        }
    }
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    return getSessionsRoot().getChildFile ("Session_" + stamp);
}

juce::File MainComponent::createSessionFolderStructure (const zynforge::NewSessionDialog::Result& r)
{
    // Fresh session => wipe any per-strip persistence left behind by
    // the previous run (gains, pans, colours, names, routing, stereo
    // flags, VCA assignments). Without this, a hard-pan from last
    // weekend's show silently ports over to a new band.
    engine.clearAllStripOverrides();

    // Resolve a safe folder name even if the engineer typed something
    // with slashes / colons in the picker.
    auto safeName = juce::File::createLegalFileName (r.name);
    if (safeName.isEmpty()) safeName = "Untitled-1";

    // Ensure the chosen Local Storage exists, then make the session
    // folder underneath it. If that name already exists, append a
    // numeric suffix so we don't trample on an existing session.
    r.location.createDirectory();

    juce::File sessionFolder = r.location.getChildFile (safeName);
    for (int i = 2; sessionFolder.exists() && i < 9999; ++i)
        sessionFolder = r.location.getChildFile (safeName + "-" + juce::String (i));
    sessionFolder.createDirectory();

    // Subfolders -- only the ones actually wired today. Clip Groups
    // and Video Files were Pro Tools-style placeholders that nothing
    // read or wrote, so they're dropped to avoid confusing the
    // engineer with empty folders.
    sessionFolder.getChildFile ("Audio Files")         .createDirectory();
    sessionFolder.getChildFile ("Bounced Files")       .createDirectory();
    sessionFolder.getChildFile ("Session File Backups").createDirectory();

    // Session document -- a small JSON file that ties the folder together.
    // (Equivalent of Pro Tools' .ptx; we use .zfproj for clarity.)
    {
        juce::DynamicObject::Ptr m (new juce::DynamicObject());
        m->setProperty ("zynforgeSession", true);
        m->setProperty ("name",            safeName);
        m->setProperty ("createdAt",       juce::Time::getCurrentTime().toISO8601 (true));
        m->setProperty ("sampleRate",      r.sampleRate);
        m->setProperty ("captureFormat",   (int) r.captureFormat);
        m->setProperty ("interleaved",     r.interleaved);
        m->setProperty ("ioPreset",        r.ioSettings);
        sessionFolder.getChildFile (safeName + ".zfproj")
                     .replaceWithText (juce::JSON::toString (juce::var (m.get())));
    }

    // WaveCache.wfm is written on demand by EditPage::saveCacheToSession
    // (on close) and read back on session open. No placeholder needed.

    return sessionFolder;
}

void MainComponent::paint (juce::Graphics& g)
{
    // Whole-window vertical gradient -- top sits 6% above bgDeep, bottom
    // sits 4% below. Subtle enough that the engineer reads it as 'deep
    // panel' rather than 'banded background', but it stops the canvas
    // feeling like a flat sheet of paint.
    {
        auto fullBounds = getLocalBounds().toFloat();
        g.setGradientFill (juce::ColourGradient (
            brand::bgDeep.brighter (0.06f), fullBounds.getCentreX(), fullBounds.getY(),
            brand::bgDeep.darker   (0.04f), fullBounds.getCentreX(), fullBounds.getBottom(),
            false));
        g.fillRect (fullBounds);
    }

    // Header = row 1 (44 px) + row 2 transport (52 px) = 96 px total.
    // The previous value (44+40 = 84) put the bottom divider INSIDE the
    // transport row and visually clipped the buttons.
    auto header = getLocalBounds().removeFromTop (44 + 52).toFloat();
    g.setGradientFill (brand::verticalGradient (brand::bgPanel, header, 0.08f, 0.18f));
    g.fillRect (header);
    g.setColour (brand::edge);
    g.drawHorizontalLine ((int) header.getBottom() - 1,
                          header.getX(), header.getRight());

    // Empty-state guidance -- when there are no strips and the mixer
    // is visible, draw a centred hint over the strips area so the
    // engineer knows their first move is "+CH". Drawn in MainComponent
    // (not the strips container) so it doesn't have to subclass.
    if (currentView == View::Mix && strips.empty())
    {
        const auto area = stripsViewport.getBounds().toFloat();
        if (area.getHeight() > 80.0f)
        {
            g.setColour (brand::textTertiary);
            g.setFont (brand::type::headline());
            g.drawText ("No channels yet",
                        area.withTrimmedBottom (area.getHeight() * 0.55f).toNearestInt(),
                        juce::Justification::centredBottom, false);
            g.setColour (brand::textSecondary);
            g.setFont (brand::fonts::body());
            g.drawText ("Click +CH (top-right) to add your first channel.",
                        area.withSizeKeepingCentre (area.getWidth(), 24.0f).toNearestInt(),
                        juce::Justification::centred, false);
            g.setColour (brand::textTertiary);
            g.setFont (brand::fonts::small());
            g.drawText ("Then arm R, press the red record button, and play back with the green triangle.",
                        area.withTrimmedTop (area.getHeight() * 0.55f).toNearestInt(),
                        juce::Justification::centredTop, false);
        }
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();

    // Toast anchored to bottom-right of the window -- independent of
    // the rest of the layout flow because it floats over everything.
    {
        const int tW = 320;
        const int tH = 56;
        const int margin = 18;
        toast.setBounds (getWidth() - tW - margin,
                         getHeight() - tH - margin,
                         tW, tH);
    }

    // Row 1 -- title + status + LOCK + + CH + DEVICE + RECORD
    // FMT / PRE moved into Session Settings.
    auto row1 = r.removeFromTop (44).reduced (12, 8);
    titleLabel   .setBounds ({});   // hidden -- keeps left edge clean
    row1.removeFromLeft (brand::space::md);
    // MIDI clock status chip on the left edge of row 1 -- visible
    // reassurance for the engineer that the outboard sync is live.
    // Empty text → zero pixels rendered, so this disappears when the
    // clock is off.
    midiStatusLabel.setBounds (row1.removeFromLeft (220).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    danteLabel.setBounds (row1.removeFromLeft (180).reduced (0, 4));
    row1.removeFromLeft (brand::space::sm);
    // Record action is owned by the transport-bar red-circle button.
    // Keep the pill alive so callers that still poll it don't crash,
    // but exclude it from the layout.
    recordButton .setBounds ({});
    deviceButton .setBounds (row1.removeFromRight (110).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    addChannelButton.setBounds (row1.removeFromRight (70).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    lockButton   .setBounds (row1.removeFromRight (76).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    backupButton .setBounds (row1.removeFromRight (96).reduced (0, 2));
    row1.removeFromRight (brand::space::sm);
    metersButton .setBounds (row1.removeFromRight (92).reduced (0, 2));
    statusLabel  .setBounds (row1);

    // FMT and PRE buttons are kept alive (still wired) but not laid out.
    formatButton .setBounds ({});
    preRollButton.setBounds ({});

    // Row 2 -- Transport bar (taller, wider) | transport label | session label | BACKUP / PATCH / METERS / OSC
    auto row2 = r.removeFromTop (52).reduced (12, 4);

    // Six-button transport bar -- bigger, since the FILE button moved to
    // the macOS menu bar.
    if (transportBar != nullptr)
        transportBar->setBounds (row2.removeFromLeft (340).reduced (0, 2));
    row2.removeFromLeft (brand::space::xl);

    // The old FILE TextButton is no longer shown; the File menu lives in
    // the macOS system menu bar. Keep loadButton out of the layout.
    loadButton.setBounds ({});

    // PATCH + MIX / EDIT view toggle are the quick-access in-app buttons.
    patchButton    .setBounds (row2.removeFromRight (90).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    vcaToggleButton.setBounds (row2.removeFromRight (68).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    vscButton      .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (brand::space::md);
    editViewButton .setBounds (row2.removeFromRight (60).reduced (0, 2));
    row2.removeFromRight (brand::space::xs);
    mixViewButton  .setBounds (row2.removeFromRight (70).reduced (0, 2));
    row2.removeFromRight (brand::space::xl);

    // Strip-width preset pill, right next to MIX/EDIT.
    stripLButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripMButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripSButton .setBounds (row2.removeFromRight (30).reduced (0, 2));
    stripXsButton.setBounds (row2.removeFromRight (32).reduced (0, 2));

    // Drive the toggle visuals from the persisted preset.
    stripXsButton.setToggleState (stripWidthPreset == StripWidth::XS, juce::dontSendNotification);
    stripSButton .setToggleState (stripWidthPreset == StripWidth::S,  juce::dontSendNotification);
    stripMButton .setToggleState (stripWidthPreset == StripWidth::M,  juce::dontSendNotification);
    stripLButton .setToggleState (stripWidthPreset == StripWidth::L,  juce::dontSendNotification);
    row2.removeFromRight (brand::space::lg);
    transportLabel.setBounds (row2.removeFromLeft (140));
    sessionLabel  .setBounds (row2);

    // Hidden duplicates of menu-bar items. BACKUP + METERS are now
    // shown in row 1 alongside the chrome buttons.
    oscButton    .setBounds ({});
    playButton   .setBounds ({});
    stopButton   .setBounds ({});

    // Big clock banner + the CPU / disk / buffer dashboard docked on
    // the right so the engineer can see headroom without leaving the
    // mixer view. (Phase meter removed -- not relevant for a pure
    // multitrack recorder + virtual soundcheck workflow.)
    auto clockRow = r.removeFromTop (96).reduced (12, 6);
    perfDashboard.setBounds (clockRow.removeFromRight (240).reduced (4, 4));
    bigClock.setBounds (clockRow);

    // Setlist + tempo row -- slim strip between BigClock and the timeline.
    auto bar = r.removeFromTop (36).reduced (12, 2);
    tempoBar  .setBounds (bar.removeFromRight (320));
    bar.removeFromRight (brand::space::md);
    nextCueLabel.setBounds (bar.removeFromRight (200).reduced (4, 4));
    bar.removeFromRight (brand::space::sm);
    setlistBar.setBounds (bar);

    // TimelineStrip is intentionally not laid out -- see the ctor.
    if (timeline != nullptr)
        timeline->setBounds ({});

    // Strips area -- width adapts so 12 strips always fit on one page,
    // matching the Live app convention. Beyond 12 strips, the viewport
    // scrolls horizontally and the strip width stays at the same 12-fit
    // value so panning reveals additional banks without changing scale.
    // Strip-width presets -- engineer can flip via the XS/S/M/L pill in
    // the header. The numbers are the target strips-per-page; the actual
    // strip width is the viewport width divided by that.
    int targetPerPage = 12;
    int floorW = 90;
    switch (stripWidthPreset)
    {
        case StripWidth::XS: targetPerPage = 24; floorW = 56;  break;
        case StripWidth::S:  targetPerPage = 16; floorW = 72;  break;
        case StripWidth::M:  targetPerPage = 12; floorW = 90;  break;
        case StripWidth::L:  targetPerPage = 8;  floorW = 130; break;
    }
    const int kStripsPerPage = targetPerPage;
    const int kMinStripW     = floorW;
    const int kMaxStripW     = 220;
    const int margin = 12;
    const int gap    = 6;
    const int total  = (int) strips.size();

    auto viewportArea = r.reduced (margin);

    // Master strip sits on the right edge of the mix area, fixed, never
    // scrolls with the channel viewport. Hidden in the EDIT view so the
    // waveforms get the full width. VCA panel (when toggled on) sits
    // between the channel viewport and the master, so engineers reach
    // for VCA → master in a natural left-to-right gesture.
    const int masterW = 140;
    if (masterStrip != nullptr)
    {
        masterStrip->setVisible (currentView == View::Mix);
        if (currentView == View::Mix)
        {
            auto masterArea = viewportArea.removeFromRight (masterW);
            viewportArea.removeFromRight (brand::space::md);
            masterStrip->setBounds (masterArea);

            if (vcaPanel != nullptr && showVcaPanel)
            {
                // ~38 px per VCA × 8 = 304 + side padding ≈ 320 px.
                // 8 VCA strips × ~64 px each + side padding ≈ 540 px.
                // Bumped from 320 so the engineer has full-size faders
                // and a roomy click target on each bus.
                auto vcaArea = viewportArea.removeFromRight (540);
                viewportArea.removeFromRight (brand::space::md);
                vcaPanel->setBounds (vcaArea);
                vcaPanel->setVisible (true);
            }
            else if (vcaPanel != nullptr)
            {
                vcaPanel->setBounds ({});
                vcaPanel->setVisible (false);
            }
        }
        else
        {
            masterStrip->setBounds ({});
            if (vcaPanel != nullptr) { vcaPanel->setBounds ({}); vcaPanel->setVisible (false); }
        }
    }

    // In EDIT view the automation toolbar takes the top 28 px of the
    // viewport area, sharing that row with the Pro Tools-style tools
    // palette pinned to the right. In MIX view both are hidden so the
    // strips get the full height.
    auto* editToolsBar = (editPage != nullptr) ? editPage->getEditToolsBar() : nullptr;
    if (automationToolbar.isVisible())
    {
        auto topRow = viewportArea.removeFromTop (44).reduced (2, 0);
        if (editToolsBar != nullptr && editToolsBar->isVisible())
        {
            const int toolsW = 458;   // 6 tool buttons (~330) + zoom controls (~125)
            editToolsBar->setBounds (topRow.removeFromRight (toolsW));
            topRow.removeFromRight (brand::space::sm);
        }
        else if (editToolsBar != nullptr)
        {
            editToolsBar->setBounds ({});
        }
        automationToolbar.setBounds (topRow);
        viewportArea.removeFromTop (brand::space::xs);
    }
    else
    {
        automationToolbar.setBounds ({});
        if (editToolsBar != nullptr) editToolsBar->setBounds ({});
    }

    // PeakTally -- a thin red bar perched on top of the strip area.
    // Reserves 4 px above the viewport. Visible from across the room
    // when any strip clips. Click anywhere on the bar to clear.
    {
        const int tallyH = 4;
        const auto tallyBounds = viewportArea.withHeight (tallyH);
        viewportArea = viewportArea.withTrimmedTop (tallyH);
        if (peakTally != nullptr)
            peakTally->setBounds (tallyBounds);
    }

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
