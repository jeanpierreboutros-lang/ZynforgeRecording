# EditTimeRuler

`Source/UI/EditTimeRuler.h` — two-strip horizontal ruler painted above the EDIT view's waveform area.

## Description

A `juce::Component` (header-only, with its own timer — 4 Hz idle, 30 Hz while the transport rolls — that keeps the playhead + session length fresh) that paints two stacked rows:

```
┌─────────────────────────────────────────────────────────────┐
│ Markers   ▼intro          ▼verse          ▼chorus           │  ← marker strip
│ Min:Secs    0:10    0:20    0:30    1:00    1:30    2:00    │  ← time scale
└─────────────────────────────────────────────────────────────┘
```

There is deliberately **no Bars|Beats** strip — this is a live recorder, not a DAW; engineers navigate by wall-clock + markers. Both rows share one X-axis scale, derived from the player's total length (or, while recording, the growing record span) and the EDIT-view's horizontal zoom, **minus the wave viewport's horizontal scroll** so ticks stay glued to the audio when zoomed in and scrolled. An empty session spans a notional 300 s (`kNotionalEmptyLaneSec`) so markers and the edit cursor can be placed before the first take.

Labels read at `timelineRate()`: the loaded session's sample rate when idle, the device rate while recording. A 44.1 kHz session reviewed on a 48 kHz device therefore reads file time.

## When to use

One instance per `EditPage`, sized to a 46 px row above the track list (20 px marker strip + 26 px scale). The host pushes only geometry — the ruler reads everything else (markers, total length, playhead, loop, edit cursor) directly from the `AudioEngine` ref it was constructed with. While the session is read-only (a take rolling, session I/O in progress, a locked session) `EditPage` turns off the ruler's mouse clicks, so marker rename is inert.

## Constructor

```cpp
explicit EditTimeRuler (AudioEngine& engine);
```

| Param | Type | Notes |
|---|---|---|
| `engine` | `AudioEngine&` | Read for markers, total length, playhead, loop region and edit cursor |

## Public methods

| Method | Purpose |
|---|---|
| `setHeaderWidth(int)` | Width of the left "labels" column. Nothing calls it today; the default 380 matches `brand::space::editHeaderW`, the lanes' header column |
| `setContentWidth(int)` | Total scrollable width, accounting for zoom |
| `setScrollOffsetX(int)` | The wave viewport's `getViewPositionX()`. Subtracted from every time→x map (`rulerTimeToX`/`rulerXToTime`) so the fixed-width ruler tracks the scrolling lanes below it. `EditPage` pushes it on `viewport.onScroll` (manual + programmatic/auto-scroll) and in `resized()`. |
| `rulerPxPerSec(double totalSec)` / `rulerTimeToX(double sec, double pxPerSec)` / `rulerXToTime(int x, double pxPerSec)` | The shared time↔x mapping. Uses the lanes' `brand::space::editWaveInset` on each side, so ruler and lanes land on the same x |
| `timelineRate()` | Sample rate the labels describe (see above) |
| `idleSignature()` | Hash of everything drawn at idle (position, length, loop, edit cursor, markers) |
| `static formatTime(double sec, int decimals)` | Label text: `M:SS` below an hour, `H:MM:SS` above; `decimals` > 0 adds tenths, rounded before the split so .95 rolls over. Public as a test seam |

The setters update the shared mapping and request repaint; scroll offset is clamped to zero and only repaints on change. The content width is clamped to at least one pixel.

## Strip 1 — Markers (top, 20 px)

Reads `engine.getMarkers().getAll()`. Each marker paints as a brand-orange downward triangle (8 px wide, 10 px tall) anchored to the bottom of the strip, with the name extending 120 px to the right (clipped). Double-clicking anywhere from 6 px left of the flag to the end of the name opens a "Rename marker" dialog (text pre-selected, OK / Cancel). An empty name is ignored, and the marker is found again by id when the dialog closes, so one removed meanwhile is left alone. The rename is saved at once. Engineer drops markers via `M` from anywhere in the app.

With a loop / selection region set, this strip also shows a readout pill: `Sel start ▸ end (length)` in `M:SS.mmm`, placed at the region's left edge and kept on screen.

## Strip 2 — Min:Secs (bottom, ~26 px)

Adaptive tick density: a 1-2-5 progression (`[0.1 … 7200]` s, with 15 s / 15 min steps) picks the labelled **major** interval whose labels won't collide (≥ 64 px apart), subdivided into mid + minor ticks of graduated height. Ticks are walked by an integer index (not a float accumulator) so a second never duplicates / skips from drift, and labels switch to tenths below 1 s/major and to H:MM:SS past an hour. Major / mid / minor ticks are 62 / 42 / 24 % of the strip height. The transport playhead draws a 2 px line + a time bubble (red while recording; tenths once zoom passes 120 px/s), flipping to the left of the line near the right edge. Its span covers both the loaded take and growing live position; during a selected punch, the bubble follows the rolling player rather than the pre-opened recorder's pinned start position. The edit cursor draws its own line; the loop region shades as a band with solid edges.

At deep zoom, tick iteration starts two ticks before the visible viewport and stops at its right edge. Finalized sessions support up to 512× timeline zoom (`kMaxTimelineZoom`); active recording remains capped at 16× (`kMaxRecordingTimelineZoom`). The ruler and waveform rows must share the same span, inset and scroll offset at either limit.

## Tokens used

- **Colours**: `brand::brandOrange` for marker flags (outlined in `sink(brandOrange, 0.40)`), `brand::textPrimary` for marker names and the edit cursor, `brand::textSecondary` for major ticks + tick labels, `brand::textTertiary` for mid ticks and the "Markers" / "Min:Secs" column labels, `brand::edge` for minor ticks, the bottom line, the header divider and the strip separator, `brand::accentPlay` / `accentRecord` for the playhead with `onSignal()` bubble text, `brand::accentEdit` for the loop band and the selection pill (text `lift(accentEdit, 0.35)` on `bgDeep`), flat `brand::bgDeep` for background (via `verticalGradient`, which now returns a solid fill)
- **Typography**: `brand::type::caption()` for the column labels, `brand::type::mono (h_caption, true)` for tick numerals, marker names + the time bubble, `brand::type::label()` for the selection readout
- **Radius**: `brand::radius::sm` on the time bubble and selection pill
- **Alpha**: `brand::alpha::muted` for the strip separator, `alpha::prominent` for minor ticks and the playhead line, `alpha::subtle` / `alpha::strong` for the loop band fill / edges, `alpha::bold` for the edit cursor and the pill background, `alpha::scrim` for the pill outline
- **Motion**: own timer — 4 Hz idle, 30 Hz while playing / recording. Rolling, every tick repaints; idle, it repaints only when `idleSignature()` changes

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Let the ruler read engine state itself — it manages its own refresh | Push markers via setters — engine is source of truth |
| Push `setScrollOffsetX` whenever the wave viewport scrolls so ticks stay aligned | Map time→x without the scroll offset — ticks drift right of the audio when zoomed in (the "ruler bug") |

## Accessibility

- Double-click marker → rename dialog with name field auto-focused + select-all
- Tick numerals are tabular mono for stable layout under scrolling
