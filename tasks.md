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

- [ ] **Optional: finish splitting `MainComponent.cpp`** (M) — down from ~5500 lines to 1590 after parts 1–6 (timer, keys, layout, cues, edit, session-IO, menu, help, tools, strips). The remaining lumps (ctor 590 lines, `rebuildStrips` 295 lines) are tightly coupled to MainComponent's member init / strip vector — extracting them needs a real refactor, not just a cut/paste. Park as low-priority unless someone wants to touch the strip-rebuild path.
- [ ] **Field-test the unified dialog chrome** (S) — the 2026-05-23 chrome pass shipped untested in a live rehearsal. Walk through every dialog (AudioDevice, NewSession, AddTracks, Export, Session Settings, Session Properties, Click Settings, Marker List, Noise Report) and every `AlertWindow.showAsync` site (reorder-during-playback, sample-rate mismatch, lock-against-overwrite, OSC bind failure). Look for clipping, divider misalignment, button width regressions.
- [ ] **Companion server TLS** (M) — currently HTTP only. Decide between self-signed cert + TLS or migrating to WebRTC. See `decisions.md` open question.

## In Progress

- *(none — close out a session by moving items here back into the appropriate list.)*

## Backlog

- [ ] **Native iPad companion** (L) — replace the polled `/state.json` web client with a SwiftUI app over WebSocket.
- [ ] **Audio-stream encryption** (M) — `/stream.wav` is plaintext PCM. Wrap in TLS or migrate to SRTP.
- [ ] **Expand audio-thread test harness, round 4** (M) — 26 tests in `Source/Tests/AudioCallbackTests.cpp` covering monitor-bus routing, master gain/mute, solo isolation, VCA mute/solo gating, click engine routing, hard-pan L/R, stereo-pair summing, recorder write path, **pre-roll history backfill**, **BWF bext metadata**. **Gaps**: VCA gain end-to-end and automation playback both need a loaded `playerScratch` feed (load a tiny real WAV in a fixture); aux send routing; stream-bus path (needs companion server); marker drop during record; punch-record automation; loop-region playback wrap.
- [ ] **Auto-arm-on-input-detect mode** (M) — optional setting where a strip arms itself when persistent signal is detected (useful for first-time pre-show configuration).
- [ ] **Extend accessibility across the UI** (L, in progress) — `PlaceholderView` set the baseline (title/description + VoiceOver announcement + focusable CTA). **Done (each headless-tested):** `ChannelStrip` (strip named after channel; R/I/M/S + gain/pan/routing titled), `TransportBar` (6 icon buttons + focus group), `SetlistBar` (cue arrows/combo/buttons), `EditToolsBar` (6 raw-Component tools → button-role handler + keyboard Return/Space + titles), `MasterStrip` (gain/mute/mono-stereo/output), `VcaPanel` (per-bus gain/mute/solo/name × 8). **Remaining — clean control-level (same pattern):** `AutomationToolbar`, EditPage zoom +/- buttons. **Remaining — bigger (custom handlers):** `PatchMatrix` as an accessible routing *table* (per-cell `AccessibilityCellInterface` + keyboard grid nav), live meter values via `AccessibilityValueInterface`, and the EDIT clip/automation lanes. See the accessibility rule in `coding-standards.md`. Note: live VoiceOver verification (Cmd+F5) still needed — tests assert names are set, not what's spoken.

### Explicit non-goals (do not implement)

- [ ] ~~Per-track plugin slots / AU / VST hosting~~ — explicitly rejected. See `decisions.md` *No plugin hosting*. Recorded here so future contributors don't relitigate.

## Recently Completed

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
