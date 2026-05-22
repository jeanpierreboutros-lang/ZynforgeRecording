# Zynforge Recording

Live multitrack recorder + virtual soundcheck. macOS-first (Universal: Apple Silicon + Intel). Built on JUCE 8 / C++20.

## What it is

A focused recording surface for engineers running front-of-house or monitors who need a no-nonsense way to capture every input on the desk, play those tracks back through the same outputs to simulate a live signal during soundcheck, and never lose a take.

## Status

`v0.7.0` — per-track colour palette.

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
- [x] Per-track MUTE and SOLO — applied to both VSC playback outputs and the monitor bus. Any solo engaged → only soloed channels are audible.
- [x] Per-channel fader (−60 .. +12 dB) and pan (L .. C .. R) — applied to VSC playback and monitor sum (constant-power pan). Recording stays pre-fader. Both persist across launches.
- [x] **System Lock** — LOCK button on the header disables every other control (recording included) so a stray click can't kill a take. Click again to UNLOCK.
- [x] **Auto-recover** — a `recording.session` marker is written when RECORD starts and deleted on clean stop. On next launch the app scans the Sessions root and offers to load any session that didn't stop cleanly. WAV/FLAC headers were already crash-safe (5-second periodic flush) so the audio itself is intact.
- [x] **Redundant write** — pick a backup folder via the BACKUP button; every track is mirrored to `<backup>/<session>/Track_NN.<ext>` as you record. If the backup drive fails mid-take, the primary write keeps going untouched.
- [x] **Per-strip input + output routing** — two dropdowns at the top of each strip pick which device input the strip captures from and which device output its VSC playback lands on. Defaults to identity (strip N ↔ device N). Persists per channel index.
- [x] **Live-style fader** — long thumb with horizontal grip lines + bright centre stripe, green track fill below the thumb, matches the ZynForge Live look.
- [x] **Pill-shaped toggle buttons** — ARM / MON / MUTE / SOLO get the Live look: dark gradient body, no tick, accent-coloured text that brightens when engaged.
- [x] **Fixed-width channel strips** in a horizontally scrollable viewport (~150 px per strip).
- [x] **PATCH page** — INPUT PATCH / OUTPUT PATCH matrix tabs. Rows = hardware channels, columns = strips. Click a circle to route, click again to clear. Wired directly to the engine's input/output routing.
- [x] **128-channel capacity** — opens up to 128 inputs / 64 outputs on the audio device.
- [x] **STREAM bus** — per-strip "Send to STREAM bus" toggle in the right-click menu. Engine sums all stream-enabled strips into the configured stereo output pair (constant-power pan, post-fader, post-mute/solo).
- [x] **Meterbridge window** — `METERS` button opens a floating window with one large LedMeter + name per strip; drag it onto a second display.
- [x] **OSC remote with console dialects** — `OSC` button starts a UDP listener on port 8000 in your choice of dialect: Generic / **DiGiCo** / **Allen & Heath (SQ / Avantis)** / **SSL Live** / **Yamaha (DM7 / RIVAGE PM)**. Handles transport (record/play/stop), snapshot recall → scene marker drop, channel name / mute sync from the desk.
- [x] Per-strip mini-spectrum (log-frequency FFT, 24 Hz refresh)
- [x] Phase correlation meter (selectable pair, smoothed)
- [x] dBFS numeric readout + per-channel clip counter (click meter to clear)
- [x] Timeline strip with marker dots + click-to-seek
- [x] Marker rename / delete via right-click menu
- [x] Loop-between-markers (set Loop In / Out on two markers; player wraps automatically)
- [x] Per-track colour palette — click the swatch on a strip → 10 presets + Custom (full colour picker), persists across launches
- [x] Per-track rename — double-click the name label OR right-click strip → Rename… (persists, click "Reset name" in the menu to revert)
- [x] **FILE** menu — Open Session…, Save Session State, Save Session As…, and Export ▶ (Export All Tracks…, Export Individual Track ▶ per channel)
- [x] Export dialog — pick format (WAV / AIFF / FLAC / MP3), sample rate (44.1 / 48 / 96 / 192 kHz), bit depth (16 / 24 / 32-float), MP3 bitrate (128 / 256 / 320 kbps). Resampling via `juce::ResamplingAudioSource`; MP3 via `lame`.
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

Part of the ZynForge family — palette directly matches ZynForge Live:

