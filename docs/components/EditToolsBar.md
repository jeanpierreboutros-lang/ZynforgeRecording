# EditToolsBar

`Source/UI/EditToolsBar.h` — edit-mode tool palette shown in the EDIT view's clock row. Header-only. Timeline and waveform zoom live on the EDIT view edge, not in this toolbar.

## Description

A horizontal row of five tool-selection buttons with vector-painted glyphs and captions. The active tool biases hit-testing in `EditPage::TrackRow::mouseDown` — Selector places the playhead on a click and selects a range on a drag, Trim trims from the nearer edge on a body drag, Grabber always moves the clip body, Fade shows drag squares on each clip's top edge and opens the fade menu on a body click.

## When to use

One instance per `EditPage`, owned by EditPage but re-parented onto MainComponent, which lays it out 330 px wide (`kEditToolsW`) at the right of the clock row, between the big clock and the status panel. It is visible only in the EDIT view.

## Tool enum

```cpp
enum class Tool : int {
    None = -1,   // no forced tool; automatic hit-testing
    Smart = 0,   // explicit Smart mode: edges trim, top edge = fade handle, body moves
    Selector,    // click = playhead, drag = range (caption "Range")
    Trim,        // body drag trims from the nearer edge
    Grabber,     // body drag always moves (caption "Move")
    Fade         // drag a top-edge square = fade; body click = fade menu
};
```

`None` is the default. Clicking an active tool again returns to `None`; Smart is an explicit selectable tool. The host's single-letter keys (EDIT view, no modifier) select a tool without toggling: `S` Smart, `R` Range, `T` Trim, `G` Move, `F` Fade.

## API

```cpp
EditToolsBar();
Tool getTool() const noexcept;
std::function<void(Tool)>  onToolChanged;
void setTool (Tool t);
```

The bar owns the tool radio-group state. `setTool` is a no-op when the tool is unchanged; otherwise it updates the buttons and fires `onToolChanged`. `EditPage` owns timeline/waveform zoom and the FOLLOW control.

## Layout

```
┌─ Smart ─ Range ─ Trim ─ Move ─ Fade ─┐
│ pointer  I-beam   |><|   hand    ◢   │
└──────────────────────────────────────┘
```

Each button is 58 px wide with a 6 px gap, glyph above a short caption. The bar is a `bgPanel` plate with an `edge` outline, inset by `space::sm` / `space::xs`.

Tool buttons use `brand::toolActive()` as the active accent. It maps to `selectFill`, the muted ember used for every selection, so the tool selection doesn't collide with any signalRecord / signalMute / signalSolo claim.

## States

| State | Visual | Behaviour |
|---|---|---|
| Inactive | Bg `bgPanel`, `edge` outline, glyph `textSecondary`, caption `textTertiary` | Click to select; radio group ensures one active |
| Hovered | Bg `controlBgHover` | `mouseEnter` flag set; reverts on `mouseExit` |
| Active | Solid `toolActive()` fill, `lift(toolActive(), 0.30)` outline, glyph + caption via `onSignal(toolActive())`; tooltip gains `-- ACTIVE` | Other tools deselect |
| Disabled | No separate disabled paint | Host disables the bar under session LOCK. Recording independently refuses edit mutations while keeping navigation available |

## Zoom and live navigation (owned by EditPage)

The EDIT view overlays `H+ / H-` for reciprocal timeline zoom, `V+ / V-` for waveform-height zoom, and `FOLLOW` to resume playhead following after a manual horizontal scroll/zoom. Wheel scrolls tracks; Shift+wheel or a horizontal trackpad gesture pans time; Cmd+wheel zooms time; Cmd+Shift+wheel zooms waveform height. These navigation controls remain active during recording and playback when the session is unlocked. Finalized-take horizontal zoom reaches 512×; live recording is capped at 16×. This toolbar has no zoom slider or zoom callbacks.

## Tokens used

- **Colours**: `brand::toolActive()` (active tool, = `selectFill`) with a `lift(…, 0.30)` outline, `brand::bgPanel` / `controlBgHover` for bar and button bg states, `brand::textSecondary` / `textTertiary` for inactive glyph / caption, `brand::onSignal(toolActive())` for active glyph + caption, `brand::edge` for bar and button outlines
- **Typography**: `brand::type::label()` for captions
- **Radius**: `brand::radius::md` on each button, `brand::radius::lg` on the bar
- **Spacing**: `brand::space::sm` / `space::xs` bar inset; 6 px literal gap between buttons

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Default to `None` (Smart) — covers 90 % of gestures | Default to a specific tool — engineers will tool-switch every action |
| Use `toolActive()` for the active state — deliberate to avoid colliding with signal colours | Use `accentRecord` / `accentPlay` for tool selection — those mean signal state, not edit mode |
| Leave zoom and follow to `EditPage` | Reintroduce a separate zoom state in this toolbar — ruler and lanes would drift |

## Accessibility

- The bar is a focus container titled "Edit tools".
- Each button has a spoken title, description and press action, plus a tooltip describing its behaviour, and takes keyboard focus.
- Return and Space activate a focused tool button; tool shortcuts are handled by the host.
