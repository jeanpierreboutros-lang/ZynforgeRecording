# LedMeter

`Source/UI/LedMeter.{h,cpp}` — the segmented forge-heat level meter used on channel strips, the master, and EDIT rows.

## Description

A `juce::Component` + `juce::Timer` that reads a `TrackState`'s atomic peak / RMS / clip values and paints them as the brand's forge-heat ladder: a green safe zone at the bottom climbing through ember → forge-orange → white-hot at clip. It self-throttles (60 Hz while the transport is active or the strip is armed/monitored, 8 Hz at idle) and early-outs the repaint when nothing visibly moved, so a 32-strip mixer doesn't burn paint cost on silence. The scale runs −60 to 0 dBFS; the display holds the higher of the new value and the decayed old one (peak × 0.80, RMS × 0.62 per tick).

## When to use

One per metered surface. Pass the `TrackState&` it should read. For a stereo pair, set the right channel's state via `setStereoPartner` so the one widget paints two bars. The EDIT row uses a narrow instance with labels off. Owners can replace the default tooltip ("Peak + RMS LED meter. Click to clear the clip indicator.") with their own.

## API

```cpp
explicit LedMeter (TrackState& s);
void setStereoPartner (TrackState* r) noexcept;   // nullptr = mono (one bar)
void setShowDbLabels (bool on) noexcept;          // dB gutter on the left
void detach() noexcept;                          // stop reading a condemned TrackState
```

The constructor takes a keep-alive (`shared_ptr`) on a recorder-owned `TrackState`, so the state stays valid after the recorder drops the track, until this meter is rebuilt. Owners still call `detach()` when the strip or row is condemned (`ChannelStrip`, `EditPage::TrackRow`): it stops the meter timer at once, and paint uses cached display values afterwards. The stereo partner is a raw pointer with no keep-alive.

## Variants

| Variant | Width | Labels | Segments |
|---|---|---|---|
| Channel mono | 16 px bar, +16 px scale when the strip has room | on with the scale; the strip turns them off otherwise | 8–20, scaled to height |
| Channel stereo | 26 px, +16 px scale when the strip has room | as mono | two bars, half-width each |
| Master | 26 px mono / 40 px stereo | auto: off in mono, on in stereo | as above |
| EDIT row | 16 px (22 px block, inset) | off | adaptive; single solid heat colour by level under 24 px tall (flat, not a gradient) |

Segment count is `round(height / 4)` clamped to 8–20. With labels off and the meter at least 160 px tall, the same dBFS marks (0 / −6 / −12 / −18 / −30 / −60) still draw as short ticks on the bar.

## States

| State | Visual | Behaviour |
|---|---|---|
| Idle | Unlit segments in `meterIdle` | Timer at 8 Hz, most repaints skipped |
| Signal | Ladder lit to peak; RMS solid, peak at `alpha::scrim` above it | 60 Hz when active/armed/monitored |
| Clip | 5 px white-hot pip latched at the top of the bar | Cleared on `mouseDown` (click to reset), which also clears the stereo partner and both clip counts. Ignored while disabled (session LOCK) or detached |

## Critical layout rule

`paint()` computes the 14 px dB-label gutter and **drops the labels entirely when the meter is narrower than 30 px** (`roomForLabels = showLabels && bounds.getWidth() >= 26`, where `bounds` is the widget inset 2 px each side). This is not just cosmetic: without it, the compact mixer/EDIT widths produce a *negative* bar area, which asserts and crashes `paintBar` (the 2026-06-14 SIGABRT). Any change to the meter's width math must preserve the "labels auto-drop below 30 px → bar gets full width, never negative" guarantee.

## Tokens used

- **Colours**: `brand::meterGreen` / `meterEmber` / `meterHot` / `meterWhiteHot` (the forge-heat ramp, via `brand::meterHeatAt(frac)`: ember above 70 %, hot above 85 %, white-hot above 95 %), `brand::meterIdle` (unlit), `brand::bgDeep` (bar back), `brand::textPrimary` / `textTertiary` (labels / gutter ticks), `brand::textSecondary` (on-bar ticks without labels), `brand::shadow::elev3()` (dims the peak band in the solid small-meter path)
- **Alpha**: `brand::alpha::scrim` (peak-only segments)
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
