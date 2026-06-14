# FineFader

`Source/UI/FineFader.h` — the app-wide vertical gain fader used by every level control (channel, VCA, bus, master).

## Description

A thin `juce::Slider` subclass with one behavioural contract: **the cap moves only when the engineer click-drags it, and it sticks to the pointer.** It deliberately ignores the scroll wheel / trackpad so a stray two-finger swipe over the mixer can never nudge a live level. Visually it's the heated-steel fader (machined cap, dB ruler, tight meter cluster) but the FineFader class itself only governs interaction; the steel look is painted by the host strip's LookAndFeel.

## When to use

Every level fader in the app is a FineFader — there is no other fader class. Channel strips, the VCA panel, bus masters, and the master strip all instantiate it so the "click-drag only, snaps to pointer" rule is universal. If you add a new level control, use FineFader; do not fall back to a raw `juce::Slider`.

## API

```cpp
struct FineFader : juce::Slider
{
    void mouseWheelMove (const juce::MouseEvent& e,
                         const juce::MouseWheelDetails& wheel) override;  // ignores + propagates
};
```

Constructed like any slider, then configured by the host:

```cpp
fader.setSliderStyle (juce::Slider::LinearVertical);
fader.setSliderSnapsToMousePosition (true);   // cap tracks the pointer (NOT relative drag)
fader.setRange (-60.0, 12.0);
```

## States

| State | Visual | Behaviour |
|---|---|---|
| Default | Cap at current dB | — |
| Hover | Cap edge lifts (`brand::tint::hover`) | — |
| Dragging | Cap follows pointer exactly | `setSliderSnapsToMousePosition(true)` — absolute, not relative |
| Wheel / trackpad over fader | **No change** | `mouseWheelMove` swallows the gesture and propagates it to the parent viewport for scrolling |
| Disabled | Dimmed cap | Non-interactive |

## The two rules (and why)

1. **Snaps to pointer.** `setSliderSnapsToMousePosition(true)` is mandatory. With it `false` (JUCE default), a click jumps to a *relative* drag origin and the cap "drops up or down" away from the pointer — the exact bug a sound engineer feels as the fader "not sticking." Always set it true.
2. **No wheel.** `mouseWheelMove` is overridden to call the base `juce::Component::mouseWheelMove` (propagate to parent for list scrolling) instead of `juce::Slider`'s (which would change the value). A mixer is a dense scroll surface; an accidental wheel-over must scroll, never re-gain a channel.

## Tokens used

- **Colours**: host-painted (steel cap via the strip LookAndFeel); groove uses `brand::structuralForge()` at `brand::alpha::wash` / `0.78f`
- FineFader itself holds no tokens — it is interaction-only. The look lives in `ChannelStrip` / `MasterStrip` paint.

## Accessibility

- Inherits `juce::Slider`'s slider role + value text. Host sets the title (e.g. "Channel gain").
- Keyboard arrows still adjust value (only the *wheel* is suppressed, not the keyboard).

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Use FineFader for every level control | Drop a raw `juce::Slider` in for a "quick" fader — it'll scroll-nudge and relative-drag |
| Always `setSliderSnapsToMousePosition(true)` | Leave snapping off — the cap won't stick to the pointer |
| Let the wheel propagate for scrolling | Re-enable wheel-to-value "for convenience" — it's the #1 live-mix footgun |
