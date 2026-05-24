# BigClockPanel

`Source/UI/BigClockPanel.{h,cpp}` — large transport-state display rendered above the mixer in the main window's header area.

## Description

A `juce::Component` that paints a numeric timer (Mins:Secs, 44 pt mono bold), transport-state label (`IDLE` / `RECORDING` / `PLAYING` / `ARMED-READY`), marker count chip, disk-free + headroom indicator, and an optional pulse animation. The breathe animation only runs while recording or armed-ready (gated via `syncPulseTimer()`).

## When to use

Exactly one instance per `MainComponent`. The host's 10 Hz timer pushes state via `setMode` / `setElapsed` / `setMarkers` / `setDiskInfo` / `setArmedReady`. Each setter does change-detection before triggering a repaint.

## Constructor

```cpp
BigClockPanel();
```

No params — state is entirely push-driven via setters.

## Public methods

| Method | Purpose |
|---|---|
| `setMode(Mode)` | Mode enum: `Idle`, `Recording`, `Playing` |
| `setElapsed(juce::int64 samples, double sampleRate)` | Formats to `HH:MM:SS` |
| `setMarkers(int count)` | Marker count chip update |
| `setDiskInfo(double freeGB, int lastWriteMs, int64 missedSamples, double headroomSec)` | Disk-health pill |
| `setArmedReady(bool)` | When true and `Mode::Idle`, paints amber border + pulses |

## States

| State | Visual | Pulse |
|---|---|---|
| `Idle` | Neutral chrome, timer at 00:00:00 (or last position) | No |
| `Recording` | `accentRecord` background bleed; timer counts up | 1 Hz sine breathe |
| `Playing` | `accentPlay` accent on the timer | No |
| `Armed-Ready` | `engagedAmber` border drawn around the panel | 1 Hz sine breathe |

## Pulse animation

`syncPulseTimer()` starts the 30 Hz timer **only** when `mode == Recording || armedReady`. Idle states stop the timer outright (no background CPU). Setters that change `mode` or `armedReady` call `syncPulseTimer()` to maintain this invariant.

This gating is critical — without it, the panel repaints 30× per second on idle, which was the root of a 7-10% idle-CPU regression.

## Tokens used

- **Colours**: `brand::accentRecord` / `accentPlay` / `engagedAmber` for state accents; `brand::bgPanel` background; `brand::textPrimary` / `textSecondary` for label text
- **Typography**: `brand::type::hero()` for the 44 pt timer numerals; `brand::type::caption()` for labels; `brand::type::display()` for big numbers (marker count chip)
- **Spacing**: `brand::space::md` between panel sections
- **Radius**: `brand::radius::xl` on the outer rounded panel
- **Motion**: `brand::motion::pulseHz` (1.0) — defines the 1 Hz breathe rhythm

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Call `setMode` / `setArmedReady` from a slow (≤ 30 Hz) host poll | Trigger repaint on every audio callback |
| Trust the panel's own change detection — no need to gate at the call site | Bypass `syncPulseTimer()` by calling `repaint()` directly when idle |
| Show `Armed-Ready` only when at least one strip is REC-armed and transport is stopped | Confuse `Armed-Ready` with `Recording` — different signal meaning |
