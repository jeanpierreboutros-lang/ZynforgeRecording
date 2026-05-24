# MasterStrip

`Source/UI/MasterStrip.{h,cpp}` — the master fader strip pinned to the right edge of the mixer area.

## Description

A `juce::Component` that paints a master fader, dB readout, peak meter, output channel routing (L + R), and a stereo/mono toggle. Sums all strips' post-fader audio into a single master output. Always visible in the MIXER view; hidden in EDIT view (waveforms get the full width).

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
| `refreshOutputs(int deviceOutCount)` | Repopulates the L + R output combos when the audio device changes topology |

## Layout

```
┌─MASTER─┐
│  L ▾   │  output L routing
│  R ▾   │  output R routing
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

The master strip reads from the engine's `masterState` (and `masterStateR` for the stereo case):

| Field | Atomic | What it controls |
|---|---|---|
| `gainDb` | `std::atomic<float>` | Fader position; UI scales by `Decibels::decibelsToGain` |
| `muted` | `std::atomic<bool>` | Output mute |
| `peak` | `std::atomic<float>` | Driven by the audio thread; UI reads at 10 Hz |
| `clipped` | `std::atomic<bool>` | Latches via the audio thread; clears via meter click |

Stereo mode flag lives at `engine.masterStereo`; when off, L pan only and R combo hides.

## States

| State | Visual | Behaviour |
|---|---|---|
| Default | Personality wash neutral grey | Fader + meter active |
| Mono | R combo hidden, single meter | Audio sums to L only |
| Hover | ~6 % brightness lift | Same hover pattern as `ChannelStrip` |
| Clipped | Meter top segment held red for `brand::motion::clipLatchMs` | Click meter to clear |

## Tokens used

- **Colours**: `brand::textPrimary` master label, `brand::accentStatus` for the dB readout, `brand::meterGreen` / `meterAmber` / `meterRed` for meter LEDs, `brand::accentRecord` for clip latch
- **Typography**: `brand::type::channelName()` for "MASTER", `brand::type::mono(11.0f, true)` for the dB readout
- **Spacing**: hardcoded 8 px row pitch (audit candidate — could be `brand::space::md`)
- **Motion**: `brand::motion::clipLatchMs`

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Hide the strip via `setVisible(false)` in EDIT view | Lay it out conditionally — JUCE handles invisible siblings cleanly |
| Read peak / clip from the engine's `masterState` atomics | Subscribe to per-sample audio events to drive the meter — atomic poll is fine at 10 Hz |
| Show R combo only when `masterStereo == true` | Reserve UI space for R combo in mono mode |

## Accessibility

- Fader supports drag + scroll wheel (JUCE default)
- dB readout right-click → context menu with `Reset to 0 dB`
- Stereo/mono toggle has a tooltip
- Meter click clears the clip latch (matches `PeakTally`'s click-to-clear pattern)
