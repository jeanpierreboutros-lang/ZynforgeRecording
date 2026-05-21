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

- `AudioEngine` — owns `juce::AudioDeviceManager`, registers itself as the `AudioIODeviceCallback`. On every callback: clears outputs, runs `MultitrackRecorder::processBlock` (input path), runs `SessionPlayer::processBlock` (output path), computes phase correlation between the selected input pair (normalised cross-correlation at zero lag, smoothed 0.85/0.15), then sums monitored input channels into outputs 0+1 (the stereo monitor bus). Owns the `MarkersManager` and routes M-key drops to the recorder's `samplesSinceStart` or the player's playback position.
- `MultitrackRecorder` — real-time-safe input path. Per-channel `juce::AbstractFifo` ring buffer (~4 s at sample rate), drained by a `TimeSliceThread` that writes the configured format per track. `CaptureFormat` enum: `Wav24` (BWF), `Wav32Float` (BWF, IEEE float — bits=32 makes JUCE's `WavAudioFormatWriter` set `usesFloatingPointData`), `Flac24` (`juce::FlacAudioFormat`, quality 5). Writer flushes header every ~5 s of audio so a crash leaves a playable file. Per-channel `PreRollBuffer` (always-on rolling ring sized to `preRollSeconds + 2s` safety) is fed every audio block; `startRecording` dumps the history into each writer BEFORE setting `recording=true` so audio that preceded the record button is captured. Meters (peak + RMS + clip) live on `TrackState` and are read by the UI thread via `std::atomic`. Atomic counters: `samplesSinceStart`, `missedSamples` (FIFO overflow), `lastWriteMs` (drain latency).
- `SessionPlayer` — real-time-safe output path for virtual soundcheck. Scans a session dir for `Track_*.wav`, wraps each in a non-blocking `juce::BufferingAudioReader` (2-second pre-fetch on its own `TimeSliceThread`, read timeout = 0). On `processBlock`, copies samples from each reader to the matching output channel index. Position + length + playing flag are `std::atomic`. Session swap path: store `playing=false`, sleep 40 ms for in-flight callbacks to drain, then mutate the reader vector.
- `MarkersManager` — per-session marker list. Persisted as `markers.json` in the session dir. Mutated only from the message thread. `setContext` re-binds + auto-loads on session open. `drop(sampleOffset)` saves immediately.
- `TrackState` — atomic meter values, armed flag, monitor flag, clip counter, last-clip sample offset, FFT FIFO/snapshot (1024-point), name. One per input channel.
- `MainComponent` — two-row header (title / status / DEVICE / RECORD on top; LOAD / PLAY / STOP / transport / session info on bottom) + `BigClockPanel` banner + horizontal grid of `ChannelStrip`s. 10 Hz timer drives transport labels + Big Clock + disk-health calc (free GB, remaining record time). Acts as `juce::KeyListener` — M drops a marker.
- `BigClockPanel` — wide banner: state lamp (REC/PLAY/IDLE), huge 56pt HH:MM:SS timer, disk-health column on the right (FREE / RECORD TIME LEFT / LAST WRITE / MISSED / MARKERS). Background tints red when recording, green when playing.
- `ChannelStrip` — name label, ARM toggle, MON toggle, `MiniSpectrum`, dBFS numeric readout, clip-count badge, `LedMeter`. Personality colour band on top. Owns a private `StripTimer` (10 Hz) that updates the dB readout and clip badge text from atomics.
- `LedMeter` — 20-segment vertical meter, peak + RMS, click anywhere to clear clip + clip count.
- `MiniSpectrum` — log-frequency magnitude bars. Polls `TrackState::fftBlockReady` at 24 Hz; on ready, copies the snapshot, applies a Hann window, runs `juce::dsp::FFT` (size 1024), reduces to 28 log-spaced visible bins, fades-down via exponential decay.
- `PhaseMeter` — horizontal correlation bar with channel-pair cycler. Reads smoothed value from `AudioEngine::getPhaseCorrelation()` at 20 Hz. Indicator colour: red below −0.2, amber up to +0.4, green above.
- `ZynForgeLookAndFeel` + `BrandColors` — shared visual identity.

## Real-time discipline

The audio callback **never** allocates, locks, or calls anything that can. Recording state transitions happen via `std::atomic` flags; writers are constructed/destroyed only on UI/background threads, with the `recording` flag opened/closed via release/acquire to fence the ring-buffer push.

## Sessions

`~/Music/Zynforge Sessions/Session_YYYY-MM-DD_HH-MM-SS/Track_NN.wav` — one file per input channel, 24-bit, device sample rate, mono.

## Not yet implemented (roadmap)

- System Lock to prevent accidental UI interaction during record
- OSC / Mackie control for transport
- LTC timecode chase
- Console name sync (Dante / A&H / SSL)
- Timeline strip with clickable markers + seek
- Configurable monitor bus output assignment (currently hardcoded to outs 0+1)
- Configurable phase-correlation channel pair (currently slides L+R together)

## Sibling project

The ZynForge Live app (plugin-insert host) lives elsewhere on the user's machine. They share visual identity but not code.
