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

- `AudioEngine` — owns `juce::AudioDeviceManager`, registers itself as the `AudioIODeviceCallback`. On every callback: clears outputs, runs `MultitrackRecorder::processBlock` (input path), then runs `SessionPlayer::processBlock` (output path).
- `MultitrackRecorder` — real-time-safe input path. Per-channel `juce::AbstractFifo` ring buffer (~4 s at sample rate), drained by a `TimeSliceThread` that writes 24-bit WAV per track. Meters (peak + RMS + clip) live on `TrackState` and are read by the UI thread via `std::atomic`.
- `SessionPlayer` — real-time-safe output path for virtual soundcheck. Scans a session dir for `Track_*.wav`, wraps each in a non-blocking `juce::BufferingAudioReader` (2-second pre-fetch on its own `TimeSliceThread`, read timeout = 0). On `processBlock`, copies samples from each reader to the matching output channel index. Position + length + playing flag are `std::atomic`. Session swap path: store `playing=false`, sleep 40 ms for in-flight callbacks to drain, then mutate the reader vector.
- `TrackState` — atomic meter values, armed flag, name. One per input channel.
- `MainComponent` — two-row header (title / status / DEVICE / RECORD on top; LOAD / PLAY / STOP / transport / session info on bottom) + horizontal grid of `ChannelStrip`s. 10 Hz timer polls track count + transport position.
- `ChannelStrip` — name label, ARM toggle, `LedMeter`. Personality colour band on top.
- `LedMeter` — 20-segment vertical meter, peak + RMS, click anywhere to clear clip.
- `ZynForgeLookAndFeel` + `BrandColors` — shared visual identity.

## Real-time discipline

The audio callback **never** allocates, locks, or calls anything that can. Recording state transitions happen via `std::atomic` flags; writers are constructed/destroyed only on UI/background threads, with the `recording` flag opened/closed via release/acquire to fence the ring-buffer push.

## Sessions

`~/Music/Zynforge Sessions/Session_YYYY-MM-DD_HH-MM-SS/Track_NN.wav` — one file per input channel, 24-bit, device sample rate, mono.

## Not yet implemented (roadmap)

- Crash-safe BWF writing (periodic header flush, bext/iXML chunks)
- Big record-state indicator + disk-health overlay (remaining time, write rate, missed-write banner)
- Markers (drop with `M`, stored as `markers.json` per session)
- Pre-roll buffer (rolling N-second buffer, dumped into file on RECORD)
- System Lock to prevent accidental UI interaction during record
- FLAC / 32-bit float capture options
- Per-track input monitor / solo
- Spectrum + phase-correlation per strip
- OSC / Mackie control for transport
- LTC timecode chase
- Console name sync (Dante / A&H / SSL)

## Sibling project

The ZynForge Live app (plugin-insert host) lives elsewhere on the user's machine. They share visual identity but not code.
