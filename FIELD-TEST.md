# ZynForge Recording — Field-Test Checklist (updated 2026-09-20)

Run this with a real audio interface and disposable sessions. Basic smoke checks are separate from the multi-hour soak. Tick boxes only after performing them; the failure column tells you when to stop and report. Current installed build: application code `df5ad36`, 350 automated test groups passing on both slices of the macOS-12 universal Release and in ASan+UBSan Debug, no app-owned Xcode-analysis diagnostics, and green GitHub Debug/Release CI. The planned SD5 / 56-input / 48 kHz / two-hour show's three-hour acceptance test is defined in [SHOW-READINESS.md](SHOW-READINESS.md) and has not yet run.

Never force-quit, unplug hardware or delete sessions during production recording. Crash tests require a disposable rig/session and a recovery plan. In daemon mode, killing only the GUI is a reattachment test; it does not necessarily stop the recording or create an orphan. Stop both processes gracefully before installing or rolling back.

**Severity legend:** 🟥 = data-loss class (stop, report immediately) · 🟧 = audio-path class (stop if reproducible) · 🟨 = UX / stage-readiness · ⬜ = cosmetic.

---

## 1. First-launch sequencing 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 1.1 | Quit the app cleanly. Re-launch. | Last valid session auto-reopens; with no remembered session, Welcome appears alone. | Empty grey app, wrong session state, or stacked modals. |
| ☐ 1.2 | Force-quit mid-recording (Cmd+Opt+Esc), re-launch. | Session Recovery dialog appears first, table lists the orphan with track count + size + modified date. | Orphan missing from table (used to silently skip user-named sessions). |
| ☐ 1.3 | Select the orphan, click Recover. | Session loads. Marker bar shows any pre-crash markers. | "Recovered..." status but session doesn't open. |
| ☐ 1.4 | Re-trigger orphan, click Delete, confirm. | Confirm dialog with full path. Row vanishes from table. | Dialog closes without deleting OR deletes without confirm. |

---

## 2. Recording + takes (data-loss class) 🟥

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 2.1 | New Session named with a space and a special char (e.g. `Show — 2026/05/24`). | Folder created with safe-name (slashes replaced). `.zfproj` file inside. | Folder creation silently fails OR creates with literal slash. |
| ☐ 2.2 | Add 4 channels, arm 2, record 30 s of real audio. | RECORD shape-distinct from PLAY, BigClock counting up, meters moving. | Meters frozen, no file growing. |
| ☐ 2.3 | Stop. Open the session folder. | `Track_NN.wav` files present, each non-zero. `recording.session` marker gone. | Marker still there = recover-on-next-launch loop. |
| ☐ 2.4 | Right-click a track → New take from current. Record more audio. | Take 2 entries replace clips. TAKE chip on EDIT row shows `TAKE 2 / 2`. | Take chip doesn't update. |
| ☐ 2.5 | Switch back to Take 1 via right-click. Quit. Re-launch + open session. | Take 1 active on relaunch (round-trips through .zfproj). | Take 2 reactivates. |
| ☐ 2.6 | Set capture format to FLAC/24 via Format & Recording dialog. Record. | New files are `.flac`, smaller than equivalent WAV. | Format ignored OR app crashes on switch. |

---

## 3. Audio path 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 3.1 | Play back a recorded session. Listen at moderate level. | Clean playback, no clicks, no dropouts. PerfDashboard CPU < 5 %. | Any audible click, glitch, or dropout. |
| ☐ 3.2 | While playing, move a fader. | Audio level follows instantly, no zipper noise. | Stepped / zippered audio. |
| ☐ 3.3 | While playing, drag the EDIT view tension handle on a Volume segment. | Curve bends visually; you hear the bend on the next playback pass. | Audible click during the drag itself. |
| ☐ 3.4 | While playing, press M to toggle a strip's mute. | Smooth fade to silence (not a hard cut). | Hard mute cut = pop. |
| ☐ 3.5 | Stop playback. Switch sample rate via Format & Recording dialog. | Warning dialog appears if SR mismatches the loaded session. | Silent switch with no warning. |

---

