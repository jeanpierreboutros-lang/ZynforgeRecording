# ZynForge Recording

Live multitrack recorder + virtual soundcheck for macOS. Built on JUCE 8 / C++20.

A focused recording surface for engineers running front-of-house or monitors: capture console inputs and play those tracks back through the same outputs during soundcheck. **Not a DAW** — no plugins, no in-the-box effects. No recording system can guarantee against every failure; use independent redundancy for important shows.

## Status — 2026-11-02

`main` adds Scene Follow and desk macros, timestamped BWF with channel names, a dead-channel alarm, the clock source in pre-flight, Reaper and DAWproject export, Export Songs, the Twin Recorder link and Mac packaging, plus a full round of punch, capture, session, editing, EDIT, network and playback fixes (see *Unreleased* in [CHANGELOG.md](CHANGELOG.md)). Automated results are in [testing.md](testing.md). **None of this work is installed on either Mac**, and none of it has been tried on real hardware: not on a real interface, a real console, a real stream loss or two real Macs. See [FIELD-TEST.md](FIELD-TEST.md). The DiGiCo addresses stay unverified until the desk test kit there is run.

Installed build (2026-10-29):

Both Macs and the [universal installer](dist/Zynforge-Recording-281c6c0-macOS-universal.dmg)
now use verified `281c6c0` and its matching protocol-4 helper. This build contains
every fix from the October bug hunt (punch, capture and session safety, network
hardening, engine/playback, EDIT view, clip editing), RECORD no longer waiting on
lead-in silence, and exact splits inside fades. It passes **689 test groups in
Release, ASan/UBSan, TSan and CI**. [INSTALL.md](INSTALL.md) owns hashes,
rollback paths and what was and was not checked at installation;
[CHANGELOG.md](CHANGELOG.md) lists the changes. Nothing under *Unreleased* in the
changelog is in this build, so until a newer one is installed those fixes do not
apply to either Mac. The rest of this README describes `main`.

Active development, pre-1.0. The app provides multitrack capture, virtual
soundcheck, non-destructive clip editing, stems and stereo bounce,
and console integration. The current build includes asynchronous STOP/save
finalization, ordered audio-dropout compensation, bounded settings work,
true float AIFF-C, and guarded session/network updates.

Physical 27-track recording/STOP timing, interactive navigation and the planned
56-input rehearsal remain open. Earlier five-hour primary-take results do not
validate this build, backups, or the complete rig. See
[show readiness](SHOW-READINESS.md), [field evidence](docs/history/FIELD-TEST-2026-10-01.md)
and [delivery history](CHANGELOG.md).

## Build

The tested Release bundle includes both GUI and capture helper, is universal
arm64 + x86_64, and is ad-hoc signed. It is not a notarized public release.
Both installations and the current DMG contain the same verified build;
see [INSTALL.md](INSTALL.md) before replacing either.

```bash
cmake -B build -G Xcode
cmake --build build --config Release
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
```

First configure fetches JUCE 8.0.4 via `FetchContent`. macOS 12.0+ Universal (Apple Silicon + Intel).

For local installation, follow [INSTALL.md](INSTALL.md). The current DMG contains both the GUI and matching `ZynforgeCapture` executable under `Contents/MacOS`. Stop all takes and quit both processes before replacement. Both Macs and the current DMG use protocol **4**, adding mutual authentication after a compatible Hello; always upgrade the matching GUI/helper together. No command is accepted before its required handshake completes.

## Documentation

Current package, hashes and installed versions live in [INSTALL.md](INSTALL.md); [testing.md](testing.md) records automated coverage, and [SHOW-READINESS.md](SHOW-READINESS.md) tracks physical acceptance. Dated audit and field reports retain their original evidence. Old installer/backup paths were removed during the October 4 and October 5 cleanups; no old app rollback bundle remains. The [documentation index](docs/README.md) distinguishes current guides from historical records.

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
| Complete documentation index and ownership | [`docs/README.md`](docs/README.md) |
| October 5 follow-up fixes and regression evidence | [`docs/REVIEW-REPAIRS-2026-10-05.md`](docs/history/REVIEW-REPAIRS-2026-10-05.md) |
| October 5 deep STOP/concurrency audit history | [`docs/AUDIT-2026-10-05.md`](docs/history/AUDIT-2026-10-05.md) |
| Local installation, verification and rollback | [`INSTALL.md`](INSTALL.md) |
| What has to be true before selling | [`docs/SELLING-CHECKLIST.md`](docs/SELLING-CHECKLIST.md) |
| Planned SD5 / 56-input show and acceptance gates | [`SHOW-READINESS.md`](SHOW-READINESS.md) |
| 2026-09-30 capture-integrity fixes and validation limits | [`docs/history/AUDIT_FIXES_2026-09-30.md`](docs/history/AUDIT_FIXES_2026-09-30.md) |
| 2026-09-20 recording-reliability audit fixes and acceptance gaps | [`docs/history/AUDIT_FIXES_2026-09-20.md`](docs/history/AUDIT_FIXES_2026-09-20.md) |
| 2026-09-15 whole-project audit and verification limits | [`docs/history/AUDIT_REPORT_2026-09-15.md`](docs/history/AUDIT_REPORT_2026-09-15.md) |
| September audit: all 32 fixes and validation limits | [`docs/history/AUDIT_FIXES_2026-09-12.md`](docs/history/AUDIT_FIXES_2026-09-12.md) |
| Manual regression and hardware checklists | [`FIELD-TEST.md`](FIELD-TEST.md), [`FIELD-TEST-AUDIT.md`](FIELD-TEST-AUDIT.md) |
| 55-channel long-take evidence and waveform fix limits | [`docs/history/FIELD-TEST-2026-10-01.md`](docs/history/FIELD-TEST-2026-10-01.md) |

