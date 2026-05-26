# ZynForge Recording

Live multitrack recorder + virtual soundcheck for macOS. Built on JUCE 8 / C++20.

A focused recording surface for engineers running front-of-house or monitors: capture every input on the desk, play those tracks back through the same outputs during soundcheck, never lose a take. **Not a DAW** — no plugins, no in-the-box effects.

## Status

Active development, pre-1.0. Ships **multitrack recording**, **virtual-soundcheck playback**, and **live OSC console integration** as a single coherent surface.

## Build

```bash
cmake -B build -G Xcode
cmake --build build --config Release
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
```

First configure fetches JUCE 8.0.4 via `FetchContent`. macOS 11.0+ Universal (Apple Silicon + Intel).

## Documentation

| Topic | File |
|---|---|
| Operating rules + workflow | [`CLAUDE.md`](CLAUDE.md) |
| Technical architecture, component map, data flow | [`architecture.md`](architecture.md) |
| Product requirements + UX rationale | [`design.md`](design.md) |
| Current priorities, in-progress, backlog | [`tasks.md`](tasks.md) |
| Architecture Decision Records | [`decisions.md`](decisions.md) |
| Coding conventions + brand-token rules | [`coding-standards.md`](coding-standards.md) |
| Build / smoke-test / field-test strategy | [`testing.md`](testing.md) |
| User-visible changes | [`CHANGELOG.md`](CHANGELOG.md) |

## Feature highlights

### Capture
- Lock-free per-channel ring + background WAV writer (up to 256 channels)
- BWF (`bext`) metadata + ~5 s periodic header flush — crashes leave playable files
- Pre-roll buffer (0 / 5 / 10 / 30 s) — last N seconds dumped into every track when RECORD is pressed
- Formats: WAV / AIFF (16 / 24 / 32-float), FLAC (16 / 24)
- **Multi-format simultaneous capture** — primary in one format, parallel backup writer in another
- Auto-recover orphan sessions on next launch; `session.report.json` written on clean stop

### Playback / virtual soundcheck
- Each `Track_NN.wav` plays through the matching hardware output during soundcheck
- Clip-aware playback: per-clip mute / lock / gain / fade, all applied on the audio thread
- Loop region between markers; Spacebar global play/pause

### Mixer / EDIT / PATCH (linked views)
- 12 strips per page (adaptive width 56–160 px), 8 personality colours rotating by index
- Per-strip fader (−60..+12 dB), constant-power pan, REC / MON / MUTE / SOLO toggles
- Adaptive LED meter (smooth gradient at small heights, 20-segment LEDs at full size)
- Stereo pairs collapse into one logical strip / row / column in **all three views**
- Pro Tools-style EDIT view: per-row size menu, custom heights, Smart / Selector / Trim / Grabber / Fade / Scrubber tools
- **Per-track automation lanes** (Volume / Pan / Mute) with curve-aware rendering (Hold / Linear+continuous tension / S-Curve), draggable per-segment tension handles, copy / paste / clear range, undo-aware drag coalesce, persisted in `.zfproj`
- **WRITE-mode automation**: Touch / Latch / Write dropdown + SUSPEND (read bypass) + PUNCH (shift-drag range on the time ruler gates writes) + per-track Automation Safe lock with a header LED
- **Keyboard automation-point navigation** in EDIT: `←` / `→` step the focused point through the active lane (seeks the playhead), `↑` / `↓` nudge its value, `Delete` removes it — all wrapped in undo

### Cues + setlist
- Drop cues at any transport position; per-cue snapshot of every strip's state
- Stable strip UUIDs — reorder the mixer without breaking cue recall
- Per-cue tempo curves with accel / rit interpolation
- Soft-takeover ramps on recall — click-free state transitions
- LCD countdown to next cue, drag-reorder, Print setlist to PDF (via HTML)

### VCA + aux sends (Pro Tools-style)
- 8 VCA groups with per-bus gain / mute / solo / colour / name
- 4 aux sends per strip with pre/post-fader switch and bus targeting
- Right-click any strip → Assign to VCA

### Console integration (OSC)
Five dialects with **full action parity** (transport, scene recall → marker, per-channel name / mute / arm / colour): Generic, DiGiCo, Allen & Heath (SQ / Avantis), SSL Live, Yamaha (DM7 / RIVAGE PM). 1-based channel indices to match console numbering.

### Companion server
HTTP server on `:9000` — browser / iPad opens `http://<this-mac>:9000/`. Polled state JSON, POST commands for mute / solo / arm / transport, continuous PCM stream for remote audition (`/stream.wav`).

### Show-day reliability
- **LOCK** button disables every other control so a stray click can't kill a take
- Redundant-write to a second drive in parallel
- Recording always **pre-fader** — fader / pan / mute / solo are monitoring concerns only

## Visual identity

Part of the ZynForge family — shares palette + fader/meter style with ZynForge Live (sibling project). Near-black canvas, eight personality wash colours rotating by strip index, LED-segment meters, brand-orange for armed-but-not-rolling.

`Inter` (UI) + `JetBrains Mono` (tabular numerals) bundled as BinaryData. Seven-step type scale. Three-step elevation tokens. All chrome routes through `Source/Theme/BrandColors.h` + `BrandTokens.h` + `DialogChrome.h` — never raw hex literals. Full rationale in [`design.md`](design.md).

## Where things live

```
Source/
├── Main.cpp           — JUCE app entry
├── Audio/             — real-time + persistence (AudioEngine, MultitrackRecorder, SessionPlayer, …)
├── UI/                — message-thread paint code (MainComponent, dialogs, strips, …)
├── Theme/             — design system (BrandColors, BrandTokens, DialogChrome, LookAndFeel)
└── Network/           — CompanionServer, NDIBridge, OscRemote
```

Sessions land in `~/Music/Zynforge Sessions/<SessionName>/` with subfolders (`Audio Files/`, `Export Files/`, `Session File Backups/`, `Clip Groups/`). The `.zfproj` document carries the ZynForge icon in Finder. See [`architecture.md`](architecture.md) §6 for the full data flow.

## Sibling project

[ZynForge Live](https://github.com/jeanpierreboutros-lang) — JUCE plugin-insert host. Shares the visual identity (`Source/Theme/`) but not code.

## License

[TODO] License file not yet added.
