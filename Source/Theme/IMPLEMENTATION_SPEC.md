# ZynForge Heated Steel — Pixel-Parity Implementation Spec

**Canonical target:** `ZynForge Heated Steel Target.html` (open it; it's built
from the real `ForgeTokens.h` values). Every number below is taken from that
file and your real source. Match the app to it, region by region.

Status legend: ✅ already in the `edited/` drop-ins · 🔶 needs a layout edit you
must apply in your tree · ⚠️ touches a file I couldn't fully verify blind.

---

## 0. Tokens (all real, from ForgeTokens.h — no invented values)

```
brandOrange #ff7733   bgDeep #0d0d12   bgPanel #121316   bgStrip #1e1e1e
edge #26282e   controlBg #2a2c30   controlBorder #141518
text: primary #fff  secondary #b8c2cc  tertiary #7a8a9a  muted #55606c
accentRecord #ff3b3b  accentPlay #4ad878  accentVS #ffb42a  accentSolo #ffd64d
meter: green #3cb878  idle #2c303a  ember #e05518  hot #ff8a24  whiteHot #ffefcf
fader: hi #33353c  lo #1c1e23  edge #555760
personality[8] (strip washes): 4c6c88 528a6a 807646 765a8c 783c4e 3c8286 805c32 7a6a38
radius: sm 2.5  md 4  lg 5  xl 8     space: xs 4 sm 6 md 8 lg 10 xl 16
type (Inter / JetBrains Mono): label10 caption11 body13 title14 headline18 hero44
```

---

## 1. Channel strip — `ChannelStrip.cpp` ✅ (in edited/)

Geometry the target uses (per strip, top→bottom):

| Region | Spec |
|---|---|
| Card | gap 8px between strips (paint `reduced(4)`), radius **xl (8)**, wash `linear-gradient(180, colour@0x33, colour@0x14 42%, #000@0x33)` over `bgStrip` |
| Spine | **5px** brandOrange down the left edge; glow `0 0 8px`; full-bright + ember bloom when armed |
| Header plate | **30px** tall, `#0b0b0e→#000`, 1px gloss top, **2px brandOrange under-seam**, 1px black divider |
| Number | far-left, **JetBrains Mono Bold 24px**, colour `#b4b7bf` (raised steel), box `{12,3,34,24}` |
| Name | Inter **Bold 13px** (`channelName()`), left-aligned, starts at x≈44, right-reserved 20px (caret) +18px (chip) |
| Colour chip | 12×12, radius sm, far right of header |
| In / Master combos | 20px (`ioH`) each, `#000` bg, 1px edge, caption 11px |
| Pan knob | **40px** mono / 30px stereo, machined radial knob + orange pointer (LookAndFeel `drawRotarySlider`) |
| R I M S | 24px (`btnH`) tall, 4px gap, mono/ui 12px |
| dB readout | mono bold 13px, hot when armed |
| Fader | track **12px** (impl; mock showed 6 — code is the source of truth), colour fill below cap; cap **30×36**, radius 6, **vertical** orange groove (LookAndFeel `drawLinearSlider`) |
| Meter | **30px** wide mono / 38 stereo, 20 segs, forge-heat ramp, idle `#2c303a` |
| dB ruler | 24px wide, mono 9px, on the LEFT of the fader |

All of the above is in the edited `ChannelStrip.cpp` + the two LookAndFeel draw
methods. **Remaining 🔶:** the per-strip **gap** is governed by where the strips
are positioned (the mixer viewport), not the strip itself — see §4.

---

## 2. Big clock — `BigClockPanel.cpp` ✅ (in edited/)

