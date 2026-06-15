# SessionRecoveryDialog

`Source/UI/SessionRecoveryDialog.h` — modal dialog surfaced at launch when one or more session folders carry an orphan `recording.session` marker (i.e. the app didn't shut down cleanly during a recording).

## Description

A `juce::Component` hosted via `juce::DialogWindow::LaunchOptions` with `DialogChrome`-styled body. Presents a sortable table of orphan sessions, lets the engineer Recover (load + clear the marker), Delete (with confirm), or Skip (close and leave orphans for next launch).

## When to use

Called from `MainComponent::offerSessionRecovery()` during startup. Skips silently when `findIncompleteSessions()` returns empty.

## API

```cpp
struct Row {
    juce::File   dir;
    juce::String name;
    juce::int64  sizeBytes;
    juce::int64  modifiedMs;
    int          trackCount;
};

using OpenCallback = std::function<void (const juce::File&)>;

static void launch (juce::Array<juce::File> orphans, OpenCallback onOpen);
```

The static `launch` instantiates the dialog and wires async modal behaviour. `onOpen` fires when the engineer picks Recover; the host typically calls `engine.loadSession(dir)`.

## Layout

```
┌─ SESSION RECOVERY ─────────────────────────────────┐
│ 2 session(s) didn't shut down cleanly. The audio   │
│ is on disk -- Recover loads, Delete removes, Skip  │
│ closes this dialog and leaves them for next launch.│
├─────────────────────────────────────────────────────┤
│ Session              │ Tracks │ Size    │ Modified │
│ ───────────────────  │ ────── │ ─────── │ ──────── │
│ Show — 2026-05-24    │   12   │ 487 MB  │ today    │
│ Rehearsal_2026-04-15 │    8   │  92 MB  │ 5w ago   │
├─────────────────────────────────────────────────────┤
│           [Recover selected] [Delete...] [Skip]     │
└─────────────────────────────────────────────────────┘
```

## Sort behaviour

`juce::TableListBoxModel`-backed. Columns sort on click. Default sort is **modified-time, newest first** — engineers almost always want the session they just crashed out of, which is the most-recently-modified.

## Actions

| Action | What it does |
|---|---|
| **Recover selected** | Deletes the `recording.session` marker in the picked session dir, calls `onOpen(dir)` so the host can load it. Closes the dialog with exit code 1. |
| **Delete...** | Confirm-then-delete the entire session directory recursively. **Audio is lost** — the confirm dialog calls this out explicitly. Removes the row from the local model and refreshes. If the list empties, the dialog closes itself. |
| **Skip** | Closes with exit code 0. Orphans stay on disk; they reappear in the dialog on next launch. |

## Tokens used

- **Colours**: `brand::accentStatus` (Recover button accent), `brand::accentRecord` (Delete button + size column for empty dirs), `brand::bgPanel` (table background), `brand::controlBgHover` (selected row), `brand::textPrimary` / `textSecondary` / `textMuted` for cells
- **Typography**: `brand::type::caption()` for the help text, `brand::fonts::body()` for cells (via the deprecated alias — TODO: migrate to `brand::type::uiBody()`)
- **Spacing**: `brand::space::md` outer padding
- **Chrome**: `dialog::paintChrome (g, host, "SESSION RECOVERY")` — same orange title stripe + flat solid body as every other dialog

## States

| State | Behaviour |
|---|---|
| Initial | Newest orphan pre-selected on row 0 |
| Row clicked | Selection moves; action buttons act on the new selection |
| Row double-clicked | Equivalent to clicking "Recover selected" |
| Delete confirm | `juce::AlertWindow` with explicit `Delete` / `Cancel` buttons + full path in the message |

## Accessibility

- Esc closes via `escapeKeyTriggersCloseButton = true`
- Tab cycles through Recover / Delete / Skip via JUCE default focus chain
- Help-text row at top explains the consequences in one sentence

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Show this dialog AT LAUNCH, before the Welcome dialog | Show it mid-session when an orphan suddenly appears |
| Default-sort by most-recent — that's what the engineer wants | Default-sort alphabetically — irrelevant to recovery flow |
| Make Delete require a confirm | Delete on single click — data loss class |
| Pre-select row 0 so Enter/double-click works immediately | Leave selection empty on open |
