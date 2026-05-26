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
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

using namespace zynforge;

namespace
{
    // Two-column reference table for the Keyboard Shortcuts dialog.
    // Painted directly (not a TableListBox) since the content is
    // small, static, and we want section headers between groups.
    // Colours follow the brand grey palette so this dialog matches
    // every other DialogChrome modal: bgPanel body, textPrimary
    // headings, textSecondary descriptions, bgDeep zebra rows.
    class ShortcutsTable final : public juce::Component
    {
    public:
        ShortcutsTable()
        {
            addAndMakeVisible (viewport);
            viewport.setViewedComponent (&content, false);
            viewport.setScrollBarsShown (true, false);
            content.setSize (640, computeContentHeight());
        }

        void resized() override
        {
            viewport.setBounds (getLocalBounds());
            content.setSize (juce::jmax (640, viewport.getWidth()
                                                - viewport.getScrollBarThickness()),
                             computeContentHeight());
        }

    private:
        struct Row { juce::String shortcut; juce::String description; };
        struct Section { juce::String title; std::vector<Row> rows; };

        static std::vector<Section> sections()
        {
            return {
                { "Transport", {
                    { "Space",        "Play / pause (stops recording if armed)" },
                    { "M",            "Drop marker at playhead" },
                }},
                { "Cues", {
                    { "1 – 9",        "Jump to cue 1 – 9" },
                }},
                { "Markers", {
                    { "Cmd + 1 – 9",  "Jump to marker 1 – 9 (recalls layout if stored)" },
                }},
                { "Edit", {
                    { "A",            "Solo selected strips" },
                    { "S",            "Split clip at playhead" },
                    { ",",            "Set range start at playhead" },
                    { ".",            "Set range end at playhead" },
                    { "4",            "Toggle snap mode" },
                    { "Cmd + Z",      "Undo last clip edit" },
                    { "Cmd + R",      "Redo" },
                    { "Cmd + X",      "Cut selected strips" },
                    { "Cmd + C",      "Copy selected strips" },
                    { "Cmd + V",      "Paste strips" },
                }},
                { "Automation (EDIT view, click a row to activate)", {
                    { "← / →",        "Walk to prev / next automation point (seeks playhead)" },
                    { "↑ / ↓",        "Nudge focused point's value" },
                    { "Delete",       "Remove focused point" },
                }},
                { "Mixer", {
                    { "Shift + click", "Toggle multi-selection (additive)" },
                    { "Cmd + A",      "Select every strip" },
                    { "Esc",          "Clear strip selection" },
                    { "Right-click strip",
                                      "Rename / Colour / Stereo / Stream send / VCA assign / Output-mute" },
                }},
                { "Edit view", {
                    { "Right-click row",  "Row size + Take swap (comp playlists)" },
                    { "Right-click clip", "Mute / Lock / Duplicate / Delete / Gain / Fade in & out" },
                    { "Drag swatch",      "Reorder strip (paused only)" },
                }},
            };
        }

        static constexpr int kHeaderH  = 26;
        static constexpr int kRowH     = 24;
        static constexpr int kColSplit = 200;   // left column width (shortcut)
        static constexpr int kHPad     = 14;

        static int computeContentHeight()
        {
            int h = 8;   // top pad
            for (const auto& s : sections())
                h += kHeaderH + (int) s.rows.size() * kRowH + 6;
            return h + 8;
        }

        class Content final : public juce::Component
        {
        public:
            void paint (juce::Graphics& g) override
            {
                int y = 8;
                bool zebra = false;
                for (const auto& s : sections())
                {
                    // Section header -- light grey on slightly lifted
                    // panel, brand-orange accent stripe so groups read
                    // distinctly without colour collisions.
                    juce::Rectangle<int> hdr (0, y, getWidth(), kHeaderH);
                    g.setColour (brand::bgElevated);
                    g.fillRect (hdr);
                    g.setColour (brand::brandOrange);
                    g.fillRect (hdr.removeFromLeft (3));
                    g.setColour (brand::textPrimary);
                    g.setFont (brand::type::uiLabel());
                    g.drawText (s.title.toUpperCase(),
                                hdr.withTrimmedLeft (kHPad),
                                juce::Justification::centredLeft, false);
                    y += kHeaderH;

                    for (const auto& r : s.rows)
                    {
                        juce::Rectangle<int> row (0, y, getWidth(), kRowH);
                        // Zebra: even rows get a slight tint over bgPanel
                        // so the eye can track across the row.
                        if (zebra)
                        {
                            g.setColour (brand::bgDeep.withAlpha (brand::alpha::scrim));
                            g.fillRect (row);
                        }
                        zebra = ! zebra;

                        // Left column -- shortcut, mono, light grey.
                        g.setColour (brand::textPrimary);
                        g.setFont (brand::type::mono (12.0f, true));
                        g.drawText (r.shortcut,
                                    row.withTrimmedLeft (kHPad).withWidth (kColSplit - kHPad),
                                    juce::Justification::centredLeft, false);

                        // Right column -- description, sans, mid grey.
                        g.setColour (brand::textSecondary);
                        g.setFont (brand::type::uiBody());
                        g.drawText (r.description,
                                    row.withTrimmedLeft (kColSplit)
                                       .withTrimmedRight (kHPad),
                                    juce::Justification::centredLeft, true);
                        y += kRowH;
                    }
                    y += 6;
                    zebra = false;   // reset zebra at each section
                }
            }
        };

        juce::Viewport viewport;
        Content        content;
    };

    class KeyboardShortcutsDialog final : public juce::Component
    {
    public:
        KeyboardShortcutsDialog()
        {
            addAndMakeVisible (table);
            okButton.setButtonText ("OK");
            dialog::stylePrimary (okButton);
            okButton.onClick = [this]
            {
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            };
            addAndMakeVisible (okButton);
            setSize (700, 560);
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "KEYBOARD SHORTCUTS");
        }

        void resized() override
        {
            table.setBounds (dialog::bodyBounds (*this).reduced (brand::space::md,
                                                                  brand::space::sm));
            auto footer = dialog::footerBounds (*this);
            okButton.setBounds (footer.removeFromRight (dialog::btnPrimary)
                                       .reduced (0, brand::space::xs));
        }

    private:
        ShortcutsTable    table;
        juce::TextButton  okButton;
    };
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
        aw->setLookAndFeel (&self->getLookAndFeel());   // grey ZynForge chrome, not JUCE default
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
    // Real two-column DialogChrome modal (no longer a centred-text
    // AlertWindow dump). Same grey/light-grey palette as every other
    // dialog in the app -- bgPanel body, brand-orange title stripe,
    // textPrimary headings, textSecondary descriptions.
    auto content = std::make_unique<KeyboardShortcutsDialog>();
    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle             = "Keyboard shortcuts";
    opts.dialogBackgroundColour  = brand::bgPanel;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar       = false;
    opts.resizable               = false;
    opts.content.setOwned (content.release());
    opts.launchAsync();
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
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
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

        // Fresh session starts EMPTY -- no auto-applied template,
        // no leftover names. Engineers add channels with +CH or pick
        // a template explicitly from File ▸ New Session from Template.
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
        //       Export Files/
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

        // Fresh session starts EMPTY (see showStartupWelcome for the
        // same rule). Templates apply only via explicit menu choice.
        self->showStatus ("New session '" + r.name
                          + "' -- add channels with +CH and arm REC to capture");
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
