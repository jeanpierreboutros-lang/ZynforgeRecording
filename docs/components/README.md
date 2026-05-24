# Component Reference

Per-component documentation for ZynForge Recording's UI surface. Started 2026-05-24 from the design-system audit that flagged "no external component docs" as a gap.

The format follows `/design-system document` output: description, when-to-use, variants, props/constructor, public methods, states, tokens used, accessibility, do's/don'ts.

## Index

### Shipped

- [`ChannelStrip`](ChannelStrip.md) — per-channel vertical strip (MIXER view)
- [`AutomationToolbar`](AutomationToolbar.md) — EDIT-view automation editing toolbar

### TODO (priority order)

- `EditPage::TrackRow` — the per-track EDIT row with waveform, automation lane, clip-edit handles
- `BigClockPanel` — transport state + timer + arming indicator
- `TransportBar` — record / play / stop / skip buttons
- `EditTimeRuler` — three-strip ruler (markers / Bars\|Beats / Min:Secs)
- `Toast` — non-modal feedback pill
- `PeakTally` — global clip indicator
- `SessionRecoveryDialog` — orphan-session recovery modal
- `WelcomeDialog` — first-launch and File ▸ New flow

## Style

- One file per component, named `<ComponentName>.md`.
- Section order matches `/design-system document` output for consistency.
- Code samples are minimal: constructor signature + one or two usage snippets.
- Tokens used section lists the `brand::*` references — if a component pulls from outside `brand::`, that's a smell to flag.
- "Do's and Don'ts" surfaces the patterns engineers actually trip on.

## What NOT to put here

- Inline implementation details (those live in the `.cpp` comments).
- Wire diagrams of how components connect (those live in `architecture.md`).
- Workflow tutorials (those live in `design.md` or user-facing docs).
