# WelcomeDialog

`Source/UI/WelcomeDialog.{h,cpp}` — first-launch Pro Tools-style "what do you want to do?" modal.

## Description

A `juce::Component` hosted via `juce::DialogWindow::LaunchOptions` with a vertical sidebar (New / Open) and a right pane that swaps content based on the sidebar selection. Surfaced when there is no valid remembered/document-opened session via `MainComponent::showStartupWelcome()` and on demand via File ▸ New Session.

## When to use

- **First launch:** after the optional tutorial. Engineer hasn't created a session yet; the right pane defaults to "New".
- **Later launch with no session:** shown only when the remembered active session is missing/invalid. A valid last session auto-reopens, and an explicit Finder/command-line `.zfproj` takes precedence.
- **File ▸ New Session...:** opens straight to the New pane.
- **File ▸ Open Session...:** opens straight to the Open pane.

## Layout

```
┌─ WELCOME ───────────────────────────────────────────┐
│ ┌─────────┐ │  Name:          [_______________]     │
│ │ ▶ New   │ │  Local Storage: [/Music/...]          │
│ │   Open  │ │  File Type:     [WAV ▾]               │
│ └─────────┘ │  Sample Rate:   [48000 ▾]             │
│             │  Bit Depth:     [24 ▾]                │
│             │  Input / Output Device [▾]            │
│             │                                       │
│             │                [Cancel] [Create]      │
└─────────────────────────────────────────────────────┘
```

## API

```cpp
struct NewResult {
    juce::String name;
    juce::File location;
    CaptureFormat captureFormat;
    double sampleRate;
    juce::String inputDeviceName;
    juce::String outputDeviceName;
    bool devicePicked { false };   // a device picker was changed by hand
};
struct DeviceChoices {
    juce::StringArray inputs, outputs;
    juce::String currentInput, currentOutput;
};
// Each returns whether it worked; the Welcome closes only on true.
using OnCreateNew = std::function<bool (const NewResult&)>;
using OnOpenExisting = std::function<bool (const juce::File&)>;
static void launch (const juce::File& defaultLocation,
                    double lastSampleRate,
                    CaptureFormat lastCaptureFormat,
                    const DeviceChoices& devices,
                    OnCreateNew onCreate,
                    OnOpenExisting onOpen);
```

Static `launch` constructs the dialog asynchronously, through `launchClosesForTake`: a take that starts behind it (a desk macro, OSC, the twin, the helper) closes it as Cancel. Its own folder picker is a native panel and is not closed. `DeviceChoices` supplies available input/output devices and the current selections. Creating a session applies and saves that pair as the chosen device (`noteChosenDevice`) only when a picker was changed by hand (`devicePicked`), so an untouched Welcome never switches the devices; empty device names in `NewResult` mean unchanged. `onCreate` fires when the engineer commits a new session; `onOpen` when they pick an existing one. Each returns whether it worked, and the Welcome stays open on false (a refused open, a create on a missing drive) so the engineer can try again. `MainComponent` owns replacement confirmation and filesystem creation: a failed create leaves the current session untouched, while a configured default template is applied and saved only after the new folder exists.

## Sidebar

Two items only — `New` and `Open`. Don't add others: anything else doesn't belong to a recorder + virtual soundcheck.

| Item | Right pane content |
|---|---|
| New | Form: name / location / file type / sample rate / bit depth / devices → `[Cancel] [Create]`. Local Storage is a plain label beside its path button, with the drive's free space under it |
| Open | `FileChooser` style picker rooted at `defaultLocation`, plus up to six recent sessions (`AudioEngine::getRecentSessions()`, remembered on record and open) as buttons |

## States

| State | Behaviour |
|---|---|
| Initial (first launch) | New sidebar selected, name field empty + focused, default-selected for typing |
| Name entered | Create button enables |
| Create clicked | Validates name, then `MainComponent::createSessionFolderStructure` creates a unique folder and all required files; a default template is applied to the new session when configured. The Welcome closes only if that worked; a refusal leaves it open with the reason |
| Open clicked | File picker opens; on selection, fires `onOpen` and closes if it returned true |
| Cancel / Esc | Closes without firing either callback |

## Validation

- Name must be non-empty. Invalid chars replaced via `juce::File::createLegalFileName`.
- Resolved folder uniqueness uses `getNonexistentChildFile`, so an existing show is never reused or overwritten.
- Location, subfolder, and `.zfproj` creation must all succeed. Failure removes the partial new folder, reports the storage error, and leaves the current session loaded.
- **Unplugged sessions drive.** When the chosen sessions folder is on a drive that is not mounted (`folderDriveMissing`, `Source/UI/RecordPosition.h`), the host passes that folder, not the engine's `~/Music` fallback, as `defaultLocation`. The free-space line then reads `Drive not connected` in `alertAmber` instead of the boot disk's space, and Create refuses with a status line naming the drive (`createSessionFromWelcome`).

## Tokens used

- **Colours**: `brand::bgPanel` body, `brand::bgElevated` sidebar, `brand::accentStatus` (Create button), `brand::featureEngaged` (sidebar selection indicator), `brand::textPrimary` / `textSecondary`, `brand::alertAmber` for `Drive not connected`
- **Typography**: `brand::type::sectionTitle()` for "Session Name" etc., `brand::type::uiBody()` for the input rows
- **Spacing**: `brand::space::md` between form rows, `brand::space::xl` between sidebar and right pane
- **Radius**: `brand::radius::md` on combo bodies, `brand::radius::lg` on the sidebar entries
- **Chrome**: `dialog::paintChrome` for the title stripe; `dialog::primeNameEditor` to auto-focus + select-all the name field

## Sequencing with SessionRecoveryDialog + Tutorial

Launch order is serialised via nested `callAfterDelay` so dialogs don't stack:

1. **Recovery** (250 ms after launch) — if orphans exist, shows first. While a take rolls (a helper take adopted after a crash) the list and its scan of the sessions folder wait for STOP
2. **Tutorial** (after recovery dismissal) — only on first-run
3. **Document/last-session open or Welcome** (after tutorial dismissal) — an explicit `.zfproj` wins; otherwise a valid last session auto-reopens; Welcome is the empty fallback

The chain is in `MainComponent` ctor; don't move it without verifying the sequence still holds.

## Accessibility

- Name field is auto-focused with select-all via `dialog::primeNameEditor`
- Esc cancels
- Enter on the name field doesn't auto-create (Enter is currently a no-op to avoid accidental session creation; engineer must click Create explicitly)

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Default the name field to empty so the engineer types theirs immediately | Pre-fill with "Session 5" — engineers will overwrite anyway |
| Sanitise the name via `createLegalFileName` so slashes / colons in show names work | Reject names with special chars — engineers actually type "Show — 2026/05/24" |
| Auto-uniquify folder names on collision | Overwrite an existing session silently |
| Wire `onOpen` through the host replacement confirmation and `openSessionFolder` funnel | Load directly from the dialog and bypass save/capture guards |
