---
version: alpha
name: ZynForge
description: >
  Shared design system for the ZynForge product family. Dark stage-side
  environment, dense control layouts, semantic signal-state colours,
  per-strip personality washes. Tuned for engineers working at 200 lux
  next to a console — calm, legible, no decoration.
colors:
  # ── Canvas / panel chrome (near-pure dark) ──────────────────────────
  bg-deep:              "#0a0a0c"   # deepest plane — main canvas
  bg-panel:             "#121316"   # header bands, big-clock background
  bg-strip:             "#18191d"   # neutral strip body (no personality wash)
  bg-elevated:          "#303030"   # popups, callouts, dialog chrome
  edge:                 "#26282e"   # 1 px outlines around panels and strips

  # ── Text (4-step, all WCAG-AA against bg-strip) ─────────────────────
  text-primary:         "#e8e8ee"
  text-secondary:       "#b8c2cc"
  text-tertiary:        "#7a8a9a"
  text-muted:           "#888a94"

  # ── Signal states (one of these per show-critical event) ────────────
  meter-green:          "#3cb878"   # signal hot, healthy
  meter-yellow:         "#f0c060"   # near threshold / pre-clip
  meter-red:            "#dc3838"   # clip / crash / record state
  accent-record:        "#ff3b3b"   # record, danger
  accent-play:          "#4ad878"   # play / transport active
  accent-vs:            "#ffb42a"   # virtual-soundcheck / macros amber
  accent-status:        "#5dd87a"   # session-clock readout green
  alert-amber:          "#e89827"   # warning, multi-select highlight
  engaged-amber:        "#ff9e2c"   # BYPASS / LIVE / LOCK active state
  feature-engaged:      "#33c4d4"   # toggle-on (cool teal, distinct from meter)

  # ── Control chrome ──────────────────────────────────────────────────
  border-subtle:        "#3a3a3a"
  border-bright:        "#5a5a5a"
  outline-hidden:       "#00000000"

  # ── Overlays (semantic neutrals; HC-immutable) ──────────────────────
  overlay-light:        "#ffffff"
  overlay-dim:          "#000000"
  text-on-accent:       "#000000"   # text painted ON accent or amber bg
  text-on-light:        "#000000"

# ── Per-strip personality wash (defining family trait) ────────────────
# Each channel strip's chrome is washed with a muted, low-saturation
# version of its personality colour — not a thin top band, the whole
# strip. The wash is dark enough that white text remains readable.
# A subtle top-light → bottom-dark vertical gradient lives inside the
# wash. 8-colour rotation; channel N picks personality[N % 8].
personality:
  - { name: dusty-blue, value: "#3a5063" }
  - { name: moss,       value: "#395646" }
  - { name: olive,      value: "#514a35" }
  - { name: violet,     value: "#4a3d57" }
  - { name: wine,       value: "#582e3a" }
  - { name: teal,       value: "#2d5051" }
  - { name: amber,      value: "#5b4528" }
  - { name: mustard,    value: "#5a4d2c" }

typography:
  # ── Display / hero / brand mark ─────────────────────────────────────
  splash-title:
    fontFamily: Menlo
    fontSize: 28px
    fontWeight: 700
    lineHeight: 1.1

  # ── Strip + dense technical data ────────────────────────────────────
  channel-name:
    fontFamily: Menlo
    fontSize: 15px
    fontWeight: 700
    lineHeight: 1.2
  plugin-name:
    fontFamily: -apple-system
    fontSize: 13px
    fontWeight: 700
    lineHeight: 1.2
  label:
    fontFamily: Menlo
    fontSize: 9px
    fontWeight: 400
    lineHeight: 1.1
  status-bar:
    fontFamily: Menlo
    fontSize: 10.5px
    fontWeight: 400
    lineHeight: 1.2
  led-label:
    fontFamily: Menlo
    fontSize: 9px
    fontWeight: 600
    lineHeight: 1
    letterSpacing: 0.04em

  # ── UI body / dialogs ───────────────────────────────────────────────
  ui-body:
    fontFamily: -apple-system
    fontSize: 13px
    fontWeight: 400
    lineHeight: 1.4
  ui-label:
    fontFamily: -apple-system
    fontSize: 11px
    fontWeight: 600
    lineHeight: 1.2
    letterSpacing: 0.02em
  hint:
    fontFamily: Menlo
    fontSize: 8.5px
    fontWeight: 400
    lineHeight: 1

