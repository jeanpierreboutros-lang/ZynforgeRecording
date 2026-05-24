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

- [ ] **Split `MainComponent.cpp`** (L) — ~5000 lines is a god class. Extract menu construction, layout, the 24 Hz refresh timer, and key handling into separate translation units. Coordinate with `architecture.md` §4.
- [ ] **Field-test the unified dialog chrome** (S) — the 2026-05-23 chrome pass shipped untested in a live rehearsal. Walk through every dialog (AudioDevice, NewSession, AddTracks, Export, Session Settings, Session Properties, Click Settings, Marker List, Noise Report) and every `AlertWindow.showAsync` site (reorder-during-playback, sample-rate mismatch, lock-against-overwrite, OSC bind failure). Look for clipping, divider misalignment, button width regressions.
- [ ] **Companion server TLS** (M) — currently HTTP only. Decide between self-signed cert + TLS or migrating to WebRTC. See `decisions.md` open question.
- [ ] **Per-session workspace layouts** (M) — strip widths, view selection, and tool selection are stored globally in `appProps`. Move into `.zfproj` so multiple gigs can have distinct layouts.
- [ ] **Crash recovery dialog on launch** (M) — detect orphan `recording.session` markers and offer to import the partial take.

## In Progress

- *(none — close out a session by moving items here back into the appropriate list.)*

## Backlog

- [ ] **Native iPad companion** (L) — replace the polled `/state.json` web client with a SwiftUI app over WebSocket.
- [ ] **Audio-stream encryption** (M) — `/stream.wav` is plaintext PCM. Wrap in TLS or migrate to SRTP.
- [ ] **Test harness for `Source/Audio/`** (L) — pure-C++ unit tests with a mocked `AudioIODeviceCallback` driver, runnable headless. See `testing.md`.
- [ ] **Marker keyboard shortcuts** (S) — Cmd+1..9 to jump between marker indices.
- [ ] **Configurable monitor bus outputs** (M) — currently pinned to outs 0+1. Surface a picker in Audio Device dialog.
- [ ] **Stereo VCA assignment shortcut** (S) — right-click a strip with a stereo pair to assign both halves at once.
- [ ] **Auto-arm-on-input-detect mode** (M) — optional setting where a strip arms itself when persistent signal is detected (useful for first-time pre-show configuration).
- [ ] **Session template "starred" picker** (S) — promote a `.zftemplate` to "default" so File ▸ New Session pre-selects it.

### Explicit non-goals (do not implement)

- [ ] ~~Per-track plugin slots / AU / VST hosting~~ — explicitly rejected. See `decisions.md` *No plugin hosting*. Recorded here so future contributors don't relitigate.

## Recently Completed

### 2026-05-24
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