## 4. Automation surface (phases 5 + 6) 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 4.1 | Switch toolbar Write dropdown to Touch. Start playback. Move a fader. | Points drop on the lane at ~50 ms intervals (thinned). | Either no points OR fan-out cloud of points per fader event. |
| ☐ 4.2 | Switch Write to Off. Move fader during playback. | No points drop. Existing lane still plays back. | New points still appearing. |
| ☐ 4.3 | Toggle SUSPEND on. Play. | Engine ignores every stored lane; fader sits at its current value. | Lane still drives the fader visually. |
| ☐ 4.4 | Shift-drag on the EDIT time ruler to define a range. | Translucent green band appears spanning the drag. | No band, OR band painted at full alpha. |
| ☐ 4.5 | Toggle PUNCH on. | Band brightens (dim → bright). | No visible state change. |
| ☐ 4.6 | With Write=Touch and PUNCH on, play through the range. Move fader. | Points only drop inside the band. | Points outside the band. |
| ☐ 4.7 | Right-click an automation point → curve picker. Pick Linear. | Tension resets to 0 (segment straightens). | Bend persists despite "Linear" pick. |
| ☐ 4.8 | Drag a tension handle, hold Shift. | Snaps to 0 / ±0.25 / ±0.5 / ±0.75 / ±1. | Free-drag during shift. |
| ☐ 4.9 | Right-click strip header → "Automation Safe — OFF" → toggle on. | Strip's R/W LED turns amber. Try to drop a point — should be blocked. | Point still drops. |
| ☐ 4.10 | Cmd+Z three times after the above. Cmd+Y / Cmd+R to redo. | Each tension drag = one undo step, not N. | Multi-step undo on a single drag. |
| ☐ 4.11 | Save, quit, re-open session. | All automation including tension AND Safe flag round-trips. | Bend or Safe flag lost. |

---

## 5. Stage-readiness shortcuts 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 5.1 | Press M at the playhead. | Marker dropped, naming dialog with text pre-selected. | Dialog appears but text not selected = mistype. |
| ☐ 5.2 | Drop 3 markers. Press Cmd+1, Cmd+2, Cmd+3. | Playhead jumps to each marker. Status bar names it. | Bare digit jumps instead = old behaviour. |
| ☐ 5.3 | Press Cmd+5 with no marker there. | Status: "No marker 5 — drop one with M first." | Silent no-op. |
| ☐ 5.4 | Save a few cues. Press 1, 2, 3 (bare digits). | Playhead jumps to each cue. | Marker jumps instead = ambiguity restored. |
| ☐ 5.5 | Press Space during recording. | First press surfaces "Tap STOP again to end" toast. Second press within 2 s actually stops. | Single press stops = no two-tap guard. |
| ☐ 5.6 | Press Esc after multi-selecting strips. | Selection clears. | Stays selected. |

---

## 6. Companion server (security) 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 6.1 | Session → Start companion server. | Status bar: "Companion on (loopback-only) — URL copied to clipboard." | LAN address shown by default. |
| ☐ 6.2 | Paste the URL into a browser tab on the SAME machine. | Companion UI loads. | 401 / connection refused. |
| ☐ 6.3 | Try the same URL from your phone (same Wi-Fi). | Connection refused (loopback only). | Phone connects = LAN exposure regression. |
| ☐ 6.4 | Visit `http://localhost:9000/state.json` WITHOUT `?t=<token>`. | 401 Unauthorized, plain-text body. | 200 OK = auth bypass. |

---

## 7. Visual / design polish ⬜

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 7.1 | Hover over a channel strip. | ~6 % brightness lift on the strip wash + brighter edge. | No hover state. |
| ☐ 7.2 | At XS strip width (24 per page), look at the REC/MON/MUTE/SOLO buttons. | Stacked vertically as a 1×4 column. | Cramped 2×2 grid = touch targets too small. |
| ☐ 7.3 | Clip a meter (drive a hot input). | Top PEAK tally bar pulses brand-red. Click clears it. | Tally doesn't latch / doesn't clear on click. |
| ☐ 7.4 | Open every dialog: New Session, Add Tracks, Session Format & Recording, Session Info & Notes, Click Settings, Audio Device, Markers list, Noise Report (after Tools → Noise scan), Session Recovery. | All have the same orange-stripe DialogChrome top + flat solid body + footer divider. | Any dialog looks "raw JUCE" = chrome regression. |
| ☐ 7.5 | Look at any saturated chip (VCA badge, BUS badge, TAKE chip, PatchPage strip number, active routing dot, active EditTools icon). | Text on coloured background reads cleanly (dark on bright, light on dark — `onSignal` does the right thing). | White-on-yellow or similar low-contrast pairs. |

