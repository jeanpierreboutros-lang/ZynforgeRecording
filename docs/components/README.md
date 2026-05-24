# Component Reference

Per-component documentation for ZynForge Recording's UI surface. Started 2026-05-24 from the design-system audit that flagged "no external component docs" as a gap.

The format follows `/design-system document` output: description, when-to-use, variants, props/constructor, public methods, states, tokens used, accessibility, do's/don'ts.

## Index

### Shipped

- [`ChannelStrip`](ChannelStrip.md) — per-channel vertical strip (MIXER view)
- [`AutomationToolbar`](AutomationToolbar.md) — EDIT-view automation editing toolbar
- [`BigClockPanel`](BigClockPanel.md) — transport state + timer + armed-ready indicator
- [`TransportBar`](TransportBar.md) — record / play / stop / skip buttons
- [`EditTimeRuler`](EditTimeRuler.md) — three-strip ruler (markers / Bars\|Beats / Min:Secs)
- [`EditPage::TrackRow`](EditPageTrackRow.md) — the per-track EDIT row with waveform, automation lane, clip-edit handles
- [`Toast`](Toast.md) — non-modal feedback pill
- [`PeakTally`](PeakTally.md) — global clip indicator

### TODO (lower priority dialogs)

- `SessionRecoveryDialog` — orphan-session recovery modal
- `WelcomeDialog` — first-launch and File ▸ New flow
- `EditToolsBar` — Smart / Selector / Trim / Grabber / Fade / Scrubber tool palette
- `MasterStrip` — master fader + stereo/mono toggle

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
