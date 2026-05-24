# PeakTally

`Source/UI/PeakTally.h` — global "any strip just clipped" indicator pinned across the top of the mixer area.

## Description

A 4 px tall horizontal bar that pulses brand-red at 2 Hz whenever any channel's clip latch is set, then holds for ~1 s after the last clip event before clearing. Click anywhere on the bar to clear every strip's clip latch in one gesture.

## When to use

Exactly one per `MainComponent`. Sits in row 1 of the layout, ABOVE the strips area but below the header chrome. Always visible — engineers learn to glance at this row for clip status without scanning every meter.

## Constructor

```cpp
PeakTally (AudioEngine& engine);
```

| Param | Type | Notes |
|---|---|---|
| `engine` | `AudioEngine&` | Polled at 10 Hz; iterates `getRecorder().getTrack(i).clipped` atomics |

## Behaviour

| State | Visual | Trigger |
|---|---|---|
| Idle | 4 px transparent strip | No strip's `clipped` atomic is set |
| Latched | 4 px brand-red bar pulsing at 2 Hz | Any strip's `clipped` was true within the last `brand::motion::clipLatchMs` (1000 ms) |
| Holding | Solid red, no pulse | Within the latch hold window after the last clip event |

## Click-to-clear

`mouseDown` walks every track in the recorder and calls `clipped.store(false)`. Engineer can clear the whole rig with one click — no per-strip clicks needed. The bar's latch hold expires naturally if no new clips come in.

## Timer

10 Hz polling timer:
1. Scan every track for `clipped == true`.
2. If any, update `lastClipMs` to current ms.
3. If `nowMs - lastClipMs < clipLatchMs`, paint red.
4. Pulse on/off at 2 Hz while latched (alternates alpha between `prominent` and `bold`).

## Tokens used

- **Colours**: `brand::accentRecord` (4 px bar)
- **Alpha**: `brand::alpha::prominent` / `bold` for pulse states
- **Motion**: `brand::motion::clipLatchMs` (1000 ms hold)

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Read clip latches directly from `TrackState::clipped` atomics | Push clip events from the audio thread — let the latch atomic be the source of truth |
| Pulse to draw the eye from across the venue | Solid-red continuously — engineers will start ignoring it |
| Use 4 px height — visible without taking strip vertical | Make taller — strip viewport is precious |

## Accessibility

- Single click target spans the full window width — no fine motor required
- Brand-red is the same signal-record colour used everywhere, so engineers don't need new colour knowledge