---

## 8. Stretch — only if everything above passed 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 8.1 | Connect a console dialect via OSC. Change a channel name and recall a scene. | Strip name updates and a named marker drops; transport/arm/mute remain unchanged. | Missing metadata/marker, or unauthenticated state change. |
| ☐ 8.2 | Enable MIDI clock out, pick a device. | Status pill in row 1: "MIDI ★ <device name>". External gear locks tempo. | Pill says enabled but external doesn't sync. |
| ☐ 8.3 | Cue a setlist of 5+ cues. Press next-cue 5 times during playback. | Soft-takeover fade between each, no clicks. | Hard cuts = ramp engine broken. |

---

## 9. Session integrity + authenticated remote control 🟥 / 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 9.1 | Make unsaved mixer + clip + cue changes, then invoke New, Open, CSV import-as-session, Create from Console, New from Template, and double-click another `.zfproj`. | Each replacement offers Save & Continue / Continue Without Saving / Cancel. Cancel preserves everything; a deliberately unwritable save cancels the replacement. | Current session disappears, or switch continues after save failure. |
| ☐ 9.2 | Save As to an existing non-empty session folder, then to a new folder on another volume. | Non-empty target is refused. New target copies in the background; source remains active until complete; clone opens with audio, mix, clips, cues and layout intact. | Folders merge, UI freezes, destination opens early, or state is missing. |
| ☐ 9.3 | Start a large cross-volume Save As, then quit once and allow cancellation. | App reports cancellation and stays open until the owned worker finishes; source is unchanged and a newly-created partial target is removed. | Quit UAF/crash, partial clone remains, or source becomes the target. |
| ☐ 9.4 | Relocate a session across volumes; separately simulate inability to remove the old folder after a complete copy. | New folder opens and is authoritative. Cleanup failure explicitly names the old folder instead of reporting total move failure. | Active path still points old, or successful copy is discarded/misreported. |
| ☐ 9.5 | During Save As, relocation, stem bounce and stereo bounce, try Record, Play, +CH, delete/reorder, and a second file operation. | Every competing mutation refuses with a wait/busy message; the running job finishes or cancels cleanly. | Structure changes under the worker, crash, corrupt/short output. |
| ☐ 9.6 | Save a default template. Create once from Welcome and once with File ▸ New Session; also choose New Session from Template while a show is open. | Default layout appears in both new sessions. Explicit template asks about the current show and creates a different session; it never overwrites the show. | Empty default session or template destructively changes current session. |
| ☐ 9.7 | Open a 24-strip session, then a 4-strip session; repeat with missing and malformed `session_mix.json`. | Valid mix shrinks exactly to 4. Invalid/missing mix falls back to the audio count. No names/stereo/tempo/cues from the first session survive. | Extra ghost strips or previous-show state leaks. |
| ☐ 9.8 | Build a continued take through `_part10`, `_part11`, and `_part100`; play/export/bounce/QC it. Then temporarily remove the last part. | Complete take is numerically ordered and sample-complete. Missing trailing part refuses instead of producing a plausible shorter result. | `part100` plays before `part11`, or truncated output is accepted. |
| ☐ 9.9 | Start Generic OSC and note the shown token. Send a state-changing command without it, with a wrong token, then with the token as the final string argument. | First two do nothing; authenticated command works. Console-specific dialect messages may update names/markers/trim but cannot arm/mute/transport. | Tokenless UDP changes the session or console dialect starts recording. |
| ☐ 9.10 | Companion: issue Record with no armed live input, then with a valid armed input. | First request returns a truthful conflict/error and does not roll; second returns success only after recording really starts. | `{ok:true}` with no take, or remote starts an unusable zero-input take. |
| ☐ 9.11 | Quit with an active take using Stop & Quit; separately make the session destination unwritable before Save & Quit. | Stop & Quit stops and saves. Save failure cancels quit and leaves the app/session available. | App quits after failed save. |

---

## What to send back

For any 🟥 or 🟧 failure:

