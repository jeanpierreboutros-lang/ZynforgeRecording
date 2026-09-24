# ZynForge Recording

Live multitrack recorder + virtual soundcheck for macOS. Built on JUCE 8 / C++20.

A focused recording surface for engineers running front-of-house or monitors: capture console inputs and play those tracks back through the same outputs during soundcheck. **Not a DAW** — no plugins, no in-the-box effects. No recording system can guarantee against every failure; use independent redundancy for important shows.

## Status

Active development, pre-1.0. Ships **multitrack recording**, **virtual-soundcheck playback**, a non-destructive **clip/region editor with take comping**, **bounce to stems + stereo mix**, and **live OSC console integration** as a single coherent surface.

## Build

As of 2026-09-24, the source macOS-12 universal Release includes the EDIT live-navigation and manual-punch fixes and passes **378 test groups with 0 failures** locally. The previously verified `ccd755e` app remains installed at `/Applications/Zynforge Recording.app` with its matching protocol-v3 capture helper; that installation and the transfer DMG do **not** yet include these fixes. [GitHub run 35841647969](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/35841647969) passed for the earlier code. Real-device punch/navigation and the exact-rig rehearsal remain unverified. Native AppKit geometry warnings remain under investigation. See [the installation record](INSTALL.md) and [Show readiness](SHOW-READINESS.md).

```bash
cmake -B build -G Xcode
cmake --build build --config Release
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
```

First configure fetches JUCE 8.0.4 via `FetchContent`. macOS 12.0+ Universal (Apple Silicon + Intel).

For local installation, follow [INSTALL.md](INSTALL.md). The app needs the matching `ZynforgeCapture` executable bundled in `Contents/MacOS`; copying the GUI bundle alone omits it. Stop all takes and quit both processes before replacement. Capture protocol is version **3** and requires a successful compatible Hello before any command is accepted.

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
| Local installation, verification and rollback | [`INSTALL.md`](INSTALL.md) |
| Planned SD5 / 56-input show and acceptance gates | [`SHOW-READINESS.md`](SHOW-READINESS.md) |
| 2026-09-20 recording-reliability audit fixes and acceptance gaps | [`AUDIT_FIXES_2026-09-20.md`](AUDIT_FIXES_2026-09-20.md) |
| 2026-09-15 whole-project audit and verification limits | [`AUDIT_REPORT_2026-09-15.md`](AUDIT_REPORT_2026-09-15.md) |
| September audit: all 32 fixes and validation limits | [`AUDIT_FIXES_2026-09-12.md`](AUDIT_FIXES_2026-09-12.md) |
| Manual regression and hardware checklists | [`FIELD-TEST.md`](FIELD-TEST.md), [`FIELD-TEST-AUDIT.md`](FIELD-TEST-AUDIT.md) |

### September safety changes

- Capture arms are frozen for each take. Session switching and input reconfiguration are refused while local or daemon capture is active.
- Track moves/deletions share a journaled transaction across media and track state. Deleted-track audio is retained under `Removed Tracks`; track-order changes clear stale index-based undo history and the clip clipboard.
- Imported media preserves existing edits. Empty arrangements stay silent, missing explicit media never substitutes another take, and cross-track clips retain source-channel identity.
- Session UUIDs, capture settings and sample rate round-trip. Daemon configuration is acknowledged before recording; STOP waits for acknowledgement and consecutive takes preserve earlier audio.
- Fresh recording refuses every existing `Track_NN` container unless the user explicitly continues or punches, including when the session cannot be read normally.
- Primary, backup, mirror, daemon-finalization and report failures remain latched and visible. Protocol-v3 commands require a compatible handshake.
- Expensive EDIT analysis/render operations run in the background and refuse stale results; playback and destructive edits are blocked for both local and daemon takes.
- Daemon confidence uses the age of actual status pushes and marks lost/stale links unavailable; companion channel names, mute/solo and colours reflect GUI controls during daemon capture. Remote RECORD uses the active session and its normal preflight, while remote STOP requires the same two taps as the local button and the companion displays the first-tap confirmation prompt.
- Snapshot retention leaves reorder-recovery journals untouched. Multipart playback/export rejects absurd part indices and propagates media read failures instead of treating a damaged take as complete.

## Feature highlights

