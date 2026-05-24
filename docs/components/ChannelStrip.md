# ChannelStrip

`Source/UI/ChannelStrip.{h,cpp}` — the per-channel vertical strip rendered in the MIXER view. One per logical strip (stereo pairs collapse to one).

## Description

A `juce::Component` that paints the strip background wash, header (colour swatch, name, REC/MON/MUTE/SOLO toggles), routing combos, fader + dB ruler, pan knob(s), peak meter, mini spectrum, and any state badges (VCA, BUS, Edit Group, R/W LED, Safe LED). Mutates `TrackState` directly for fast state, and uses callbacks for actions that need engine-level effects (rename, colour, gain, pan, routing).

## When to use

Always one per logical strip in the MIXER view. Stereo pairs use a single `ChannelStrip` with `pairState` set to the R channel's `TrackState`.

## Variants

| Variant | How it's distinguished | Visual |
|---|---|---|
| Mono | `pairState == nullptr` | Single pan knob centred under fader |
| Stereo pair | `pairState != nullptr` | Two pan knobs (L + R), independent |
| Bus track | `state.isBus.load()` returns `true` | "BUS" amber chip top-right; no R/MON buttons; no input combo |
| VCA-grouped | `state.vcaGroup.load() >= 0` | "V N" teal chip top-right |
| Edit-group member | `state.editGroup.load() >= 0` | "EG N" warm-amber chip below VCA slot |

## Constructor

```cpp
ChannelStrip (int index, TrackState& state,
              ColourCallback onColourPicked  = {},
              NameCallback   onRename        = {},
              FloatCallback  onGainDb        = {},
              FloatCallback  onPan           = {},
              IntCallback    onInputRouted   = {},
              IntCallback    onOutputRouted  = {},
              TrackState*    stereoPartner   = nullptr,
              FloatCallback  onPanR          = {});
```

| Param | Type | Required | Notes |
|---|---|---|---|
| `index` | `int` | yes | 0-based physical strip index (matches `Track_NN.wav` numbering minus 1) |
| `state` | `TrackState&` | yes | Strip's mutable state. Atomics read/written by both UI and audio thread |
| `onColourPicked` | `std::function<void(Colour)>` | no | Fires when the engineer picks a colour in the swatch popup |
| `onRename` | `std::function<void(String)>` | no | Fires when the inline name editor commits |
| `onGainDb` | `std::function<void(float dB)>` | no | Fires on fader move. Host typically writes automation + broadcasts to edit-group peers |
| `onPan` / `onPanR` | `std::function<void(float)>` | no | Pan -1..+1 |
| `onInputRouted` / `onOutputRouted` | `std::function<void(int)>` | no | Hardware channel index, or -1 for unrouted |
| `stereoPartner` | `TrackState*` | no | When set, treats this as a stereo pair |

## Public methods

| Method | Purpose |
|---|---|
| `setMenuCallbacks(...)` | Wires delete / add / link-stereo handlers for the right-click menu |
| `setAvailableInputs(int n)` / `setAvailableOutputs(int n)` | Populates the routing combo entries when the device topology changes |
| `refreshRoutingSelection()` | Re-reads `TrackState`'s routing atomics; called on the 10 Hz timer to mirror PATCH-view changes |
| `refreshAppearance()` | Re-reads name + colour; cached against `lastAppliedColour` so repeat ticks are no-ops |
| `setSelected(bool)` / `isSelected()` | Multi-select highlight. Engineer toggles via shift/cmd-click |
| `setAutomationLed(bool writeArmed, bool safeOn)` | Updates the 6 px R/W LED in the strip header. Host polls every 10 Hz |
| `onAfterArmedToggle` etc. | Optional callbacks fired AFTER local state mutation. Host uses them to broadcast to edit-group peers |

## States

| State | Visual | Behaviour |
|---|---|---|
| Default | Strip wash in personality colour, neutral chrome | Hover lift on `mouseEnter` |
| Hovered | ~6 % brightness lift on the wash + brighter edge | `mouseEnter` flips `hovered = true` and repaints; `mouseExit` reverts |
| Selected | 2 px `accentSolo` outline overlay | Set via `setSelected(true)`; host manages the selection set |
| Armed (REC on) | "R" button glows `accentRecord` | Audio-thread signal-arm path is independent of paint state |
| Monitoring | "I" button glows `accentPlay` | Drives meter when not playing/recording |
| Muted | "M" button glows `brandOrange`; lane content dimmed | |
| Soloed | "S" button glows `accentSolo` | Other strips' mute behaviour controlled by engine |
| Edit-group writing | Red 6 px LED below colour swatch | Polled from `engine.isAutomationWriting()` + `engine.isTrackAutomationSafe()` |
| Automation Safe | Amber 6 px LED, replaces the red | Engine refuses every write while on |

## Tokens used

- **Colours**: `brand::stripColour(index)` for personality wash, `brand::accentRecord` / `accentPlay` / `accentSolo` / `brandOrange` for signal state, `brand::featureEngaged` for VCA chip, `brand::accentVS` for Edit Group chip, `brand::alertAmber` for BUS chip
- **Typography**: `brand::type::caption()` for label text, `brand::type::channelName()` for the strip name
- **Spacing**: `brand::space::xs` / `sm` / `md` between buttons and groups; some literal pixels still in the layout (audit TODO)
- **Radius**: `brand::radius::sm` for chips, `brand::radius::md` for the swatch
- **Alpha**: `brand::alpha::dimmed` for the swatch outline sheen

## Right-click menu

Surfaces the per-strip actions that don't fit in the strip header: Rename, Add channel, Delete channel, Link/Unlink stereo pair, Change/Reset colour, Reset name, Send to STREAM bus, Mute physical output, Automation Safe toggle, Assign to VCA submenu, Assign to Edit Group submenu, Send to bus submenu.

## Accessibility

- **Keyboard**: not yet wired; strips are mouse-driven for now. Multi-select via shift/cmd-click; deselect with Esc.
- **Tooltips**: every button + combo has a `setTooltip` call; engineers see hover help for `R`/`I`/`M`/`S`/routing combos/pan/dB readout.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Read `TrackState` atomics for fast UI mirroring | Block the audio thread by holding a UI lock while audio reads |
| Wire `on*Toggle` callbacks for edit-group broadcast | Mutate `state.editGroup` from inside ChannelStrip (host owns persistence) |
| Use `setAutomationLed` from a slow (10 Hz) host poll | Animate the LED at 60 Hz — it's a status indicator, not a meter |
| Listen to `onToggleSelection (additive)` for shift/cmd-click | Implement multi-select inside the strip — host owns the selection set |
