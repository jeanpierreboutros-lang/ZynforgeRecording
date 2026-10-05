# MasterStrip

`Source/UI/MasterStrip.{h,cpp}` — the master fader strip pinned to the right edge of the mixer area.

## Description

A `juce::Component` that paints a master fader, dB readout, peak meter, one output-routing combo (a consecutive pair in stereo mode, one channel in mono), and a stereo/mono toggle. Sums all strips' post-fader audio into a single master output. Always visible in the MIXER view; hidden in EDIT view (waveforms get the full width).

## When to use

Exactly one per `MainComponent`. Width 140 px, full strip-area height. Layout via `MainComponent::resized()` removes it from the right edge before the channel strips lay out.

## Constructor

```cpp
MasterStrip (AudioEngine& engine);
```

## Public methods

| Method | Purpose |
|---|---|
| `setVisible(bool)` | Host hides on EDIT view |
| `refreshOutputs()` | Repopulates the output combo when the audio device changes topology |

## Layout

```
┌─MASTER─┐
│Out1-2 ▾│  pair in stereo, single channel in mono
│  [ST]  │  stereo / mono toggle
│   |    │
│   |    │  fader (-60..+12 dB)
│   ●    │
│   |    │
│   |    │
│ -12.3  │  dB readout (mono bold)
│ ▮▮▮▮  │  peak meter (LedMeter)
└────────┘
```

## State sources

The master strip reads from `engine.getMasterState()` (and `getMasterStateR()` for the stereo case):

| Field | Atomic | What it controls |
|---|---|---|
| `gainDb` | `std::atomic<float>` | Fader position; UI scales by `Decibels::decibelsToGain` |
| `muted` | `std::atomic<bool>` | Output mute |
| `peak` | `std::atomic<float>` | Driven by the audio thread; UI reads at 10 Hz |
| `clipped` | `std::atomic<bool>` | Latches via the audio thread; clears via meter click |

Use `engine.getMasterStereo()` / `setMasterStereo()` for mode and `setMasterOutputs()` for routing. The single combo lists output pairs in stereo and individual outputs in mono.

## States

| State | Visual | Behaviour |
|---|---|---|
| Default | Personality wash neutral grey | Fader + meter active |
| Mono | Single-channel routing choices, mono meter | Audio sums to the selected mono output |
| Hover | ~6 % brightness lift | Same hover pattern as `ChannelStrip` |
| Clipped | Meter clip pip follows the latched `TrackState::clipped` flag | Click meter to clear |

## Tokens used

- **Colours**: `brand::textPrimary` master label, `brand::accentStatus` for the dB readout, `brand::meterGreen` / `meterAmber` / `meterRed` for meter LEDs, `brand::accentRecord` for clip latch
- **Typography**: `brand::type::channelName()` for "MASTER", `brand::type::mono(11.0f, true)` for the dB readout
- **Spacing**: hardcoded 8 px row pitch (audit candidate — could be `brand::space::md`)

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Hide the strip via `setVisible(false)` in EDIT view | Lay it out conditionally — JUCE handles invisible siblings cleanly |
| Read peak / clip from the engine's `masterState` atomics | Subscribe to per-sample audio events to drive the meter — atomic poll is fine at 10 Hz |
| Refresh pair/single-channel choices when mode changes | Document or add independent L/R combos without changing the routing contract |

## Accessibility

- Uses `FineFader`: drag and keyboard adjust gain; wheel/trackpad gestures propagate for scrolling without changing level
- Double-click the fader to reset to 0 dB
- Stereo/mono toggle has a tooltip
- Meter click clears the local clip latch; there is no global PeakTally bar
