# Zynforge Recording — Claude operating notes

## Project

JUCE 8 / C++20 / CMake, macOS-first (Universal). Live multitrack recorder + virtual soundcheck. Sibling project to the ZynForge Live plugin-insert host.

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

JUCE 8.0.4 is pulled via `FetchContent` on first configure. The artefact lives at `build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app`.

## Architecture map

- `AudioEngine` — owns `juce::AudioDeviceManager`, registers itself as the `AudioIODeviceCallback`. On every callback: clears outputs, runs `MultitrackRecorder::processBlock` (input path), runs `SessionPlayer::processBlock` (output path), computes phase correlation between the selected input pair (normalised cross-correlation at zero lag, smoothed 0.85/0.15), applies the mute/solo gate (any-solo-engaged → only soloed channels audible; else muted channels silenced) to both the VSC playback outputs and the monitor bus, applies per-channel playback **gain** to the VSC outputs, then sums monitored + audible input channels into outputs 0+1 with **constant-power pan** + per-channel gain. Owns the `MarkersManager` and routes M-key drops to the recorder's `samplesSinceStart` or the player's playback position.
- `MultitrackRecorder` — real-time-safe input path. Per-channel `juce::AbstractFifo` ring buffer (~4 s at sample rate), drained by a `TimeSliceThread` that writes the configured format per track. `CaptureFormat` enum: `Wav24` (BWF), `Wav32Float` (BWF, IEEE float — bits=32 makes JUCE's `WavAudioFormatWriter` set `usesFloatingPointData`), `Flac24` (`juce::FlacAudioFormat`, quality 5). Writer flushes header every ~5 s of audio so a crash leaves a playable file. Per-channel `PreRollBuffer` (always-on rolling ring sized to `preRollSeconds + 2s` safety) is fed every audio block; `startRecording` dumps the history into each writer BEFORE setting `recording=true` so audio that preceded the record button is captured. **Optional `backupWriter` per channel mirrors every drain to a secondary directory (`setBackupDirectory`); a failed backup write drops just that channel's backup writer and flags `backupFailed`. The primary write keeps going.** **`startRecording` writes a `recording.session` JSON marker (start time, sample rate, track count) into the session dir; `stopRecording` deletes it. `AudioEngine::findIncompleteSessions` scans the Sessions root for any directory that still has this marker.** Meters (peak + RMS + clip) live on `TrackState` and are read by the UI thread via `std::atomic`. Atomic counters: `samplesSinceStart`, `missedSamples` (FIFO overflow), `lastWriteMs` (drain latency), `backupActive`, `backupFailed`.
- `SessionPlayer` — real-time-safe output path for virtual soundcheck. Scans a session dir for `Track_*.wav`, wraps each in a non-blocking `juce::BufferingAudioReader` (2-second pre-fetch on its own `TimeSliceThread`, read timeout = 0). On `processBlock`, copies samples from each reader to the matching output channel index. Position + length + playing flag are `std::atomic`. Loop region (`loopStart` / `loopEnd` atomics): when both ≥ 0 and end > start, `processBlock` wraps position back to start on hitting end, and caps the per-block playback length so the loop boundary lands cleanly on a buffer edge. Session swap path: store `playing=false`, sleep 40 ms for in-flight callbacks to drain, then mutate the reader vector.
- `MarkersManager` — per-session marker list. Persisted as `markers.json` in the session dir. Mutated only from the message thread. `setContext` re-binds + auto-loads on session open. `drop(sampleOffset)` saves immediately. CRUD: `getMarker(i)`, `removeMarker(i)`, `renameMarker(i, name)`, `setMarkerSample(i, sample)`. `getAll()` exposes the underlying vector for UI iteration.
- `StripColours` — per-channel colour overrides, persisted via `juce::PropertiesFile` (`~/Library/Application Support/Zynforge Recording/`). Keys `strip_color_<channelIndex>` → ARGB int. Owned by `AudioEngine`; `setTrackColour (channelIndex, juce::Colour)` updates both the persistent store and the live `TrackState::colourARGB` atomic. Pass an alpha-0 colour to revert to the default personality.
- `StripNames` — per-channel display-name overrides, persisted via the same `juce::PropertiesFile`. Keys `strip_name_<channelIndex>` → string. `AudioEngine::setTrackName (channelIndex, juce::String)` writes both `TrackState::name` and the persistent store; an empty string reverts to the default "In N" label. Names are reapplied on `audioDeviceAboutToStart`.
- `StripGains` — per-channel playback gain (dB) + pan (-1..+1), persisted via the same `juce::PropertiesFile`. Keys `strip_gain_<channelIndex>` (double) and `strip_pan_<channelIndex>` (double). `AudioEngine::setTrackGainDb` / `setTrackPan` write the live atomic AND the persistent file; both are reapplied on `audioDeviceAboutToStart`.
- `StripRouting` — per-channel input + output device-channel routing, persisted via the same `juce::PropertiesFile`. Keys `strip_in_<channelIndex>` / `strip_out_<channelIndex>` (int). -1 means unrouted. `AudioEngine::setTrackInputRouting` / `setTrackOutputRouting` write the live atomic AND the file. Defaults to identity (strip N ↔ device N). On every audio callback the engine builds a `routedInputs[]` pointer array (strip i → device input `inputRouting[i]`) and passes it to the recorder; player output is rendered into `playerScratch[i]` then summed into `outputs[outputRouting[i]]` honouring mute/solo and per-channel gain.
- `TrackState` — atomic meter values, armed flag, monitor flag, mute flag, solo flag, gainDb (−60..+12, default 0), pan (−1..+1, default 0), clip counter, last-clip sample offset, FFT FIFO/snapshot (1024-point), colour override (`colourARGB`, 0 = default), name. One per input channel. Mute / solo / gain / pan are monitoring-only concerns — none affects the recording path (armed channels still hit disk pre-fader).
- `MainComponent` — two-row header (title / status / LOCK / FMT / PRE / DEVICE / RECORD on top; FILE (popup menu) / PLAY / STOP / transport / session info / BACKUP on bottom) + `BigClockPanel` banner + `TimelineStrip` + horizontal grid of `ChannelStrip`s. 10 Hz timer drives transport labels + Big Clock + disk-health calc (free GB, remaining record time). Acts as `juce::KeyListener` — M drops a marker. The FILE popup hosts: Open Session…, Save Session State (writes `session_settings.json` with capture format / pre-roll / phase pair / loop region / per-track names+colours), Save Session As… (`juce::File::copyDirectoryTo`), Export All Tracks → `ExportDialog` → destination chooser → `TrackExporter` loop, Export Individual Track ▶ (same pipeline for one channel). **System Lock** (`sessionLocked` flag, LOCK button) disables every other control including RECORD until the engineer clicks UNLOCK; status line reflects the locked state. **BACKUP** button picks a folder; forwarded to `AudioEngine::setBackupDirectory`. On startup, 250 ms after the window appears, `offerSessionRecovery` calls `AudioEngine::findIncompleteSessions` and pops a menu of any sessions that didn't stop cleanly; picking one clears the marker and loads the session for playback.
- `ExportDialog` — modal `juce::DialogWindow` content with combo boxes for format / sample rate / bit depth / MP3 bitrate. Disables bit-depth combo when MP3 is chosen and clamps to 16/24 for FLAC. Returns `std::optional<ExportOptions>` via callback.
- `TrackExporter` — owns its own `juce::AudioFormatManager` (basic formats + FLAC). `exportTrack` builds a `juce::AudioFormatReaderSource` + `juce::ResamplingAudioSource` chain, writes to a target `WavAudioFormat` / `AiffAudioFormat` / `FlacAudioFormat` writer at the chosen bit depth. For MP3 it renders to a temp 24-bit WAV at the target sample rate then `juce::ChildProcess` launches `lame -b <bitrate> --quiet <src> <dest>`. `findLameBinary()` checks `/opt/homebrew/bin/lame`, `/usr/local/bin/lame`, `/usr/bin/lame`, then `which lame`.
- `BigClockPanel` — wide banner: state lamp (REC/PLAY/IDLE), huge 56pt HH:MM:SS timer, disk-health column on the right (FREE / RECORD TIME LEFT / LAST WRITE / MISSED / MARKERS). Background tints red when recording, green when playing.
- `ChannelStrip` — colour swatch + name label (double-click to rename inline via `Label::setEditable(false, true)`), 2×2 button grid (ARM red + MON green / MUTE amber + SOLO yellow), `MiniSpectrum`, dBFS numeric readout, clip-count badge, horizontal **pan** slider, vertical **fader** (`-60..+12 dB`), `LedMeter` to the right of the fader. Right-click on the strip body opens a context popup: Rename…, Change colour…, Reset colour, Reset name. Full-strip personality wash (resolved from `TrackState::colourARGB` if non-zero, else the default rotation). Clicking the swatch opens a `StripColourPicker` via `juce::CallOutBox`. Owns a private `StripTimer` (10 Hz) that updates the dB readout and clip badge text from atomics.
- `StripColourPicker` — popup component shown via `CallOutBox`. 10 fixed preset swatches (8 personalities + slate + graphite), a `Default` reset button (sends a transparent colour to revert), and a `Custom…` button that opens a `juce::ColourSelector` in a second `CallOutBox`. Reports the chosen colour through the supplied `Callback`.
- `LedMeter` — 20-segment vertical meter, peak + RMS, click anywhere to clear clip + clip count.
- `MiniSpectrum` — log-frequency magnitude bars. Polls `TrackState::fftBlockReady` at 24 Hz; on ready, copies the snapshot, applies a Hann window, runs `juce::dsp::FFT` (size 1024), reduces to 28 log-spaced visible bins, fades-down via exponential decay.
- `PhaseMeter` — horizontal correlation bar with channel-pair cycler. Reads smoothed value from `AudioEngine::getPhaseCorrelation()` at 20 Hz. Indicator colour: red below −0.2, amber up to +0.4, green above.
- `TimelineStrip` — horizontal session timeline. Reads `SessionPlayer` position/total at 20 Hz; paints background trough, progress fill, loop band (yellow), playhead, and a red triangle + label per marker. Left-click empty area seeks; left-click a marker seeks to it; right-click a marker opens a popup (Rename / Delete / Set Loop In / Set Loop Out / Clear Loop). Rename uses a modal `juce::AlertWindow` text editor.
- `ZynForgeLookAndFeel` + `BrandColors` — shared visual identity. `drawLinearSlider` is overridden to draw the ZynForge Live fader: thin track, green fill from thumb to bottom, long dark thumb (~38 px tall) with 5 horizontal grip lines + a white centre stripe. Horizontal style is repurposed for the pan slider.

## Real-time discipline

The audio callback **never** allocates, locks, or calls anything that can. Recording state transitions happen via `std::atomic` flags; writers are constructed/destroyed only on UI/background threads, with the `recording` flag opened/closed via release/acquire to fence the ring-buffer push.

## Sessions

`~/Music/Zynforge Sessions/Session_YYYY-MM-DD_HH-MM-SS/Track_NN.wav` — one file per input channel, 24-bit, device sample rate, mono.

## Not yet implemented (roadmap)

- OSC / Mackie control for transport
- LTC timecode chase
- Console name sync (Dante / A&H / SSL)
- Configurable monitor bus output assignment (currently hardcoded to outs 0+1)
- Configurable phase-correlation channel pair (currently slides L+R together)
- Drag-to-move markers on the timeline
- Visual indicator in `BigClockPanel` for backup-failed state

## Sibling project

The ZynForge Live app (plugin-insert host) lives elsewhere on the user's machine. They share visual identity but not code.
