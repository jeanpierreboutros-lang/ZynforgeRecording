# BigClockPanel

`Source/UI/BigClockPanel.{h,cpp}` — large transport-state display in the main window's clock row, above the mixer in MIXER and above the tracks in EDIT.

## Description

A `juce::Component` that paints a state lamp and label (`IDLE` / `ARMED` / `REC` / `PLAY`), an `HH:MM:SS` timer in bold tabular mono, a large record-time-left slot, and up to four disk-health rows (`FREE`, missed samples, `LAST WRITE`, `MARKERS`). The breathe animation runs only while recording (gated via `syncPulseTimer()`).

The clock row is 72 px tall in MIXER and 56 px in EDIT. The status panel (`PerfDashboard`, 332 px) sits to its right, and in EDIT the edit tools bar sits between them. Master loudness and true peak live on that status panel, not on the clock.

The component is opaque: `paint()` fills its bounds with `bgDeep` before drawing the plate, so its own repaints don't force the parent to repaint.

## When to use

Exactly one instance per `MainComponent`. The host's 10 Hz timer pushes state via `setMode` / `setElapsed` / `setMarkers` / `setDiskInfo` / `setArmedReady`. Each setter does change-detection before triggering a repaint; `setDiskInfo` rounds free space to 0.1 GB and time left to whole seconds first, so float jitter doesn't repaint.

## Constructor

When daemon capture is active, the host supplies the daemon's timeline position and sample rate from `EngineStatus`, not the idle local player's counters. The panel formats the values; it does not infer capture success or synchronization. The host separately surfaces protocol-v4 primary/backup/mirror/recovery/report and disk-health fields, including latched failures after stop. Local and external capture use the same recording-state guard.

```cpp
BigClockPanel();
```

No params — state is entirely push-driven via setters.

## Public methods

| Method | Purpose |
|---|---|
| `setMode(Mode)` | Mode enum: `Idle`, `Recording`, `Playing`. Entering `Recording` restarts the heat ramp. |
| `setElapsed(juce::int64 samples, double sampleRate)` | Formats to `HH:MM:SS`; a rate ≤ 0 falls back to 48 kHz |
| `setMarkers(int count)` | `MARKERS` row value |
| `setDiskInfo(double freeGB, int lastWriteMs, juce::int64 missedSamples, double remainingSeconds, DiskReading reading)` | Time-left slot and disk-health rows. `DiskReading`: `Fresh`, `CopyStale`, `PrimaryStale` |
| `setArmedReady(bool)` | Lights the lamp amber and reads `ARMED`. No border, no pulse. |

## States

| State | Visual | Pulse |
|---|---|---|
| `Idle` | `bgPanel` plate, 1 px `edge` border, muted lamp, `IDLE`; the host passes 0, so the timer reads 00:00:00 | No |
| `Recording` | Whole plate turns `takePlate` (80–100 % mix, ramped in over ~300 ms), 2 px `accentRecord` border, white lamp and type, `REC` at display size when the row is tall; timer counts up | 1 Hz sine breathe |
| `Playing` | `accentPlay` lamp and timer on an `accentPlay` wash at `alpha::edgeSoft`; `PLAY` | No |
| Armed-ready | Amber lamp (`signalArmedReady()`) and `ARMED`; otherwise as `Idle` | No |

Disk readings:

- With no estimate (`remainingSeconds` ≤ 0 or infinite) the large slot reads `DISK FREE` with free GB, and the `FREE` row is dropped. Otherwise it reads `RECORD TIME LEFT` as `NhMMm`.
- `CopyStale`: the caption turns `alertAmber` and reads `PRIMARY TIME LEFT`, since a backup drive isn't answering.
- `PrimaryStale`: free space and time left read `STALE` in `alertAmber`.
- A negative `freeGB` reads `--`.
- Missed samples > 0 read `MISSED SAMPLES` with the count in `accentRecord` (white on the take plate); otherwise the row reads `NO MISSED WRITES`.

The timer keeps its width. The right-hand column takes what is left, up to 300 px. A row that doesn't fit (under 16 px tall or 136 px wide) isn't drawn; rows give way in the order `MARKERS`, `LAST WRITE`, missed samples, `FREE`.

## Pulse animation

`syncPulseTimer()` starts a 15 Hz pulse timer only when reduced motion is off and `mode == Recording`. Other states stop the timer and reset the phase; host status updates may still repaint when values change. Only `setMode` calls `syncPulseTimer()`.

Each tick advances the phase by 2π/15 (one full sine per second), steps the ~300 ms heat ramp, and repaints the panel. Under reduced motion the plate is drawn static at mid-pulse and full heat. This bounds animation work; it does not establish total application idle CPU, which also depends on audio, meters, session state and other timers.

## Tokens used

- **Colours**: `brand::takePlate` / `accentRecord` for a rolling take; `accentPlay` for playback; `signalArmedReady()` (`engagedAmber`) for the armed lamp; `alertAmber` for stale disk readings; `bgDeep` base fill, `bgPanel` plate, `edge` idle border; `textPrimary` / `textSecondary` / `textMuted` for type and the idle lamp
- **Typography**: `brand::type::mono(…, true)` for the timer, sized to 0.95 × the inner height and capped at `brand::type::h_hero` (48 pt); `type::ui(h_display or h_subhead, true)` for the state label; `type::caption()` for the time-left caption; `type::mono(h_subhead or h_headline, true)` for its value; `type::channelName()` / `type::mono(h_body, true)` for row labels / values
- **Spacing**: `brand::space::lg` side inset, `space::xs` vertical inset, `space::md` lamp-to-label gap, `space::xl` padding around the timer
- **Radius**: `brand::radius::lg` on the plate
- **Motion**: no token read; the 1 Hz breathe comes from the local `kPulseHz` (15) tick, matching `brand::motion::pulseHz` (1.0)

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Call `setMode` / `setArmedReady` from a slow (≤ 30 Hz) host poll | Trigger repaint on every audio callback |
| Trust the panel's own change detection — no need to gate at the call site | Bypass `syncPulseTimer()` by calling `repaint()` directly when idle |
| Show armed-ready only when at least one strip is REC-armed and transport is stopped | Confuse armed-ready with `Recording` — different signal meaning |
