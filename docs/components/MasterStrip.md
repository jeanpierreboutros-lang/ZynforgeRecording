# MasterStrip

`Source/UI/MasterStrip.{h,cpp}` — the master fader strip pinned to the right edge of the mixer area.

## Description

A `juce::Component` with a master fader, dB readout, peak + RMS meter, one output-routing combo (a consecutive pair in stereo mode, one channel in mono), a stereo/mono toggle and a MUTE toggle. The master bus it controls sums every audible channel's virtual-soundcheck playback plus every channel with MON on; mute and solo gates apply. Always visible in the MIXER view; hidden in EDIT view (waveforms get the full width).

## When to use

Exactly one per `MainComponent`. Width 140 px, full strip-area height. Layout via `MainComponent::resized()` removes it from the right edge (plus a `space::md` gap) before the channel strips lay out.

## Constructor

```cpp
explicit MasterStrip (AudioEngine& engine);
```

Meters `engine.getMasterState()`, with `getMasterStateR()` as the stereo partner in stereo mode. Starts a 10 Hz sync timer.

## Public methods

| Method | Purpose |
|---|---|
| `setVisible(bool)` | Host shows it only in the MIXER view |
| `refreshOutputs()` | Rebuilds the output combo. The strip's own 10 Hz timer calls it; it rebuilds only when the device's active output count or the stereo mode changed |

## Layout

```
┌◇ MASTER ┐  forge mark + title on the dark header plate
│  [ST]   │  stereo / mono toggle (reads ST or MONO)
│Out 1-2 ▾│  pair in stereo, single channel in mono
│ MIX OFF │  only while the mix is held off its pair
│ [MUTE]  │
│ -12.3 dB│  gain readout
│  |  ▮▮  │
│  ●  ▮▮  │  fader (-60..+12 dB) beside the meter (LedMeter)
│  |  ▮▮  │
│ -12.3 dB│  fader value box
│ZYNFORGE │  etched maker's mark
└─────────┘
```

The meter is 26 px wide in mono and 40 px in stereo. The fader runs −60 to +12 dB in 0.1 dB steps, skewed around −15 dB, and double-click returns it to 0 dB.

## Output combo

Stereo lists pairs (`Out 1-2`, `Out 3-4`, …), mono lists single outputs. The list shows at least 8 entries either way; entries past the device's active outputs read `(off)`. The last item, `Off (no output)`, calls `setMasterOutputs(-1, -1)`: the master still meters but lands on no interface channel.

## State sources

The master strip reads from `engine.getMasterState()` (and `getMasterStateR()` for the stereo case):

| Field | Atomic | What it controls |
|---|---|---|
| `gainDb` | `std::atomic<float>` | Fader position, via `getMasterGainDb()` / `setMasterGainDb()` |
| `muted` | `std::atomic<bool>` | Output mute, via `getMasterMuted()` / `setMasterMuted()` |
| `peak` / `rms` | `std::atomic<float>` | Driven by the audio thread; the meter samples them on its own timer (8–60 Hz) |
| `clipped` | `std::atomic<bool>` | Latches via the audio thread; clears via meter click |

Use `engine.getMasterStereo()` / `setMasterStereo()` for mode and `setMasterOutputs()` for routing. The 10 Hz timer pulls gain, mute, output, mode and held-off state back from the engine, so changes made elsewhere (session restore, a MIDI control surface) show on the strip.

## States

| State | Visual | Behaviour |
|---|---|---|
| Default | `bgStrip` plate with a 30 px `steelMasterHi` header, `edge` outline | Fader + meter active |
| Mono | Toggle reads `MONO`, single-channel routing choices, one meter bar | Audio sums to the selected mono output |
| Muted | MUTE toggle ticked in `accentRecord` | Master output muted |
| Held off | Combo outline turns `accentRecord`, `MIX OFF: STRIP ON PAIR` shows under it, tooltip explains | The audio callback keeps the mix off its pair because a strip is patched to one of those outputs (`isMasterHeldOff()`) |
| Clipped | Meter clip pip follows the latched `TrackState::clipped` flag | Click meter to clear |
| Disabled | JUCE disabled controls | Host disables the strip under session LOCK |

## Tokens used

- **Colours**: `brand::bgStrip` plate, `brand::steelMasterHi` header plate, `brand::edge` plate + combo outline, `brand::gloss()` for the cold forge mark and the etched mark's lip, `brand::bgDeep` for the combo, fader track and etched mark face, `brand::textPrimary` for title, readout, combo and toggle text, `brand::textTertiary` combo arrow, `brand::selectFill` stereo toggle tick, `brand::accentRecord` MUTE tick and held-off warning, `brand::faderFill` fader thumb; meter colours come from `LedMeter`
- **Typography**: `brand::type::sectionTitle()` for "MASTER", `brand::type::caption()` for the gain readout, `brand::type::micro()` for the held-off label, `brand::type::label()` for the etched mark
- **Spacing**: `brand::space::md` / `lg` inset; rows sized by `space::ioH` / `ctrlH` / `btnH` / `xl`, gaps `space::sm` / `xs`
- **Radius**: `brand::radius::lg` on the plate

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Hide the strip via `setVisible(false)` in EDIT view | Lay it out conditionally — JUCE handles invisible siblings cleanly |
| Read peak / clip from the engine's `masterState` atomics | Subscribe to per-sample audio events to drive the meter — the meter's own atomic poll is enough |
| Refresh pair/single-channel choices when mode changes | Document or add independent L/R combos without changing the routing contract |

## Accessibility

- The strip is a focus container titled "Master"; the fader, MUTE, mode toggle and output combo have spoken titles ("Master gain", "Master mute", "Mono / stereo", "Master output"), and the toggle and combo carry their tooltips as help text
- Uses `FineFader`: drag and keyboard adjust gain; wheel/trackpad gestures propagate for scrolling without changing level
- Double-click the fader to reset to 0 dB
- Stereo/mono toggle and output combo have tooltips
- Meter click clears the local clip latch; there is no global PeakTally bar
