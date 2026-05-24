# TransportBar

`Source/UI/TransportBar.{h,cpp}` — horizontal row of six icon buttons (RECORD / PLAY / PAUSE / STOP / SKIP-BACK / SKIP-FWD) in the main header.

## Description

Hosts six `IconButton` instances (vector-drawn glyphs, no bitmaps). Polls the engine at 10 Hz to mirror transport state — `recordButton` glows on `engine.isRecording()`, `playButton`'s glyph swaps to "PAUSE" when playing. Doesn't drive the transport itself — fires callbacks the host wires to engine state mutators.

## When to use

Exactly one per `MainComponent`. Lives in row 2 of the header layout. RECORD action is owned by this bar (was previously duplicated on the top-row red pill, which now hides).

## Constructor

```cpp
TransportBar (AudioEngine& engine);
```

| Param | Type | Notes |
|---|---|---|
| `engine` | `AudioEngine&` | Polled at 10 Hz for transport state |

## Callbacks

| Callback | Signature | Purpose |
|---|---|---|
| `onRecord` | `void()` | Engineer pressed RECORD |
| `onPlay` | `void()` | Engineer pressed PLAY / PAUSE (one button, glyph swaps) |
| `onStop` | `void()` | Engineer pressed STOP (host implements the two-tap guard while recording) |
| `onSkipBack` | `void()` | "Skip to previous marker / start" |
| `onSkipFwd` | `void()` | "Skip to next marker / end" |

## IconButton (nested)

Each transport button is an `IconButton` — a custom-painted button that draws a vector glyph (target ring for RECORD, triangle for PLAY, square for STOP) coloured by the button's "base colour." State logic:

| State | Background | Glyph |
|---|---|---|
| Default | `controlBg` | base colour at 60 % |
| Hover | `controlBgHover` | base colour at 80 % |
| Down (pressed) | `controlBgDown` | base colour at 100 % |
| Active (e.g. `recordButton` while recording) | base colour at 100 % | white via `onSignal()` |
| Disabled | `controlBg` faded | base colour at 30 % |

## RECORD button shape distinctness

Per a 2026-05-23 UX audit, the RECORD button has a **permanent brand-red 2 px border** at idle so its silhouette differs from PLAY's at-a-glance under stage glare. The glyph is a **concentric-ring target** (outer ring + inner disc) instead of a solid circle. Engineers can read RECORD vs PLAY by silhouette alone without relying on colour.

## Tokens used

- **Colours**: `brand::accentRecord` (RECORD base), `brand::accentPlay` (PLAY), `brand::textSecondary` (STOP / SKIP base), `brand::controlBg` / `controlBgHover` / `controlBgDown` for the three hover states
- **Typography**: none directly — buttons paint glyphs, not text
- **Spacing**: `brand::space::sm` between buttons
- **Radius**: `brand::radius::lg` on each button

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Wire `onStop` to the host's two-tap STOP-while-recording guard | Stop a rolling recording without the guard — engineers will fat-finger it under pressure |
| Trust the bar's own 10 Hz poll for visual state | Push transport state in via setters — the bar reads engine atomics directly |
| Use the same RECORD shape language elsewhere if you build a new record button | Make a square RECORD or a red triangle — breaks the silhouette agreement |
