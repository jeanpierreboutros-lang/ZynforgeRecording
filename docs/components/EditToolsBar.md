# EditToolsBar

`Source/UI/EditToolsBar.h` — edit-mode tool palette pinned to the right of the EDIT view's automation toolbar. Timeline and waveform zoom live on the EDIT view edge, not in this toolbar.

## Description

A horizontal row of six tool-selection buttons with vector-painted glyphs and captions. The active tool biases hit-testing in `EditPage::TrackRow::mouseDown` — e.g. Selector treats clicks as range drags, Trim biases clip-edge zones, Grabber biases clip-body movement.

## When to use

One instance per `EditPage`, owned by EditPage but laid out by MainComponent on the same 28 px row as the AutomationToolbar.

## Tool enum

```cpp
enum class Tool {
    None,        // no forced tool; automatic hit-testing
    Smart,       // explicit Smart mode
    Selector,    // Drag a range
    Trim,        // Edge drag = trim, body click = no-op
    Grabber,     // Body drag = move
    Fade,        // Click a clip = fade dialog
    Scrubber    // Drag = playhead chases mouse
};
```

`None` is the default. Clicking an active tool again returns to `None`; Smart is an explicit selectable tool.

## API

```cpp
EditToolsBar();
Tool getTool() const noexcept;
std::function<void(Tool)>  onToolChanged;
void setTool (Tool t);
```

The bar owns the tool radio-group state. `EditPage` owns timeline/waveform zoom and the FOLLOW control.

## Layout

```
┌─ Smart ─ Range ─ Trim ─ Move ─ Fade ─ Scrub ─┐
│ pointer  I-beam   ◀▶    hand     /\    scrub  │
└────────────────────────────────────────────────────────┘
```

Tool buttons use `brand::toolActive()` (cool-teal `featureEngaged`) as the active accent — deliberately chosen so the tool selection doesn't collide with any signalRecord / signalMute / signalSolo claim. Replaces a previous hardcoded blue literal.

## States

| State | Visual | Behaviour |
|---|---|---|
| Inactive | Bg `bgPanel`, glyph `textSecondary` | Click to select; radio group ensures one active |
| Hovered | Bg `controlBgHover` | `mouseEnter` flag set; reverts on `mouseExit` |
| Active | Solid `toolActive()` fill, glyph via `onSignal(toolActive())` | Other tools deselect |
| Disabled | Bg `controlBg` faded, glyph at 30 % | Not yet wired — all tools always enabled |

## Zoom and live navigation (owned by EditPage)

The EDIT view overlays `H+ / H-` for reciprocal timeline zoom, `V+ / V-` for waveform-height zoom, and `FOLLOW` to resume playhead following after a manual horizontal scroll/zoom. Wheel scrolls tracks; Shift+wheel or a horizontal trackpad gesture pans time; Cmd+wheel zooms time; Cmd+Shift+wheel zooms waveform height. These controls remain active during recording and playback. This toolbar has no zoom slider or zoom callbacks.

## Tokens used

- **Colours**: `brand::toolActive()` (active tool, = `featureEngaged`), `brand::bgPanel` / `controlBgHover` / `controlBg` for bg states, `brand::textSecondary` for inactive glyph, `brand::onSignal(toolActive())` for active glyph, `brand::edge` for button outlines
- **Typography**: captions use the theme's UI text tokens
- **Radius**: `brand::radius::md` on each button
- **Spacing**: `brand::space::sm` between buttons; `brand::space::md` between tool block and zoom block

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Default to `None` (Smart) — covers 90 % of gestures | Default to a specific tool — engineers will tool-switch every action |
| Use `toolActive()` for the active state — deliberate to avoid colliding with signal colours | Use `accentRecord` / `accentPlay` for tool selection — those mean signal state, not edit mode |
| Leave zoom and follow to `EditPage` | Reintroduce a separate zoom state in this toolbar — ruler and lanes would drift |

## Accessibility

- Each button has a spoken title, description and press action, plus a tooltip describing its behaviour.
- Return and Space activate a focused tool button; tool shortcuts are handled by the host.
