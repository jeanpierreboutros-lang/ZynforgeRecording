# EditToolsBar

`Source/UI/EditToolsBar.h` — Pro Tools-style edit-mode tool palette pinned to the right of the EDIT view's automation toolbar.

## Description

A horizontal row of six tool-selection buttons + zoom controls. Each button is a vector-painted glyph (no bitmaps). The active tool biases hit-testing in `EditPage::TrackRow::mouseDown` — e.g. Selector treats clicks as loop-region drags, Trim biases clip-edge zones, Grabber biases clip-body movement.

## When to use

One instance per `EditPage`, owned by EditPage but laid out by MainComponent on the same 28 px row as the AutomationToolbar.

## Tool enum

```cpp
enum class Tool {
    None,        // Smart mode -- edge zones trim, body moves
    Selector,    // Drag a loop region
    Trim,        // Edge drag = trim, body click = no-op
    Grabber,     // Body drag = move
    Fade,        // Click a clip = fade dialog
    Scrubber    // Drag = playhead chases mouse
};
```

`None` is the default. Engineers learn to leave it on `None` (which is effectively "Smart") and only switch to a specific tool for a targeted operation.

## API

```cpp
EditToolsBar();
Tool getTool() const noexcept;
std::function<void(Tool)>  onToolChanged;
std::function<void(float)> onZoomChanged;
void setZoom (float z);
```

Host wires `onZoomChanged` to `EditPage::setZoom`; the bar itself owns the tool radio-group state.

## Layout

```
┌─ Smart ─ Selector ─ Trim ─ Grabber ─ Fade ─ Scrubber ─┐  ◀ ▶ ─[──●────] ─
│  pointer   I-beam   ◀▶    hand       /\   scrub-tool │  zoom buttons + slider
└────────────────────────────────────────────────────────┘
```

Tool buttons use `brand::toolActive()` (cool-teal `featureEngaged`) as the active accent — deliberately chosen so the tool selection doesn't collide with any signalRecord / signalMute / signalSolo claim. Replaces a previous hardcoded blue literal.

## States

| State | Visual | Behaviour |
|---|---|---|
| Inactive | Bg `bgPanel`, glyph `textSecondary` | Click to select; radio group ensures one active |
| Hovered | Bg `controlBgHover` | `mouseEnter` flag set; reverts on `mouseExit` |
| Active | Bg gradient on `toolActive()`, glyph via `onSignal(toolActive())` | Other tools deselect |
| Disabled | Bg `controlBg` faded, glyph at 30 % | Not yet wired — all tools always enabled |

## Zoom controls

- Two arrow buttons: `◀ ▶` decrement / increment zoom in `1.0 / 2.0 / 4.0 / 8.0 / 16.0` steps
- A horizontal slider for continuous zoom between `1.0` (fit) and `16.0` (16× zoom)
- `Alt+1..4` recall stored zoom presets (handled by MainComponentKeys, not this bar)

## Tokens used

- **Colours**: `brand::toolActive()` (active tool, = `featureEngaged`), `brand::bgPanel` / `controlBgHover` / `controlBg` for bg states, `brand::textSecondary` for inactive glyph, `brand::onSignal(toolActive())` for active glyph, `brand::edge` for button outlines
- **Typography**: zoom percentage label uses `brand::type::caption()` (mono variant if showing decimals)
- **Radius**: `brand::radius::md` on each button
- **Spacing**: `brand::space::sm` between buttons; `brand::space::md` between tool block and zoom block

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Default to `None` (Smart) — covers 90 % of gestures | Default to a specific tool — engineers will tool-switch every action |
| Use `toolActive()` for the active state — deliberate to avoid colliding with signal colours | Use `accentRecord` / `accentPlay` for tool selection — those mean signal state, not edit mode |
| Wire `onZoomChanged` to `EditPage::setZoom`; the bar doesn't know about content width | Compute scrollbar position inside the bar — that's EditPage's concern |

## Accessibility

- Each button has a `setTooltip` describing the tool's behaviour
- Keyboard: not yet wired (tools are mouse-selected); planned for the keyboard-nav pass
