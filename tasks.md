# Project Tasks

## How to use this file

Claude and human developers share this file as the single source of truth for what's next, what's in flight, and what just shipped. Update at the end of every productive session:

- Move newly-started items from **Backlog** to **In Progress**.
- Move completed items from **In Progress** to **Recently Completed** with the date.
- Promote backlog items into **Current Priorities** when they become the next ~5 things to ship.
- Prune **Recently Completed** entries older than 14 days — full history lives in `CHANGELOG.md`.

Effort scale: **S** (≤1 hour), **M** (1–4 hours), **L** (half-day or more).

---

## Current Priorities

- [x] **Per-session persistence consolidation / schema** (M) — **Done.** Aux **sends**, `strip_isbus_*`, `strip_vca_*`, and `strip_editgroup_*` are all out of global `appProps` and authoritative in `session_mix.json`; the engine no longer reads/writes any of them to appProps (they also went stale after a reorder, since `swapTracks` never swapped them). `session_mix.json` carries `formatVersion` (v1) and the loader is hardened against corrupt/truncated files (unparseable → ignored, absurd `trackCount` → clamped to 256, out-of-range index → skipped). The automation `safe`/`vTrim`/`pTrim` flags already round-trip via the `.zfproj` snapshot — never leaked. Covered by an extended save→load→save round-trip test + a corrupt-file resilience test + a device-restart regression test.
- [ ] **Optional: finish splitting `MainComponent.cpp`** (M) — down from ~5500 lines to 1590 after parts 1–6 (timer, keys, layout, cues, edit, session-IO, menu, help, tools, strips). The remaining lumps (ctor 590 lines, `rebuildStrips` 295 lines) are tightly coupled to MainComponent's member init / strip vector — extracting them needs a real refactor, not just a cut/paste. Park as low-priority unless someone wants to touch the strip-rebuild path.
- [ ] **Field-test the unified dialog chrome** (S) — the 2026-05-23 chrome pass shipped untested in a live rehearsal. Walk through every dialog (AudioDevice, NewSession, AddTracks, Export, Session Settings, Session Properties, Click Settings, Marker List, Noise Report) and every `AlertWindow.showAsync` site (reorder-during-playback, sample-rate mismatch, lock-against-overwrite, OSC bind failure). Look for clipping, divider misalignment, button width regressions.
- [x] **Companion server TLS** (M) — **Resolved 2026-06-05: tunnel-based, no in-app TLS.** JUCE has no server-side TLS; bundling one + a self-signed cert is a worse security/UX trade than fronting loopback with a tunnel (Tailscale / Cloudflare / `ssh -L`) that gives real CA-backed TLS for free. Companion stays loopback-only (`startCompanionServerOnLan` stays unwired), the host UI warns if ever exposed, and README documents the tunnel recipes. See `decisions.md` *Companion server is loopback-only…* (TLS resolution).
- [ ] **Field-verify RF64 on a real >4 GiB take** (S) — RF64 for WAV shipped 2026-06-05 (no more multi-part splits; periodic header flush for crash-safety). A unit test can't write 4 GiB, so record a real take past 4 GiB (≈8.3 h mono / ~16 min × 32ch @ 24-bit/48k) and confirm: (a) it's a single `Track_01.wav` that opens in Pro Tools / Reaper / Logic with full length; (b) `file` / a hex peek shows the `RF64` + `ds64` header; (c) a hard-kill mid-take leaves a file that still opens to ~the last-flush point. See `decisions.md` *RF64 for WAV via JUCE + periodic header flush*.

## In Progress

- *(none — close out a session by moving items here back into the appropriate list.)*

## Backlog

