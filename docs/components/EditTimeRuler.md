# EditTimeRuler

`Source/UI/EditTimeRuler.h` — three-strip horizontal ruler painted above the EDIT view's waveform area.

## Description

A `juce::Component` (header-only, with its own 4 Hz timer that triggers repaint to keep session-length tracking fresh) that paints three stacked rows:

```
┌─────────────────────────────────────────────────────────────┐
│ Markers   ▼intro          ▼verse          ▼chorus           │  ← marker strip
│ Bars|Beats │1   │2   │3   │4   │5   │6   │7   │8   │9   │10│  ← bars/beats
│ Min:Secs    0:10    0:20    0:30    1:00    1:30    2:00    │  ← time scale
└─────────────────────────────────────────────────────────────┘
```

Each row shares the same X-axis scale, derived from the player's total length and the EDIT-view's horizontal zoom.

## When to use

One instance per `EditPage`, sized to a 64 px row above the track list. The host doesn't push state — the ruler reads everything (markers, tempo map, punch range, total length) directly from the `AudioEngine` ref it was constructed with.

## Constructor

```cpp
EditTimeRuler (AudioEngine& engine);
```

| Param | Type | Notes |
|---|---|---|
| `engine` | `AudioEngine&` | Read for markers, tempo map, total length, punch range |

## Public methods

| Method | Purpose |
|---|---|
| `setHeaderWidth(int)` | Width of the left "labels" column (matches `TrackRow::headerW`) |
| `setContentWidth(int)` | Total scrollable width, accounting for zoom |

Both setters just call `repaint()` — the actual paint reads everything fresh each frame.

## Strip 1 — Markers (top, 20 px)

Reads `engine.getMarkers().getAll()`. Each marker paints as a brand-orange downward triangle anchored to the bottom of the strip, with the name extending 120 px to the right (clipped). Double-clicking on the flag or name opens a rename dialog (text pre-selected). Engineer drops markers via `M` from anywhere in the app.

## Strip 2 — Bars|Beats (middle, 18 px)

Walks the engine's tempo map event-by-event. For each segment between tempo events, uses `60 / bpm` seconds per beat. Bar lines: `engagedAmber` major ticks with the bar number labelled. Beats: `textMuted` minor ticks, **hidden when a bar would be < 24 px wide** so the strip stays readable at low zoom.

Handles tempo changes mid-session correctly — accelerandi land bar boundaries on the right samples per segment (unit-tested via `snapSampleToGrid` with mid-session 120→240 BPM jumps).

## Strip 3 — Min:Secs (bottom, ~26 px)

Adaptive tick density: picks a major tick from `[1, 2, 5, 10, 30, 60, 120, 300, 600, 1800]` seconds based on `pxPerSec` so labels never collide (minimum 70 px between major labels). Minor ticks at 1/5 of the major.

## Punch range overlay

Cross-cuts strips 2 and 3 as a translucent green band when the engine has an automation punch range set (`engine.getAutomationPunchIn() / Out()`). Band brightens when PUNCH is armed on the toolbar, dims when only the range is stored. Shift-drag on the bottom strip defines the range; shift-click without drag clears it.

## Tokens used

- **Colours**: `brand::brandOrange` for marker flags, `brand::engagedAmber` for bar lines, `brand::textMuted` for beat ticks, `brand::textSecondary` for tick labels, `brand::accentStatus` for the punch band, `brand::edge` for separator lines, `brand::bgDeep` for background
- **Typography**: `brand::type::caption()` for "Markers" / "Bars|Beats" / "Min:Secs" column labels, `brand::type::mono (10.5f, true)` for tick numerals
- **Alpha**: `brand::alpha::ghost` for separator, `brand::alpha::prominent` for punch band edge, `brand::alpha::dimmed` for un-armed punch fill
- **Motion**: own 4 Hz timer drives repaints

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Let the ruler read engine state itself — it manages its own refresh | Push markers / tempo changes via setters — engine is source of truth |
| Use shift-drag for punch range; engineer learns this once | Add more interaction modes on the ruler — it's a status surface plus marker-rename, not an edit tool |
| Trust the tempo-map walker for music sessions with mid-song tempo changes | Assume constant BPM — the walker handles the map correctly |

## Accessibility

- Double-click marker → rename dialog with name field auto-focused + select-all
- Shift-drag and shift-click are the only mouse interactions
- Tick numerals are tabular mono for stable layout under scrolling
