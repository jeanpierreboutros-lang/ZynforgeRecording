# Zynforge Recording

Live multitrack recorder + virtual soundcheck. macOS-first (Universal: Apple Silicon + Intel). Built on JUCE 8 / C++20.

## What it is

A focused recording surface for engineers running front-of-house or monitors who need a no-nonsense way to capture every input on the desk, play those tracks back through the same outputs to simulate a live signal during soundcheck, and never lose a take.

## Status

`v0.3.0` — reliability bundle.

- [x] Project structure (CMake, JUCE 8 via `FetchContent`)
- [x] ZynForge visual identity (near-black panels, per-strip personality colours, LED-segment meters)
- [x] Audio engine (multichannel input capture, lock-free per-channel FIFO, background WAV writer)
- [x] Channel strips with live input metering
- [x] Audio device selector
- [x] Virtual soundcheck playback (LOAD SESSION + PLAY / STOP, each track → matching output)
- [x] BWF metadata (bext chunk) + periodic header flush every ~5 s (crash-safe)
- [x] Big record indicator: huge HH:MM:SS timer, REC/PLAY/IDLE lamp, visible across the room
- [x] Disk-health overlay: free GB, record time remaining, last write ms, missed-write counter, marker count
- [x] Markers — press **M** during record/playback to drop, auto-persisted to `markers.json` per session
- [ ] Pre-roll buffer (capture audio before record is pressed)
- [ ] System Lock (prevent accidental keypresses during record)
- [ ] FLAC / 32-bit float capture options
- [ ] Per-track monitor / solo button
- [ ] LTC timecode input
- [ ] Console name sync (Dante / A&H / SSL)

## Build

```bash
cmake -B build -G Xcode
cmake --build build --config Release
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
```

First configure fetches JUCE 8.0.4 via `FetchContent`. Set `CMAKE_OSX_DEPLOYMENT_TARGET` lower if you need broader macOS support.

## Visual identity

Part of the ZynForge family. Shared aesthetic with the ZynForge Live host:

- Near-black canvas (`#07080a` / `#0f1014`)
- Each channel strip carries a personality colour (azure, coral, violet, mint, amber, pink, teal, lemon — rotates by strip index)
- LED-segment meters (peak + RMS, exponential decay, clip hold)
- Bold, typographic transport controls

## Layout

```
Source/
├── Main.cpp                  — JUCE app entry + window
├── Audio/
│   ├── AudioEngine.*         — AudioDeviceManager + AudioIODeviceCallback
│   ├── MultitrackRecorder.*  — lock-free FIFO + background WAV writer (BWF + periodic flush + counters)
│   ├── SessionPlayer.*       — per-track BufferingAudioReader playback for VSC
│   ├── Markers.*             — per-session marker list, persisted as markers.json
│   └── TrackState.h          — per-track atomic meter + arm state
├── UI/
│   ├── MainComponent.*       — header + Big Clock banner + channel-strip grid + M-key handler
│   ├── BigClockPanel.*       — large state lamp, huge timer, disk-health strip
│   ├── ChannelStrip.*        — name, ARM, meter
│   └── LedMeter.*            — segment meter
└── Theme/
    ├── BrandColors.h         — palette + per-strip personality colours
    └── ZynForgeLookAndFeel.* — JUCE LookAndFeel override
```

## Sessions

Recordings are written to `~/Music/Zynforge Sessions/Session_YYYY-MM-DD_HH-MM-SS/`, one `Track_NN.wav` per channel (24-bit, device sample rate).

## Virtual soundcheck

1. Click **LOAD SESSION**, pick any `Session_*` folder from `~/Music/Zynforge Sessions/`.
2. Press **PLAY** — each `Track_NN.wav` plays out the matching output channel of the current device (Track 1 → out 1, Track 2 → out 2, …).
3. The console can now treat the recorder outputs as its source bank instead of the live mics, so the band can leave and you keep mixing.

Playback uses non-blocking `juce::BufferingAudioReader` per track; disk I/O happens on a background thread, the audio thread only copies pre-fetched samples.

## Markers

Press **M** at any time during record or playback to drop a marker at the current position. Markers are persisted to `markers.json` inside the session folder and re-loaded automatically when the session is opened. The Big Clock shows the running marker count.

## Crash-safe recording

WAV files are written with BWF (`bext`) metadata — originator, originator reference, origination date/time — so a hard crash still leaves an identifiable file. The writer thread flushes the WAV header every ~5 seconds of audio so a power loss mid-record leaves a playable file with current-size headers (only the last few seconds are at risk).