- [ ] **Native iPad companion** (L) — replace the polled `/state.json` web client with a SwiftUI app over WebSocket.
- [ ] **Audio-stream encryption** (M) — `/stream.wav` is plaintext PCM. Wrap in TLS or migrate to SRTP.
- [ ] **Expand audio-thread test harness, round 8** (M) — `Source/Tests/AudioCallbackTests.cpp` now covers monitor-bus routing, master gain/mute, solo isolation, VCA mute/solo gating, click engine routing, hard-pan L/R, stereo-pair summing, recorder write path, pre-roll history backfill, BWF bext metadata, marker drop during record, VCA gain end-to-end, volume-automation playback, aux-send routing, and (round 7) **stream-bus output sum, loop-region playback wrap, and punch-record arm mapping** via the `recordTestSession` + `fillPlaybackBuffer` playback feed. **Remaining gaps / ideas**: companion-stream `/stream.wav` capture content (needs the embedded server stood up headlessly), automation *write* during a record pass (touch/latch capture), crossfade render across two overlapping clips, sample-rate-mismatch playback resample. Position-windowed punch entry/exit lives in `MainComponent::servicePunch` (UI timer) — not reachable from the headless engine harness.
- [ ] **Auto-arm-on-input-detect mode** (M) — optional setting where a strip arms itself when persistent signal is detected (useful for first-time pre-show configuration).
- [ ] **Extend accessibility across the UI** (L, in progress) — `PlaceholderView` set the baseline (title/description + VoiceOver announcement + focusable CTA). **Done (each headless-tested):** `ChannelStrip` (strip named after channel; R/I/M/S + gain/pan/routing titled), `TransportBar` (6 icon buttons + focus group), `SetlistBar` (cue arrows/combo/buttons), `EditToolsBar` (6 raw-Component tools → button-role handler + keyboard Return/Space + titles), `MasterStrip` (gain/mute/mono-stereo/output), `VcaPanel` (per-bus gain/mute/solo/name × 8). **Remaining — clean control-level (same pattern):** `AutomationToolbar`, EditPage zoom +/- buttons. **Remaining — bigger (custom handlers):** `PatchMatrix` as an accessible routing *table* (per-cell `AccessibilityCellInterface` + keyboard grid nav), live meter values via `AccessibilityValueInterface`, and the EDIT clip/automation lanes. See the accessibility rule in `coding-standards.md`. Note: live VoiceOver verification (Cmd+F5) still needed — tests assert names are set, not what's spoken.

### Explicit non-goals (do not implement)

- [ ] ~~Per-track plugin slots / AU / VST hosting~~ — explicitly rejected. See `decisions.md` *No plugin hosting*. Recorded here so future contributors don't relitigate.

## Recently Completed

### 2026-06-04 — EDIT Pro Tools-parity pass + undo/keyboard fixes
- [x] **Mixer undo** (M) — fader/pan/mute/solo/rename/recolour/routing are now undoable via a coalesced 10 Hz poll (`pollMixerUndo`) that records one `MixerSnapshotAction` once a change settles (~300 ms); recording is excluded; `editUndo` flushes any pending change first. Resolves the priority raised earlier the same day.
- [x] **Keyboard reliability** (S) — Cmd+Z/Cmd+Shift+Z/Cmd+R/Cmd+X/C/V/Cmd+E now match on key code (not `getTextCharacter` under Cmd, which silently broke Cmd+Z); undo/redo handled early. Full `keyPressed` audit done.
- [x] **Delete confirmation + result-code fix** (S) — Delete on a clip/range pops a "Delete recording?" confirm; fixed the AlertWindow first-button = id 1 (not 0) inversion that made confirm a no-op (also in `removeLastCapture`).
- [x] **PT-parity edit features** (L) — **Strip Silence** (slider settings box: threshold/min-silence/min-clip/pad; engine `stripSilence`), **Heal Separation** (Cmd+H; `healSeparationAt/Range`), **Consolidate** (range + per-clip; `consolidateRange`), **clip-gain corner fader** (drag to ride, Alt-click resets to 0 dB, waveform scales with gain), **Zoom to Selection** (E; `EditPage::zoomToSamples`), **numeric selection readout** in the ruler, **clip menu** Rename/Consolidate (engine `setClipName`). All undoable.
- [x] **Export "Export failed" fix** (S) — `exportTracksTo` reads from `Audio Files/` (was the session root).

