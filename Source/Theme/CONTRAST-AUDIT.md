# Heated Steel — contrast / 200-lux legibility audit

> Historical token and component snapshot. The ratios below describe the listed colour pairs, not a fresh WCAG audit or a measured 200-lux field test of the current installed app. See [design.md](../../design.md) and [SHOW-READINESS.md](../../SHOW-READINESS.md) for current design and rig limits.

Goal (from the field constraints): every piece of **information** must read in
bright FOH light (~200 lux on screen) without a separate high-contrast mode.
Ratios below are WCAG contrast (foreground vs the surface it sits on). AA needs
≥4.5:1 for normal text, ≥3:1 for large/bold UI.

## Verdict: the Heated Steel additions all pass; one field fix applied.

| Element | Pair | Ratio | Verdict |
|---|---|---|---|
| Timer (idle) | accentStatus `#5dd87a` on bgPanel | ~9:1 | ✅ AAA |
| Timer (recording) | meterWhiteHot `#ffefcf` on ember bg | ~14:1 | ✅ AAA |
| Channel name | textPrimary white on strip wash | ~12:1 | ✅ AAA |
| dB readout | white / meterHot on strip | ≥7:1 | ✅ |
| Stamped number | debossFace `#b4b7bf` on near-black plate | ~13:1 | ✅ AAA |
| Orange spine / seam | brandOrange `#ff7733` on near-black | ~7:1 | ✅ (structural, not text) |
| REC / PLAY label | textPrimary on panel | ~12:1 | ✅ |
| Toolbar wordmark | white + textTertiary on header | ✅ / ~5:1 | ✅ AA |
| **Clock disk-health labels (recording)** | **textTertiary on ember bg** | **~4.0:1** | ⚠️ **was just under AA → fixed** |

## Fix applied
The big-clock disk-health / loudness **readout** (FREE, RECORD TIME LEFT, LAST
WRITE, MARKERS, LOUDNESS, TRUE PEAK) used `textTertiary` labels. While recording,
the ember-tinted background lightens and drops that to ~4:1 — readable at a desk,
marginal at 200 lux. Bumped the whole readout one tier:
- labels: `textTertiary` → **`textSecondary`** (~7.6:1 on ember, ~10:1 idle)
- values: `textSecondary` → **`textPrimary`** (white)

Hierarchy preserved (labels dimmer than values), now comfortably AA/AAA in
daylight. Alert states (missed samples, hot true-peak) keep `accentRecord`.

## Deliberately low-contrast (left as-is — not information)
- `textMuted` (#55606c) — the design system's **disabled** tier. Used for the
  `OUT` footer, ruler tick labels, empty-combo placeholders. Low contrast is the
  intended "inactive" signal; not primary info.
- Hammered-steel texture, brushed grain, 1px `edge` dividers — decorative; meant
  to sit below the content threshold.
- Spine/seam glow alphas — decoration around the solid bright bar (which passes).

## High-contrast mode status
No separate high-contrast mode is implemented. The listed colour pairs cleared
the snapshot's contrast checks, but that does not establish that every current
component is WCAG-compliant or legible on the planned stage rig. Check the
actual app under the target lighting and with VoiceOver. `structuralForge()` is
an accessor that could support a later high-contrast variant.