spacing:
  # ── Gap scale (5-step) ──────────────────────────────────────────────
  xs:  4px      # tight icon-to-text padding
  sm:  6px      # sibling controls on the same row
  md:  8px      # section inner padding
  lg:  10px     # section outer margin
  xl:  16px     # large section gaps

  # ── Control heights ─────────────────────────────────────────────────
  ctrl-h:       22px
  io-h:         20px   # I/O selectors (intentionally < ctrl-h)
  row-h:        26px   # standard row pitch
  btn-h:        24px   # action buttons

  # ── Toolbar button widths (3-step) ──────────────────────────────────
  btn-narrow:   72px
  btn-wide:     90px
  btn-extra:    180px

rounded:
  sm:    2.5px   # micro chips, fader thumb caps
  md:    4px     # picker rows, dots, small panels
  lg:    5px     # knobs, fader thumb body, toolbar buttons
  xl:    8px     # dialog backgrounds, callout boxes

# ── Motion ────────────────────────────────────────────────────────────
# Two motion domains: audio-domain (show-critical, tuned by ear) and
# UI-domain (visual feedback). Keep them separate — UI animation should
# never gate audio events.
motion:
  # Audio-domain
  crossfade:           100ms    # equal-power chain/state crossfade
  recall-ramp-blocks:  1        # parameter recall gain ramp, in audio blocks
  meter-fast-tau:      70ms     # fast meter envelope time constant
  meter-slow-tau:      250ms    # slow meter envelope time constant
  peak-hold:           1500ms   # peak indicator dwell before fall

  # UI-domain
  hover:               120ms    # button hover / click feedback
  engine-reattach:     60ms     # device-dialog close → audio re-attach defer
  web-broadcast:       200ms    # companion state push interval (5 Hz)

components:
  channel-strip:
    bg:                "{personality[N].value}"   # personality wash, gradient top→bottom
    bg-neutral:        "{colors.bg-strip}"
    edge:              "{colors.edge}"
    header-typography: "{typography.channel-name}"
    gap:               "{spacing.xs}"

  panel:
    bg:                "{colors.bg-panel}"
    edge:              "{colors.edge}"
    radius:            "{rounded.xl}"

  button:
    # Small dark-grey rounded panel with light text. Highlighted state
    # uses a bright translucent accent (e.g. record→accentRecord,
    # transport→accentPlay, macros→accentVS, feature-engaged→teal).
    bg:                "{colors.bg-elevated}"
    bg-on:             "{colors.engaged-amber}"
    text:              "{colors.text-primary}"
    text-on:           "{colors.text-on-accent}"
    height:            "{spacing.btn-h}"
    radius:            "{rounded.lg}"

  led:
    # Circular dot for status indicators (signal / midi / warn).
    body-on:           "{colors.meter-green}"
    body-off:          "{colors.bg-elevated}"
    body-warn:         "{colors.alert-amber}"
    rim:               "{colors.border-bright}"
    typography:        "{typography.led-label}"

  fader:
    track:             "{colors.bg-deep}"
    thumb:             "{colors.text-secondary}"
    thumb-radius:      "{rounded.lg}"
    label:             "{typography.label}"

  meter:
    # Vertical LED-segment bar, 20 discrete painted segments with black
    # gaps between them — reads like physical SSL/Yamaha hardware, not
    # a smooth gradient. Click clears clip.
    segments:          20
    green-pct:         70       # bottom 70% green
    amber-pct:         15       # next 15% amber (meter-yellow)
    red-pct:           15       # top 15% red (meter-red)
    bg:                "{colors.bg-deep}"
    gap:               1px      # black gap between segments
---

# ZynForge — Design System

> Engineer-first dark UI for live and recording audio applications.
> Built so the operator can scan critical state at a glance under stage
> or studio lighting.

## Overview

ZynForge apps run on a laptop next to a console or DAW. The operator's
eye is constantly moving between the screen and the work. Every visual
decision is made for that environment:

- **Dark by default.** Near-black ground (`bg-deep`, `bg-panel`,
  `bg-strip`) so the screen disappears into a dim room.
- **High-contrast accents only when they mean something.** Show-critical
  state uses semantic signal colours: `meter-green` for hot,
  `meter-yellow` near threshold, `meter-red` for clip / crash / record,
  `accent-vs` (amber) for virtual-soundcheck / macros, `accent-play` for
  transport. The eye learns one colour = one meaning.
- **Dense without being cramped.** Strip layouts favour vertical stacks
  of small controls; dialogs use a roomier scale. The two are
  deliberately distinct (`spacing.xs/sm` for strips, `dialog-row-h=24`
  for dialogs).
- **Touch + glance, not click.** Double-click opens editors; single-
  clicks on loaded controls are deliberate no-ops so a stray elbow
  during a session doesn't churn the UI.
- **High-contrast mode is a first-class citizen.** Settings → Display
  → High Contrast toggles every `paint()` colour through `get*()`
  getters that boost values when enabled. The operator can flip it
  mid-session without restarting.