1. The row number(s) that failed.
2. What you saw vs what was expected.
3. The Console.app crash report if one fired (`~/Library/Logs/DiagnosticReports/Zynforge*.ips`).
4. The session folder if data-loss class.

For 🟨 / ⬜: a list is fine; we'll batch them in the next session.

## Throughput + RF64 — field soak (hardware-only, not unit-testable)

Headless tests now cover write integrity at 64 channels (0 missed samples,
full-length files) and the RF64 split policy. Two things still need a real
rig + real time:

1. **>4 GiB single take (RF64).** Record one mono WAV past 4 GiB:
   - Approximately 8.3 h for one mono file at 24-bit/48 kHz, or 4.1 h for one stereo file. Adding separate mono tracks does not shorten this per-file threshold.
   - Verify: (a) one continuous `Track_01.wav` (no `_part02`); (b) it opens
     full-length in Pro Tools / Reaper / Logic; (c) `xxd -l 16 Track_01.wav`
     shows `RF64` + a `ds64` chunk; (d) hard-kill mid-take (Activity Monitor →
     Force Quit) leaves a file that still opens to ~the last 5 s flush.
2. **High channel count at high rate, under real disk load.** 96–128 ch @
   96k to a single drive (+ backup), full set length. Watch
   `session.report.json` → `missedSamples: 0`, all files identical length,
   and the live disk-health flag never trips. Record the monitoring method
   and confirm no overloads and equal expected durations; do not depend on untracked `/tmp` scripts.

### Turnkey verification — `tools/verify_take.sh`

