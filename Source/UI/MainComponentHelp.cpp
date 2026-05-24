// Onboarding + help-dialog methods on MainComponent. Extracted from
// MainComponent.cpp as part of the 2026-05-24 god-class split.
//
// Includes: showFirstRunTutorial (5-step Pro Tools-style walkthrough),
// showKeyboardShortcuts (text dump of every shortcut), showUserGuide
// (long-form feature guide), showAboutDialog, showStartupWelcome
// (two-pane New / Open dialog), launchNewSessionDialog (the older
// File > New flow that still backs File menu), offerSessionRecovery
// (orphan-session table on launch).

#include "MainComponent.h"
#include "NewSessionDialog.h"
#include "SessionRecoveryDialog.h"

using namespace zynforge;

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
        "MARKERS\n"
        "    Cmd + 1 - 9   Jump to marker 1 - 9 (recalls layout if stored)\n"
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
        "AUTOMATION (EDIT view, click a row to make it active)\n"
        "    Left / Right  Walk to prev / next automation point (seeks playhead)\n"
        "    Up / Down     Nudge focused point's value\n"
        "    Delete        Remove focused point\n"
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