### 2026-06-04 — EDIT-page overhaul + menu/persistence/launch bug-fix pass
- [x] **Frozen greyed-out menus — root cause** (S) — `MainComponent` is the macOS `MenuBarModel` but never called `menuItemsChanged()`, so the native menu cached its enabled states from the empty launch state forever (Undo/Redo, all of Edit, Track, Export stayed grey even after a session loaded / edits were made / strips selected). New `refreshMenuStateIfChanged()` polls a signature of every menu-gating condition from the 10 Hz timer and refreshes on change. **This was the "nothing works since day 1" bug.**
- [x] **Empty app on launch** (M) — the app restored the active-session *dir* (for Save) but never loaded its *content*, so it came up with 0 channels and everything that needs tracks/a loaded player was disabled. New `openSessionFolder()` (canonical full open) + auto-reopen of the last session in `showStartupWelcome` + a "size the mixer from loaded audio when there's no `session_mix.json`" recovery fallback. Fixed the incomplete Welcome `onOpen` (was `loadSession` only).
- [x] **"Export failed"** (S) — `exportTracksTo` searched the session root for `Track_*.wav`; recordings live in `Audio Files/`. Now searches the subfolder (legacy root fallback). Fixed both Export All + Export Individual.
- [x] **Aux sends leaked across sessions** (S) — sends were in global `appProps` keyed by index; now per-session in `session_mix.json` (`setTrackSend` no longer writes appProps; loader no longer reads them). Verified: 139 test groups, 0 failures.
- [x] **EDIT clips render as DAW region blocks** (M) — each clip is a bordered block with a name-header bar and **its own waveform** (mapped to the clip's file region, so move/slip-trim show the right audio). Removed the yellow clip-start cut-flags; gap-masking; continuous thumbnail kept only as the no-clips fallback.
- [x] **EDIT: pinned header on horizontal scroll** (M) — the row header (swatch/name/R-I-S-M/VIEW/meter/routing) used to scroll off at zoom>1. Now floats pinned via `paintHeader(g, headerOriginX())` + a `ScrollViewport` that re-pins on scroll; mouse gates routed through `inWavePane/inSwatch/inNameZone`.
- [x] **EDIT: markers lane, snap-on-drag, timeline grid** (M) — markers lane draws real markers (was 6 fake ticks + literal "markers"); Snap now applies to clip Move + trim drags (was split-at-playhead only); faint 1-2-5s grid behind the clips.
- [x] **EDIT: PT tool keys + merged cursor + reliable Delete + slim meter** (M) — single-key tools `S/T/R/G/F/B`, `Cmd+E` Separate; stopped-transport merges the playhead + edit cursor into one line; `switchView` grabs keyboard focus so Delete/tool keys arrive; header meter slimmed 80→22 px.
- [x] **Menu split: Edit vs Track** (S) — Edit = timeline/audio editing only; new **Track** menu for channel/strip management (cut/copy/paste/delete strips, solo, batch rename/colour, selection). Menu bar now `File · Edit · Track · Session · Help`.
- [x] **Channels default to neutral grey + gradient colour picker** (S) — new strips are grey (`brand::stripDefaultGrey`) not the per-index personality wash; `StripColourPicker` is a hue×shade gradient with the original pinned as swatch 0 + an OK button; "Custom…" selector removed.

### 2026-06-03
- [x] **Audio-thread test harness — round 7 (stream bus, loop wrap, punch record)** (M) — 3 new tests closing the last round-4 gaps: stream-bus output sum + drop-out; loop-region playback wrap (stays in-window, wraps backward, keeps sounding); punch-record arm mapping (only punch-armed tracks reach disk). The punch test caught a real bug: `setTrackPunchArmed` wiped lower tracks' punch-arms when growing the vector — fixed to preserve existing flags. Total tests 128 → 131, 0 failures. See `CHANGELOG.md`.
- [x] **Audio-thread test harness — round 6 (playback feed)** (M) — closed the round-4 gaps that needed a loaded `playerScratch` feed. New `recordTestSession()` + `fillPlaybackBuffer()` helpers record real `Track_NN.wav` files via the capture path, then drive deterministic playback through the callback. 5 new tests: session playback reaches the monitor sum; VCA gain attenuates playback on the monitor sum + on a routed hardware output; volume automation attenuates by playhead position; aux send routes playback through a bus track to its output. Total tests 123 → 128, 0 failures. See `CHANGELOG.md`.

### 2026-05-25
- [x] **Auto-split on WAV/AIFF/FLAC chunk-size ceiling** (M) — new `Track_NN_part02.<ext>` rollover when a writer approaches its format's chunk-size ceiling (3.9 GiB WAV/FLAC, 1.9 GiB AIFF). Both primary and backup writers roll independently. ADR in `decisions.md` explains why auto-split over RF64 promotion. Test count 77 → 79. See `CHANGELOG.md`.
- [x] **Audio-thread test harness — round 5 (markers, clip latch, output mute)** (M) — marker drop during record (engine entry point), no-context returns -1, sequential drops auto-increment; master clip latch sets at peak ≥ 0.999, stays sticky across blocks, clears via direct atomic store; `outputMuted` round-trip per strip. Total tests 71 → 77. See `CHANGELOG.md`.
- [x] **Audio-thread test harness — round 4 (pre-roll + BWF metadata)** (M) — pre-roll history backfill round-trip (low-amp pre-record segment lands at the front of the WAV, high-amp live segment lands at the tail); pre-roll = 0 baseline; BWF `bext` chunk metadata round-trip (originator, origination date / time / description). Total tests 68 → 71. See `CHANGELOG.md`.
- [x] **Audio-thread test harness — round 3 (recorder write path)** (M) — 6 new tests drive the IO callback while recording into a temp dir, then read `Track_NN.wav` back and assert length + content + `session.report.json`. Total tests 62 → 68, 0 failures. See `CHANGELOG.md`.
- [x] **Audio-thread test harness — round 2 (9 more tests)** (M) — solo isolation (incl. solo-overrides-mute), VCA mute/solo gating, VCA group state round-trip, click engine follows monitor bus, hard-pan L/R, stereo-pair summing. Total tests 53 → 62, 0 failures. Surfaced an engine design note: VCA *gain* applies only on the per-strip output-routing path (not monitor sum). See `CHANGELOG.md`.
- [x] **Audio-thread test harness — first 8 tests** (M) — new `AudioCallbackTests.cpp` drives `audioDeviceIOCallbackWithContext` headlessly with synthetic buffers. Covers monitor-bus routing (including the just-shipped `setMasterOutputs` fix), master gain/mute, per-track mute, silence invariants. New `AudioEngine::prepareForTests(sr, blockSize)` lets tests set up scratch buffers without a real device. Test count 45 → 53. See `CHANGELOG.md`.
- [x] **Configurable monitor bus outputs** (M) — real-time click, NDI transmit, and Companion stream now route through `masterOutL` / `masterOutR` instead of hardcoded 0+1. New Monitor Bus card in the Audio Device dialog with L + R pickers; stale MON tooltip fixed. See `CHANGELOG.md`.
- [x] **Per-session workspace layouts auto-save** (M) — view / strip width / VCA-panel visibility / EDIT zoom now write into `.zfproj` on every change. New `saveUILayoutToActiveSession` + `EditPage::onZoomChanged` wiring. See `CHANGELOG.md`.
- [x] **Default session template** (S) — File ▸ Templates ▸ Set default template; the starred `.zftemplate` is auto-applied after every File ▸ New Session. See `CHANGELOG.md`.
- [x] **Stereo VCA / Edit Group persistence fix** (S) — `setTrackVcaGroup` / `setTrackEditGroup` now propagate to the R half of stereo pairs so assignments survive relaunch. Menu labels read "(L+R)" on stereo strips. See `CHANGELOG.md`.
- [x] **MainComponent split, parts 5 + 6 (tools + multi-selection)** (M) — extracted 467-line `MainComponentTools.cpp` (click track, punch, noise, soundcheck report, session properties) and 306-line `MainComponentStrips.cpp` (multi-selection ops). MainComponent.cpp down to 1590 lines (5522 → 1590 across the split day, -71 %). See `CHANGELOG.md`.
- [x] **Closed stale "crash recovery dialog on launch" priority** (S) — `offerSessionRecovery` + `SessionRecoveryDialog` already implement this; row removed from priorities.

### 2026-05-24
- [x] **MainComponent split, part 4 (help / onboarding)** (M) — extracted 343-line `MainComponentHelp.cpp`; MainComponent.cpp down to 2335 lines. See `CHANGELOG.md`.
- [x] **Keyboard automation-point navigation in EDIT** (S) — Left/Right walks focused point, Up/Down nudges value, Delete removes; undo-wrapped; focus ring in paint. See `CHANGELOG.md`.
- [x] **Marker keyboard shortcuts (`Cmd+1..9`)** (S) — was already implemented in `MainComponentKeys.cpp`; closed stale backlog row. Pro Tools-style Memory Location recall (zoom + visibleTracks) carries through.
- [x] **Tooltip pass on EditPage TrackRow + 4 component docs + alpha catalog + timer change-detection + spacing token sweep** (M) — round-2 audit punch list. See `CHANGELOG.md`.
- [x] **Automation phase 6 -- drag-handle continuous curve editing** (M) — new `AutomationPoint::tension` field, curve-aware lane rendering, draggable midpoint handles, `setAutomationTensionAt`, legacy ExpUp/ExpDown auto-mapped on load. Resolves the phase 5 deferral. See `CHANGELOG.md`.
- [x] **Automation phase 5 -- advanced write controls** (L) — Touch/Latch/Write dropdown, SUSPEND + PUNCH toggles with shift-drag range on the time ruler, WRITE point thinning, per-track Automation Safe lock with header LED, `safe`/`vTrim`/`pTrim` persisted in `.zfproj`. See `CHANGELOG.md`.

### 2026-05-23 (later in day)
- [x] **Phase 1 live-show safety pass** (M) — Record button shape distinctness, global PeakTally bar, STOP-while-recording two-tap guard, touch-target stacking at XS, brandOrange consolidation. See `CHANGELOG.md`.

### 2026-05-23
- [x] **Unified dialog chrome** (M) — extracted `Source/Theme/DialogChrome.h`; refactored 9 dialogs + `AlertWindow` LookAndFeel override.
- [x] **Motion + feedback polish** (M) — BigClock pulse, hover lifts on strips, `Toast` component, path-drawn cue arrows.
- [x] **Gradient sweep** (S) — every flat `g.fillAll(...)` replaced with `brand::verticalGradient(...)`.
- [x] **Design system audit fixes** (M) — personality saturation, `brandOrange` surfaced on the BigClock, `shadow::elev*` + `onSignal()` helpers, `h_subhead` + `h_hero` font sizes.
- [x] **Takes persistence + stable strip UUIDs + .zfproj `formatVersion=2` + reorder-during-playback modal** (M) — fixed two real data-loss bugs.

### Earlier
- *(see `CHANGELOG.md` for the full shipped history.)*