The helper provides partial WAV diagnostics, not complete acceptance. It assumes a single uninterrupted take and currently rejects intentional continuation parts. It can exit 0 while hashing is pending and does not prove channel mapping or equal expected durations. See [helper limitations](SHOW-READINESS.md#verification-helper-limitations). After stopping a disposable single-take session, run:

```bash
tools/verify_take.sh                       # newest session under ~/Music/Zynforge Sessions
tools/verify_take.sh "/path/to/Session"    # or a specific session folder
```

Its implemented checks include:

- no `Track_NN_partNN.wav` split files exist (RF64 = one continuous file);
- each WAV opens at full length (`ffprobe` duration + frame count);
- header is `RIFF` (<4 GiB) or `RF64` + `ds64` (>4 GiB) — and it **flags any
  file that crossed 4 GiB without RF64 promotion** (the exact failure mode);
- `session.report.json` exists and reports `missedSamples: 0`; the displayed track count still needs manual reconciliation;
- every file's on-disk sha256 matches the report's manifest (once the report
  flips `sha256Pending:false` — re-run if it's still hashing).

Still do by hand: (1b) open in another DAW, and (1d) the hard-kill-mid-take
crash-safety check on disposable data. A survivor without a clean-stop report will not pass the helper even when audio is recoverable. Requires `ffprobe`, `xxd`, `shasum`, `jq`, `python3`; these are not all stock macOS tools. Hash-pending output is incomplete verification, even if the exit code is 0.

## September regression acceptance

- [ ] With primary/backup roots aliased or overlapping, recording refuses before primary files are touched.
- [ ] During local and daemon takes, arm/input/session replacement attempts leave capture unchanged; STOP completes only after acknowledgement.
- [ ] Two daemon takes preserve the first audio and create correctly routed continuation parts; status/time and session path agree after GUI reattachment.
- [ ] Move/delete mixed mono/stereo tracks; reopen and verify media, UUID-linked cues, sends, clips/takes and automation. Removed audio remains archived, and old undo/clipboard state is cleared.
- [ ] Import into an edited session; verify splits/fades/comps and intentionally empty tracks survive. Paste a stereo source channel to an unrecorded track beyond the original duration; verify playback/export and missing-source silence.
- [ ] Check locked split/ripple/crop behavior, earlier inactive-take deletion, empty automation restore, sorted marker naming and analysis menu commands.
- [ ] Reopen capture settings/sample rate and check recovery file counts. Exercise loop boundaries for audible gaps.
- [ ] Follow the exact-rig rehearsal and backup checks in SHOW-READINESS.md. Record results; no checkbox is pre-passed by the automated suite.

### 2026-09-20 reliability follow-up

- [ ] Put an existing `Track_01.wav`, `.flac`, `.aif`, or `.aiff` in a disposable session whose project metadata cannot be read. Press RECORD as a fresh take: it must refuse without changing the file. Explicit Continue must create a continuation instead.
- [ ] Configure a missing/unwritable backup and one valid mirror. RECORD must either refuse an unsafe primary or roll with the valid primary while showing the unavailable copy. The warning must remain visible after STOP; the selected backup path must survive relaunch.
- [ ] During a disposable take, disconnect a redundant drive. Primary capture must continue, the runtime failure must latch, and STOP/report must not claim clean redundancy. Reconnect and confirm the next take resets health only after writers open.
- [ ] Put primary, backup, and mirrors on combinations of the same and separate physical volumes. Confirm minutes remaining reflects aggregate byte rate per volume and does not count failed/skipped copies.
- [ ] Locally arm StereoMix, assign stream sends, leave physical stream outputs unassigned, and capture known signal. The StereoMix file must be audible. Remove all stream sends and confirm RECORD refuses the empty mix. Daemon mode must refuse StereoMix with a clear explanation.
- [ ] While daemon recording is active, try Play from the toolbar, keyboard, MCU, OSC, Companion, and timecode chase. Every path must leave playback stopped. Strip Silence, Normalize, Consolidate, and transient rebuild/navigation must also refuse.
- [ ] Force a daemon configuration error, correct it, then record. The callback must be restored and the valid configuration must capture. STOP must distinguish a completed take with a finalization warning from a take that may still be rolling.
- [ ] On an edited stereo/multipart session with fades, gain, and cross-track clips, run Strip Silence and compare against the audible arrangement. Locked clips must remain unchanged; a fully silent unlocked track must become empty. The UI must remain responsive.
- [ ] Run Normalize, Strip Silence, transient detection, and Consolidate on long material, then change session/source state before completion. Stale results must not apply. Pre-create `_999`; Consolidate must create `_1000` or later without replacement.
- [ ] Change output mute and stream sends, Save/reopen, undo/redo, and then create/open another session. State must round-trip in the original and reset in the replacement.
- [ ] Make one autosave metadata/snapshot destination fail. Confirm an immediate warning, no false clean marker, and a retry after about 15 seconds. A failed snapshot must not leave a partial backup folder.

## Control Surfaces — bench verification (hardware-only)

The protocol logic is unit-tested; these confirm it against real gear. Open
**Session ▸ Control Surfaces** for all of it.

### OSC console (DiGiCo / SSL Live / Yamaha / Allen & Heath)
1. Tick the console -> it goes live (receiver on the listen port). Open its
   settings; note the `osc.udp://<this-mac>:<port>/` connection string.
2. On the **console**, point its OSC target at this Mac's IP + that port (UDP).
3. Turn on **"Debug: log OSC traffic"**, open **"View OSC Log..."**.
4. From the desk: change a channel name / gain trim and recall a scene. Confirm
   in the log you see `<- /Console/... ` (or `/sq/`, `/sslnet/`, `/Yamaha/`),
   names/Trim-Follow update, and a marker drops. Console dialect input is
   intentionally read-only for arm/mute/transport; those messages must not
   change the session.
5. **Bidirectional:** set the **Console IP + Receive Port**, hit **"Request ALL
   channel names"**. Watch the log for the `-> request...` line, then for
   incoming `<- .../name "..."` replies, and the channel names populating.
   *If no replies arrive, the request address is wrong for this model* — note
   what the desk DOES send (some push names on connect) and adjust the request
   string in `AudioEngine::requestConsoleChannelNames`.
6. **Create session from console** -> a session sized + named from the desk.

### MIDI control surface (Mackie Control / FaderPort in MCU mode)
1. In the surface's settings pick **MIDI In/Out**, then tick it on.
2. Move a surface **fader** -> the matching channel gain moves; move the app
   fader -> the motor fader tracks back.
3. **Mute / Solo / Arm** buttons toggle the channel + light the surface LED.
4. **V-pot** turn -> pan; the LED ring shows pan position.
5. **Meters** on the surface follow the channel levels; **scribble strips**
   show channel names.
6. **Bank** / **Channel** buttons move the 8-fader window across all channels
   (faders/names/meters re-populate for the new bank).
7. **Transport** (Play/Stop) on the surface drives + reflects the app.
