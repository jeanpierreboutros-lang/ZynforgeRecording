# AutomationToolbar

`Source/UI/AutomationToolbar.{h,cpp}` — the horizontal toolbar pinned above the EDIT view that hosts the automation editing tools.

## Description

A `juce::Component` that surfaces tool / parameter / WRITE-mode picking. Hands user intent back via lambdas; never touches the engine directly. The EDIT view's `TrackRow` reads this toolbar's state on every mouse event to know which tool / parameter to act on.

## When to use

One instance per `EditPage`. Lives in the host's layout (`MainComponent::resized`) and is set as the active toolbar via `editPage->setAutomationToolbar(...)`.

## Layout

A single row, left-to-right:

```
AUTOMATION [Select] [+ Point] [Delete]   Lane: [Volume v]   [Clear all]   Write: [Off v] [Trim] [Suspend] [Punch]
```

## Public state

| Enum | Values |
|---|---|
| `Tool` | `Select`, `AddPoint`, `DeletePoint` |
| `Param` | `Volume`, `Pan`, `Mute`, `Click`, `Tempo` |
| `WriteMode` | `Off`, `Touch`, `Latch`, `Write` |

## Callbacks

| Callback | Signature | Purpose |
|---|---|---|
| `onToolChanged` | `void(Tool)` | Engineer picked a tool radio button |
| `onParamChanged` | `void(Param)` | Lane parameter picker changed |
| `onClearAll` | `void()` | "Clear all" button — host should snapshot for undo |
| `onWriteModeChanged` | `void(WriteMode)` | WRITE mode dropdown changed. Mutually exclusive with TRIM |
| `onTrimModeChanged` | `void(bool)` | TRIM toggle changed. Mutually exclusive with WRITE |
| `onSuspendChanged` | `void(bool)` | Engine ignores stored automation while on |
| `onPunchChanged` | `void(bool)` | Writes only fire inside the engine's punch range while on |

## Silent setters

When the host needs to update the UI without re-firing callbacks (e.g. restoring from `.zfproj`):

```cpp
void setParamSilently     (Param);
void setWriteModeSilently (WriteMode);
void setSuspendSilently   (bool);
void setPunchSilently     (bool);
void setTrimSilently      (bool);
```

## States

| State | Visual | Behaviour |
|---|---|---|
| Tool not selected | Button bg `bgElevated`, text `textPrimary` | Click to select |
| Tool selected | Button bg in the tool's accent (Play green / Status green / Record red), text via `onSignal(bg)` | Radio group ensures one selected at a time |
| WRITE Off | Combo neutral chrome | Lanes are read-only during playback |
| WRITE non-Off | Combo background `accentRecord`, text white | Lanes record automation on fader moves; TRIM auto-disabled |
| TRIM on | Button bg `engagedAmber` | Fader moves nudge per-track trim, not lane shape; WRITE auto-disabled |
| SUSPEND on | Button bg `textMuted` | Engine ignores stored automation at read time |
| PUNCH on | Button bg `accentStatus` | Writes gated to engine's `[in, out)` range |

## Tokens used

- **Colours**: `brand::accentPlay` (Select), `brand::accentStatus` (Add / PUNCH), `brand::accentRecord` (Delete / WRITE-on indicator), `brand::engagedAmber` (TRIM), `brand::textMuted` (SUSPEND), `brand::bgElevated` (button bg), `brand::edge` (combo outline), `brand::inputBg` (combo bg), `brand::onSignal(bg)` for foreground
- **Typography**: `brand::type::sectionTitle()` for "AUTOMATION", `brand::type::captionBold()` for `Lane:` and `Write:` labels
- **Spacing**: `brand::space::sm` between control groups
- **Radius**: `brand::radius::md` on combos and buttons
- **Tooltips** on every control — visible hover help under stage lighting

## Mutual exclusion

WRITE and TRIM are exclusive by UI rule. Picking any WRITE mode other than `Off` clears the TRIM toggle and emits `onTrimModeChanged(false)`. Toggling TRIM on resets WRITE to Off and emits `onWriteModeChanged(WriteMode::Off)`. Host doesn't need to deconflict — the toolbar already does.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Wire all six callbacks once in MainComponent ctor | Forget `onWriteModeChanged` — engine reads its mode atomically; UI must keep them in sync |
| Use `set*Silently` from session-restore code | Fire `onChange` manually in restore paths |
| Treat `Tempo` and `Click` params as "no per-track lane" | Implement per-strip Click/Tempo lanes — those parameters are global |
| Read tool/param from the toolbar in EDIT mouse handlers | Cache tool/param in `TrackRow` — toolbar can change between events |

## Accessibility

- **Tooltips**: every control has one. WRITE combo tooltip explains Touch/Latch/Write semantics.
- **Keyboard**: not yet wired; tool / WRITE-mode selection is mouse-driven.