### September safety changes

- Capture arms are frozen for each take. Session switching and input reconfiguration are refused while local or daemon capture is active.
- Track moves/deletions share a journaled transaction across media and track state. Deleted-track audio is retained under `Removed Tracks`; track-order changes clear stale index-based undo history and the clip clipboard.
- Imported media preserves existing edits. Empty arrangements stay silent, missing explicit media never substitutes another take, and cross-track clips retain source-channel identity.
- Session UUIDs, capture settings and sample rate round-trip. Daemon configuration is acknowledged before recording; STOP waits for acknowledgement and consecutive takes preserve earlier audio.
- Fresh recording refuses every existing `Track_NN` container unless the user explicitly continues or punches, including when the session cannot be read normally.
- Primary, backup, mirror, daemon-finalization and report failures remain latched and visible. Current protocol-v4 commands require a compatible authenticated handshake; unchanged pre-audit protocol-v3 packages retain their earlier handshake contract.
- Expensive EDIT analysis/render operations run in the background and refuse stale results; playback and destructive edits are blocked for both local and daemon takes.
- Daemon confidence uses the age of actual status pushes and marks lost/stale links unavailable; channel names, mute/solo and colours in the status snapshot reflect GUI controls during daemon capture. Remote RECORD uses the active session and its normal preflight, while remote STOP requires the same two taps as the local button.
- Snapshot retention leaves reorder-recovery journals untouched. Multipart playback/export rejects absurd part indices and propagates media read failures instead of treating a damaged take as complete.

## Feature highlights

These describe `main`, which is ahead of the installed `281c6c0`: see *Status*.

