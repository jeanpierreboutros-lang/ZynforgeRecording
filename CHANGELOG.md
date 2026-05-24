# Changelog

All notable user-facing changes to ZynForge Recording are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versioning targets [Semantic Versioning](https://semver.org/spec/v2.0.0.html), though the project is pre-1.0 and minor / patch boundaries are pragmatic.

## How this file is maintained

- Add new entries under `## [Unreleased]` as you ship changes.
- When cutting a release, rename the section to `## [X.Y.Z] – YYYY-MM-DD` and start a fresh `## [Unreleased]`.
- Use the categories below. If none apply, the entry probably isn't user-visible and doesn't belong here.
- Keep entries terse but specific. Link to commits or `decisions.md` ADRs when context matters.

**Categories:** `Added` · `Changed` · `Fixed` · `Removed` · `Deprecated` · `Security`.

---

## [Unreleased]

### Changed (latest)
- **MainComponent split, part 4.** Help / onboarding cluster extracted to `Source/UI/MainComponentHelp.cpp` (343 lines). Covers `showFirstRunTutorial`, `showKeyboardShortcuts`, `showUserGuide`, `showAboutDialog`, `showStartupWelcome`, `launchNewSessionDialog`, `offerSessionRecovery`. MainComponent.cpp: 2654 → 2335 lines. Cumulative since start of the split day: 5522 → 2335. While extracting, the Keyboard shortcuts dialog gained two new sections — `MARKERS` (`Cmd+1..9`) and `AUTOMATION` (Left/Right/Up/Down/Delete on focused point) — both already implemented, just not previously documented in the user-facing list.

### Added (latest)
- **EDIT view -- keyboard automation-point navigation.** When a row is active (any prior click sets it) and the toolbar param is Volume / Pan / Mute, `Left` / `Right` walks the focused point through the active lane and seeks the playhead to it; `Up` / `Down` nudges the focused point's value (Volume 0.5 dB, Pan 0.05, Mute toggles between 0 and 1); `Delete` / `Backspace` removes it. Goes through the same `runAutomationEdit` wrapper as mouse edits so Cmd+Z reverts. The focused point paints a 12 px accent-Play ring so the engineer sees where Up/Down/Delete will act. Closes the keyboard-nav row from the design-system audit (was the last unchecked priority).
- **Tooltip pass on EditPage TrackRow header.** Name label, R/I/M/S buttons each have a one-line description so a first-time engineer can understand the row without reading docs.
- **Four more component docs:** `SessionRecoveryDialog`, `WelcomeDialog`, `EditToolsBar`, `MasterStrip`. Component-docs index now shows 12 / 8 covered (was 8 / 8 in the original audit deliverable; the remaining ~55 helpers + setting dialogs are documented on demand).
- **Alpha catalog in `BrandColors.h`.** The previously-unmapped values (0.06, 0.10, 0.14, 0.22, 0.25, 0.32, 0.45, 0.75) are now listed with their purpose so future contributors don't reach for an arbitrary literal when an existing tier already covers the case.

### Changed (latest)
- **Change-detection on AudioDeviceDialog InputMeter + PatchPage routing matrix timers.** Both previously called `repaint()` unconditionally every 60 ms; now compute a hash / quantised peak and only repaint when state actually changed. Idle CPU savings small but the pattern matches the rest of the app.
- **Spacing sweep -- `removeFromTop(24/22/20/26)` literals replaced with `brand::space::btnH / ctrlH / ioH / rowH` tokens** across `Source/UI/*.{cpp,h}`. Visible behaviour unchanged; closes the last open token-coverage row from the audit.

### Changed (later)
- **AudioEngine split (god class part 1):** Extracted the automation lane subsystem into `Source/Audio/AudioEngineAutomation.cpp` (551 lines). Covers `findLane`, `getAutomation`, `addAutomationPoint`, `removeAutomationPointNear`, `setAutomationCurveAt`, `setAutomationTensionAt`, `automationValueAt`, `clearAutomation`, `clearAutomationForTrack`, `copyAutomationRange`, `clearAutomationRange`, `pasteAutomationRange`, `setTrackAutomationSafe`, `isTrackAutomationSafe`, `writeAutomationPointThinned`, `setAutomationTrim`, `getAutomationTrim`, `clearAllAutomationTrims`, `automationToJson`, `loadAutomationFromJson`, plus the file-local `addPointLocked` helper. Real-time read path (`automationValueAt`) stays disciplined -- `ScopedTryLock` + atomic fallback unchanged. AudioEngine.cpp drops 2660 → 2128 lines.
- **MainComponent split (part 3):** Session IO cluster extracted to `Source/UI/MainComponentSessionIO.cpp` (747 lines). Covers save / load / export / import / template ops. New `Source/UI/SessionProjPath.h` consolidates the previously-duplicated `findSessionProj` helper across three call sites. MainComponent.cpp 3872 → 3138 lines; cumulative 5522 → 3138 since start of day.

### Added (later)
- **More tests** -- `Source/Tests/MarkerTests.cpp` (6 tests) and `Source/Tests/EngineStateTests.cpp` (7 tests). Total test count 9 → 22. Marker tests round-trip through a real temp directory; engine state tests cover strip count / naming / colour / clamp behaviour / stereo / `swapTracks` / `clearAllStripOverrides` (the same path that caused the "first two tracks pre-panned from stale stripGains" bug earlier this week) / VCA assignment.

### Deferred
- **EditPage split.** Attempted, reverted in this session. The two big nested classes (`TrackRow` 2600 lines, `TrackList`) have inline method bodies inside `EditPage.cpp`; extracting them requires either promoting them to file-scope or moving the class definitions to a header. Real refactor deferred to its own dedicated session -- the safer half-measure (extracting just the EditPage shell methods) doesn't compile because the shell depends on `TrackList` internals.

### Changed
- **MainComponent god-class split, part 2.** Two more clusters extracted:
  - `MainComponentCues.cpp` (628 lines) -- `loadSetlistFromActiveSession`, `saveSetlistToActiveSession`, `jumpToCue`, `promptCueName`, `addCueAtTransport`, `renameCurrentCue`, `updateCueAtTransport`, `startCueRampTo`, `updateCueRamp`, `printSetlist`. Carries its own file-local `snapshotStrip` (TrackState) helper.
  - `MainComponentEdit.cpp` (457 lines) -- the full undo / redo / cut / copy / paste / solo / crop / split / range-marker / marker-drop / automation-transaction surface. Carries the anonymous-namespace block with `snapshotStrip(eng,idx)`, `restoreStrip`, `physicalFromLogical`, `currentPlayheadSamples`, `AutomationSnapshotAction`, and `MixerSnapshotAction` -- all of which were only used by these methods.
  - `findSessionProj` is duplicated as a `static` helper in both `MainComponent.cpp` and `MainComponentCues.cpp` while the session-IO cluster still lives in `MainComponent.cpp`; should consolidate into a shared header when that cluster gets extracted next.
  - **Main file: 4896 → 3872 lines (-1024 cumulative).** Going from 5522 at the start of today to under 4000 lines in two passes. Menu construction (`getMenuForIndex` + `menuItemSelected`) and session I/O remain the two big lumps.

### Security
- **CompanionServer is no longer wide-open.** Used to bind `0.0.0.0` with zero auth, so anyone on the same Wi-Fi could arm tracks. Now binds `127.0.0.1` by default, requires a per-start 32-hex-char access token via `?t=<token>` or `Authorization: Bearer <token>`, and copies the full URL (with token) to the clipboard on startup so the engineer can paste it on a phone. LAN exposure is opt-in via the new `startCompanionServerOnLan(port)` API. See `decisions.md`.

### Added
- **Test harness** (`Source/Tests/AutomationTests.cpp`). 9 automation lane tests cover insert/sort, kSnap collapse, Mute discretisation, tension interpolation, Safe-lock blocking, write thinning, punch gating, JSON round-trip, and legacy curve migration. Run with `ZYNFORGE_RUN_TESTS=1` or `--run-tests`; results go to stderr + `~/Library/Logs/Zynforge/test-report.log`. Exit code is the failure count. `AudioEngine::setTestModeSkipAudioInit(true)` lets tests construct an engine without opening a 256-channel audio device.

### Fixed
- **Write paths silently no-op'd on uninitialised lane storage.** `addAutomationPoint`, `removeAutomationPointNear`, `clearAutomationRange`, `pasteAutomationRange`, and `writeAutomationPointThinned` all early-returned when `track >= automationData.size()` -- which was usually true on a fresh strip since `setStripCount` doesn't pre-populate per-strip lane storage. Caught by the new test harness on its first run. Now auto-resizes (consistent with `findLane` / `setAutomationTrim` / `setTrackAutomationSafe` / `loadAutomationFromJson`).
- **`writeAutomationPointThinned` duplicated `addAutomationPoint`'s body** (kSnap, Mute snap, sort). Both now share an `addPointLocked` helper. The misleading "we drop ours first / CriticalSection is recursive" comment is gone.
- **Curve-picker "Reset bend -- none" label hack** -- the "-- none" suffix was a workaround for the fact that the item was already disabled by the boolean param. Label is now just "Reset bend (handle)"; enabled state continues to encode the rest.
- **Tension drag-handle paint-vs-hit-test inconsistency in the lane-paint path** -- `paint()` had its own inline `toolbar->getParam()` switch that fell through to Volume for Click/Tempo, while `hitTestTensionHandle` already used `currentLaneParam()`. Both now use the helper.
- **PUNCH range band painted at full alpha when PUNCH wasn't armed** (already shipped earlier today, noted here for completeness).
- **Bare `1`..`9` cue-vs-marker ambiguity.** Pressing a digit used to jump to a cue OR a marker depending on which existed -- silent fall-through made the same key do different things in different sessions. Bare digits now do cues only; markers are always `Cmd+N`. See `decisions.md`.
- **Modal stacking on first launch.** Recovery + tutorial + welcome each used their own `callAfterDelay` with no coordination; on a first-run launch with crashed sessions, three modals could stack. Now serialised through a single nested callback chain: recovery → tutorial → welcome.

### Added (earlier today)
- **Session recovery dialog** (`Source/UI/SessionRecoveryDialog.h`) replaces the easy-to-miss popup menu. Sortable table shows session name, track count, on-disk size, and last-modified date for every orphan; Recover loads the session and clears the `recording.session` marker, Delete removes the directory after a confirm, Skip leaves orphans for next launch. Dialog is DialogChrome-styled and fires at launch before the WelcomeDialog.
- **Curve picker > Reset bend** menu entry on every automation point -- wipes per-segment tension back to 0 without changing the shape preset. Disabled when the segment isn't bent.
- **Shift-snap on tension drag** -- holding Shift while dragging a curve handle snaps tension to a 0.25 grid so matched ramps across multiple segments are easy to dial in.

### Changed
- **Cmd+1..9 jumps to marker N** (Pro Tools "Memory Location" shortcut). Distinct from the bare 1..9 cue-jump so the two don't compete for the same key; surfaces a status hint when the slot is empty.
- **MainComponent.cpp split (god-class refactor, part 1):** extracted `timerCallback` + `updateTransportLabels` into `MainComponentTimer.cpp`, `keyPressed` into `MainComponentKeys.cpp`, and `paint` + `resized` into `MainComponentLayout.cpp`. Main file drops 5522 → 4896 lines; the high-traffic refresh tick, shortcut table, and layout math are each editable in isolation. Menu construction (`getMenuForIndex`, `menuItemSelected`) is the next clean cut but is intertwined with too many helpers for a safe one-shot extraction -- left for a follow-up pass.

### Changed
- **Design audit pass:** swept hardcoded colour literals out of chrome paint paths. Two new tokens:
  - `brand::inputBg` = deep black for text-editor and combo bodies (was inline `Colour(0xff000000)` across `SetlistBar`, `AutomationToolbar`, `AddTracksDialog`, `SessionPropertiesDialog`).
  - `brand::accentEdit` = Pro Tools-blue-adjacent for EDIT loop/selection bands (was inline `Colour::fromRGB(0x3a, 0x90, 0xe0)` in EditPage).
- Replaced `juce::Colours::white` on saturated-accent backgrounds (VCA / BUS chips, EDIT take chip, PatchPage strip headers and active dots, EditToolsBar active tool icon) with `brand::onSignal(bg)` so foreground legibility follows the bg's perceived brightness instead of being pinned white. Yellow / amber / green / teal chips now self-correct.
- Dropped a needless `juce::Font(...)` wrapper around an already-`Font` in PatchPage.

### Fixed
- **Crash-recovery scan now finds user-named sessions.** `AudioEngine::findIncompleteSessions` used to filter to `Session_*` dirs only, silently skipping orphans from any session the engineer named themselves. Filter dropped -- every subdir of the Sessions root is now scanned for the `recording.session` marker.
- **Tension drag-handles hit-test the wrong lane** when the toolbar's parameter and the row's lane-mode disagreed (e.g. toolbar on Pan, row defaulted to Volume). Lane painting used the toolbar override but hit-test fell back to raw lane-mode, so the visible handle and the clickable target were on different lanes. New `TrackRow::currentLaneParam()` is the single source of truth for both paint and hit-test.
- **PUNCH range painted at full alpha when PUNCH wasn't armed**, making it look live even though writes weren't gated. Band now dims to 8% fill + 45% edge alpha when PUNCH is off, brightens when armed.

### Added (earlier)
- **Automation phase 6 -- continuous-curve drag handles**:
  - New `AutomationPoint::tension` float (-1..+1) bends Linear segments into ease-in (< 0) or ease-out (> 0) via a `t^(2^(-tension*4))` shaping function. 0 = straight.
  - **Tension drag handle** painted at the midpoint of every non-Hold segment in the EDIT lane. Drag vertically to bend the curve; what you see is the actual shape that `automationValueAt` produces at playback time (the lane renderer now mirrors the engine's interpolator exactly instead of step-painting).
  - The EDIT lane renderer used to draw stepped polylines regardless of curve type; it now produces curve-aware paths (Hold steps, Linear with tension, SCurve smoothstep, legacy ExpUp / ExpDown power curves).
  - `setAutomationTensionAt(...)` engine API promotes Hold / SCurve segments to Linear when bent (handle is a Linear affordance; preset shapes remain reachable via the right-click curve-picker menu).
  - Picking a preset in the curve-picker menu now resets per-segment tension to 0 (so "Linear" always means straight, not "still bent from a previous drag").
  - Legacy `ExpUp` / `ExpDown` enum values are auto-mapped to Linear with `tension = ±0.5` on load so old `.zfproj` files keep their visual shape and become drag-bendable.
  - `.zfproj` round-trips the new `t` key per point (omitted when |tension| < 1e-4 to keep small lanes compact).
  - Undo: tension drags open / close the same automation transaction as point drags, collapsing the whole gesture into one undo step.

### Added (earlier)
- **Automation phase 5 -- advanced write controls**:
  - **Touch / Latch / Write dropdown** on the automation toolbar replaces the single WRITE toggle. `AudioEngine::AutomationWriteMode` already had the three states; the toolbar now surfaces them so the engineer's mental model matches Pro Tools / LiveTrax even though the engine currently treats Touch/Latch/Write identically at the write-path level. Picking any non-Off value still forces TRIM off.
  - **SUSPEND toggle** -- engine ignores every lane at read time (`AudioEngine::automationReadSuspended` atomic, short-circuits `automationValueAt`). Lets the engineer audition raw fader / pan / mute moves without disturbing the stored pass.
  - **PUNCH toggle + range** -- shift-drag on the EDIT time ruler defines a `[in, out)` range painted as a translucent `accentStatus` band; the toolbar PUNCH button gates WRITE-mode drops to that range. Outside the range, write calls are no-ops (`AudioEngine::isInsideAutomationPunchRange`). Shift-click without drag clears the range.
  - **WRITE point thinning** -- new `AudioEngine::writeAutomationPointThinned (..., minSamplesBetween)` skips drops less than ~50 ms apart so a 60 Hz fader callback stream stops fanning out into automation cruft. `writeAutoIfPlaying` now calls the thinned path.
  - **Per-track Automation Safe lock** -- new `TrackAutomation::safe` atomic + `setTrackAutomationSafe(int, bool)` blocks every write (Add, Remove, Paste, Range-clear, WRITE-mode drops, TRIM offsets) on a single track. Exposed as 'Automation Safe' in the ChannelStrip context menu; persists through `.zfproj` via `automationToJson` (`safe`, `vTrim`, `pTrim` keys).
  - **Strip R/W LED** -- 6 px dot under the colour swatch on each ChannelStrip header. Red = WRITE-mode armed AND playback rolling; amber = Safe-locked (Safe wins). MainComponent's 10 Hz polling pushes state via `ChannelStrip::setAutomationLed`.

### Added (earlier)
- **Phase 1 live-show safety pass** (from the 2026-05-23 UX audit):
  - **Record button is shape-distinct from Play**: permanent brand-red 2 px border at idle + concentric-ring "target" glyph (outer ring + inner disc) instead of a solid circle. Readable as RECORD vs PLAY by silhouette under stage glare without relying on colour alone.
  - **Global PEAK tally** (`Source/UI/PeakTally.h`) — a 4 px brand-red bar pinned across the top of the mixer that pulses at 2 Hz whenever any strip clips and holds for ~1 s after the last clip. Click anywhere on the bar to clear every strip's clip latch in one gesture.
  - **STOP-while-recording two-tap guard** — pressing STOP (or spacebar) while `engine.isRecording()` now arms first and surfaces a `Toast.Kind::Warning` ("Tap STOP again to end the recording"). A second tap within 2 s actually stops; any later tap re-arms. Prevents fat-fingered ends-of-takes.
  - **Touch-target compliance at XS strip width** — the REC/MON/MUTE/SOLO 2×2 grid auto-stacks vertically to a 1×4 column when strip width is < 78 px so each toggle gets full strip width (~70 px). Restores the 44 pt minimum tap target.
  - **`brandOrange` consolidation** — orange is now reserved for `signalMute()` + brand assertion only. New `signalArmedReady()` (= `engagedAmber`) for the BigClock armed-but-idle border; new `toolActive()` (= `featureEngaged`) replaces a hardcoded blue literal in `EditToolsBar`; the click-track button moves from `brandOrange` to `accentVS`.
- Unified dialog chrome via `Source/Theme/DialogChrome.h`. Every modal — first-party `DialogWindow`s *and* `juce::AlertWindow::showAsync(...)` — now wears the AudioDevice dialog's look: orange title stripe, gradient `bgPanel` background, footer divider, `accentStatus` Apply / `bgElevated` Cancel buttons.
- `Toast` non-modal feedback component (`Source/UI/Toast.h`). Bottom-right pill, fade-in / hold / fade-out, queued. Surfaces every `showStatus(...)` call as a calm non-blocking acknowledgement.
- Hover affordance on `ChannelStrip`, `EditPage::TrackRow`, and `VcaStripView` — ~6% brightness lift + brighter edge.
- BigClock pulse animation — REC background breathes at 1 Hz; brand-orange armed-but-idle border pulses to draw the eye.
- Vector cue-navigation arrows in `SetlistBar` (path-drawn triangles replacing the unicode `◂` / `▸` glyphs).
- Design system tokens: `brand::shadow::elev1/elev2/elev3`, `brand::onSignal(bg)`, `h_subhead = 22 pt`, `h_hero = 44 pt`.
- Documentation set: `architecture.md`, `tasks.md`, `decisions.md`, `coding-standards.md`, `testing.md`, this `CHANGELOG.md`.

### Changed
- Personality palette pass 2: moss / olive / violet / teal swatches bumped ~12–15% on saturation and luminance so XS-width strips read against `bgDeep`.
- `brand::uiFamily` renamed to `"Inter"` and `monoFamily` to `"JetBrains Mono"` (matches bundled BinaryData faces). `ZynForgeLookAndFeel` still resolves legacy `"SF Pro"` / `"SF Mono"` for any caller that hasn't migrated.
- Every previously-flat painted surface now uses `brand::verticalGradient(...)` — `MainComponent`, `BigClockPanel`, `VcaPanel`, `TimelineStrip`, `MiniSpectrum`, `StripColourPicker`, `Meterbridge`, marker / noise / export / session-settings dialogs.

### Fixed
- **Data loss: Takes (comp playlists) were RAM-only.** `engine.playlistsToJson()` / `loadPlaylistsFromJson()` now serialise full take state into `.zfproj`. Engineers no longer lose alternate takes on app close.
- **Silent corruption: cue snapshots referenced strips by array index.** New `TrackState::stripId` (UUID) is captured in each `StripSnapshot` and resolved on recall. Pre-v2 cues (no `uid`) fall back to index lookup.
- `.zfproj` now carries `formatVersion: 2` — schema migrations are now explicit.
- Strip reorder during playback now shows a modal `AlertWindow` warning instead of silently dropping the drag.

---

## [0.0.1] – 2026-05-17

First public commit. Baseline JUCE 8 / C++20 / CMake skeleton.

### Added
- Multitrack recorder with per-channel `AbstractFifo` + background WAV writer, periodic header flush, parallel backup writer.
- Virtual-soundcheck player with clip-aware playback via `BufferingAudioReader`.
- OSC remote with feature parity across five console dialects (Generic, DiGiCo, A&H SQ, SSL Live, Yamaha / RIVAGE).
- Embedded HTTP companion server (`/`, `/state.json`, `/cmd`, `/stream.wav`).
- Marker + cue + setlist persistence to `markers.json` and `.zfproj`.
- Brand-aligned visual identity (palette, fader/meter style, LED-segment meters).
