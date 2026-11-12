# TransportBar

`Source/UI/TransportBar.{h,cpp}` — horizontal row of six icon buttons (START / END / PLAY-PAUSE / STOP / RECORD / LOOP) in the main header.

## Description

Hosts six `IconButton` instances (vector-drawn glyphs, no bitmaps). Polls the engine at 10 Hz to mirror transport state — RECORD reflects `engine.isRecording()` and blinks while a track is armed, PLAY lights its active state while playing, LOOP lights while loop-play is on, and STOP flashes whenever playback or recording stops, whatever stopped it. The host wires `onRequestRecord`, `onRequestPlay` and `onRequestStop` to its preflight/finalization paths; without host callbacks it may control playback, but RECORD does nothing and STOP refuses to end a live take.

## When to use

Exactly one per `MainComponent`. Lives at the left of row 2 of the header layout, 340 px wide. The buttons are squares sized to the bar's height (40 px there) with 4 px gaps, centred horizontally. RECORD action is owned by this bar; there is no other on-screen RECORD button. The host disables the bar under session LOCK.

## Constructor

```cpp
explicit TransportBar (AudioEngine& engine);
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

START, END and LOOP are wired inside the bar. There are no `onSkipBack`/`onSkipFwd` public callbacks. START moves the player to 0, END to the session's total length; both are inert while recording. LOOP toggles loop-play: playback wraps within the selected range, or the whole take with no range. It never makes or clears a range. LOOP refuses to turn on while recording or in PUNCH mode (a loop wrap would keep a punch recording past its out point).

Without a host callback, PLAY toggles playback of a loaded session, and STOP stops and rewinds playback but does nothing while recording.

## Capture-daemon host contract

`engine.isRecording()` includes external capture. Normal live capture keeps the two-tap STOP guard; a deliberate local manual or selected punch ends on one RECORD or STOP press. The host routes these controls through its capture-finalization path. With the current matching protocol-v4 GUI/helper pair, the host waits for a daemon reply that distinguishes command acceptance, capture completion and clean finalization before clearing the recording display, reloading completed media or saving the stopped session. If capture stopped but report/recovery/redundancy finalization failed, the transport resets but the error remains visible; if completion is uncertain, recording state stays armed. A click alone is not evidence that capture has stopped. Playback refuses at the engine boundary during ordinary local/daemon capture; the explicitly gated selected-punch window is the exception that allows pre/punch/post-roll playback.

Local device loss closes the mix writer and queues engine finalization once, preserving the stopped endpoint and warning. Repeated STOP must not reload away later edits. This does not remove the need to test physical device loss.

While metadata persistence is busy, the host still lets Space stop transport
that is already playing or recording. This does not authorize starting playback,
changing a finalizing take, bypassing LOCK, or skipping ordinary two-tap STOP.
Remote STOP returns pending until required media/metadata completion; an idle
capture flag alone is not the acknowledgment. Generic OSC taps reach the same
two-tap guard: a press is `/zynforge/stop 1`, `/zynforge/stop 0` and
`/zynforge/record 0` are releases and count for nothing, a `/zynforge/stop`
with no value is refused (since 2026-11-11), STOP presses within 250 ms of the
last one count once, and stopping over Generic OSC is `/zynforge/stop` only
(`/zynforge/play 0` still stops playback, except during a selection punch,
when a remote stop of playback is refused: STOP ends the punch).

After a daemon STOP, a status the helper built before the command is dropped,
and the window adopts a rolling take only from a status received after the take
it last ended (`daemonTakeEndedAtMs`). A stale "recording" status can no longer
bring a finished take back onto the bar.

Nothing modal opens over a rolling take, because a modal swallows the click on
STOP and takes Space. The host greys the menu items that open one while a take
rolls, and a take that starts behind an open prompt closes the prompts marked
`closesForTake` (see [DialogChrome](DialogChrome.md)).

## IconButton (nested)

Each transport button is an `IconButton` — a custom-painted button that draws a vector glyph (target ring for RECORD, triangle for PLAY, square for STOP) coloured by the button's "base colour." State logic:

| State | Background and glyph |
|---|---|
| Idle | Flat `controlBg` body, 1 px `edge` outline, per-button accent glyph; RECORD keeps a 2 px `accentRecord` outline at `alpha::prominent` instead |
| Hover / pressed | Flat `controlBgHover` / `controlBgDown` |
| Recording | Solid `accentRecord` body, opaque red outline, `onSignal(accentRecord)` glyph |
| Armed, not recording | Blinks at 1 Hz (0.5 s on / 0.5 s off, off the 10 Hz timer): the on half is a `sink(accentRecord, 0.30)` body with an opaque red outline; the off half is idle. Armed means any track armed with the transport stopped, the same signal as the big clock's |
| PLAY / LOOP active | Their accent-coloured body (`accentPlay`; `selectFill` for LOOP), a 2 px `lift(…, 0.20)` ring and `onSignal(baseColour)` glyph |
| STOP flash | A `textSecondary` wash at up to 85 % over the button, decaying ×0.55 per tick |

## RECORD button shape distinctness

Per a 2026-05-23 UX audit, the RECORD button has a **permanent brand-red 2 px border** at idle so its silhouette differs from PLAY's at-a-glance under stage glare. The glyph is a **concentric-ring target** (outer ring + inner disc) instead of a solid circle. Engineers can read RECORD vs PLAY by silhouette alone without relying on colour.

## Tokens used

- **Colours**: `brand::accentRecord` (RECORD base), `brand::accentPlay` (PLAY), `brand::textSecondary` (STOP, and LOOP at rest), `brand::selectFill` (LOOP on), `brand::textPrimary` (START/END), `brand::controlBg` / `controlBgHover` / `controlBgDown` for the three hover states, `brand::edge` for idle outlines
- **Alpha**: `brand::alpha::prominent` (idle RECORD outline)
- **Typography**: none directly — buttons paint glyphs, not text
- Geometry is defined in `TransportBar.cpp`; the current tile uses a 9 px radius. Do not infer token use from earlier specifications.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Wire `onRequestStop` to the host's normal two-tap guard and deliberate-punch one-press path | Stop an ordinary live take with one accidental press |
| Trust the bar's own 10 Hz poll for visual state | Push transport state in via setters — the bar reads engine atomics directly |
| Use the same RECORD shape language elsewhere if you build a new record button | Make a square RECORD or a red triangle — breaks the silhouette agreement |

## Accessibility

The bar is a focus container titled "Transport". Each icon button has a spoken
title ("Go to start", "Go to end", "Play", "Stop", "Record", "Loop region"),
its tooltip as help text and JUCE button actions, and is reachable with Tab. A
mouse click does not leave keyboard focus on a button, so Return can't press it
again (two Returns would end a take). Native keyboard and VoiceOver interaction
still require a manual pass; vector glyphs alone are not their accessible names.
