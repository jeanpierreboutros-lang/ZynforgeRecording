# ZynForge Recording — Field-Test Checklist (2026-05-24 build)

Run this with a real audio interface plugged in. 20 minutes if nothing breaks. Tick boxes as you go; the failure column tells you whether to stop and report or keep going.

**Severity legend:** 🟥 = data-loss class (stop, report immediately) · 🟧 = audio-path class (stop if reproducible) · 🟨 = UX / stage-readiness · ⬜ = cosmetic.

---

## 1. First-launch sequencing 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 1.1 | Quit the app cleanly. Re-launch. | Welcome dialog appears alone. | Multiple modals stack on top of each other. |
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
| ☐ 6.1 | Tools → Start companion. | Status bar: "Companion on (loopback-only) — URL copied to clipboard." | LAN address shown by default. |
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
| ☐ 7.4 | Open every dialog: New Session, Add Tracks, Session Format & Recording, Session Info & Notes, Click Settings, Audio Device, Markers list, Noise Report (after Tools → Noise scan), Session Recovery. | All have the same orange-stripe DialogChrome top + gradient body + footer divider. | Any dialog looks "raw JUCE" = chrome regression. |
| ☐ 7.5 | Look at any saturated chip (VCA badge, BUS badge, TAKE chip, PatchPage strip number, active routing dot, active EditTools icon). | Text on coloured background reads cleanly (dark on bright, light on dark — `onSignal` does the right thing). | White-on-yellow or similar low-contrast pairs. |

---

## 8. Stretch — only if everything above passed 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 8.1 | Connect a console via OSC. Move a fader on the console. | Strip in app responds. | No reaction. |
| ☐ 8.2 | Enable MIDI clock out, pick a device. | Status pill in row 1: "MIDI ★ <device name>". External gear locks tempo. | Pill says enabled but external doesn't sync. |
| ☐ 8.3 | Cue a setlist of 5+ cues. Press next-cue 5 times during playback. | Soft-takeover fade between each, no clicks. | Hard cuts = ramp engine broken. |

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
   - ≈ 8.3 h mono @ 24-bit/48k, or ~16 min × 32ch.
   - Verify: (a) one continuous `Track_01.wav` (no `_part02`); (b) it opens
     full-length in Pro Tools / Reaper / Logic; (c) `xxd -l 16 Track_01.wav`
     shows `RF64` + a `ds64` chunk; (d) hard-kill mid-take (Activity Monitor →
     Force Quit) leaves a file that still opens to ~the last 5 s flush.
2. **High channel count at high rate, under real disk load.** 96–128 ch @
   96k to a single drive (+ backup), full set length. Watch
   `session.report.json` → `missedSamples: 0`, all files identical length,
   and the live disk-health flag never trips. Use the external monitor
   (`/tmp/zynforge_*.sh` pattern) to confirm overloads = 0, spread = 0.

### Turnkey verification — `tools/verify_take.sh`

The manual half of checks (1a–1c) and (2) is automated. After stopping the
take, run:

```bash
tools/verify_take.sh                       # newest session under ~/Music/Zynforge Sessions
tools/verify_take.sh "/path/to/Session"    # or a specific session folder
```

It checks every recorded WAV in one pass and **exits non-zero on any
problem**:

- no `Track_NN_partNN.wav` split files exist (RF64 = one continuous file);
- each WAV opens at full length (`ffprobe` duration + frame count);
- header is `RIFF` (<4 GiB) or `RF64` + `ds64` (>4 GiB) — and it **flags any
  file that crossed 4 GiB without RF64 promotion** (the exact failure mode);
- `session.report.json` exists, `missedSamples: 0`, track count matches;
- every file's on-disk sha256 matches the report's manifest (once the report
  flips `sha256Pending:false` — re-run if it's still hashing).

Still do by hand: (1b) open in another DAW, and (1d) the hard-kill-mid-take
crash-safety check — then run `verify_take.sh` on the survivor. Requires
`ffprobe`, `xxd`, `shasum`, `jq`, `python3` (stock on a dev Mac).

## Control Surfaces — bench verification (hardware-only)

The protocol logic is unit-tested; these confirm it against real gear. Open
**Session ▸ Control Surfaces** for all of it.

### OSC console (DiGiCo / SSL Live / Yamaha / Allen & Heath)
1. Tick the console -> it goes live (receiver on the listen port). Open its
   settings; note the `osc.udp://<this-mac>:<port>/` connection string.
2. On the **console**, point its OSC target at this Mac's IP + that port (UDP).
3. Turn on **"Debug: log OSC traffic"**, open **"View OSC Log..."**.
4. From the desk: move a channel mute / change a name / recall a scene / hit
   transport. Confirm in the log you see `<- /Console/... ` (or `/sq/`,
   `/sslnet/`, `/Yamaha/`) lines, and the app reacts (mute toggles, marker
   drops, transport follows).
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
