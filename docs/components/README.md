# Component Reference

Per-component documentation for ZynForge Recording's UI surface. Started 2026-05-24 from the design-system audit that flagged "no external component docs" as a gap.

The format follows `/design-system document` output: description, when-to-use, variants, props/constructor, public methods, states, tokens used, accessibility, do's/don'ts.

## Index

References are kept in line with the current headers. The 2026-11-04 pass
checked every document here against the `821131a` source, after that day's
review fixes: `BigClockPanel`, `ChannelStrip`, `DialogChrome`,
`EditPage::TrackRow`, `FineFader`, `LedMeter`, `MasterStrip`,
`SessionRecoveryDialog`, `TransportBar` and `WelcomeDialog` changed;
`EditTimeRuler`, `EditToolsBar` and `Toast` did not. None of that source has run
on real hardware yet. These documents
describe implementation; they do not establish field or accessibility acceptance.
[Testing](../../testing.md), [installation](../../INSTALL.md),
[architecture](../../architecture.md) and [show readiness](../../SHOW-READINESS.md)
own validation and operating limits.

### Shipped

- [`ChannelStrip`](ChannelStrip.md) — per-channel vertical strip (MIXER view)
- [`MasterStrip`](MasterStrip.md) — master fader + stereo/mono toggle
- [`FineFader`](FineFader.md) — app-wide click-drag-only level fader (channel / master)
- [`LedMeter`](LedMeter.md) — segmented forge-heat level meter (mixer + EDIT)
- [`DialogChrome`](DialogChrome.md) — shared dialog chrome + control-styling helpers (title badge, fields, buttons, check rows)
- [`EditToolsBar`](EditToolsBar.md) — Smart / Selector / Trim / Grabber / Fade tool palette
- [`BigClockPanel`](BigClockPanel.md) — transport state + timer + armed-ready indicator
- [`TransportBar`](TransportBar.md) — start / end / play / stop / record / loop buttons
- [`EditTimeRuler`](EditTimeRuler.md) — two-strip ruler (markers / Min:Secs)
- [`EditPage::TrackRow`](EditPageTrackRow.md) — the per-track EDIT row with waveform and clip-edit handles
- [`Toast`](Toast.md) — non-modal feedback pill
- [`SessionRecoveryDialog`](SessionRecoveryDialog.md) — orphan-session recovery modal
- [`WelcomeDialog`](WelcomeDialog.md) — first-launch and File ▸ New flow

### Removed

- [`PeakTally`](PeakTally.md) — historical global clip bar; per-strip meters now own clip indication.

### TODO

Other component files are mostly internal helpers (StripColourPicker, etc.) and modal dialogs for specific settings flows (ExportDialog, BackupMirrorDialog, etc.). Document on demand when an engineer extends one of them; no value in pre-emptively covering every helper class.

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