### Capture
- Per-channel audio rings (up to 256 channels) feed a separate writer and buffer for each copy; bounded capture-boundary synchronization preserves exact audio/gap order on overflow
- **Copies are independent** — each destination has its own writer and buffer. A stalled backup or mirror loses audio only on itself, stays sample-aligned, and the banner and report name it. If a copy stops responding at STOP, the app finishes without it and leaves it out of later takes until it comes back. A full or read-only backup or mirror is left out and named instead of refusing RECORD
- **Stereo tracks capture as ONE interleaved stereo file** (`Track_NN.wav`, 2 channels) — not two mono stems; drag it straight into your DAW. Backup + mirror copies are stereo too; legacy two-mono-file sessions still open
- WAV BWF (`bext`) metadata and periodic header flush; crash recovery remains dependent on successful writes and storage durability. Every WAV carries its time of day as a BWF TimeReference (locked LTC/MTC when chase is live, otherwise the computer's clock), so a DAW can spot takes to their original time, and its strip name as `sTRK1=`/`sTRK2=` in the bext Description, while the file stays `Track_NN.wav`. AIFF and FLAC have no bext. The time is as good as its source: with timecode, expect it within about a frame (two on MTC). It is not sample-accurate against an independent recorder.
- After a crash, AIFF 16/24 takes stay readable, and takes from older builds are repaired when the session opens. FLAC takes left by a crash open at once
- Dead-channel alarm during a take: an armed input that had signal and then reads exact digital zero for 3 s raises a channel fault; 8 or more going together inside 2 s show as one input-stream alarm; an input still at exact zero 60 s in raises a quiet notice. They appear in the status line, never touch the take, and stay in `session.report.json` with start, end and channel name after they clear. A quiet channel with a noise floor never alarms. A console sending post-gate or muted direct outs delivers real digital zero, and that will alarm.
- Pre-flight shows the audio device's clock source where the device reports one, and warns on an internal clock (right only if this device is the master). The source is recorded in `session.report.json`.
- Pre-roll buffer (Off / 5 / 10 / 30 s, set in Session ▸ Session Settings) — last N seconds are included in a fresh take; punch/continue starts at its chosen timeline boundary without prepending earlier audio
- **Continue a take** — pressing RECORD twice to stop a normal take parks the transport at its end; the separate STOP control rewinds. RECORD from a stopped playhead with no EDIT cursor appends a new continuation part (`Track_NN_partXX`) in the same session, wherever the playhead is parked; the parts stitch into one take on playback and in EDIT. Continuing never touches the existing file
- **Punch-in recording — anywhere, any take** — while playing an existing take, press RECORD to punch at the live playhead (not an old edit cursor); or stop and place an EDIT cursor, then RECORD. With the transport stopped, RECORD at a cursor asks first (*Replace audio?*): Return is **Record After Take**, which clears the cursor and appends; **Replace From Cursor** punches; Esc cancels. Press RECORD or STOP once to punch out. A selected range captures at exact audio-buffer boundaries, temporarily suspends loop playback and continues through post-roll. A range punches only if you selected it in this run, and once: after the punch it stays selected for Loop and edits, and the next RECORD appends until you select it again. A range restored with a saved session never punches until you touch it. Loop never selects anything. RECORD from a desk macro, OSC, control surface or twin never punches; it is refused and says why. The pill beside the transport time warns whenever the next RECORD would replace audio. During the punch, the live input replaces the old take in monitoring/direct outputs. Audio before and after the punched section stays intact in the **same session and original track file**. Multi-part takes are flattened into that file. The writer inherits the original container and bit depth even when capture settings changed, refuses a backup/mirror missing the original take, preserves originals until every splice commits, and recovers interrupted `.punchbase` originals on reopen while archiving partial replacements. Local stop also saves the session project and mix metadata
- Formats: WAV / AIFF (16 / 24-bit PCM), WAV / AIFF-C (32-bit float), FLAC (16 / 24-bit PCM). Float AIFF-C keeps the `.aif` extension; capture/export/punch check final flush errors
- **Multi-format simultaneous capture** — primary in one format, backup and mirrors each in their own, set in File ▸ Backup & Mirror Drives or from the BACKUP button. Backup defaults to WAV 24
- **Fail-visible redundancy** — configured backup/mirror paths must open before they count as active; open/write failures stay latched through stop, skipped copies are counted, and disk-time estimates aggregate writers that share a physical volume
- **Replica ownership is checked before anything is written** — a nonempty backup or mirror without matching primary-session provenance sits the take out, untouched, and the banner and report name it. The take itself still rolls. Choose a new empty destination to bring that copy back. Retain the original capture layout and sample rate, or start a new session.
- Saved capture-gain references and input-only stereo auto-arm keep playback activity separate from new input detection.
- **StereoMix file capture is independent of physical stream outputs** — live stream sends are recorded even when no hardware stream bus is assigned. Solo never reaches the stream bus or the file: it is a monitoring tool, and only a mute takes a strip out of the mix. An empty stream mix or unsupported daemon StereoMix configuration is refused before RECORD. Session ▸ Recording & Sync ▸ Stream Bus Output sends the bus to an output pair, saved with the session. A stereo strip's STREAM send carries both channels
- Auto-recover orphan sessions on next launch; `session.report.json` metadata is written on clean stop and final SHA-256 hashes are filled in asynchronously
- If the capture daemon dies or its link drops mid-take, the app reconnects by itself. Session ▸ Recording & Sync ▸ Reconnect Capture Daemon does it on request, and Quit and turning the daemon off keep working. Daemon STOP does not freeze the window. After a crash during a daemon take, relaunching restores the take's strips, names, routing and cues

### Playback / virtual soundcheck
- Each `Track_NN.wav` plays through the matching hardware output during soundcheck (a stereo file's two channels route to the L + R outputs)
- Clip-aware playback: per-clip mute / lock / gain / fade (linear **or equal-power**), all applied on the audio thread
- Cross-track clips read from a file-keyed reader cache, so a clip pasted onto another track plays the copied audio
- Loop a region between markers or a selection, or with nothing selected the whole session; Loop never makes a selection. Spacebar global play/pause
- **Every track enters together.** A loop start and the next clip are read ahead, so a loop pass loses nothing. A locate, or PLAY from a marker, waits for the disk and brings every track in on the same sample
- **The mix stays off patched outputs.** On an interface with more than two outputs, the master mix stays off any output a strip is patched to, so a 1:1 patch never puts the whole mix on console channels 1 and 2. The master strip shows MIX OFF and the status line says why. Pick a spare pair, or Off, on the master strip. A rig whose first outputs really are monitors can turn on Session ▸ Recording & Sync ▸ Let the Mix Share Outputs With Patched Strips. The meters keep working
- **Transport Sync.** Timecode that does not start at zero needs the session's start timecode, set in Session ▸ Recording & Sync ▸ Transport Sync, typed in or taken from the first take's time stamp. Timecode outside the session stops chase with a status line, and leaves the playhead where it is
- Playback refuses while either the local engine or capture daemon owns a live take, including remote, MCU, and timecode entry points

### Mixer / EDIT / PATCH (linked views)
- **Compact, console-style strips** — the dB ruler + fader + meter hug each other (no wasted gutter). Width presets **XS / S / M / L** (M ≈ 12 per page, L = 8 with the **full channel name on its own row**), plus a **GRID** view: 12 strips per row × 2 rows = **24 faders on one page** (scrolls vertically). Channels default to neutral grey and recolour from a hue×shade **gradient** swatch picker
- Per-strip fader (−60..+12 dB) — **click-drag only** (the cap snaps to the pointer; the scroll wheel scrolls the mixer, never the level), constant-power pan, REC / MON / MUTE / SOLO toggles
- Adaptive LED meter — a 20-segment LED ladder at full size; tiny meters fall back to a **solid colour-by-level** bar (flat, no gradient)
- Stereo pairs collapse into one logical strip / row / column in **all three views**, and every per-strip change that belongs to the pair — mute / solo / arm / gain / routing / **colour / name** — mirrors onto both halves from whichever view you make it in
- Pro Tools-style EDIT view: per-row size menu, custom heights, captioned **Smart / Range / Trim / Move / Fade** tools with single-key shortcuts (**S / R / T / G / F**, Cmd+E to separate), a row header that stays pinned to the left when you scroll the timeline, a graduated DAW time ruler (playhead time bubble, edit cursor that merges with the playhead when stopped, loop shading) and a draggable timeline minimap when zoomed. Wheel scrolls tracks, Shift+wheel or horizontal trackpad gesture pans time, Cmd+wheel zooms time and Cmd+Shift+wheel zooms waveform height. H+/H- zoom time in reciprocal steps, including back to 1x fit-to-take. During recording/playback, manual horizontal navigation pauses auto-follow; **FOLLOW** returns to the live playhead
- **Dark clips** — each recorded clip is a dark body with the waveform in the track's own colour at honest levels, so EDIT stays calm in a dark room. V+/V- or Shift-wheel magnifies a quiet take on demand, and clip gain just scales the waveform; the live-capture envelope stays forge orange while a take builds
- **Region editing** — Separate (`Cmd+E`), Heal Separation (`Cmd+H`), Clear / ripple Clear (`Delete` / `Shift+Delete`, with a confirm), multi-clip selection (Shift+click), Duplicate (`D`) / Nudge (`Alt+←/→`, numpad, configurable step via `N`), Zoom to Selection (`E`), a clip clipboard (`Cmd+X/C/V`) that pastes at the playhead — including **cross-track** — plus a numeric selection readout in the ruler. All clip edits are non-destructive and Cmd+Z-undoable
- **Strip reorder** — drag a row's colour swatch in EDIT: a drop line shows where the strip lands, the list autoscrolls at its edges, and the move commits once on release (stereo pairs move as one; files, clips and cues follow). The MIXER's selection move uses the same single transaction
- **Cleanup & delivery tools (Pro Tools-style)** — **Consolidate** flattens a range or clip to a uniquely numbered flat file; it runs off the UI thread and rejects stale results. A **clip-gain corner fader** rides each clip's level (drag, Alt-click resets to 0 dB) and the waveform scales with the gain. Mixer moves (fader / pan / mute / solo / rename / colour / routing / output mute / stream send) are Cmd+Z-undoable too; recordings are not (delete them explicitly, with a confirm)
- **Edit groups** keep clip edits phase-coherent — split / trim / move / fade and selection propagate across grouped tracks. Every clip command acts on the whole stereo strip and its edit group
- **Crop and Ripple Delete** (Crop is `Ctrl+Cmd+C`) move markers, cues and other takes with the audio, as one undo step. A locked clip in the way refuses the edit. Undo and redo keep audio recorded since the step
- Cmd+X in EDIT cuts a selected clip, or clears a selected range. Removing a channel always asks first
- Clips with their own file (a consolidated clip, a clip pasted from another strip) draw the audio they play
- Master mute is a hard kill for the selected speaker pair, including any direct strip or stream route that overlaps it; unrelated FOH/direct outputs remain live
- Edit-group assignments save **per session** (in `session_mix.json`), so they never leak across sessions

### Export / deliver
- Export defaults to **the session's own sample rate**, so "just export it" is a straight copy — a conversion is something you choose, and the dialog says which one you're getting
- **File ▸ Export ▸ Export Audio** is one dialog with three choices. **Raw takes** exports the recorded files without clip edits, in the format you pick. **Edited tracks** renders each ticked strip's clip arrangement (positions, fades, clip gain, mutes, active take) to flat 24-bit WAVs, so the edits actually reach the deliverable. **Stereo mix** sums the whole edited arrangement through gain / pan / mute / solo + master to one 24-bit stereo WAV. Tracks are ticked for the first two, all on by default. Stereo mix and Export Songs ignore master MUTE, and warn about a silent mix or a solo left on
- Edited tracks and Stereo mix render offline on a worker thread (no real-time risk) and **stream to disk in fixed windows** — a multi-hour show bounces in a few MB of RAM; cross-track clips render from their own source file; the bounce cancels + joins cleanly if you quit mid-render
- Raw takes export to WAV / AIFF / FLAC with windowed-sinc sample-rate conversion, on import too (aliases about 100 dB down; a 3-hour 56-channel file set takes roughly 18 minutes under load) — **stereo-pair tracks export as one interleaved stereo file**, not two mono stems; **continue-recorded takes export whole** (all `Track_NN_partXX` continuations are stitched, not just the first part); a partial-export failure (e.g. disk full mid-batch) reports *INCOMPLETE* rather than success, and never leaves a truncated file behind. **Export runs on a worker thread** — a 32-track batch no longer freezes the app — and a same-rate export is a straight copy (no resampler in the path)
- **Imported stereo files stay one stereo track** — collapsed to a single strip with one stereo meter, persisted across reopen, and bounced/exported as one stereo file. Sources with more than two channels are imported as one mono track per source channel, preserving every channel. **Imports are converted to the session's sample rate** on the way in, so a 44.1 k file dropped into a 48 k session can't play back at the wrong speed
- **Post-show QC report** — one click scans every track for peak / integrated LUFS / clipping events (with timecode) / noise floor, pops a sortable table and writes a text report next to the exports
- **Verified show handoff** — File ▸ Export ▸ Create Verified Show Handoff copies the complete stopped session to a new/empty folder on a background worker, compares SHA-256 for every original and copied file except temporary files and the waveform cache, and adds a channel-map CSV, timeline CSV (tracks/markers/cues), and `Handoff Manifest.json`. An interrupted or failed copy retains `HANDOFF INCOMPLETE.txt`; never deliver that folder as verified. The manifest verifies the *transfer*, not that the original take or external backup drives passed show-readiness checks. It flags pending/missing/incomplete source capture hashes, skipped mirrors, and unreported audio for manual review, including legacy root-level takes.
- **Detect songs → markers** — multi-track quorum scan (crowd noise on ambient mics doesn't fool it) drops a named marker at every song start for instant next-day navigation
- **Console link (pluggable per console family)** — pick your desk in the connect dialog. Every supported family gets the two things that matter to a recorder, both read-only and safe on any desk: **channel names land on the strips** so takes arrive labelled, and **every scene / snapshot recall during a take drops a named marker** so the show is navigable the next morning. Outside a take a recall never leaves a marker. Turn on **Session ▸ Console ▸ follow scenes to markers** and a recall in virtual soundcheck instead moves the playhead to the marker of that name (the next one ahead when a scene appears twice), without starting or stopping the transport. Follow is off by default, and refuses under LOCK, during a punch and during a session operation. With an X32 / M32 connected, **Create Session from Connected Console** (in Control Surfaces) also links the desk's stereo pairs; other desks report no stereo data, so the session keeps the names and pairs are linked by hand. Supported: **Behringer X32 / Midas M32** (full control — repatch to the card returns for virtual soundcheck and back, with the show patch queried and stashed first, plus head-amp gain capture on show night and restore on VSC day), **Behringer WING**, **DiGiCo SD / Quantum**, **Yamaha CL / QL / RIVAGE / DM** (SCP over TCP), **Allen & Heath dLive / Avantis / SQ** (MIDI over TCP), and generic OSC. On the large-format desks the console's own Virtual Soundcheck does the repatch — ZynForge records and plays the card returns. **Anything that could change your desk is gated:** on connect the app probes the console, and until it answers the way the profile expects, the link stays strictly read-only and the repatch / gain items grey out. Only the X32 / M32 address model is trusted outright; the others are implemented from the published protocols and unlock once your desk confirms itself. The console address must be a numeric IP. A session keeps its own head-amp gains and saved patch. The show patch is saved with the session before the desk moves to card returns, so a crash or quit leaves a way back.

- **DAW handoff.** File ▸ Export ▸ Export Reaper Project writes a `.RPP`, and Export DAWproject writes a `.dawproject` for Cubase, Studio One and Bitwig, both into `Export Files/` beside the session and neither ever replacing an earlier export. Each carries tracks (a stereo pair is one stereo track), the active take's clips with fades, clip gain and mute, faders, pans, colours, markers and sample rate, at a default 120 BPM in 4/4. Audio is referenced in place, by a path relative to the project file, so keep the session folder together. A missing audio file stops the export and names the file. Solo is not carried: the summary after each export lists what was left out or approximated, and the same notes travel inside the project. DAWproject has no field for clip gain, fade shape or one channel of a stereo file; those are listed too. Show handoffs now include both project files at the top of the folder, reading the handoff's own audio.
- **Export Songs.** File ▸ Export ▸ Export Songs lists one range per marker, each running to the next marker and the last to the session end. Tick the songs, edit the names, and choose a stereo mix, multitrack stems or both, in any export format. `Stereo pair = one file` writes each pair as one interleaved stem; untick it for separate L and R files. Each song lands in `Export Files/Songs/NN - Name/`, cut sample-accurately, with stems showing the edited arrangement and the mix following the same rules as a whole-session bounce. While it runs, the same menu item reads Cancel Song Export; a cancelled file leaves nothing behind.

### Metering
- Adaptive LED meters per strip + master; sticky clip latch
- The meterbridge stays open when you click elsewhere, so it can live on a second display
- **Master loudness (ITU-R BS.1770)** — momentary and integrated (gated) LUFS + a 4×-oversampled true-peak (dBTP) on the post-fader master, shown on the status panel beside the clock in both views at every window size (true-peak goes red within 1 dB of full scale)

### Cues + setlist
- Drop cues at any transport position; each holds a snapshot of every strip's state
- Explicit **Recall** button alongside the dropdown / ◀ ▶ navigation (and the cue dropdown still recalls on pick)
- Stable strip UUIDs — reorder the mixer without breaking cue recall
- Soft-takeover ramps on recall — click-free state transitions
- LCD countdown to next cue, drag-reorder, Print setlist to PDF (via HTML)

### Console integration (OSC)
Five inbound dialects: Generic, DiGiCo, Allen & Heath (SQ / Avantis), SSL Live, and Yamaha (DM / RIVAGE). Console dialects accept the read-only values a recorder needs — channel names, gain/trim for Trim-Follow, scene recalls and markers — but cannot arm, mute, or drive transport. **Generic OSC control is authenticated:** the access token is shown in Control Surfaces ▸ Open Sound Control once the listener is running, with a Copy button. It changes each time the listener starts, and every state-changing Generic command must include it as its final string argument. A tokenless UDP packet cannot start a take. Channel indices are 1-based to match console numbering.

**Trim-Follow** moves a channel only by what the desk's preamp has changed since that channel was captured, and by no more than +12 / -30 dB. A channel captured with no desk reading has no reference: it stays at 0 dB offset, and the Session menu and status line say which channels those are.

**Desk macro control.** Session ▸ Console ▸ desk macro control lets a console's macro keys run the recorder. It is off by default and off again on every launch. It listens on its own UDP port (default 9301) and answers one desk IP address; datagrams from any other address are dropped and counted. It accepts `/zynforge/macro/record`, `stop`, `play`, `pause`, `marker`, `marker/next`, `marker/prev` and `marker/goto` (integer marker number), as an address alone or with an integer. Integer messages are the most reliable type on DiGiCo. Integer 0 is ignored, because a key can send it on release, and identical commands inside 250 ms count once. RECORD runs its full pre-flight and never punches, STOP keeps the two-tap guard, and LOCK refuses everything; nothing here can arm, mute or patch. A source address can be forged on a LAN, so use this only on a network you trust (`decisions.md`).

**Console log.** Tick "Debug: log OSC traffic" in a console's Control Surfaces settings, then Session ▸ Console ▸ Save Console Log writes everything both console paths sent and received: time, sender, address, argument types and values, with access tokens removed.

### Control surfaces (MIDI)
Bidirectional **Mackie Control / FaderPort (MCU)** surface: motor **faders ↔ channel gain**, **mute / solo / arm** with LED feedback, **V-pots → pan** with ring feedback, **scribble strips** show names, **bank / channel** buttons page through all tracks, and **meters** mirror to the surface. Plus a **master (9th) fader** for the monitor level, **a jog wheel that moves the playhead**, and the surface's **7-segment time display** showing the playhead as `HH:MM:SS:FF`. Banking clears the old bank's lights and names. Channel state is applied straight off the MIDI thread (atomic); transport is marshalled to the message thread.

### Twin Recorder
**Twin Recorder (opt-in).** Two Macs that receive the same console feed can record together. Each keeps its own interface, disks and session; the leader's RECORD and confirmed STOP also start and stop the follower, a marker dropped on the leader lands at the follower's own capture position, and each Mac shows the other's health. If the network drops, both takes keep rolling and the header shows TWIN LOST. After a take, both session reports record the link's history and the leader shows a comparison of the two takes. This protects against a computer, drive or interface failure. Both Macs run the same app, so it does not protect against an app bug: it is not an independent recorder. Pair with the code shown on the follower, on a private show network: the link is authenticated, not encrypted. Open it from Session ▸ Network & Surfaces ▸ Twin Recorder.

### Show-day reliability
- A console dropping its network link — desk power-cycle, switch reboot — is handled as an ordinary disconnect. It used to abort the app on the next connect or quit
- **Measured pre-flight check** — one menu action before doors: device / SR / clock config, **measured** disk write speed vs. what the armed channel count demands, free-space headroom, every mirror drive verified mounted *and writable*, live CPU callback load, session-vs-device sample-rate mismatch, and signal presence on each armed input. The write-speed probe is skipped while a take is rolling (it would write 16 MB to the take's own volume); the SMART drive-health poll runs off the UI thread so a slow or unresponsive drive can't stall the app mid-show
- **LOCK** button disables every other control so a stray click can't kill a take. LOCK also covers right-click menus on strips and the setlist bar, the patch matrix and EDIT rows, and survives a strip rebuild. Cmd+Q and the window's close box refuse under LOCK
- Anything that threatens a take shows in a red banner under the transport, large enough to read from the mix position, and stays up until it clears. A refused RECORD uses the same banner and clears itself
- **Device settings lock while a take is rolling** — the DEVICE panel is a floating window you can leave open, and changing the interface, sample rate or buffer size restarts the audio device, which ends the recording. Once RECORD is pressed those combos grey out and say why, and closing the panel won't revert anything either. Nothing you can click in it can stop a take
- **Backup and mirror destinations are validated, not trusted** — File ▸ Backup & Mirror Drives, or the BACKUP button, is one dialog: backup in the first row with its own format, then the mirrors. A mirror writes the same folder layout the session itself uses, so a root pointing at (or inside) your sessions folder would have landed on the take's own files. The picker refuses a root that holds the session, sits inside it, duplicates another mirror, or is already the backup destination, and says which row is the problem. The same check runs again at record start, because a mirror you configured last week is still there when you open a session somewhere new. If a mirror can't be written at all — drive not mounted — the recording banner tells you *during* the take that you have fewer copies than you configured
- Redundant-write to a second drive in parallel, plus N-way mirrors, each in its own format — all of which count toward the "minutes remaining" estimate and the DISK STRUGGLING warning. The free-space readout tracks the volume the take actually lands on, including a custom Local Storage location
- Recording always **pre-fader** — fader / pan / mute / solo are monitoring concerns only
- **Auto-save + backup session** — Session ▸ *Auto-Save & Backup…* (Off / 1 / 2 / 5 / 10 / 15 min) periodically saves the session and drops a complete, restorable **backup session** (all session-defining files, not the multi-GB audio) into `Session File Backups/<Name>_<stamp>/`, keeping the 10 newest. Auto-save pauses during a take. These metadata snapshots exclude audio and cannot restore audio overwritten by a punch; keep independent media backups
- Auto-save only marks a session clean after every metadata file and the backup snapshot succeeds. Failure is shown immediately and retried after 15 seconds, then at growing spaces up to the normal interval. If the session's drive disappears, Auto-Save says so, and Save, Save As and Import wait for it
- Device settings and backup and mirror drives are locked while a take is rolling — each one would otherwise change something mid-show, and each says so rather than failing quietly
- **RF64** large takes (one continuous file past 4 GiB) + a fast, hardware-accelerated SHA-256 integrity manifest. A historical 6 h+ soak had no crash or memory growth; the latest build still needs its own real-device acceptance. After hashing finishes, run `tools/verify_take.sh` for primary-file integrity, then check routing and backup copies independently

### Workflow polish
- Every text / number prompt (rename track, marker name, cue name, clip gain, +CH, New Session, …) opens with its field focused + text selected; **Enter** confirms the primary action without reaching for the mouse
- Waveform cache builds in the background on import / record and is flushed to `WaveCache.wfm` as soon as the post-STOP scan finishes; a cache older than its audio is discarded on reopen

### Session integrity
- New, Open, New from CSV, New from Console, Finder `.zfproj` opens, and template-based session creation all offer **Save & Continue / Continue Without Saving / Cancel** before replacing the current session. A failed save cancels the switch, close, or quit instead of discarding the current state
- **Save As is a clone, not a merge:** it requires a new or empty destination, saves the source first, copies on a cancellable worker, and switches to the clone only after a complete copy. A failed/cancelled copy leaves the source session active and removes a newly-created partial destination
- Moving a session is similarly transactional. Recording, playback start, strip changes, bounces, and competing session operations are gated while a move/copy/bounce owns the session; a cross-volume move keeps the complete destination authoritative and warns if the old copy could not be removed
- Opening a session clears all previous session state, loads audio and the exact mixer track count (including shrinking), then restores playlists after default clips are seeded. A malformed or missing `session_mix.json`, or one listing zero strips over recorded audio, falls back to the audio count instead of leaking the previous session's strips
- A session's mixer, cues and settings are only ever saved into the session they were loaded from. If a session could not be opened, or has not been opened yet, Auto-Save skips it and says so in the status line
- A session with saved channels keeps at least one strip. To start over, use File ▸ New Session
- A damaged `session_mix.json`, `session_settings.json`, `markers.json` or `.zfproj` is kept under a dated name, you are told, and the newest good backup is offered
- Opening the session that is already open does nothing and says so, so it cannot lose unsaved changes
- Save As, Move and Show Handoff copy the session's own files. A half-written temporary file is skipped, and a waveform cache that can't be read is left out, which costs only a rescan. Any other file that can't be copied stops the copy and says so
- A session from an earlier build that used features this version does not have opens, plays and saves. Before anything can overwrite them, its original documents are copied once to `Session File Backups/Before Feature Removal/`, and the status line says so. Audio is never touched. A session with several takes plays its active take and saves them all
- Save As, Move and Show Handoff work on a read-only session after asking. Save As and Move keep unsaved changes in the new copy; a handoff does not, because it is verified against its source
- Remove Last Capture removes the session you last recorded, names it, and moves it to the Trash. If the Trash refuses, nothing is removed
- `session_mix.json` also owns output-mute and stream-send state; both reset on session replacement and participate in undo
- `.zfproj` files are real macOS documents: double-clicking one or sending it to an already-running app opens its containing session through the same guarded path
- A configured default template is applied to every new session, including the Welcome flow. Choosing **New Session from Template** creates a new session; it never destructively applies a template over the open show

## Visual identity

Design values are sourced from the **FORGE family design system** (`../ZynForgeBrand/tokens.json` → generated, vendored `Source/Theme/ForgeTokens.h`) — one token edit retunes the whole ZynForge family. See `ZynForgeBrand/FORGE.md`.

Part of the ZynForge family — shares palette + fader/meter style with ZynForge Live (sibling project). **Flat design: every surface is a solid fill (no gradients or glossy sheens).** Near-black canvas, neutral-grey strips by default (recoloured per channel from a hue/shade picker), LED-segment meters, and **bright orange reserved for STATE** (armed / peaking), never permanent chrome.

Native macOS sans-serif for UI text + bundled `JetBrains Mono` for tabular numerals. (The former files named as Inter were HTML error pages, not fonts, and have been removed.) A named type scale (UI ladder + mono `monoStamp/monoCounter/readout/…`), a 14-step `alpha::` scale, `space::`/`radius::`/`motion::`/`shadow::` tokens. All chrome routes through `Source/Theme/` tokens — **never raw hex, raw fonts, raw gradients, or ad-hoc opacities** — enforced by `tools/design_audit.sh` (run as a pre-commit gate; raw colours/fonts/alphas fail, and a spacing ratchet stops raw `reduced()` px from growing). A second pre-commit gate, `tools/invariants_audit.sh`, does the same for **correctness**: 34 rules encoding the bug classes that repeat audits kept finding re-violated in new places (freed-`TrackState` reads, `.wav`-only take globs, unguarded device restarts mid-take, …). Full rationale in [`design.md`](design.md).

## Where things live

```
Source/
├── Main.cpp           — JUCE app entry
├── Audio/             — real-time + persistence (AudioEngine, MultitrackRecorder, SessionPlayer, …)
├── UI/                — message-thread paint code (MainComponent, dialogs, strips, …)
├── Theme/             — design system (BrandColors, BrandTokens, DialogChrome, LookAndFeel)
└── Network/           — RemoteControl, ConsoleLink, TwinLink, CaptureLink, DeskMacro
```

Sessions land in `~/Music/Zynforge Sessions/<SessionName>/` with subfolders (`Audio Files/`, `Export Files/`, `Session File Backups/`) and per-session state files at the root: **`<Name>.zfproj`** (cues, playlists, UI layout), **`session_mix.json`** (per-strip mix), **`markers.json`** (markers), **`session.report.json`** (sha256 + counts on clean stop), and **`WaveCache.wfm`** (versioned thumbnail cache). The `.zfproj` document carries the ZynForge icon in Finder and opens the session when double-clicked. See [`architecture.md`](architecture.md) §6 for the full data flow.

## Sibling project

[ZynForge Live](https://github.com/jeanpierreboutros-lang) — JUCE plugin-insert host. Shares the visual identity (`Source/Theme/`) but not code.

## License

Copyright Jean-Pierre Boutros, all rights reserved. See [LICENSE](LICENSE). The third-party components in the shipped app are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
