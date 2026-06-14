# ZynForge Recording

Live multitrack recorder + virtual soundcheck for macOS. Built on JUCE 8 / C++20.

A focused recording surface for engineers running front-of-house or monitors: capture every input on the desk, play those tracks back through the same outputs during soundcheck, never lose a take. **Not a DAW** — no plugins, no in-the-box effects.

## Status

Active development, pre-1.0. Ships **multitrack recording**, **virtual-soundcheck playback**, a non-destructive **clip/region editor with take comping**, **bounce to stems + stereo mix**, and **live OSC console integration** as a single coherent surface.

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
- **Stereo tracks capture as ONE interleaved stereo file** (`Track_NN.wav`, 2 channels) — not two mono stems; drag it straight into your DAW. Backup + mirror copies are stereo too; legacy two-mono-file sessions still open
- BWF (`bext`) metadata + ~5 s periodic header flush — crashes leave playable files
- Pre-roll buffer (0 / 5 / 10 / 30 s) — last N seconds dumped into every track when RECORD is pressed
- **Punch-in recording** — re-record a *section* of a take, keeping the audio before the punch-in and after the punch-out. Arm PUNCH, set the punch region, arm the tracks to overdub, and play through it. Capture-safe: the recorder writes a clean fresh file and the section is spliced into the take on stop (temp + atomic swap, original moved aside first), so a failure reverts to the original rather than corrupting it. Primary + backup + every mirror are spliced together so all drives stay identical
- Formats: WAV / AIFF (16 / 24 / 32-float), FLAC (16 / 24)
- **Multi-format simultaneous capture** — primary in one format, parallel backup writer in another
- Auto-recover orphan sessions on next launch; `session.report.json` written on clean stop