The personality is **calm professionalism**. Not playful, not
minimalist-to-the-point-of-empty, not corporate-sterile. The visual
language is closer to a high-end studio outboard rack with sensible
typography than to a consumer app.

## Colors

The palette is rooted in **3 background levels**, **4 text levels**,
**a deliberately small set of signal colours**, and **an 8-colour
personality rotation** for per-strip identity.

- **Backgrounds (3-step):** `bg-deep` for the deepest plane, `bg-panel`
  for header bands and clock readouts, `bg-strip` for neutral strip
  bodies, `bg-elevated` for popups. Adjacent steps are perceptibly
  distinct.
- **Text (4-step):** `text-primary` for headlines, then `text-secondary
  / text-tertiary / text-muted` for descending importance. All four
  pass WCAG AA against `bg-strip`.
- **Signal states (one colour = one meaning):**
  - `meter-green` — audio is hot and healthy
  - `meter-yellow` — near threshold, pre-clip, "watch this"
  - `meter-red` / `accent-record` — clip, crash, record, danger
  - `accent-play` / `accent-status` — transport active, session clock
  - `accent-vs` — virtual-soundcheck / macros / load
  - `alert-amber` — warning, multi-select highlight, drag-hover
  - `engaged-amber` — BYPASS / LIVE / LOCK active state
  - `feature-engaged` — cool teal toggle-on, deliberately distinct
    from any meter colour