### Capture
- Lock-free per-channel ring + background WAV writer (up to 256 channels)
- **Stereo tracks capture as ONE interleaved stereo file** (`Track_NN.wav`, 2 channels) — not two mono stems; drag it straight into your DAW. Backup + mirror copies are stereo too; legacy two-mono-file sessions still open
- BWF (`bext`) metadata + ~5 s periodic header flush — crashes leave playable files
- Pre-roll buffer (0 / 5 / 10 / 30 s) — last N seconds dumped into every track when RECORD is pressed
- **Continue a take** — pressing RECORD twice to stop a normal take parks the transport at its end; the separate STOP control rewinds. RECORD from the end appends a new continuation part (`Track_NN_partXX`) in the same session; the parts stitch into one take on playback and in EDIT. Continuing never touches the existing file
- **Punch-in recording — anywhere, any take** — while playing an existing take, press RECORD to punch at the live playhead (not an old edit cursor); or stop and place an EDIT cursor, then RECORD. Press RECORD or STOP once to punch out. Audio before and after the punched section stays intact in the **same session and original track file**. Multi-part takes are flattened into that file. The writer inherits the original container and bit depth even when capture settings changed, keeps originals until every primary/backup/mirror splice commits, and reports a failed rollback instead of silent success. Local stop also saves the session project and mix metadata
- Formats: WAV / AIFF (16 / 24 / 32-float), FLAC (16 / 24)
- **Multi-format simultaneous capture** — primary in one format, parallel backup writer in another
- **Fail-visible redundancy** — configured backup/mirror paths must open before they count as active; open/write failures stay latched through stop, skipped copies are counted, and disk-time estimates aggregate writers that share a physical volume
- **StereoMix file capture is independent of physical stream outputs** — live stream sends are recorded even when no hardware stream bus is assigned; an empty stream mix or unsupported daemon StereoMix configuration is refused before RECORD
- Auto-recover orphan sessions on next launch; `session.report.json` written on clean stop

