# Zynforge Recording — Claude operating notes

## Project

JUCE 8 / C++20 / CMake, macOS-first (Universal). Live **multitrack recording + playback** software with **virtual soundcheck**. Sibling project to the ZynForge Live plugin-insert host.

The engineer's surface: record every input on the desk, play those tracks back through the same outputs during soundcheck, never lose a take.

## Workflow rules (always)

1. **After every code change**, update `CLAUDE.md` and `README.md` to reflect what's actually in the codebase.
2. **Build** with `cmake --build build --config Release` after every change. Don't claim success without a successful build.
3. **Commit and push to `origin/main`** after every change. No asking — auto-push is the policy for both ZynForge projects.
4. **Do not credit** Harrison LiveTrax / Waves Tracks Live as inspiration anywhere — not in code, comments, docs, or commit messages.

## Build

```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

JUCE 8.0.4 is pulled via `FetchContent` on first configure. Artefact: `build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app`.

## Architecture map

- `AudioEngine` — owns `juce::AudioDeviceManager`, registers as the `AudioIODeviceCallback`. Per callback: clears outputs, runs `MultitrackRecorder::processBlock` (input path), runs `SessionPlayer::processBlock` into a per-track scratch buffer, applies mute / solo gate to both VSC outputs and monitor bus, sums monitored + audible channels into outputs 0+1 with constant-power pan + per-channel gain (the monitor accumulator is `juce::AudioBuffer<double>` for clip-proof internal precision), feeds the stream bus into outputs `streamOutL/R` AND the optional `StereoMix.wav` writer AND the optional `CompanionServer` audition stream, drives meter peak/RMS from playerScratch when `player.isPlaying()` (recorder always drives meters from inputs), runs `TimecodeChase::feedLtc` on the configured LTC source strip, computes phase correlation between the selected input pair. Owns `MarkersManager` + the persistent stores + `CompanionServer` + `OscRemote` + `TimecodeChase`.
- `MultitrackRecorder` — real-time-safe input path. Per-channel `juce::AbstractFifo` ring buffer, drained by a `TimeSliceThread` writing the configured format. `CaptureFormat` enum: `Wav16/24/32Float`, `Aiff16/24/32Float`, `Flac16/24`. Periodic header flush every ~5 s so a crash leaves playable files. Per-channel `PreRollBuffer` rings for buffered history dumped to writers before `recording=true`. **Backup writer per channel** (`backupWriter`) mirrors every drain to a secondary directory AND can use a **different `CaptureFormat`** from the primary via `setBackupCaptureFormat` — engineer runs WAV/24 primary + FLAC/24 backup. `recording.session` JSON marker for crash recovery. **`stopRecording` also writes `session.report.json`** with stop time, total samples + seconds, missed samples, backup status, per-track clip count + routing + isStereo. Meters (peak + RMS + clip) live on `TrackState`. `removeTrackAt(int)` deletes a single track and shifts state down; `setTrackCount` is the bulk path.
- `SessionPlayer` — real-time-safe output path. Scans a session dir for `Track_*.wav`, wraps each in a non-blocking `juce::BufferingAudioReader`. Loop region (`loopStart/End` atomics) wraps from end → start cleanly. Session swap defers the reader-vector mutation until in-flight callbacks drain. **Clip-aware render path**: when a track has an active clip list, `processBlock` walks each clip, reads from `fileStartSamples` only inside its timeline span, skips clips with `muted=true`, applies `juce::Decibels::decibelsToGain(clip.gainDb)`, and multiplies by the linear fade envelope when `fadeInSamples` / `fadeOutSamples` are non-zero. Locked clips refuse trim / move / fade edits at the engine boundary.
- `MarkersManager` — per-session marker list, `markers.json`. CRUD: drop/get/remove/rename/setSample. Mutated only from the message thread.
- `StripColours / StripNames / StripGains / StripRouting` — per-channel overrides persisted via `juce::PropertiesFile` (`~/Library/Application Support/Zynforge Recording/`). Re-applied on `audioDeviceAboutToStart` and inside `setStripCount`. `appProps` also holds `recordStereoMix`, `ltcSourceStrip`, `cloudUploadCommand`, `strip_stereo_<n>` (logical stereo pair flag).
- `OscRemote` — `juce::OSCReceiver`. Five dialect parsers — `Generic` (`/zynforge/*`), `DiGiCo` (`/Console/*`), `AllenHeath` (`/sq/*`), `SSL` (`/sslnet/*`), `Yamaha` (`/Yamaha/*` + `/RIVAGE/*`). **Full feature parity across dialects** via the shared `dispatchChannelOp` helper: every dialect supports `record / play / stop` transport, `marker` drop, snapshot/scene recall, and per-channel `name / mute / arm / colour`. 1-based channel indices on the wire to match console conventions.
- `TimecodeChase` (`Source/Audio/TimecodeChase.h`, header-only) — LTC + MTC chase. Phase 1: LTC PRESENCE via zero-crossing rate (per-second count in 1500..5000 → LTC bit clock range). MTC quarter-frame + full-frame hooks. Exposes `isRunning()` + last MTC hr/mn/sc/fr atomics.
- `CompanionServer` (`Source/Network/CompanionServer.{h,cpp}`) — embedded HTTP server on user-chosen port (default 9000). Listener thread + worker-per-connection. Endpoints:
  - `GET /` → embedded HTML / JS bundle (dark ZynForge theme, touch-sized 48px hit targets, polls every 500 ms)
  - `GET /state.json` → channels (name, colour, armed/muted/soloed, peak) + transport state
  - `POST /cmd` → `{action, channel?, value?}` — mute / solo / arm per channel; play / stop / record transport
  - `GET /stream.wav` → continuous 48 kHz 16-bit stereo PCM with 24-h fake-length WAV header so any browser / iOS Safari `<audio>` element plays it as a live stream
  - Reads use `waitUntilReady(true, 200ms)` with a 5 s overall deadline so half-open clients don't wedge workers.
- `TrackState` — atomic meter values, armed flag (default **false** — engineer arms explicitly), monitor / mute / solo flags, gainDb (−60..+12, default 0), pan (−1..+1, default 0), clip counter, `isStereo` flag (L-of-pair marker), colourARGB, name, FFT FIFO/snapshot. Mute / solo / gain / pan affect monitoring only — recording stays pre-fader.
- `MainComponent` — macOS native menu bar (`MenuBarModel`):
  - **File**: **New Session…** (Pro Tools-style picker: name, Local Storage path, file type WAV/AIFF/FLAC, sample rate, bit depth, interleaved flag — see `NewSessionDialog`), Open Session…, **Import Audio Files…** (multi-select WAV/AIFF/FLAC/MP3/OGG/M4A/CAF; auto-detects mono vs stereo per file → mono goes to a single track, stereo writes L + R as two mono Track_NN.wav files with `isStereo=true` on the L), Save Session State, Save Session As…, Export ▶, Choose Backup Folder, Quit
  - **Edit**: Solo / Snap / Split / Range / Remove last capture
  - **Session**: Patch, Meterbridge, OSC (dialect picker + Stop), Upload session to cloud + Configure cloud upload command, Start/Stop companion server on :9000, Session Settings
  - Header: LOCK / +CH / DEVICE / RECORD on row 1; transport bar + MIXER/EDIT toggle + PATCH on row 2. **+ CH** prompts for count + Mono / Stereo mode. **Spacebar** toggles play/pause globally.
- `ChannelStrip` — colour swatch + name (double-click to rename inline) + 2×2 button grid (REC/MON/MUTE/SOLO) + dBFS readout + clip badge + horizontal pan + vertical fader (−60 .. +12 dB) + `LedMeter`. Right-click menu: Rename, Add channel, Delete channel, Link to next channel (stereo) / Unlink stereo pair, Change colour, Reset colour/name, Send to STREAM bus. `addMouseListener(this, true)` so right-click anywhere on the strip (including buttons/fader/combos) opens the menu. `refreshAppearance()` polls TrackState atomics every 10 Hz so changes from OSC / EDIT view / stereo-pair sibling appear in mixer button visuals. Touch-friendly drag sensitivity (160 horizontal, 250 vertical).
- `LedMeter` — adaptive: under 60 px height paints a smooth gradient bar; 60..80 px scales segment count (≥3 px each); above 80 px uses the full 20-segment LED look. `setStereoPartner(TrackState*)` adds a second bar driven by the partner's peak/RMS atomics. `setShowDbLabels(bool)` hides the 14 px label gutter when the host widget is too narrow (EDIT view rows).
- `EditPage` — per-track row, one entry per **logical** strip (stereo pairs collapse to a single row). Header has colour swatch (click → colour picker), name (double-click to rename), REC/MUTE/SOLO buttons, INPUT/OUTPUT routing combos (stereo strips list pairs `In 1-2 / Out 1-2`), live LedMeter (stereo when applicable). Waveform pane shows `juce::AudioThumbnail` of the matching `Track_NN.wav` (stereo: two stacked lanes painted in the channel's brightened personality colour). Right-click anywhere on the row opens a Pro Tools-style **size menu** (micro/mini/small/medium/large/jumbo/extreme/fit to window — per-row independent heights). Drag the bottom edge of any row for a custom pixel-precise height. Re-entrancy guard + `SafePointer` on the menu so a list rebuild between open and dismiss can't crash. 24 Hz playhead overlay. Owns the **`EditToolsBar`** (Pro Tools-style 6-button icon strip — Smart / Selector / Trim / Grabber / Fade / Scrubber) painted along the top edge; the active tool biases every `TrackRow::mouseDown` so Trim/Grabber change clip-body behaviour while Selector seeds a loop region, Fade pops the clip menu, and Scrubber parks the playhead on click + drag.
- `PatchPage` — modal `DialogWindow` with INPUT / OUTPUT tabs. **Iterates logical strips** (stereo pairs collapse to one column). Each column header has the strip's colour + number + actual track name (mirrors mixer/EDIT) + **M / ST pill** (click to toggle mono ↔ stereo). Active dot painted in the strip colour; click-and-drag diagonal patching for incremental routing.
- `AudioDeviceDialog` — custom card / tile layout in the ZynForge palette (OUTPUT / INPUT / SAMPLE RATE / BUFFER SIZE cards, each with its own accent colour stripe). Apply / Cancel — snapshot on open via `createStateXml`; Cancel restores via `initialise(..., savedXml, true)`.
- `BigClockPanel` — wide banner: state lamp + 56pt HH:MM:SS + disk-health (free GB, record time left, last write ms, missed-write count, marker count). Background tints red recording, green playing.
- `ExportDialog` + `TrackExporter` — pick format (WAV / AIFF / FLAC / MP3), sample rate, bit depth, MP3 bitrate. WAV/AIFF/FLAC via JUCE writers + `ResamplingAudioSource`; MP3 via `lame` ChildProcess.
- `Meterbridge` — floating window with one large `LedMeter` per logical strip; drag onto a second display.
- `StripColourPicker` — `CallOutBox` with 10 presets (8 personality colours + slate + graphite tokens) + Default reset + Custom (`juce::ColourSelector`).
- `MiniSpectrum`, `PhaseMeter`, `TimelineStrip` — FFT, correlation, marker timeline. Unchanged behavioural-wise from earlier.
- `BrandColors.h` + `BrandTokens.h` — colour palette (matches ZynForge Live's `DESIGN.md`), `radius::{sm/md/lg/xl}`, `space::{xs/sm/md/lg/xl/ctrlH/ioH/rowH/btnH}`, `type::*` (font roles), `verticalGradient()` helper. Every painted surface goes through the gradient helper so the look stays consistent.
- `ZynForgeLookAndFeel` — overrides `drawLinearSlider` (fader cap is a wide horizontal pill, gradient fill in the channel's colour, drop shadow, channel-coloured centre stripe) and `drawToggleButton` (pill body with hover/press gradients). Knobs / sliders use the channel's colour.

## Real-time discipline

The audio callback never allocates / locks / calls anything that can. Recording state transitions happen via `std::atomic` flags; writers are constructed/destroyed only on UI/background threads. The double-precision monitor accumulator pre-allocates in `audioDeviceAboutToStart`. The companion's audio ring is pre-sized at construction. The threaded mix writer drains on its own `TimeSliceThread` so disk I/O never reaches the audio thread.

## Sessions

Pro Tools-style named session folder. **File ▸ New Session…** builds the layout up front; subsequent record / save / export operations all live inside it.

```
<Local Storage>/<SessionName>/
├── <SessionName>.zfproj        ← session document (JSON: name, sr, format, ioPreset, …)
├── Audio Files/                ← Track_NN.wav per physical track
├── Bounced Files/              ← StereoMix.wav + every Export ▶ destination defaults here
├── Clip Groups/                ← reserved for grouped clips
├── Session File Backups/       ← reserved for `.zfproj` snapshots
├── Video Files/                ← reserved for video reference
├── WaveCache.wfm               ← waveform cache placeholder
├── markers.json
├── recording.session           ← auto-deleted on clean stop
└── session.report.json         ← stop time, totals, miss count, backup status, per-track stats
```

- `Track_NN.wav` — one file per **physical** track, configured bit depth, device sample rate, **mono** (stereo strips write L and R as separate mono files). When a session was created before the named-folder refactor `SessionPlayer::loadSession` falls back to the session root for legacy scans.
- `StereoMix.wav` — when `recordStereoMix` is on, the engine's stream-bus output captured live to a 2-channel WAV inside `Bounced Files/`.
- `<backup_root>/<SessionName>/Audio Files/Track_NN.<ext>` — backup copy in primary or alternate format, mirroring the same Audio Files/ layout.

The active session folder is persisted in `appProps` as `activeSessionDir`, so `RECORD` after a new-session pick always lands inside the named folder (no more auto-stamped `Session_YYYY-MM-DD_HH-MM-SS` unless the engineer hits RECORD without creating a session first).

## OSC remote

All five dialects support the same action set after the parity pass. 1-based channel indices.

| Action | Generic | DiGiCo | A&H SQ | SSL Live | Yamaha / RIVAGE |
|---|---|---|---|---|---|
| Record | `/zynforge/record b` | `/Console/Transport/record b` | `/sq/transport/record b` | `/sslnet/transport/record b` | `/Yamaha/Transport/record b` |
| Play | `/zynforge/play b` | `/Console/Transport/play b` | `/sq/transport/play b` | `/sslnet/transport/play b` | `/Yamaha/Transport/play b` |
| Stop | `/zynforge/stop` | `/Console/Transport/stop` | `/sq/transport/stop` | `/sslnet/transport/stop` | `/Yamaha/Transport/stop` |
| Marker | `/zynforge/marker [s]` | `/Console/Marker [s]` | `/sq/marker [s]` | `/sslnet/marker [s]` | `/Yamaha/Marker [s]` |
| Scene | `/zynforge/scene i` | `/Console/Snapshots/recall i` | `/sq/scene/recall i` | `/sslnet/snapshot/recall i` | `/Yamaha/Scene/recall i` |
| Channel ops | `/zynforge/channel/N/{name,mute,arm,colour}` | `/Console/Channels/N/...` | `/sq/chN/...` | `/sslnet/channel/N/...` | `/Yamaha/CH/N/...` (capitalised) |

## Stereo channel collapsing

A stereo pair lives as two adjacent physical tracks with `isStereo=true` on the L track. Every view collapses the pair into one logical entry:

- **Mixer**: one `ChannelStrip` controls both halves; mute/solo/arm/gain/pan/colour/name mirror to the partner; LedMeter shows two bars.
- **EDIT**: one row, two stacked waveform lanes, two-bar meter.
- **PATCH**: one column, labelled `INS N (L+R)`. Single click patches L → row, R → row+1 simultaneously.

Toggling stereo (via right-click menu, PATCH M/ST pill, or +CH dialog) calls `setTrackStereo` which persists the flag and forces a mixer rebuild on the next tick.

## Companion server

`Session → Start companion server on :9000`. Browser / iPad opens `http://<this-mac>:9000/` on the same LAN. State refreshes every 500 ms via polled JSON. The `<audio>` element streams the monitor bus as PCM WAV. POST endpoint accepts mute/solo/arm/transport commands.

## Cloud upload

`Session → Configure cloud upload command…` — store a template like `rclone copy {SESSION} myremote:bucket/` (or `aws s3 sync`, `rsync`, …). `Session → Upload session to cloud…` expands `{SESSION}` to the active session dir and `juce::ChildProcess`-launches the command.

## Recently shipped (this session)

- **LTC Phase 2 — bit-perfect biphase-mark decoder** with sync-word lookup, BCD parsing, fps inference, drop-frame flag.
- **MTC quarter-frame accumulator** — 8-nibble assembly to hr/mn/sc/fr.
- **VCA / group buses** — 8 buses with gainDb/muted/soloed/colour; per-strip vcaGroup atomic; audio thread sums vca.gainDb into effective per-strip gain; channelAudible honours VCA mute + solo-in-place; persistent assignments + names + colours; **`VcaPanel`** holds 8 compact `VcaStripView` mini-strips (fader / M / S / name / colour swatch / live dB readout) toggled via the new VCA header button. Right-click any strip → **Assign to VCA**.
- **Comp playlists (Take swap)** — `Playlist`/`Take` model per track; right-click EDIT row → **Take ▸** to swap, create-from-current, rename, delete. **TAKE n/m** chip painted on rows with >1 take.
- **Soft-takeover ramps** — per-strip + per-VCA gain/pan ramp targets; `tickRamps(numSamples)` steps per block from the audio callback so cue recalls don't click.
- **64-bit audio path** — `outputAccum` (juce::AudioBuffer<double>) for all per-strip / stream / monitor sums; single float downcast at the end of `audioDeviceIOCallbackWithContext`. Click + companion stream feed run AFTER the downcast (additive).
- **NoiseAnalyzer** — post-record FFT (4096-point) heuristic for 50/60 Hz hum, sub-80 Hz mic bumps, noise floor + crest factor. Writes `noise_report.json`. **`NoiseReportDialog`** sortable TableListBox replaces the AlertWindow text dump.
- **EDIT view zoom + horizontal scroll** — `[−]` `[%]` `[+]` in the tools bar; content widens past the viewport via list->setSize(contentW). Both scrollbars enabled.
- **Strip drag-reorder in EDIT** — mouse-down on the colour swatch arms a reorder; >8 px movement activates; each row-height delta calls `engine.swapTracks`. Refused during playback; force-reloads the session in mouseUp so SessionPlayer readers map to the renamed files.
- **MIDI clock master** — `MidiClockOut` HighResolutionTimer at 24 PPQN; `setSessionTempoBpm` drives clock tempo; start/stopPlayback fire midi-start/continue/stop. Session menu picker.
- **Session templates** — JSON layout per `.zftemplate` under `~/Library/Application Support/Zynforge Recording/Templates/`.
- **Session backup snapshots** — every cue add/update/delete writes a timestamped `.zfproj` copy to `Session File Backups/` (rolling 10).
- **Disk-space pre-flight** + **sample-rate mismatch warning** + **cue jump 1–9** + **output muting (separate from monitor)** + **lock-against-overwrite**.
- **UX polish** — first-run tutorial, Help menu (Keyboard Shortcuts / User Guide / Quick Start / About), VCA badge per strip, MIDI status pill, sortable noise table, empty-state mixer hint, Cmd+A select all strips, Esc clear selection, Cmd+Q routes to confirmAndQuit.
- **Per-cue tempo curves** — `Cue::tempoCurve` (vector of offset+bpm points). On recall, installed into the engine's tempo map; audio thread already pushes BPM to ClickEngine + MidiClockOut each block, so accels / rits within a song play back automatically.
- **Marker list dialog** — `MarkerListDialog` sortable table with rename / delete; double-click to jump.
- **Time signature** — numerator + denominator stored on engine, persisted in appProps.
- **Drag-reorder cues** — Move cue up / Move cue down in the SetlistBar right-click menu.
- **LCD countdown to next cue** — "Next: Song Name in 0:32" pill above the timeline.
- **Print setlist** — File ▸ Print setlist writes `setlist.html` into the session dir and opens it in the default browser (engineer prints / saves-as-PDF from there).

## Roadmap

- WebRTC remote audition (currently HTTP-streamed WAV)
- Audio-stream encryption for the companion server
- iOS / iPad native companion (currently web)
- Configurable monitor bus outputs (currently outs 0+1)

## Sibling project

ZynForge Live (plugin-insert host) lives elsewhere. They share visual identity but not code.
