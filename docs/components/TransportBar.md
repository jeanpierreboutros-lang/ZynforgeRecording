# TransportBar

`Source/UI/TransportBar.{h,cpp}` — horizontal row of six icon buttons (START / END / PLAY-PAUSE / STOP / RECORD / LOOP) in the main header.

## Description

Hosts six `IconButton` instances (vector-drawn glyphs, no bitmaps). Polls the engine at 10 Hz to mirror transport state — RECORD reflects `engine.isRecording()` and PLAY lights its active state while playing. The host wires `onRequestRecord`, `onRequestPlay` and `onRequestStop` to its preflight/finalization paths; without host callbacks it may control playback, but RECORD does nothing and STOP refuses to end a live take.

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
| `onRequestRecord` | `void()` | Host record preflight and punch handling |
| `onRequestPlay` | `void()` | Host play/pause handling |
| `onRequestStop` | `void()` | Host normal two-tap guard and one-press punch stop |

START, END and LOOP are wired inside the bar. There are no `onSkipBack`/`onSkipFwd` public callbacks.

## Capture-daemon host contract

`engine.isRecording()` includes external capture. Normal live capture keeps the two-tap STOP guard; a deliberate local manual or selected punch ends on one RECORD or STOP press. The host routes these controls through its capture-finalization path. With the current matching protocol-v4 GUI/helper pair, the host waits for a daemon reply that distinguishes command acceptance, capture completion and clean finalization before clearing the recording display, reloading completed media or saving the stopped session. If capture stopped but report/recovery/redundancy finalization failed, the transport resets but the error remains visible; if completion is uncertain, recording state stays armed. A click alone is not evidence that capture has stopped. Playback refuses at the engine boundary during ordinary local/daemon capture; the explicitly gated selected-punch window is the exception that allows pre/punch/post-roll playback.

Local device loss closes the mix writer and queues engine finalization once, preserving the stopped endpoint and warning. Repeated STOP must not reload away later edits. This does not remove the need to test physical device loss.

While metadata persistence is busy, the host still lets Space stop transport
that is already playing or recording. This does not authorize starting playback,
changing a finalizing take, bypassing LOCK, or skipping ordinary two-tap STOP.
Remote STOP returns pending until required media/metadata completion; an idle
capture flag alone is not the acknowledgment.

## IconButton (nested)

Each transport button is an `IconButton` — a custom-painted button that draws a vector glyph (target ring for RECORD, triangle for PLAY, square for STOP) coloured by the button's "base colour." State logic:

| State | Background and glyph |
|---|---|
| Idle | Themed control body and per-button accent glyph; RECORD retains its red outline |
| Hover / pressed | `controlBgHover` / `controlBgDown` with the themed gradient finish |
| Recording | Solid record-accent base with `onSignal(accentRecord)` glyph |
| Armed, not recording | Alternates the record border/body pulse without claiming rolling capture |
| PLAY / LOOP active | Their accent-coloured body and `onSignal(baseColour)` glyph |

## RECORD button shape distinctness

Per a 2026-05-23 UX audit, the RECORD button has a **permanent brand-red 2 px border** at idle so its silhouette differs from PLAY's at-a-glance under stage glare. The glyph is a **concentric-ring target** (outer ring + inner disc) instead of a solid circle. Engineers can read RECORD vs PLAY by silhouette alone without relying on colour.

## Tokens used

- **Colours**: `brand::accentRecord` (RECORD base), `brand::accentPlay` (PLAY), `brand::textSecondary` (STOP), `brand::textPrimary` (START/END), `brand::brandOrange` (LOOP), `brand::controlBg` / `controlBgHover` / `controlBgDown` for the three hover states
- **Typography**: none directly — buttons paint glyphs, not text
- Geometry is defined in `TransportBar.cpp`; the current tile uses a 9 px radius. Do not infer token use from earlier specifications.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Wire `onRequestStop` to the host's normal two-tap guard and deliberate-punch one-press path | Stop an ordinary live take with one accidental press |
| Trust the bar's own 10 Hz poll for visual state | Push transport state in via setters — the bar reads engine atomics directly |
| Use the same RECORD shape language elsewhere if you build a new record button | Make a square RECORD or a red triangle — breaks the silhouette agreement |

## Accessibility

The bar and each icon button have explicit spoken titles, descriptions/help and
JUCE button actions. Native keyboard and VoiceOver interaction still require a
manual pass; vector glyphs alone are not their accessible names.