### Playback / virtual soundcheck
- Each `Track_NN.wav` plays through the matching hardware output during soundcheck (a stereo file's two channels route to the L + R outputs)
- Clip-aware playback: per-clip mute / lock / gain / fade (linear **or equal-power**), all applied on the audio thread
- Cross-track clips read from a file-keyed reader cache, so a clip pasted onto another track plays the copied audio
- Loop region between markers; Spacebar global play/pause

### Mixer / EDIT / PATCH (linked views)
- **Compact, console-style strips** — the dB ruler + fader + meter hug each other (no wasted gutter). Width presets **XS / S / M / L** (M ≈ 12 per page, L = 8 with the **full channel name on its own row**), plus a **GRID** view: 12 strips per row × 2 rows = **24 faders on one page** (scrolls vertically). Channels default to neutral grey and recolour from a hue×shade **gradient** swatch picker
- Per-strip fader (−60..+12 dB) — **click-drag only** (the cap snaps to the pointer; the scroll wheel scrolls the mixer, never the level), constant-power pan, REC / MON / MUTE / SOLO toggles
- Adaptive LED meter (smooth gradient at small heights, 20-segment LEDs at full size)
- Stereo pairs collapse into one logical strip / row / column in **all three views**
- Pro Tools-style EDIT view: per-row size menu, custom heights, captioned **Smart / Range / Trim / Move / Fade / Scrub** tools with single-key shortcuts (**S / R / T / G / F / B**, Cmd+E to separate), a row header that stays pinned to the left when you scroll the timeline, a graduated DAW time ruler (playhead time bubble, edit cursor that merges with the playhead when stopped, loop shading) and a draggable timeline minimap when zoomed
- **Live capture waveform** — each armed lane draws a red envelope growing in real time during a take (built from the input meter, no disk reads), replaced by the full-resolution file waveform on stop
- **Region editing** — Separate (`Cmd+E`), Heal Separation (`Cmd+H`), Clear / ripple Clear (`Delete` / `Shift+Delete`, with a confirm), multi-clip selection (Shift+click), Duplicate (`D`) / Nudge (`Alt+←/→`, numpad, configurable step via `N`), Zoom to Selection (`E`), a clip clipboard (`Cmd+X/C/V`) that pastes at the playhead — including **cross-track** — plus a numeric selection readout in the ruler. All clip edits are non-destructive and Cmd+Z-undoable
- **Cleanup & delivery tools (Pro Tools-style)** — **Strip Silence** (slider settings box: threshold / min-silence / min-clip / pad) separates a take around its silent gaps; **Consolidate** flattens a range or clip to one new flat file; a **clip-gain corner fader** rides each clip's level (drag, Alt-click resets to 0 dB) and the waveform scales with the gain. Mixer moves (fader / pan / mute / solo / rename / colour / routing) are Cmd+Z-undoable too; recordings are not (delete them explicitly, with a confirm)
- **Edit groups** keep clip edits phase-coherent — split / trim / move / fade and selection propagate across grouped tracks
- **Take comping** — capture takes, then either menu-comp (*Comp selection from ▸ Take N*) or the visual **swipe-comp lanes** (a sub-lane per take; drag across a take to pull that section into the active comp)
- **Per-track automation lanes** (Volume / Pan / Mute) with curve-aware rendering (Hold / Linear+continuous tension / S-Curve), draggable per-segment tension handles, copy / paste / clear range, undo-aware drag coalesce, persisted in `.zfproj`
- **WRITE-mode automation**: Touch / Latch / Write dropdown + SUSPEND (read bypass) + PUNCH (shift-drag range on the time ruler gates writes) + per-track Automation Safe lock with a header LED
- **Keyboard automation-point navigation** in EDIT: `←` / `→` step the focused point through the active lane (seeks the playhead), `↑` / `↓` nudge its value, `Delete` removes it — all wrapped in undo

### Export / deliver
- **Bounce edited tracks (stems)** — renders each track's clip arrangement (positions, fades, clip gain, mutes, active take) to flat 24-bit WAVs, so the edits actually reach the deliverable
- **Bounce stereo mix** — sums the whole edited arrangement through gain / pan / mute / solo + volume/pan/mute automation + VCA + master to a 24-bit stereo WAV
- Both render offline on a worker thread (no real-time risk) and **stream to disk in fixed windows** — a multi-hour show bounces in a few MB of RAM; cross-track clips render from their own source file
- Per-track / per-format export (WAV / AIFF / FLAC) with sample-rate conversion — **stereo-pair tracks export as one interleaved stereo file**, not two mono stems
- **Imported stereo files stay one stereo track** — collapsed to a single strip with one stereo meter, persisted across reopen, and bounced/exported as one stereo file
- **Post-show QC report** — one click scans every track for peak / integrated LUFS / clipping events (with timecode) / noise floor, pops a sortable table and writes a text report next to the exports
- **Detect songs → markers** — multi-track quorum scan (crowd noise on ambient mics doesn't fool it) drops a named marker at every song start for instant next-day navigation
- **Console link (pluggable per console family)** — pick your desk in the connect dialog. On a **Behringer X32 / Midas M32** one menu action repatches the inputs to the card returns for virtual soundcheck and back (the show patch is queried and stashed first, never assumed), and head-amp gains capture on show night into the session and restore to the desk on VSC day. **DiGiCo, Yamaha, SSL, Allen & Heath** ship as native-VSC profiles — ZynForge records and plays their record card and the console's own Virtual Soundcheck does the repatch (deeper over-the-wire control for those is on the roadmap)

### Metering
- Adaptive LED meters per strip + master; sticky clip latch
- **Master loudness (ITU-R BS.1770)** — momentary / short-term / integrated (gated) LUFS + a 4×-oversampled true-peak (dBTP) on the post-fader master, shown on the big clock panel (true-peak goes red within 1 dB of full scale)

### Cues + setlist
- Drop cues at any transport position; per-cue snapshot of every strip's state **and the full automation lanes** — switching to a cue swaps in that song's volume / pan / mute moves
- Explicit **Recall** button alongside the dropdown / ◀ ▶ navigation (and the cue dropdown still recalls on pick)
- Cue recall is authoritative: every automation lane is cleared first, then the cue's snapshot is applied, so a cue without entries on a track doesn't leave another cue's curve behind
- Stable strip UUIDs — reorder the mixer without breaking cue recall
- Per-cue tempo curves with accel / rit interpolation
- Soft-takeover ramps on recall — click-free state transitions
- LCD countdown to next cue, drag-reorder, Print setlist to PDF (via HTML)

### VCA + aux sends
- 8 VCA groups with per-bus gain / mute / solo / colour / name
- VCA gain + mute applied on **both** the routed per-strip outputs and the stereo monitor / master sum; stereo pairs read the left track's lane so both halves follow the curve
- 4 aux sends per strip with pre/post-fader switch and bus targeting
- Right-click any strip → Assign to VCA. VCA + edit-group assignments save **per session** (live in `session_mix.json`), no longer leak across sessions

### Metronome (click track)
- Real-time click engine + offline-rendered click WAV. **Downbeat accent follows the session time signature** (3/4 → every 3, 6/8 → every 6, …) — the bar length isn't hard-wired to 4
- Voice + subdivision per accent vs. off-beat; click-track regen on tempo change

### Console integration (OSC)
Five dialects with **full action parity** (transport, scene recall → marker, per-channel name / mute / arm / colour): Generic, DiGiCo, Allen & Heath (SQ / Avantis), SSL Live, Yamaha (DM7 / RIVAGE PM). 1-based channel indices to match console numbering.

### Control surfaces (MIDI)
Bidirectional **Mackie Control / FaderPort (MCU)** surface: motor **faders ↔ channel gain**, **mute / solo / arm** with LED feedback, **V-pots → pan** with ring feedback, **scribble strips** show names, **bank / channel** buttons page through all tracks, and **meters** mirror to the surface. Plus a **master (9th) fader** for the monitor level, **jog-wheel transport scrub**, and the surface's **7-segment time display** showing the playhead as `HH:MM:SS:FF`. Channel state is applied straight off the MIDI thread (atomic); transport is marshalled to the message thread.

### Companion server
HTTP server on `:9000` — start it from **Session ▸ Start companion server on :9000…** (it copies the access URL, with token, to your clipboard); a browser / iPad then opens that URL. Polled state JSON, POST commands for mute / solo / arm / transport, continuous PCM stream for remote audition (`/stream.wav`).

**Security & secure remote access.** The companion binds **loopback only (`127.0.0.1`)** and **every** request — the state poll, command POST, and the `/stream.wav` audio stream — needs a 32-hex access token (regenerated each start) via `?t=<token>` or `Authorization: Bearer`. The web client threads that token onto each sub-request automatically (the audio element carries it in its URL, since it can't send a header), so a token-less request to any endpoint, including the stream, gets a 401. On the same machine that's secure — localhost traffic isn't sniffable. The transport is **plaintext HTTP**, so it is *not* exposed to the LAN by default, and you should **not** serve it raw over Wi-Fi (the token and the audio stream would be sniffable). To reach it from a phone/tablet or off-machine, put a **tunnel** in front of loopback — the tunnel terminates real, CA-backed TLS and adds its own identity, which is stronger than any self-signed cert this app could ship:

- **Tailscale** (easiest, zero-config): `tailscale serve https / http://127.0.0.1:9000` → open the `https://<machine>.<tailnet>.ts.net/` URL on any device on your tailnet.
- **Cloudflare Tunnel**: `cloudflared tunnel --url http://127.0.0.1:9000` → gives a one-off `https://…trycloudflare.com` URL.
- **SSH port-forward** (LAN, no extra service): on the phone-side machine, `ssh -L 9000:127.0.0.1:9000 user@<mac>` then browse `http://127.0.0.1:9000/` locally.

Why no built-in HTTPS: JUCE has no server-side TLS, so in-app HTTPS would mean bundling a TLS stack + a **self-signed** cert (scary browser warnings, more attack surface) — a worse security/UX trade than a tunnel that gives a trusted cert for free. See `decisions.md` *Companion server is loopback-only with a per-session access token*.

### Show-day reliability
- **Measured pre-flight check** — one menu action before doors: device / SR / clock config, **measured** disk write speed vs. what the armed channel count demands, free-space headroom, every mirror drive verified mounted *and writable*, live CPU callback load, session-vs-device sample-rate mismatch, and signal presence on each armed input
- **LOCK** button disables every other control so a stray click can't kill a take
- Redundant-write to a second drive in parallel
- Recording always **pre-fader** — fader / pan / mute / solo are monitoring concerns only
- **Auto-save + backup session** — Session ▸ *Auto-Save & Backup…* (Off / 1 / 2 / 5 / 10 / 15 min) periodically saves the session and drops a complete, restorable **backup session** (all session-defining files, not the multi-GB audio) into `Session File Backups/<Name>_<stamp>/`, keeping the 10 newest. Recordings are always written live and crash-safe independent of this
- **RF64** large takes (one continuous file past 4 GiB) + a fast, hardware-accelerated SHA-256 integrity manifest written on stop. Field-verified with a 6 h+ overnight soak (0 crashes, flat RAM). Validate any take with `tools/verify_take.sh`

### Workflow polish
- Every text / number prompt (rename track, marker name, cue name, clip gain, +CH, New Session, …) opens with its field focused + text selected; **Enter** confirms the primary action without reaching for the mouse
- Cue switching repaints the EDIT automation lanes immediately so the curve visibly updates per song
- Waveform cache builds in the background on import / record and is flushed to `WaveCache.wfm` as soon as the scan finishes, not only on app quit — reopening a session paints waveforms instantly

## Visual identity

Design values are sourced from the **FORGE family design system** (`../ZynForgeBrand/tokens.json` → generated, vendored `Source/Theme/ForgeTokens.h`) — one token edit retunes the whole ZynForge family. See `ZynForgeBrand/FORGE.md`.

Part of the ZynForge family — shares palette + fader/meter style with ZynForge Live (sibling project). Near-black canvas, neutral-grey strips by default (recoloured per channel from a gradient picker), LED-segment meters, brand-orange for armed-but-not-rolling.

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

Sessions land in `~/Music/Zynforge Sessions/<SessionName>/` with subfolders (`Audio Files/`, `Export Files/`, `Session File Backups/`, `Clip Groups/`) and per-session state files at the root: **`<Name>.zfproj`** (cues, playlists, automation, UI layout), **`session_mix.json`** (per-strip mix + session tempo), **`markers.json`** (markers), **`session.report.json`** (sha256 + counts on clean stop), and **`WaveCache.wfm`** (versioned thumbnail cache). The `.zfproj` document carries the ZynForge icon in Finder. See [`architecture.md`](architecture.md) §6 for the full data flow.

## Sibling project

[ZynForge Live](https://github.com/jeanpierreboutros-lang) — JUCE plugin-insert host. Shares the visual identity (`Source/Theme/`) but not code.

## License

[TODO] License file not yet added.
