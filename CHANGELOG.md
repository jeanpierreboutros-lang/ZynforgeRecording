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

### Added (latest)
- **SMART drive-health status via `diskutil`** -- new `MultitrackRecorder::querySmartStatus(File mountPoint)` shells out to `/usr/sbin/diskutil info <path>` and parses the `SMART Status` line (`Verified` / `Failing` / `Not Supported`). macOS-only (the function returns `Unknown` on other platforms; full IOKit/Obj-C++ glue would be hours and produce roughly the same answer). UI timer polls primary + backup volumes every ~30 s; if either returns `Failing`, the status bar reads `⚠ SMART FAILING -- replace this drive`. Slow (50-200 ms blocking subprocess) but runs once per 720 ticks so it's invisible.
- **Auto-arm on input detect** -- new `engine.setAutoArmOnInputDetect(bool)` (persisted in `appProps`) + `serviceAutoArm(periodTicks, ampThreshold)` polled by the UI timer at 24 Hz. Walk the stage hitting each mic; tracks whose peak stays above -40 dBFS for ~0.5 s arm themselves. Useful for first-time channel-discovery on an unfamiliar console. Session menu toggle. No-op while recording (mid-take arming is the audio thread's domain). Per-strip streak resets on silence so transient noise doesn't latch a permanent arm.
- **Mirror Drives dialog** -- File ▸ Mirror Drives... opens a modal where the engineer adds / removes mirror destinations and picks each one's format independently. Persisted via `engine.setMirrors` → `appProps[mirror_count, mirror_root_<n>, mirror_format_<n>]`. The engine restores them on launch (skipping any whose root has gone missing). The N-way mirror engine API shipped earlier today is now actually usable from the UI.
- **Mocked-failure tests** -- exercise the primary-failure crash resistance (deleting the Audio Files subtree mid-take), the disk-struggling threshold logic (force-feed a 1 GiB/s expected rate, three consecutive samples trip the flag), and the failed-mirror isolation (a mirror pointing at a regular file instead of a directory is silently absent; primary + good mirrors keep going).
- **Primary-writer failure detection (hot-swap surfacing)** -- the drain loop now checks `writeFromFloatArrays` return value on the primary writer, closes the handle and flips `primaryFailed` if the write fails (disk full, path disappeared, permissions). Audio continues to land on backup + mirrors without interruption; UI status bar reads `⚠ PRIMARY WRITE FAILED -- recording on backup/mirror` so the engineer can swap drives or accept the reduced redundancy. `session.report.json` now carries `primaryFailed: bool` at the top level.
- **Disk-keep-up health monitor (SMART proxy)** -- `MultitrackRecorder::updateDiskHealth(expectedBytesPerSec)` compares actual `getDiskBytesPerSec()` to expected via a 0.7/0.3 EMA. Three consecutive samples under 85 % keep-up flips `isDiskStruggling()`; the UI timer polls + updates every ~0.5 s and surfaces `⚠ DISK STRUGGLING -- missed samples imminent` in the status bar **before** missed samples actually start landing. Resets automatically when disk catches up. Real cross-platform SMART/IOKit is out of scope; this proxy catches the same failures (sustained writer-thread fall-behind) engineers actually care about.
- **N-way mirror destinations (3+ simultaneous copies)** -- on top of primary + backup, the engineer can register any number of additional mirror destinations via `engine.getRecorder().setMirrors({ { root, format }, ... })`. Each mirror writes a full parallel copy of every armed track in its own configurable format, with independent auto-split state, byte counter, and failure flag. Useful for broadcast / archival workflows that want primary (fast SSD) + backup (different drive) + off-board mirror (NAS / second machine) all running concurrently. Each mirror's file list + sample count + SHA-256 lands in `session.report.json` under a per-track `mirrors: [...]` array. Failed mirror (disk full, path disappeared) marks `failed: true` and skips subsequent writes; primary + backup + other mirrors keep going. UI for picking mirror roots is a follow-up; engine API is in.
- **SHA-256 per part file in `session.report.json`** -- on `stopRecording` every `Track_NN_partXX.<ext>` is streamed through a SHA-256 digest (offline, after the writer threads finish — no audio-thread cost), and the hex digests land in the report as parallel `sha256` / `backupSha256` arrays. Mix engineers reading the report months later can verify any imported file is bit-identical to what landed at the gig. New `juce_cryptography` module added to CMake to make `juce::SHA256` available.
- **Free-space pre-flight + live "minutes remaining" estimate** -- `MultitrackRecorder::estimateBytesPerSecondForArmedTracks()` projects worst-case throughput from the current arm + format config (assumes FLAC = uncompressed for pessimism); `estimateMinutesRemaining(primary, backup)` divides each volume's free bytes by that rate and returns whichever drive runs out first. Engine caches the value in `diskMinutesRemaining`; UI's record-button status reports "Recording 24/24 tracks → SessionName -- ~127 min remaining" (or "⚠ DISK ~12 min remaining" when under 30 min). Timer refreshes the estimate every ~2 s during the take.
- **`session.report.json` enumerates part files per track** -- each track entry now carries a `files` array listing every `Track_NN.wav` + `Track_NN_partXX.wav` the recorder produced (in order), plus a `totalSamplesPrimary` that sums across all parts. When a backup is active, mirrors as `backupFiles` + `totalSamplesBackup`. Mix engineers reading the report after the show see at a glance which files belong to which track and can confirm a long take rolled correctly. Test that opens the report after a forced auto-split and verifies both `Track_01.wav` + `Track_01_part02.wav` are listed in order. Test count 79 → 80.
- **Auto-split on WAV / AIFF / FLAC chunk-size ceilings** -- a recording that crosses ~3.9 GiB (WAV / FLAC) or ~1.9 GiB (AIFF) per file now rolls cleanly to `Track_NN_part02.<ext>`, `Track_NN_part03.<ext>` ... instead of silently producing a malformed-header tail. New `WriterChannel` tracks bytes written + part number + base path + format spec; the drain loop projects the next write's size and rotates the writer before the limit is crossed (so the closing header rewrite stays valid). Backup writers roll independently with the same naming under the backup root. Practical impact: a 24-hour installation or all-day festival capture at any format no longer hits a file-format wall.
- **Recorder write-path test extended with the auto-split case** -- new `MultitrackRecorder::setAutoSplitThresholdBytesForTests(int64)` lets unit tests force the roll path at a 256 KB threshold instead of waiting for 3.9 GiB of actual writes. Test drives ~3 s of audio, asserts `Track_01.wav` + `Track_01_part02.wav` both exist and are valid WAVs with audible content. Plus a smaller smoke test that the production thresholds (> 3 GiB for WAV, > 1 GiB for AIFF, AIFF < WAV) restore after the override clears. Total tests 77 → 79.
- **Marker drop during record + master clip latch + output mute tests** -- 6 more: marker drop with no session returns -1; marker drop during recording captures the live `samplesSinceStart` position; sequential marker drops auto-increment and stay in time order; master clip latch sets when output peak ≥ 0.999 (DC input of 4.0 trips it via the monitor sum); clip latch stays sticky across silent blocks until explicitly cleared; per-strip `outputMuted` round-trips independently of stereo partner. Total tests 71 → 77.
- **Pre-roll + BWF metadata tests** -- 3 more end-to-end tests: pre-roll dumps pre-record history (the constant 0.30 segment fills the first 0.4 s of the WAV, then the live 0.70 segment lands at the tail); `setPreRollSeconds (0)` produces a live-only file; recorded WAVs carry BWF `bext` chunk metadata (originator = "Zynforge Recording", origination date / time / description all populated). Total tests 68 → 71.
- **Recorder write-path tests, end-to-end** -- 6 new tests in `AudioCallbackTests.cpp` drive the IO callback while recording is rolling into a temp dir, then read the resulting `Track_NN.wav` back from disk and assert length + content. Covers: armed strip produces `Track_01.wav` of the expected length with non-silent content, two armed strips produce two separate files, unarmed strip produces silent / absent file, `session.report.json` is written on stop, `isRecording()` flips correctly through start/stop, `stopRecording()` without `startRecording()` is a no-op. Total tests 62 → 68.
- **Audio-thread test harness expanded to 17 tests** -- 9 more groups covering solo isolation (with and without per-track mute), VCA mute + VCA solo (`channelAudible` gating), VCA group state round-trip, click-engine output routing (regression test for the monitor-bus routing fix), and stereo-pair summing (hard-left pan + hard-right pan = L+R spread). Test fixture now also wipes every per-strip and per-VCA atomic at construction so leftover `appProps` (vcaGroup, vcaSoloed, etc.) doesn't bleed between tests -- the source of two flakes hit during this batch. Surfaced a real engine design note: VCA *gain* applies only on the per-strip output-routing path, not on the monitor sum. Total tests now 53 → 62.
- **Real-time audio-thread test harness** -- `Source/Tests/AudioCallbackTests.cpp` exercises `AudioEngine::audioDeviceIOCallbackWithContext` directly with synthetic input buffers, no CoreAudio device required. 8 new tests cover the monitor-bus path: silent-input-silent-output, armed+monitored input passes through, master mute kills the sum, per-track mute removes contribution, `setMasterOutputs` routes to the chosen output pair (regression test for the bug we just shipped a fix for), master gain attenuation, stopped-player-no-armed-input silence, and audio-load percentage update. Test count 45 → 53. New `AudioEngine::prepareForTests(sr, blockSize)` mirrors `audioDeviceAboutToStart` without needing a real `juce::AudioIODevice`. Test fixture also forces a clean baseline on every relevant atomic so tests don't inherit the user's real `appProps` file (per-strip gain / pan / routing, master gain / mute / stereo) -- a real flake source we hit while writing this.

### Changed (latest)
- **Monitor bus is fully configurable** — no more hardcoded outputs 0+1 on the side-channel feeds.
  - Real-time click mix in `AudioEngine::audioDeviceIOCallbackWithContext` now routes through `masterOutL` / `masterOutR` (was pinned to outs 0+1).
  - NDI Audio transmit and Companion HTTP stream both follow the same monitor pair (were also pinned to 0+1).
  - **Audio Device dialog gains a "Monitor Bus" card** with L + R hardware-output pickers. Drives `engine.setMasterOutputs`, persisted in `appProps` under `masterOutL`/`masterOutR`, mirrored in the existing MasterStrip combo. Dialog grows from 640 × 460 → 640 × 560 to accommodate.
  - Fixed stale tooltip on the per-strip `MON` button: was hardcoded to "(outputs 1 + 2)", now reads "output pair set in Audio Device dialog".

### Removed (latest)
- **Bars|Beats ruler strip + everything bar/beat-visual** in the EDIT view. Live recorders navigate by wall-clock + markers, not by bars -- the 100+ amber bar numbers across the top of the ruler were visual clutter for zero operational gain. Cut:
  - The middle Bars|Beats strip in `EditTimeRuler` (the entire `kBarsBeatsH` band + the tempo-map walker that lit it). Ruler is now Markers (top, 20 px) + Min:Secs (bottom, 26 px), 46 px total.
  - The per-row click-beat overlay in `EditPage::TrackRow::paint` (faint vertical ticks at every beat under every non-click row when a Click track was present).
  - `SnapMode::Bars` from both `MainComponent::SnapMode` and `AudioEngine::SnapMode`, plus the tempo-map-walking `snapSampleToGrid` math for bars. Snap cycle is now Off ↔ Markers.
  - The three `snapSampleToGrid -- Bars ...` tests in `EngineStateTests` (45/48 tests remain, 0 failures).
- **Tempo math survives:** Click track render, cue tempo ramps, BPM in the header tempo bar, BPM in cue snapshots all still work. Just no visual bars in the ruler / rows.

### Added (latest)
- **Default session template.** File ▸ Templates ▸ Set default template lets the engineer "star" a `.zftemplate`. The default is auto-applied right after File ▸ New Session creates the session folder, so every show starts pre-patched with the engineer's preferred strip count / names / colours / routings. The starred template is marked `[default]` in the New Session from Template list; persisted in `appProps` so the choice survives relaunch.
- **Per-session workspace layouts auto-save.** View, strip width, VCA-panel visibility, and EDIT zoom now write into the session's `.zfproj` on every change (previously only on explicit Save). Reopening a show restores its workspace exactly as the engineer left it. Global `appProps["stripWidthPreset"]` stays as the fallback for new / unsaved sessions. New `MainComponent::saveUILayoutToActiveSession()`; new `EditPage::onZoomChanged` callback so the host knows when to write.

### Fixed (latest)
- **Stereo strips lost their VCA / Edit Group assignment on relaunch.** `ChannelStrip` had been mirroring `state.vcaGroup` to `pairState` in memory, but `AudioEngine::setTrackVcaGroup` (and `setTrackEditGroup`) only persisted the L track's assignment to `appProps`. The host callbacks now propagate the call to `i+1` when `step == 2`, so both halves come back assigned on the next launch. Menu labels say "Assign to VCA (L+R)" / "Assign to Edit Group (L+R)" on stereo strips so the engineer knows it covers the pair.

### Changed (latest)
- **MainComponent split, parts 5 + 6.** Two more clusters extracted in one pass:
  - `MainComponentTools.cpp` (467 lines) -- `generateOrRefreshClickTrack`, `togglePunchMode`, `servicePunch`, `runNoiseAnalysis`, `writeSoundcheckReport`, `showSessionProperties`.
  - `MainComponentStrips.cpp` (306 lines) -- multi-selection cluster: `clearStripSelection`, `selectAllStrips`, `deleteSelectedStrips`, `colourSelectedStrips`, `physicalFromLogicalIdx`, `moveSelectedStrips`, `showBatchRenameDialog`, `showBatchColourDialog`.
  - **MainComponent.cpp: 2335 → 1590 lines.** Cumulative since the start of the split work: 5522 → 1590 (-71 %). Remaining big lumps are the ctor (~590 lines, hard to move without breaking init ordering) and `rebuildStrips` (~295 lines, tightly coupled to MainComponent's strip vector).
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
