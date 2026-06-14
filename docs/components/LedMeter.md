# LedMeter

`Source/UI/LedMeter.{h,cpp}` — the segmented forge-heat level meter used on channel strips, the master, and EDIT rows.

## Description

A `juce::Component` + `juce::Timer` that reads a `TrackState`'s atomic peak / RMS / clip values and paints them as the brand's forge-heat ladder: a green safe zone at the bottom climbing through ember → forge-orange → white-hot at clip. It self-throttles (60 Hz while the transport is active or the strip is armed/monitored, 8 Hz at idle) and early-outs the repaint when nothing visibly moved, so a 32-strip mixer doesn't burn paint cost on silence.

## When to use

One per metered surface. Pass the `TrackState&` it should read. For a stereo pair, set the right channel's state via `stereoR` so the one widget paints two bars. The EDIT row uses a narrow instance with labels off.

## API

```cpp
explicit LedMeter (TrackState& s);
void setStereoRight (TrackState* r);   // nullptr = mono (one bar)
void setShowLabels (bool on);          // dB gutter on the left
```

## Variants

| Variant | Width | Labels | Segments |
|---|---|---|---|
| Mixer mono | ~16 px | auto (off when < 30 px) | 8–20, scaled to height |
| Mixer stereo | ~26 px | auto | two bars, half-width each |
| EDIT row | narrow | off | adaptive; smooth gradient under ~24 px tall |

## States

| State | Visual | Behaviour |
|---|---|---|
| Idle | Green floor segments dark | Timer at 8 Hz, most repaints skipped |
| Signal | Ladder lit to peak; RMS solid, peak half-alpha above it | 60 Hz when active/armed/monitored |
| Clip | White-hot pip latched at top | Cleared on `mouseDown` (click to reset) |

## Critical layout rule

`paint()` computes the dB-label gutter width and **drops the labels entirely when the meter is narrower than 30 px** (`roomForLabels = showLabels && width > 30`). This is not just cosmetic: without it, the compact mixer/EDIT widths produce a *negative* bar area, which asserts and crashes `paintBar` (the 2026-06-14 SIGABRT). Any change to the meter's width math must preserve the "labels auto-drop below 30 px → bar gets full width, never negative" guarantee.

## Tokens used

- **Colours**: `brand::meterGreen` / `meterEmber` / `meterHot` / `meterWhiteHot` (the forge-heat ramp, via `brand::meterHeatAt(frac)`), `brand::meterIdle` (unlit), `brand::bgDeep` (bar back), `brand::textPrimary` / `textTertiary` (labels + ticks)
- **Typography**: `brand::type::label()` (dB gutter)
- **Radius**: `brand::radius::sm` (bar), `1.5f` micro (segments)

## Accessibility

- TODO: live values via `AccessibilityValueInterface` (tracked in `tasks.md` accessibility item). Today it sets a tooltip only.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Let the meter own its refresh cadence | Drive it from an external timer — it throttles itself |
| Keep peak/RMS/clip on the atomic `TrackState` | Pass values from the message thread per-frame — the audio thread publishes atomics |
| Preserve the < 30 px label-drop when changing widths | Reintroduce a fixed label gutter — it makes `barArea` negative and crashes at compact widths |
| Use `meterHot`/white-hot for *signal* only | Reuse the hot meter colours for chrome — chrome is `structuralForge()` (ember-subdued) |
