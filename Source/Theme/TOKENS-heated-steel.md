# Heated Steel — token promotion (source of truth)

The Heated Steel surfaces use a handful of new colours. Today they live in the
hand-written `BrandColors.h` (which is correct — it's not generated). This file
is the **handoff to lock them into the canonical pipeline** so ZynForge **Live**
and the rest of the family inherit them.

`ForgeTokens.h` is GENERATED (`ZynForgeBrand/generate.py` from `tokens.json`) and
marked DO-NOT-EDIT — so the new values must be added **upstream**, not here.

---

## 1. Add to `tokens.json` (ZynForgeBrand repo)

In the `colors` block:

```json
"steelHeaderHi": "#0b0b0e",
"steelHeaderLo": "#000000",
"debossFace":    "#b4b7bf"
```

Notes:
- `debossInk` reuses the existing `#000000` (already `inputBg`) — no new token.
- `structuralForge` is an **alias** of `brandOrange` (#ff7733), not a new colour.
  Keep it as an accessor (below) so the identity-orange can later diverge from
  the mute-state orange without a find-and-replace.

## 2. Re-run the generator

```
python ZynForgeBrand/generate.py    # emits the new lines into ForgeTokens.h
```

It will add to `namespace zynforge::forge`:

```cpp
inline constexpr unsigned int steelHeaderHi = 0xff0b0b0e;
inline constexpr unsigned int steelHeaderLo = 0xff000000;
inline constexpr unsigned int debossFace    = 0xffb4b7bf;
```

Re-vendor the regenerated `ForgeTokens.h` into each consuming app.

## 3. Point `BrandColors.h` at the generated values

Replace the literal mirrors in the Heated Steel block with:

```cpp
inline const auto steelHeaderHi  = juce::Colour (forge::steelHeaderHi);
inline const auto steelHeaderLo  = juce::Colour (forge::steelHeaderLo);
inline const auto debossInk      = juce::Colour (forge::inputBg);      // == #000000
inline const auto debossFace     = juce::Colour (forge::debossFace);
inline juce::Colour structuralForge() noexcept { return brandOrange; }
```

(Until step 2 ships, `BrandColors.h` keeps the literal values so the app builds
standalone — functionally identical, just not yet flowing from `forge::`.)

## 4. Components already consume the tokens

`ChannelStrip.cpp` now draws its header + stamp through `brand::steelHeaderHi/Lo`
and `brand::debossInk/Face` (migrated off inline hex). The other inlined headers
(`MasterStrip`, `BigClockPanel`, `Meterbridge`) follow the same pattern — point
their remaining `0xff0b0b0e` / `0xffb4b7bf` literals at the same `brand::` tokens
when you do the next cleanup sweep.

---

### Why structuralForge() is a function, not a constant
The spine / stamp / seam read as `brandOrange` today, but they mean *structure*,
not *mute*. Keeping `structuralForge()` as its own accessor lets a future HC or
re-skin pass widen/shift the identity-orange without touching `signalMute()`. The
`getStructuralForge(bool hc)` variant in the original `ForgeSteel.h` shows the HC
hook — wire it to your `highContrastEnabled()` flag when you do the HC pass.