| Element | Spec |
|---|---|
| Height | 128px, radius lg (5) |
| Idle | `bgPanel`, edge border, green `accentStatus` timer |
| **Recording** | bg `linear-gradient(meterEmber@26 → meterHot@1f)`, **inset orange frame** (`#0e0f12` 2px + `brandOrange@55` 1px), **orange corner brackets** (top-left/right, 20px, 2px), timer **meterWhiteHot** w/ `0 0 18px meterHot` glow |
| Left | forge-mark (28px, hot when recording) + REC lamp + label (Inter bold 18px) |
| Centre | timer **JetBrains Mono Bold 52px** |
| Right | 7-row stat grid, label Inter 10px tertiary / value mono bold 10.5px secondary, 240px wide |

Corner brackets are the one piece not yet in your build — add four 2px
`brandOrange` L-strokes at the panel corners (the target shows the exact inset).

---

## 3. Transport + toolbar buttons — `TransportBar.cpp` ✅ / LookAndFeel ✅

- Transport tiles: **44×40**, radius **9px**, matte (gloss reduced), 1px `edge`
  border. Icons: play `accentPlay`, **stop `textSecondary`**, record `accentRecord`
  ring+dot with persistent red stroke, **loop `brandOrange`**, rtz/ffwd `textSecondary`.
- Header pills (DEVICE/WAV24/PATCH/VSC/LOCK): `drawButtonBackground` flattened to
  matte; radius md. Shape ✅. **Colours 🔶** — see §5.

---

## 4. Mixer layout / strip gap — `MainComponentLayout.cpp` 🔶⚠️

The target shows **carded strips with an 8px gap**. Whether your build shows the
gap depends on how strips are laid out in the mixer viewport. Find the strip
loop in `resized()` (the section after the toolbar rows) and ensure each strip
gets `width = stripW` with an **8px gap** between (`r.removeFromLeft(8)` between
strips), and the master separated by a 1px `edge` divider + 8px.
Strip width per preset (S/M/L/XS) stays as-is; only the **gap** + the master
divider need to match. *I couldn't fully read this loop blind — send me the
strip-layout block of `resized()` and I'll write the exact edit.*

---

## 5. Toolbar layout + button colours — `MainComponentLayout.cpp` + `MainComponent` 🔶⚠️

This is the biggest parity gap and the riskiest blind. The target toolbar is:

```
[forge-mark] ZYNFORGE RECORDING        … DEVICE  WAV 24  PATCH  •VSC   LOCK
[rtz][ffwd][▶play][■stop][●rec][⟲loop]  00:00/00:00                    (right)
```

Your real `resized()` (read from your tree) currently:
- sets `titleLabel.setBounds({})` → **un-hide it**: give row1 a left slot
  `titleLabel.setBounds(row1.removeFromLeft(230))` and paint the forge-mark to its
  left (or prepend a glyph component).
- row1 has METERS/BACKUP/CH; row2 has S/M/L/MIXER/EDIT/VSC/VCA/PATCH. The mock
  shows a slimmer set. **Decide which buttons to keep** — parity means hiding
  METERS/BACKUP/S/M/L/MIXER/EDIT/VCA from the top strip (or moving them into a
  `…` overflow) and surfacing DEVICE/WAV24/PATCH/VSC/LOCK as the mock does.
- The mock's header pills are **neutral dark**; your buttons are colour-coded via
  `buttonColourId` set in `MainComponent`'s button construction. For the mock look,
  set those to `controlBg` (neutral) and keep only the VSC amber dot + LOCK red.

**This requires editing `MainComponent` (button colours) + `MainComponentLayout`
(row1/row2 bounds).** It's structural and I should do it seeing the full files —
paste `MainComponent`'s button-construction block + the full `resized()` and I'll
produce exact edits. Doing it blind risks breaking your toolbar.

---

## Recommended order
1. Build the `edited/` files (✅ items) → strip, clock, transport, cap, knob all land.
2. Compare against the target HTML side by side; nudge the ✅ numbers if needed.
3. Send me the strip-layout loop + toolbar `resized()` + button construction →
   I write the 🔶 layout edits exactly.

That last step is what turns "very close" into "pixel-parity" — and it's the part
that must be done with the real files in view, not blind.
