# Zynforge Recording

Live multitrack recorder + virtual soundcheck. macOS-first (Universal: Apple Silicon + Intel). Built on JUCE 8 / C++20.

## What it is

A focused recording surface for engineers running front-of-house or monitors who need a no-nonsense way to capture every input on the desk, play those tracks back through the same outputs to simulate a live signal during soundcheck, and never lose a take.

## Status

`v0.2.0` — recording + virtual soundcheck playback.

- [x] Project structure (CMake, JUCE 8 via `FetchContent`)
- [x] ZynForge visual identity (near-black panels, per-strip personality colours, LED-segment meters)
- [x] Audio engine (multichannel input capture, lock-free per-channel FIFO, background WAV writer)
- [x] Channel strips with live input metering
- [x] Audio device selector
- [x] Virtual soundcheck playback (LOAD SESSION + PLAY / STOP, each track → matching output)
- [ ] Crash-safe BWF writing (periodic header flush + timestamp/scene chunks)
- [ ] Big record-state indicator + disk-health overlay
- [ ] Markers (drop with `M` during record/playback)
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
│   ├── MultitrackRecorder.*  — lock-free FIFO + background WAV writer
│   ├── SessionPlayer.*       — per-track BufferingAudioReader playback for VSC
│   └── TrackState.h          — per-track atomic meter + arm state
├── UI/
│   ├── MainComponent.*       — header (record + transport) + channel-strip grid
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