- Near-black canvas (`#0a0a0c` / `#12131 6`)
- Every channel strip is washed end-to-end with a muted personality colour (dusty blue, moss, olive, violet, wine, teal, amber, mustard — rotates by strip index, mirrors Live's INS 1–8)
- LED-segment meters (20 segments, green/amber/red, exponential decay, click to clear clip)
- Bright signal-green session clock (`#5dd87a`) reused from Live
- Status accents reserved for state, not decoration: red = record, green = play, amber = virtual soundcheck

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

## Fader + pan

Every channel has its own playback gain and pan:

- **Fader** — vertical slider next to the LED meter, range −60 to +12 dB, default 0 dB. Double-click to reset to 0 dB.
- **Pan** — horizontal slider above the fader, −1 (full L) to +1 (full R), constant-power law (centre = −3 dBFS to each bus). Double-click to recentre.

Both apply to:

- **VSC playback** — Track N's playback to output N is multiplied by the fader gain. Pan is **not** applied to VSC playback (the console expects 1-to-1 routing).
- **Monitor bus** (outputs 0+1) — fader gain × constant-power pan when summing into the stereo monitor.

Recording is always **pre-fader** — armed channels are written to disk at the device's input level regardless of fader / pan settings. Both persist per channel index across launches via `juce::PropertiesFile`.

## Channel buttons

Each strip carries a 2×2 grid of toggles:

|  | Left | Right |
|---|---|---|
| **Row 1** | **ARM** (red) — channel is captured when RECORD is engaged | **MON** (green) — input is summed into the stereo monitor bus (outs 0+1) |
| **Row 2** | **MUTE** (amber) — silences this channel in the monitor + VSC playback. Does NOT stop recording — the take is still written to disk. | **SOLO** (yellow) — as soon as any track is soloed, only soloed tracks are audible. Solo overrides mute. |

The mute / solo logic runs inside the audio callback, so changes are instant and click-free. Recording always captures armed channels regardless of mute or solo state — both are monitoring concerns, not recording ones.

## Show-day reliability

### System Lock

The **LOCK** button (header row 1, between status and FMT) disables every other control — RECORD included — so a stray bump or accidental click cannot stop a take. The button itself stays clickable; click it again to UNLOCK.

While locked, the status line shows `LOCKED — click UNLOCK to resume control`. Strip mute/solo/fader/pan/buttons are all blocked; markers (the M key) still work.

### Redundant write

Click **BACKUP** (header row 2, right side) and pick a folder — ideally on a separate physical drive. From that point on, every track is written to **both** locations in parallel:

- Primary: `~/Music/Zynforge Sessions/Session_*/Track_NN.<ext>`
- Backup:  `<your folder>/Session_*/Track_NN.<ext>`

The backup writer is fed from the same FIFO drain as the primary, so the two files stay byte-for-byte in sync until a write fails. If the backup drive fills up, disconnects, or otherwise errors, that channel's backup writer is dropped; the primary keeps going and a `backupFailed` flag is set so the engineer can be informed. Backup target can only be changed when not actively recording.

### Auto-recover

When recording starts, the app writes a `recording.session` marker (JSON: start timestamp, sample rate, track count) into the session folder. On clean stop the marker is removed.

If the app or machine dies mid-take, that marker remains. On next launch, ~250 ms after the window appears, the app scans `~/Music/Zynforge Sessions/` and pops up a menu of any session that didn't stop cleanly. Pick one → marker is cleared, session is loaded for playback so you can inspect or export it. WAV / FLAC headers are flushed every 5 s of audio (see *Crash-safe recording* below), so files remain playable up to that boundary regardless of how the previous run ended.

## OSC remote (console integration)

Click **OSC** in the header → pick a dialect → the app listens on UDP port 8000. All major mixing consoles can fire OSC over the same network — point them at this Mac's IP:8000.

| Dialect | Snapshot/scene → marker | Transport sync | Channel name sync | Channel mute sync |
|---|:-:|:-:|:-:|:-:|
| Generic `/zynforge/*` | `/zynforge/scene <int>` | `/zynforge/record <0\|1>` `/zynforge/play <0\|1>` `/zynforge/stop` | `/zynforge/channel/<N>/name <string>` | `/zynforge/channel/<N>/mute <0\|1>` |
| **DiGiCo** | `/Console/Snapshots/recall <int>` | `/Console/Transport/record` `/Console/Transport/play` | `/Console/Channels/<N>/name` | `/Console/Channels/<N>/mute` |
| **Allen & Heath** (SQ / Avantis) | `/sq/scene/recall <int>` | `/sq/transport/record` `/sq/transport/play` | `/sq/ch<N>/name` | `/sq/ch<N>/mute` |
| **SSL Live** | `/sslnet/snapshot/recall <int>` | `/sslnet/transport/record` `/sslnet/transport/play` | `/sslnet/channel/<N>/name` | `/sslnet/channel/<N>/mute` |
| **Yamaha** (DM7 / RIVAGE PM) | `/Yamaha/Scene/recall <int>` or `/RIVAGE/Scene/recall` | `/Yamaha/Transport/record` `/RIVAGE/Transport/record` … | `/Yamaha/CH/<N>/Name` `/RIVAGE/CH/<N>/Name` | `/Yamaha/CH/<N>/Mute` `/RIVAGE/CH/<N>/Mute` |

So a DiGiCo Quantum console firing snapshot 17 will drop a marker named "Scene 17" in the current session. Allen & Heath SQ-7 / Avantis sending `/sq/ch3/name "BD In"` retitles strip 3 to "BD In" on the fly.

## STREAM bus

Right-click any strip → **Send to STREAM bus**. The strip's playback (post-fader, post-mute/solo, with its pan) is summed into a dedicated stereo pair that you can route to a streaming encoder, a broadcast feed, or a separate amp. The output pair is set programmatically via `AudioEngine::setStreamOutputs(L, R)`; defaults to disabled until configured.

## Meterbridge

Click **METERS** → a floating window opens with one big LED meter + name per strip. Drag it to a second screen. Engineers can keep this dedicated to gain-staging while running the rest of the app on the main display.

## Patch page

Click **PATCH** (header row 2, far right) to open a modal patch matrix with two tabs:

- **INPUT PATCH** — rows are the device's hardware inputs (IN 1…N), columns are the channel strips. Click a circle to route that hardware input to the corresponding strip. Click the active circle again to clear (strip becomes unrouted → no audio captured).
- **OUTPUT PATCH** — same layout but rows are hardware outputs and columns route VSC playback. Each strip plays Track_NN.wav out the chosen hardware output.

Each column is radio-style: a strip can be patched to at most one hardware channel at a time. Strip header in the matrix is coloured to match the strip's personality colour for quick visual matching.

The matrix reads / writes the same `inputRouting` / `outputRouting` state on `TrackState` that the per-strip dropdowns at the top of each channel strip use, persisted via `StripRouting` across launches.

## FILE menu

The **FILE** button (header row 2) opens a popup with everything session-level:

- **Open Session…** — pick a `Session_*` folder for VSC playback (same as the old LOAD button)
- **Save Session State** — writes a `session_settings.json` next to the audio files containing the current capture format, pre-roll, phase pair, loop region, and per-track names + colours. Auto-greyed when no session is active.
- **Save Session As…** — pick a destination folder; the entire active session (audio + `markers.json` + state) is copied there. Useful for archiving before tweaking.
- **Export ▶**
    - **Export All Tracks…** — opens the export dialog, then a destination chooser. Every track is re-encoded to the chosen format / rate / bit depth, with the track's display name embedded in the output filename (`Track_NN - <name>.<ext>`).
    - **Export Individual Track ▶** — submenu lists every track by its name; pick one, run through the same dialog, choose destination.

### Export options

| Format | Bit depths | Notes |
|---|---|---|
| WAV  | 16, 24, 32-float | Most compatible. 32-bit is IEEE float. |
| AIFF | 16, 24, 32-float | Apple-native PCM container. |
| FLAC | 16, 24            | Lossless compressed; FLAC spec maxes at 24-bit. |
| MP3  | n/a                | Bitrate selectable (128 / 256 / 320 kbps). Needs `lame` installed (`brew install lame`). |

| Sample rate | Use |
|---|---|
| 44.1 kHz | CD / streaming |
| 48 kHz   | Video / broadcast |
| 96 kHz   | High-res production |
| 192 kHz  | Mastering archive |

Resampling is done via `juce::ResamplingAudioSource`. For MP3, the track is first rendered to a temp 24-bit WAV at the target sample rate, then `lame` is invoked with `-b <bitrate> --quiet`. The temp WAV is deleted on success.

"Active session" = the recording in progress, or (if not recording) the session currently loaded for playback.

## Track names

- **Double-click** a strip's name label to edit it inline.
- **Right-click** anywhere on the strip body to open a context menu with **Rename…**, **Change colour…**, **Reset colour**, and **Reset name**.
- Names are saved via `juce::PropertiesFile` next to the colour overrides and re-applied to each `TrackState::name` when the audio device starts.

## Track colours

Each channel strip carries a small colour swatch to the left of its name. Click it to open a 10-swatch palette popup:

- 8 ZynForge personality washes (dusty blue, moss, olive, violet, wine, teal, amber, mustard)
- 2 neutrals (slate, graphite)
- **Custom…** opens a full HSV / sliders colour picker
- **Default** reverts that strip to its index-based personality colour

Choices persist across launches via `juce::PropertiesFile` (stored under `~/Library/Application Support/Zynforge Recording/`), keyed by channel index. The chosen colour is read from `TrackState::colourARGB` (atomic) every paint, so changing a strip's colour is instant.

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
