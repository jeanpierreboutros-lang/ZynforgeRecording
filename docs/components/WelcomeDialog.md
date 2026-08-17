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
│ ┌─────────┐ │  Session Name: [_______________]      │
│ │ ▶ New   │ │  Location:     [/Music/...] [Browse]  │
│ │   Open  │ │  Format:       [WAV 24-bit ▾]         │
│ └─────────┘ │  Sample Rate:  [48000 ▾]              │
│             │  I/O:          [Mono inputs 1-8 ▾]    │
│             │                                       │
│             │                [Cancel] [Create]      │
└─────────────────────────────────────────────────────┘
```

## API

```cpp
struct NewResult {
    juce::String        name;
    juce::File          location;
    CaptureFormat       captureFormat;
    double              sampleRate;
    bool                interleaved;
    juce::String        ioSettings;   // preset key for I/O routing
};

using NewCallback  = std::function<void (const NewResult&)>;
using OpenCallback = std::function<void (const juce::File&)>;

static void launch (juce::File defaultRoot,
                    double currentSampleRate,
                    CaptureFormat currentFormat,
                    NewCallback onCreate,
                    OpenCallback onOpen);
```

Static `launch` constructs the dialog asynchronously. `onCreate` fires when the engineer commits a new session; `onOpen` when they pick an existing one. `MainComponent` owns replacement confirmation and filesystem creation: a failed create leaves the current session untouched, while a configured default template is applied and saved only after the new folder exists.

## Sidebar

Two items only — `New` and `Open`. Earlier builds had Sketch / Cloud / Learn entries; **explicitly removed** because they don't apply to ZynForge's recorder + virtual-soundcheck mission. Don't re-add them.

| Item | Right pane content |
|---|---|
| New | Form: name / location / format / sample rate / I/O preset → `[Cancel] [Create]` |
| Open | `FileChooser` style picker rooted at `defaultRoot`, plus a recent-sessions list pulled from `appProps` |

## States

| State | Behaviour |
|---|---|
| Initial (first launch) | New sidebar selected, name field empty + focused, default-selected for typing |
| Name entered | Create button enables |
| Create clicked | Validates name, then `MainComponent::createSessionFolderStructure` creates a unique folder and all required files; a default template is applied to the new session when configured |
| Open clicked | File picker opens; on selection, fires `onOpen` and closes |
| Cancel / Esc | Closes without firing either callback |

## Validation

- Name must be non-empty. Invalid chars replaced via `juce::File::createLegalFileName`.
- Resolved folder uniqueness uses `getNonexistentChildFile`, so an existing show is never reused or overwritten.
- Location, subfolder, and `.zfproj` creation must all succeed. Failure removes the partial new folder, reports the storage error, and leaves the current session loaded.

## Tokens used

- **Colours**: `brand::bgPanel` body, `brand::bgElevated` sidebar, `brand::accentStatus` (Create button), `brand::featureEngaged` (sidebar selection indicator), `brand::textPrimary` / `textSecondary`
- **Typography**: `brand::type::sectionTitle()` for "Session Name" etc., `brand::type::uiBody()` for the input rows
- **Spacing**: `brand::space::md` between form rows, `brand::space::xl` between sidebar and right pane
- **Radius**: `brand::radius::md` on combo bodies, `brand::radius::lg` on the sidebar entries
- **Chrome**: `dialog::paintChrome` for the title stripe; `dialog::primeNameEditor` to auto-focus + select-all the name field

## Sequencing with SessionRecoveryDialog + Tutorial

Launch order is serialised via nested `callAfterDelay` so dialogs don't stack:

1. **Recovery** (250 ms after launch) — if orphans exist, shows first
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
| Wire `onOpen` to `engine.loadSession(...)` | Re-implement session loading inside the dialog |
