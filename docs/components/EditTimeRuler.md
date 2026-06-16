# EditTimeRuler

`Source/UI/EditTimeRuler.h` — two-strip horizontal ruler painted above the EDIT view's waveform area.

## Description

A `juce::Component` (header-only, with its own timer — 4 Hz idle, 30 Hz while the transport rolls — that triggers repaint to keep the playhead + session length fresh) that paints two stacked rows:

```
┌─────────────────────────────────────────────────────────────┐
│ Markers   ▼intro          ▼verse          ▼chorus           │  ← marker strip
│ Min:Secs    0:10    0:20    0:30    1:00    1:30    2:00    │  ← time scale
└─────────────────────────────────────────────────────────────┘
```

There is deliberately **no Bars|Beats** strip — this is a live recorder, not a DAW; engineers navigate by wall-clock + markers (tempo math still drives the click track + cue ramps, it just isn't visualised here). Both rows share one X-axis scale, derived from the player's total length and the EDIT-view's horizontal zoom, **minus the wave viewport's horizontal scroll** so ticks stay glued to the audio when zoomed in and scrolled.

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
| `setScrollOffsetX(int)` | The wave viewport's `getViewPositionX()`. Subtracted from every time→x map (`rulerTimeToX`/`rulerXToTime`) so the fixed-width ruler tracks the scrolling lanes below it. `EditPage` pushes it on `viewport.onScroll` (manual + programmatic/auto-scroll) and in `resized()`. |

All three setters just call `repaint()` — the actual paint reads everything fresh each frame.

## Strip 1 — Markers (top, 20 px)

Reads `engine.getMarkers().getAll()`. Each marker paints as a brand-orange downward triangle anchored to the bottom of the strip, with the name extending 120 px to the right (clipped). Double-clicking on the flag or name opens a rename dialog (text pre-selected). Engineer drops markers via `M` from anywhere in the app.

## Strip 2 — Min:Secs (bottom, ~26 px)

Adaptive tick density: a 1-2-5 progression (`[0.1 … 7200]` s) picks the labelled **major** interval whose labels won't collide (≥ 64 px apart), subdivided into mid + minor ticks of graduated height. Ticks are walked by an integer index (not a float accumulator) so a second never duplicates / skips from drift, and labels switch to tenths below 1 s/major and to H:MM:SS past an hour. The transport playhead draws a bright line + a time bubble (red while recording, on a grow-to-fit timebase); the edit cursor draws its own line; the loop region shades as a band.

## Punch range overlay

Overlays the Min:Secs scale as a translucent band when the engine has an automation punch range set (`engine.getAutomationPunchIn() / Out()`). Band brightens when PUNCH is armed on the toolbar, dims when only the range is stored. Shift-drag on the time scale defines the range; shift-click without drag clears it.

## Tokens used

- **Colours**: `brand::brandOrange` for marker flags, `brand::textSecondary` for major ticks + labels, `brand::textMuted` for mid ticks, `brand::edge` for minor ticks + separator lines, `brand::accentPlay` / `accentRecord` for the playhead, `brand::textPrimary` for the edit cursor, `brand::accentEdit` for the loop band, `brand::accentStatus` for the punch band, `brand::bgDeep` for background
- **Typography**: `brand::type::caption()` for "Markers" / "Min:Secs" column labels, `brand::type::mono (10.5f, true)` for tick numerals + the time bubble
- **Alpha**: `brand::alpha::muted` for the strip separator, `brand::alpha::prominent` for tick / playhead lines, `brand::alpha::subtle` for the loop band fill
- **Motion**: own timer — 4 Hz idle, 30 Hz while playing / recording — drives repaints

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Let the ruler read engine state itself — it manages its own refresh | Push markers / tempo changes via setters — engine is source of truth |
| Use shift-drag for punch range; engineer learns this once | Add more interaction modes on the ruler — it's a status surface plus marker-rename, not an edit tool |
| Push `setScrollOffsetX` whenever the wave viewport scrolls so ticks stay aligned | Map time→x without the scroll offset — ticks drift right of the audio when zoomed in (the "ruler bug") |

## Accessibility

- Double-click marker → rename dialog with name field auto-focused + select-all
- Shift-drag and shift-click are the only mouse interactions
- Tick numerals are tabular mono for stable layout under scrolling