### Playback / virtual soundcheck
- Each `Track_NN.wav` plays through the matching hardware output during soundcheck (a stereo file's two channels route to the L + R outputs)
- Clip-aware playback: per-clip mute / lock / gain / fade (linear **or equal-power**), all applied on the audio thread
- Cross-track clips read from a file-keyed reader cache, so a clip pasted onto another track plays the copied audio
- Loop region between markers; Spacebar global play/pause
- Playback refuses while either the local engine or capture daemon owns a live take, including remote, MCU, and timecode entry points

### Mixer / EDIT / PATCH (linked views)
- **Compact, console-style strips** — the dB ruler + fader + meter hug each other (no wasted gutter). Width presets **XS / S / M / L** (M ≈ 12 per page, L = 8 with the **full channel name on its own row**), plus a **GRID** view: 12 strips per row × 2 rows = **24 faders on one page** (scrolls vertically). Channels default to neutral grey and recolour from a hue×shade **gradient** swatch picker
- Per-strip fader (−60..+12 dB) — **click-drag only** (the cap snaps to the pointer; the scroll wheel scrolls the mixer, never the level), constant-power pan, REC / MON / MUTE / SOLO toggles
- Adaptive LED meter — a 20-segment LED ladder at full size; tiny meters fall back to a **solid colour-by-level** bar (flat, no gradient)
- Stereo pairs collapse into one logical strip / row / column in **all three views**, and every per-strip change that belongs to the pair — mute / solo / arm / gain / routing / **colour / name** — mirrors onto both halves from whichever view you make it in
- Pro Tools-style EDIT view: per-row size menu, custom heights, captioned **Smart / Range / Trim / Move / Fade / Scrub** tools with single-key shortcuts (**S / R / T / G / F / B**, Cmd+E to separate), a row header that stays pinned to the left when you scroll the timeline, a graduated DAW time ruler (playhead time bubble, edit cursor that merges with the playhead when stopped, loop shading) and a draggable timeline minimap when zoomed. Wheel scrolls tracks, Shift+wheel or horizontal trackpad gesture pans time, Cmd+wheel zooms time and Cmd+Shift+wheel zooms waveform height. H+/H- zoom time in reciprocal steps, including back to 1x fit-to-take. During recording/playback, manual horizontal navigation pauses auto-follow; **FOLLOW** returns to the live playhead
- **Pro Tools-style waveform** — each recorded clip is a light, **channel-coloured block with the waveform drawn DARK on top** (loud fills dark, quiet lets the colour show), at honest levels (no over-amplified hiss). While recording, each armed lane builds a live forge-orange envelope in real time (from the captured audio, no disk reads) — including while **continuing** or **punching** a take — and the clean file waveform draws in on stop. **Clip gain just scales the waveform** (no gain line); the clip is uncluttered at rest (gain handle on hover)
- **Region editing** — Separate (`Cmd+E`), Heal Separation (`Cmd+H`), Clear / ripple Clear (`Delete` / `Shift+Delete`, with a confirm), multi-clip selection (Shift+click), Duplicate (`D`) / Nudge (`Alt+←/→`, numpad, configurable step via `N`), Zoom to Selection (`E`), a clip clipboard (`Cmd+X/C/V`) that pastes at the playhead — including **cross-track** — plus a numeric selection readout in the ruler. All clip edits are non-destructive and Cmd+Z-undoable
- **Cleanup & delivery tools (Pro Tools-style)** — **Strip Silence** analyzes the current rendered arrangement (edits, fades, gain, stereo/multipart and cross-track sources) and separates it around silent gaps; **Consolidate** flattens a range or clip to a uniquely numbered flat file; Normalize, Consolidate, Strip Silence and transient detection run off the UI thread and reject stale results. A **clip-gain corner fader** rides each clip's level (drag, Alt-click resets to 0 dB) and the waveform scales with the gain. Mixer moves (fader / pan / mute / solo / rename / colour / routing / output mute / stream send) are Cmd+Z-undoable too; recordings are not (delete them explicitly, with a confirm)
- **Edit groups** keep clip edits phase-coherent — split / trim / move / fade and selection propagate across grouped tracks
- **Take comping** — capture takes, then either menu-comp (*Comp selection from ▸ Take N*) or the visual **swipe-comp lanes** (a sub-lane per take; drag across a take to pull that section into the active comp)
- **Per-track automation lanes** (Volume / Pan / Mute) with curve-aware rendering (Hold / Linear+continuous tension / S-Curve), draggable per-segment tension handles, copy / paste / clear range, undo-aware drag coalesce, persisted in `.zfproj`
- **WRITE-mode automation**: Touch / Latch / Write dropdown + SUSPEND (read bypass) + PUNCH (shift-drag range on the time ruler gates writes) + per-track Automation Safe lock with a header LED
- **Keyboard automation-point navigation** in EDIT: `←` / `→` step the focused point through the active lane (seeks the playhead), `↑` / `↓` nudge its value, `Delete` removes it — all wrapped in undo

### Export / deliver
- Export defaults to **the session's own sample rate**, so "just export it" is a straight copy — a conversion is something you choose, and the dialog says which one you're getting
- **Bounce edited tracks (stems)** — renders each track's clip arrangement (positions, fades, clip gain, mutes, active take) to flat 24-bit WAVs, so the edits actually reach the deliverable
- **Bounce stereo mix** — sums the whole edited arrangement through gain / pan / mute / solo + volume/pan/mute automation + VCA + master to a 24-bit stereo WAV
- Both render offline on a worker thread (no real-time risk) and **stream to disk in fixed windows** — a multi-hour show bounces in a few MB of RAM; cross-track clips render from their own source file; the bounce cancels + joins cleanly if you quit mid-render
- Per-track / per-format export (WAV / AIFF / FLAC) with sample-rate conversion — **stereo-pair tracks export as one interleaved stereo file**, not two mono stems; **continue-recorded takes export whole** (all `Track_NN_partXX` continuations are stitched, not just the first part); a partial-export failure (e.g. disk full mid-batch) reports *INCOMPLETE* rather than success, and never leaves a truncated file behind. **Export runs on a worker thread** — a 32-track batch no longer freezes the app — and a same-rate export is a straight copy (no resampler in the path)
- **Imported stereo files stay one stereo track** — collapsed to a single strip with one stereo meter, persisted across reopen, and bounced/exported as one stereo file. **Imports are converted to the session's sample rate** on the way in, so a 44.1 k file dropped into a 48 k session can't play back at the wrong speed
- **Post-show QC report** — one click scans every track for peak / integrated LUFS / clipping events (with timecode) / noise floor, pops a sortable table and writes a text report next to the exports
- **Verified show handoff** — File ▸ Export ▸ Create Verified Show Handoff copies the complete stopped session to a new/empty folder on a background worker, compares SHA-256 for every original and copied file, and adds a channel-map CSV, timeline CSV (tracks/markers/cues), and `Handoff Manifest.json`. An interrupted or failed copy retains `HANDOFF INCOMPLETE.txt`; never deliver that folder as verified. The manifest verifies the *transfer*, not that the original take or external backup drives passed show-readiness checks. It flags pending/missing/incomplete source capture hashes, skipped mirrors, and unreported audio for manual review, including legacy root-level takes.
- **Detect songs → markers** — multi-track quorum scan (crowd noise on ambient mics doesn't fool it) drops a named marker at every song start for instant next-day navigation
- **Console link (pluggable per console family)** — pick your desk in the connect dialog. Every supported family gets the two things that matter to a recorder, both read-only and safe on any desk: **channel names land on the strips** so takes arrive labelled, and **every scene / snapshot recall drops a named marker** so the show is navigable the next morning. Supported: **Behringer X32 / Midas M32** (full control — repatch to the card returns for virtual soundcheck and back, with the show patch queried and stashed first, plus head-amp gain capture on show night and restore on VSC day), **Behringer WING**, **DiGiCo SD / Quantum**, **Yamaha CL / QL / RIVAGE / DM** (SCP over TCP), **Allen & Heath dLive / Avantis / SQ** (MIDI over TCP), and generic OSC. On the large-format desks the console's own Virtual Soundcheck does the repatch — ZynForge records and plays the card returns. **Anything that could change your desk is gated:** on connect the app probes the console, and until it answers the way the profile expects, the link stays strictly read-only and the repatch / gain items grey out. Only the X32 / M32 address model is trusted outright; the others are implemented from the published protocols and unlock once your desk confirms itself.

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
- Master mute is a hard kill for the selected speaker pair, including any direct strip or stream route that overlaps it; unrelated FOH/direct outputs remain live
- 4 aux sends per strip with pre/post-fader switch and bus targeting
- Right-click any strip → Assign to VCA. VCA + edit-group assignments save **per session** (live in `session_mix.json`), no longer leak across sessions

### Metronome (click track)
- Real-time click engine + offline-rendered click WAV. **Downbeat accent follows the session time signature** (3/4 → every 3, 6/8 → every 6, …) — the bar length isn't hard-wired to 4
- Voice + subdivision per accent vs. off-beat; click-track regen on tempo change

### Console integration (OSC)
Five inbound dialects: Generic, DiGiCo, Allen & Heath (SQ / Avantis), SSL Live, and Yamaha (DM / RIVAGE). Console dialects accept the read-only values a recorder needs — channel names, gain/trim for Trim-Follow, scene recalls and markers — but cannot arm, mute, or drive transport. **Generic OSC control is authenticated:** the app shows a per-start token when OSC begins listening, and every state-changing Generic command must include that token as its final string argument. A tokenless UDP packet cannot start a take. Channel indices are 1-based to match console numbering.

### Control surfaces (MIDI)
Bidirectional **Mackie Control / FaderPort (MCU)** surface: motor **faders ↔ channel gain**, **mute / solo / arm** with LED feedback, **V-pots → pan** with ring feedback, **scribble strips** show names, **bank / channel** buttons page through all tracks, and **meters** mirror to the surface. Plus a **master (9th) fader** for the monitor level, **jog-wheel transport scrub**, and the surface's **7-segment time display** showing the playhead as `HH:MM:SS:FF`. Channel state is applied straight off the MIDI thread (atomic); transport is marshalled to the message thread.

### Companion server
HTTP server on `:9000` — start it from **Session ▸ Start companion server on :9000…** (it copies the access URL, with token, to your clipboard); a browser / iPad then opens that URL. Polled state JSON, POST commands for mute / solo / arm / transport, continuous PCM stream for remote audition (`/stream.wav`, served at the device's real sample rate).

**Security & secure remote access.** The companion binds **loopback only (`127.0.0.1`)** and **every** request — the state poll, command POST, and the `/stream.wav` audio stream — needs a 64-hex access token (regenerated each start, compared in constant time) via `?t=<token>` or `Authorization: Bearer`. Concurrent connections are capped so a peer that opens sockets and sits on them can't exhaust the worker pool. The web client threads that token onto each sub-request automatically (the audio element carries it in its URL, since it can't send a header), so a token-less request to any endpoint, including the stream, gets a 401. On the same machine that's secure — localhost traffic isn't sniffable. The transport is **plaintext HTTP**, so it is *not* exposed to the LAN by default, and you should **not** serve it raw over Wi-Fi (the token and the audio stream would be sniffable). To reach it from a phone/tablet or off-machine, put a **tunnel** in front of loopback — the tunnel terminates real, CA-backed TLS and adds its own identity, which is stronger than any self-signed cert this app could ship:

The companion also links to a separate **read-only confidence monitor** at `/confidence?t=<token>`; Session ▸ Network & Surfaces ▸ Copy read-only confidence URL copies that link once the server is running. It shows recording state, per-track activity, writer/backup health, missed samples, disk load and stale/disconnected status, with optional user-enabled audible warnings. It exposes no arm, mute, solo or transport controls. During capture-daemon recording its status feed uses the daemon's last reported metrics and marks stale updates; it never silently substitutes local writer metrics. This is an observer, not a substitute for watching the recorder: a sleeping device or failed network may miss an alarm. Use the same TLS tunnel for another device.

- **Tailscale** (easiest, zero-config): `tailscale serve https / http://127.0.0.1:9000` → open the `https://<machine>.<tailnet>.ts.net/` URL on any device on your tailnet.
- **Cloudflare Tunnel**: `cloudflared tunnel --url http://127.0.0.1:9000` → gives a one-off `https://…trycloudflare.com` URL.
- **SSH port-forward** (LAN, no extra service): on the phone-side machine, `ssh -L 9000:127.0.0.1:9000 user@<mac>` then browse `http://127.0.0.1:9000/` locally.

Why no built-in HTTPS: JUCE has no server-side TLS, so in-app HTTPS would mean bundling a TLS stack + a **self-signed** cert (scary browser warnings, more attack surface) — a worse security/UX trade than a tunnel that gives a trusted cert for free. See `decisions.md` *Companion server is loopback-only with a per-session access token*.

### Show-day reliability
- A console dropping its network link — desk power-cycle, switch reboot — is handled as an ordinary disconnect. It used to abort the app on the next connect or quit
- **Measured pre-flight check** — one menu action before doors: device / SR / clock config, **measured** disk write speed vs. what the armed channel count demands, free-space headroom, every mirror drive verified mounted *and writable*, live CPU callback load, session-vs-device sample-rate mismatch, and signal presence on each armed input. The write-speed probe is skipped while a take is rolling (it would write 16 MB to the take's own volume); the SMART drive-health poll runs off the UI thread so a slow or unresponsive drive can't stall the app mid-show
- **LOCK** button disables every other control so a stray click can't kill a take
- **Device settings lock while a take is rolling** — the DEVICE panel is a floating window you can leave open, and changing the interface, sample rate or buffer size restarts the audio device, which ends the recording. Once RECORD is pressed those combos grey out and say why, and closing the panel won't revert anything either. Nothing you can click in it can stop a take
- **Mirror destinations are validated, not trusted** — a mirror writes the same folder layout the session itself uses, so a root pointing at (or inside) your sessions folder would have landed on the take's own files. The picker refuses a root that holds the session, sits inside it, duplicates another mirror, or is already the backup destination, and says which row is the problem. The same check runs again at record start, because a mirror you configured last week is still there when you open a session somewhere new. If a mirror can't be written at all — drive not mounted — the recording banner tells you *during* the take that you have fewer copies than you configured
- Redundant-write to a second drive in parallel, plus N-way mirrors — all of which count toward the "minutes remaining" estimate and the DISK STRUGGLING warning. The free-space readout tracks the volume the take actually lands on, including a custom Local Storage location
- Recording always **pre-fader** — fader / pan / mute / solo are monitoring concerns only
- **Auto-save + backup session** — Session ▸ *Auto-Save & Backup…* (Off / 1 / 2 / 5 / 10 / 15 min) periodically saves the session and drops a complete, restorable **backup session** (all session-defining files, not the multi-GB audio) into `Session File Backups/<Name>_<stamp>/`, keeping the 10 newest. Recordings are always written live and crash-safe independent of this
- Auto-save only marks a session clean after every metadata file and the backup snapshot succeeds. Failure is shown immediately and retried after 15 seconds instead of delaying until the next normal interval
- Device settings, mirror drives and click-track generation are all locked while a take is rolling — each one would otherwise stop or silence something mid-show, and each now says so rather than failing quietly
- **RF64** large takes (one continuous file past 4 GiB) + a fast, hardware-accelerated SHA-256 integrity manifest written on stop. Field-verified with a 6 h+ overnight soak (0 crashes, flat RAM). Validate any take with `tools/verify_take.sh`

### Workflow polish
- Every text / number prompt (rename track, marker name, cue name, clip gain, +CH, New Session, …) opens with its field focused + text selected; **Enter** confirms the primary action without reaching for the mouse
- Cue switching repaints the EDIT automation lanes immediately so the curve visibly updates per song
- Waveform cache builds in the background on import / record and is flushed to `WaveCache.wfm` as soon as the scan finishes, not only on app quit — reopening a session paints waveforms instantly

### Session integrity
- New, Open, New from CSV, New from Console, Finder `.zfproj` opens, and template-based session creation all offer **Save & Continue / Continue Without Saving / Cancel** before replacing the current session. A failed save cancels the switch, close, or quit instead of discarding the current state
- **Save As is a clone, not a merge:** it requires a new or empty destination, saves the source first, copies on a cancellable worker, and switches to the clone only after a complete copy. A failed/cancelled copy leaves the source session active and removes a newly-created partial destination
- Moving a session is similarly transactional. Recording, playback start, strip changes, bounces, and competing session operations are gated while a move/copy/bounce owns the session; a cross-volume move keeps the complete destination authoritative and warns if the old copy could not be removed
- Opening a session clears all previous session state, loads audio and the exact mixer track count (including shrinking), then restores playlists/automation after default clips are seeded. A malformed or missing `session_mix.json` falls back to the audio count instead of leaking the previous session's strips
- `session_mix.json` also owns output-mute and stream-send state; both reset on session replacement and participate in undo
- `.zfproj` files are real macOS documents: double-clicking one or sending it to an already-running app opens its containing session through the same guarded path
- A configured default template is applied to every new session, including the Welcome flow. Choosing **New Session from Template** creates a new session; it never destructively applies a template over the open show

## Visual identity

Design values are sourced from the **FORGE family design system** (`../ZynForgeBrand/tokens.json` → generated, vendored `Source/Theme/ForgeTokens.h`) — one token edit retunes the whole ZynForge family. See `ZynForgeBrand/FORGE.md`.

Part of the ZynForge family — shares palette + fader/meter style with ZynForge Live (sibling project). **Flat design: every surface is a solid fill (no gradients or glossy sheens).** Near-black canvas, neutral-grey strips by default (recoloured per channel from a hue/shade picker), LED-segment meters, and **bright orange reserved for STATE** (armed / peaking), never permanent chrome.

Native macOS sans-serif for UI text + bundled `JetBrains Mono` for tabular numerals. (The former files named as Inter were HTML error pages, not fonts, and have been removed.) A named type scale (UI ladder + mono `monoStamp/monoCounter/readout/…`), a 14-step `alpha::` scale, `space::`/`radius::`/`motion::`/`shadow::` tokens. All chrome routes through `Source/Theme/` tokens — **never raw hex, raw fonts, raw gradients, or ad-hoc opacities** — enforced by `Tools/design_audit.sh` (run as a pre-commit gate; raw colours/fonts/alphas fail, and a spacing ratchet stops raw `reduced()` px from growing). A second pre-commit gate, `Tools/invariants_audit.sh`, does the same for **correctness**: 12 rules encoding the bug classes that repeat audits kept finding re-violated in new places (freed-`TrackState` reads, `.wav`-only take globs, unguarded device restarts mid-take, …). Full rationale in [`design.md`](design.md).

## Where things live

```
Source/
├── Main.cpp           — JUCE app entry
├── Audio/             — real-time + persistence (AudioEngine, MultitrackRecorder, SessionPlayer, …)
├── UI/                — message-thread paint code (MainComponent, dialogs, strips, …)
├── Theme/             — design system (BrandColors, BrandTokens, DialogChrome, LookAndFeel)
└── Network/           — CompanionServer, NDIBridge, OscRemote
```

Sessions land in `~/Music/Zynforge Sessions/<SessionName>/` with subfolders (`Audio Files/`, `Export Files/`, `Session File Backups/`) and per-session state files at the root: **`<Name>.zfproj`** (cues, playlists, automation, UI layout), **`session_mix.json`** (per-strip mix + session tempo), **`markers.json`** (markers), **`session.report.json`** (sha256 + counts on clean stop), and **`WaveCache.wfm`** (versioned thumbnail cache). The `.zfproj` document carries the ZynForge icon in Finder and opens the session when double-clicked. See [`architecture.md`](architecture.md) §6 for the full data flow.

## Sibling project

[ZynForge Live](https://github.com/jeanpierreboutros-lang) — JUCE plugin-insert host. Shares the visual identity (`Source/Theme/`) but not code.

## License

[TODO] License file not yet added.
