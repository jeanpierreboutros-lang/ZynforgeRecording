# SessionRecoveryDialog

`Source/UI/SessionRecoveryDialog.h` — modal dialog surfaced at launch when one or more session folders carry an orphan `recording.session` marker (i.e. the app didn't shut down cleanly during a recording).

## Description

A `juce::Component` hosted via `juce::DialogWindow::LaunchOptions` (async, native title bar "Recover sessions", resizable, 640 × 440) with `DialogChrome`-styled body. Presents a sortable table of orphan sessions, lets the engineer Recover (load + clear the marker), Delete (with confirm), or Skip (close and leave orphans for next launch).

## When to use

Called from `MainComponent::offerSessionRecovery()` during startup, after a capture-daemon take has been reattached. The host builds the list (`sessionsToRecover`): it drops any session a capture helper holds a take lock on, and the live take's session. While a helper too old to report its locks is running, it offers nothing and says so in the status bar. Skips silently when the list is empty.

Never over a rolling take. While a take rolls (local, or a helper take adopted after a crash), `offerSessionRecovery` sets `recoveryOfferHeld` and returns without scanning the sessions folder, which may be the take's drive; `serviceModalsAroundTakes` offers the list once the take's files are final. The window is launched with `launchClosesForTake` (`FloatingLaunch.h`): a take that starts behind it (OSC, a desk macro, the twin, the helper, a punch window) closes it as Skip, nothing opened or deleted, and it is offered again after STOP. Its Delete confirm and "Delete failed" alerts are marked `closesForTake` too, so they close as Cancel with it.

## API

```cpp
struct Row {
    juce::File   dir;
    juce::String name;
    juce::int64  sizeBytes  { 0 };
    juce::int64  modifiedMs { 0 };
    int          trackCount { 0 };
};

using OpenCallback   = std::function<void (const juce::File&)>;
using RemoveCallback = std::function<juce::String (const juce::File&)>;   // empty = removed, else why not

static juce::DialogWindow* launch (juce::Array<juce::File> orphans, OpenCallback onOpen,
                                   RemoveCallback onRemove, std::function<void()> onClosed = {});
```

The static `launch` instantiates the dialog, launches it through `launchClosesForTake` and returns its window (`nullptr` when there is nothing to show). `onOpen` fires when the engineer picks Recover; the host owns session loading. `onRemove` fires when a Delete is confirmed; the host owns removal and returns a reason when it refuses. The public constructor takes the same callbacks, with `onRemove` optional. `onClosed` fires when the actual dialog is destroyed and sequences the next startup step. An empty orphan list creates no dialog and does not call `onClosed`.

## Layout

```
┌─ ◇ SESSION RECOVERY ───────────────────────────────────────┐
│ 2 sessions didn't shut down cleanly. The audio is on disk  │
│ -- Recover loads the session, Delete removes it, Skip      │
│ closes this dialog and leaves them for next launch.        │
├────────────────────────────────────────────────────────────┤
│ Session              │ Tracks │ Size     │ Modified         │
│ ──────────────────── │ ────── │ ──────── │ ──────────────── │
│ Show — 2026-05-24    │     12 │ 487.0 MB │ 2026-05-24 22:41 │
│ Rehearsal_2026-04-15 │      8 │  92.4 MB │ 2026-04-15 19:02 │
├────────────────────────────────────────────────────────────┤
│                  [Recover selected] [Delete...] [Skip]     │
└────────────────────────────────────────────────────────────┘
```

## Sort behaviour

Track-file counts are taken from the session's `Audio Files` directory, with a legacy root-directory fallback. They count `Track_*` WAV / FLAC / AIFF files and skip `_part` continuations, so a split take counts once. This is a media-file count, not necessarily the number of logical mixer strips (stereo layouts differ). Size is the whole session folder, recursive, in decimal KB / MB / GB.

Recovery of an interrupted track-order transaction is handled by the session-open path through `TrackFileTransaction`, separately from this orphan-recording dialog. The session-open path also restores interrupted punch originals from `.punchbase` sidecars and archives any partial replacement under `Session File Backups/Interrupted Punch*`; configured backup/mirror copies are checked before another punch start. A `Track_NN.punchinstall` marker says a punch was being installed: with its main `.punchbase` sidecar still there the install had not committed, so the base comes back and the punch's `.punchtmp*` files are archived too; with the main sidecar gone it had, so the spliced take stays and any leftover base part is archived, never put back on it. Preserve those folders, `Session File Backups/reorder_*` and `Removed Tracks` when diagnosing a failed recovery. Recover is not a guarantee that every recorded sample survived a crash.

`juce::TableListBoxModel`-backed. Columns sort on click; names sort naturally. Default sort is **modified-time, newest first** — engineers almost always want the session they just crashed out of, which is the most-recently-modified.

## Actions

| Action | What it does |
|---|---|
| **Recover selected** | Calls `onOpen(dir)` so the host can load it; the host deletes the `recording.session` marker only after `openSessionFolder` succeeds and the session state saves (a failed open or save keeps the session listed with its own status). Closes the dialog with exit code 1. |
| **Delete...** | Confirm, then call `onRemove(dir)`. The host asks again whether a take may be writing there and refuses if so; otherwise it deletes the session directory. **Audio is lost** — the confirm dialog calls this out explicitly. A refusal shows a "Delete failed" alert with the reason and keeps the row. On success the row leaves the local model and the table refreshes; if the list empties, the dialog closes itself. With no `onRemove`, a confirmed Delete does nothing. |
| **Skip** | Closes with exit code 0. Orphans stay on disk; they reappear in the dialog on next launch. |

## Tokens used

- **Colours**: `dialog::stylePrimary` (`selectFill`) on Recover, `brand::accentRecord` at 18 % alpha with `brand::textDanger` text on Delete, `brand::bgElevated` / `textPrimary` on Skip, `brand::accentRecord` for the size of an empty folder, `brand::bgPanel` (table background), `brand::controlBgHover` (selected row), `brand::textPrimary` for cells and `textTertiary` for a zero track count, `brand::textSecondary` for the help text, `brand::bgDeep` window background
- **Typography**: `brand::type::caption()` for the help text, `brand::type::uiBody()` for cells
- **Spacing**: 12 px body inset, 6 px under the help text, 8 px between footer buttons (literals); header 22 px, rows 24 px
- **Chrome**: `dialog::paintChrome (g, host, "SESSION RECOVERY")` — same orange title stripe + flat solid body as every other dialog

## States

| State | Behaviour |
|---|---|
| Initial | Newest orphan pre-selected on row 0 |
| Row clicked | Selection moves; action buttons act on the new selection |
| Row double-clicked | Equivalent to clicking "Recover selected" |
| Delete confirm | `juce::AlertWindow` built and marked `closesForTake` (never `showAsync`, which can't be marked), with explicit `Delete` (1) / `Cancel` (0, Esc) buttons + full path in the message. Only 1 deletes |

## Accessibility

- Esc closes via `escapeKeyTriggersCloseButton = true`
- Tab cycles through Recover / Delete / Skip via JUCE default focus chain
- Help-text row at top explains the consequences in one sentence

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Show this dialog AT LAUNCH, before the Welcome dialog | Show it mid-session when an orphan suddenly appears, or over a rolling take |
| Default-sort by most-recent — that's what the engineer wants | Default-sort alphabetically — irrelevant to recovery flow |
| Make Delete require a confirm | Delete on single click — data loss class |
| Pre-select row 0 so Enter/double-click works immediately | Leave selection empty on open |