- **Personality rotation (8 muted colours):** each strip's chrome is
  washed with a low-saturation tint from `personality[]` based on the
  strip's index. The wash is dark enough that `text-primary` stays
  readable. A subtle top-light → bottom-dark gradient lives inside the
  wash. The defining trait of the ZynForge family.
  - **Recording app override (2026-06-04):** at the user's request, new
    channels in **ZynForge Recording** default to a neutral grey
    (`stripDefaultGrey`), not the per-index personality wash — engineers
    colour the channels that matter from the `StripColourPicker` (a hue×shade
    gradient with the strip's current colour pinned + an OK button). The
    `personality[]` palette stays defined for reference and for ZynForge
    **Live**, which is unchanged. See `decisions.md`.

The complete implementation palette has many more derived tokens; the
front matter exposes the semantic anchors that everything else derives
from.

## Typography

Two font families, used for distinct purposes:

- **Menlo (monospace)** — every place numeric data appears: channel
  names, labels (dB ticks, CPU%, latency ms), LED captions, status
  bar, hints. Fixed-width gives a stable visual rhythm when values
  change.
- **-apple-system (SF Pro)** — plugin/device names, dialog body,
  picker manufacturer column. Brand strings read more cleanly in
  proportional type.

Sizes follow a deliberately tight scale (8.5 / 9 / 10.5 / 11 / 13 /
15 / 28). LED labels use uppercase + 4% letter-spacing (`led-label`
token) so they read as labels, not words.

## Layout

The layout is **dense vertical stacks for strips** and **generous
breathing room for dialogs**. They use different spacing scales.

- **Strip (`xs=4, sm=6`):** packs identifier + controls + I/O + fader
  + meter into a narrow column. Reducing scale steps means controls
  can stack without visual noise.
- **Dialog (`dialog-row-h=24, dialog-row-gap=4`):** any callout or
  inline panel uses the dialog scale — roughly 1.5× the strip scale.
  Label+control rows have breathing room.
- **Toolbar (`btn-narrow=72, btn-wide=90, btn-extra=180`):** three
  button-width steps, used consistently across toolbar and Setup.

## Elevation & Depth

Use **tonal layering**, not stacked shadows.

- **`bg-deep` → `bg-strip` → `bg-panel` → `bg-elevated`** is the
  elevation hierarchy. The luminance gap between adjacent layers is
  enough to read as "this is on top of that" without drop shadows.
- An `elevationHighlight` (≈8% white top stroke) + `elevationShadow`
  (≈25% black bottom shadow) pair are applied to popups when the layer
  gap alone isn't enough.

In a dark UI, large drop shadows hurt depth perception (they read as
smudges, not lift). Tonal layers respect the dark environment.

## Shapes

A 4-step corner-radius scale:

- `sm` (2.5 px) — micro chips, fader thumb caps (≤ 24 px tall)
- `md` (4 px) — picker rows, dots, small panels (26–50 px tall)
- `lg` (5 px) — fader thumb body, knob caps, toolbar buttons
- `xl` (8 px) — dialog backgrounds, callout boxes

Adjacent steps are perceptibly distinct (≈ 1.6× ratio). Jump a step
when you want a clear visual hierarchy between two surfaces.

## Motion

Two distinct domains; the audio one is show-critical.

**Audio-domain motion:**

- **`crossfade` (100 ms equal-power)** — any A/B chain or state switch.
  Short enough that the operator perceives the switch as immediate;
  long enough that the equal-power ramp eliminates clicks.
- **`recall-ramp-blocks` (1 audio block)** — parameter recall gain
  ramp. One buffer ≈ 2.7 ms at 48 kHz / 128 frames. Eliminates clicks
  on recall without visible latency.
- **`meter-fast-tau` (70 ms) / `meter-slow-tau` (250 ms)** — exponential
  decay coefficients for meter envelopes. Fast for peak chase, slow
  for release tail.
- **`peak-hold` (1500 ms)** — peak indicator dwell. Long enough to
  register a transient clip; short enough that the dot doesn't confuse
  the operator about whether the signal is currently hot.

**UI-domain motion:**

- **`hover` (120 ms)** — button hover / click feedback animation.
- **`engine-reattach` (60 ms)** — audio device dialog close → engine
  callback re-attach defer. Lets in-flight Core Audio property
  notifications settle before the audio thread resumes.
- **`web-broadcast` (200 ms = 5 Hz)** — companion device state push
  interval. Responsive without saturating the WebSocket pipe.

## Components

Five composite tokens exposed in the front matter, defined to be
universal across ZynForge apps:

- **`channel-strip`** — vertical column for one signal source/sink.
  Chrome is washed with `personality[index % 8]`, with a subtle
  top-light → bottom-dark gradient inside the wash. Header uses
  `channel-name` typography on a darker variant of the wash.
- **`panel`** — bordered container for grouped controls (header bands,
  big-clock background, transport area). `bg-panel` background,
  `edge` outline, `xl` radius.
- **`button`** — small dark-grey rounded panel with light text. The
  default state uses `bg-elevated`; engaged states fill with a bright
  translucent accent (record → `accent-record`, play → `accent-play`,
  macros → `accent-vs`, feature-on → `feature-engaged`). Sparingly
  used — most buttons are quiet, the few highlighted ones command
  attention.
- **`led`** — circular dot for status (signal / midi / warn). Body
  colour switches between `meter-green` (alive), `alert-amber` (warn),
  `bg-elevated` (idle).
- **`fader`** — vertical linear slider for level. Custom paint draws
  dB ticks at -inf, -60, -30, -10, 0, +6. Thumb is rounded
  (`thumb-radius: lg`).
- **`meter`** — vertical LED-segment bar with 20 discrete painted
  segments (NOT a gradient). Bottom ~70% green, next ~15% amber, top
  ~15% red. Black gaps between segments make it read as physical
  hardware. Click clears clip.

## Do's and Don'ts

These should be mechanically enforced (pre-commit hook + CI). Six
rules:

| ✅ Do                                                        | ❌ Don't                                                          |
|-------------------------------------------------------------|------------------------------------------------------------------|
| Use `Colors::getX()` getters in every `paint()`             | Inline `juce::Colour(0xff...)` anywhere outside the colour header |
| Use `Fonts::X()` for every font set                         | Inline `juce::Font(...)` / `juce::FontOptions(...)` outside the fonts header |
| Use `Spacing::X` constants in `setBounds`                   | Raw 4-int `setBounds(int,int,int,int)` literals                  |
| Use CSS variables in companion-device HTML                  | Hex literals outside the `:root` block                           |
| Use semantic neutrals (`textOnAccent`, `overlayLight`, etc) | Bare `juce::Colours::white / black / transparentBlack`           |
| ASCII-only in tooltip + label text                          | Em-dash `—`, en-dash `–`, arrows, bullets in user-visible strings |

The pre-commit + audit script enforce these; drift cannot enter the
repo silently.

**Brand-immutable carve-out:** any app-icon / logo colour tokens
should be exempt from HC remapping. A logo is a brand mark; remapping
its colours in HC mode produces a different logo, not a more-legible
one. Confine the carve-out to literal brand assets — every other
paint-loop colour MUST go through the `get*()` getters.

## High-Contrast Mode

Three-layer system, all maintained together:

1. **Static tokens** — raw hex used in `setColour()` constructor calls
   for initial state. Do NOT respond to HC toggle on their own.
2. **HC getters** — `inline get*()` functions that read a
   `highContrastEnabled()` atomic at runtime and return a brighter /
   more-saturated value when HC is on. Used in EVERY `paint()` call.
3. **`refreshColors()` per component** — re-applies HC-aware values to
   buttons/labels when the HC toggle fires.

Rule: every token used in a `paint()` call must have a corresponding
`get*()` getter.
