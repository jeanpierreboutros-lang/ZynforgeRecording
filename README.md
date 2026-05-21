# Zynforge Recording

Live multitrack recorder + virtual soundcheck. macOS-first (Universal: Apple Silicon + Intel). Built on JUCE 8 / C++20.

## What it is

A focused recording surface for engineers running front-of-house or monitors who need a no-nonsense way to capture every input on the desk, play those tracks back through the same outputs to simulate a live signal during soundcheck, and never lose a take.

## Status

`v0.6.0` — timeline + marker workflow.

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
- [x] Pre-roll buffer — cycle PRE button (0 / 5 / 10 / 30 s); buffered history is dumped to each track on RECORD
- [x] Capture format selector — cycle FMT button (WAV 24 / WAV 32-float / FLAC 24)
- [x] Per-track monitor (MON button on each strip → sum to stereo monitor bus, outputs 0+1)
- [x] Per-strip mini-spectrum (log-frequency FFT, 24 Hz refresh)
- [x] Phase correlation meter (selectable pair, smoothed)
- [x] dBFS numeric readout + per-channel clip counter (click meter to clear)
- [x] Timeline strip with marker dots + click-to-seek
- [x] Marker rename / delete via right-click menu
- [x] Loop-between-markers (set Loop In / Out on two markers; player wraps automatically)
- [ ] System Lock (prevent accidental keypresses during record)
- [ ] LTC timecode input
- [ ] Console name sync (Dante / A&H / SSL)
- [ ] OSC / Mackie HUI remote
- [ ] Auto-recover incomplete sessions on next launch
- [ ] Redundant write to secondary drive

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

Recordings are written to `~/Music/Zynforge Sessions/Session_YYYY-MM-DD_HH-MM-SS/`, one `Track_NN.{wav,flac}` per channel at the device sample rate.

## Capture format

Cycle the **FMT** button in the header:

| Setting | Bits | Container | Notes |
|---|---|---|---|
| WAV 24 | 24-bit PCM | WAV + BWF metadata | Default. Compatible everywhere. |
| WAV 32F | 32-bit IEEE float | WAV + BWF metadata | Effectively clip-proof at the digital file level. Larger files. |
| FLAC 24 | 24-bit lossless | FLAC | ~50% disk footprint vs WAV 24. |

Format is locked while recording.

## Pre-roll

Cycle the **PRE** button: `0 / 5 / 10 / 30 s`. When pre-roll is non-zero, every channel keeps a rolling history of the last N seconds. When you press RECORD, that history is dumped into the file before live capture begins — so the count-in / first hit / squawk before the first chorus is never lost.

## Monitor

Each channel strip has a **MON** toggle. Engaged channels are summed (post-input) into the device's stereo monitor bus (outputs 0 and 1) so you can soloed-check signals into headphones without affecting recording or playback.

## Diagnostics

- **Mini-spectrum** on every strip — log-frequency FFT (1024-point) updated at 24 Hz. Quickly spot feedback or a microphone with the wrong polar pattern.
- **Phase correlation meter** between any selected input pair (default 1/2). −1 = inverted, 0 = decorrelated, +1 = in phase. Use `<` and `>` to slide the pair across the input bank.
- **dBFS numeric readout** under each strip's spectrum — peak gain in dB, refreshed at 10 Hz. Reads `-inf` when silent.
- **Clip log** — each channel keeps a count of clip events; the strip shows `CLIP × N`. Click the meter to reset.

## Virtual soundcheck

1. Click **LOAD SESSION**, pick any `Session_*` folder from `~/Music/Zynforge Sessions/`.
2. Press **PLAY** — each `Track_NN.wav` plays out the matching output channel of the current device (Track 1 → out 1, Track 2 → out 2, …).
3. The console can now treat the recorder outputs as its source bank instead of the live mics, so the band can leave and you keep mixing.

Playback uses non-blocking `juce::BufferingAudioReader` per track; disk I/O happens on a background thread, the audio thread only copies pre-fetched samples.

## Markers + timeline

Press **M** at any time during record or playback to drop a marker at the current position. Markers are persisted to `markers.json` inside the session folder and re-loaded automatically when the session is opened.

The timeline strip beneath the Big Clock shows:

- The session length end-to-end
- A green playhead at the current position
- A red triangle dot for each marker, labelled with its name
- A yellow band over the loop region (when one is set)

Interactions:

- **Click** anywhere on the strip to seek
- **Click a marker dot** to seek to it
- **Right-click a marker** to bring up:
    - **Rename…** — edit the marker label
    - **Delete** — remove it
    - **Set as Loop In / Out** — define a loop region between two markers
    - **Clear Loop** — disable looping

Once Loop In + Out are set, the player wraps from Out back to In every pass.

## Crash-safe recording

WAV files are written with BWF (`bext`) metadata — originator, originator reference, origination date/time — so a hard crash still leaves an identifiable file. The writer thread flushes the WAV header every ~5 seconds of audio so a power loss mid-record leaves a playable file with current-size headers (only the last few seconds are at risk).
