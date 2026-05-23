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

### Added
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
