# ZynForge Recording — Field-Test Checklist (updated 2026-11-03)

Run this with a real audio interface and disposable sessions. Basic smoke checks are separate from the multi-hour soak, which ends only when someone presses STOP twice: nothing stops it unattended. Tick boxes only after performing them; the failure column tells you when to stop and report. The development Mac has `9a40f54`, the Mac mini and the current DMG have `281c6c0`, all protocol 4, and `main` (now on JUCE 8.0.17-security) is ahead of both: unless a section says otherwise, a check describes `main`. Automated results are in [testing.md](testing.md). Physical capture is not yet accepted. The [installation record](INSTALL.md) contains the current DMG and hashes. The planned SD5 / 56-input / 48 kHz / two-hour show's three-hour acceptance test is defined in [SHOW-READINESS.md](SHOW-READINESS.md) and has not yet run on this build. See the [55-channel long-take evidence](docs/history/FIELD-TEST-2026-10-01.md).

Never force-quit, unplug hardware or delete sessions during production recording. Crash tests require a disposable rig/session and a recovery plan. In daemon mode, killing only the GUI is a reattachment test; it does not necessarily stop the recording or create an orphan. Stop both processes gracefully before installing or rolling back.

**Severity legend:** 🟥 = data-loss class (stop, report immediately) · 🟧 = audio-path class (stop if reproducible) · 🟨 = UX / stage-readiness · ⬜ = cosmetic.

---

## JUCE 8.0.17-security — not yet performed

`main` moved from JUCE 8.0.4 to 8.0.17-security. The development Mac has it, now as `eca170e`; the mini does not. The automated suite passes on it, but the framework sits under the device, files, text and network, so these checks are on the real rig. Compare against `9a40f54` on the same Mac where a step says so.

- [ ] 🟨 **Text.** Open MIXER, EDIT and PATCH at the smallest window and full screen, then the alarm banner with three alarms, the EDIT ruler at deep zoom, DEVICE and Export Audio. No label is cut off or overlaps where it didn't on `9a40f54`: text is now measured with fallback fonts and shaping.
- [ ] 🟧 **Device.** Launch with the interface chosen. It opens at the same rate and buffer as before and the rate does not change at launch. Record 10 minutes on every input, STOP, and run `tools/verify_take.sh`.
- [ ] 🟥 **Older sessions.** Open a session recorded on `281c6c0` or `9a40f54`, with WAV, AIFF and FLAC takes. All play from start to end, and one export matches the same export from the older build.
- [ ] 🟧 **Files from elsewhere.** Import a WAV and an AIFF written by other software, one of them carrying markers or loop metadata. Both import and play.
- [ ] 🟧 **Desk and remotes.** On an X32 / M32: names, Scene Follow, a desk macro and a remote RECORD all behave as on `9a40f54`. On the MIDI control surface, faders and banks.
- [ ] 🟨 **Quit.** A normal quit leaves no crash report and the interface is not wedged on the next launch. macOS may ask for the microphone once after the install.

**First long take, 2026-11-03 (partial).** 55 tracks for 4 h 10 min through Dante Virtual Soundcard on `5c7a3e4`, to an external exFAT SSD: 0 missed samples, 0 ms device stall, every file complete. Quit left no crash report. EDIT then showed only the start of the take on the rows on screen, and the post-STOP rescan, the report's hashes and quit were very slow. `577592c` fixes the first; the second comes from that exFAT drive and is still open. `tools/verify_take.sh` has not run yet, because quitting cancelled the hashes before any was saved. *Device* still needs its `verify_take.sh` pass, and the Dante path is not the SD5 / HDSPe rig.

**Three-hour take, 2026-11-04 (partial).** 54 tracks for 3 h 01 min on `577592c`, same drive: 0 missed samples, 0 ms device stall, every file complete, and the rows on screen kept the whole take after STOP. The drive again read about 2.6 GB/s: hashes took 2 h 30 min and the waveform rescan 3 h 10 min after STOP. The zoomed-out waveform looked doubled; `eca170e` draws one shape. `tools/verify_take.sh` has not run on it yet.

- [ ] 🟨 **One waveform.** On `eca170e`, open that three-hour session in EDIT zoomed out to the whole take, then zoom in step by step with H+. Each lane shows one waveform in one colour at every zoom, with no paler shape behind it, and loud passages still read loud.
- [ ] 🟧 **After a long take, with the fix.** On `577592c`, reopen that session and leave it open: note how long EDIT takes to draw all 55 lanes and the report's hashes take to finish, then run `tools/verify_take.sh` (exit 0). Record a new take of at least 30 minutes and STOP: every lane keeps the whole take drawn from STOP until its rescan finishes, on screen and scrolled off. Quit and note how long it takes. Repeat the new take on an APFS drive and compare the rescan and quit times.

## 2026-11-03 fix round — not yet performed

None of this has been tried on real hardware. These fixes are in the development Mac's `eca170e`, not in the mini's `281c6c0`. Use disposable sessions.

### Device identity (only real hardware can prove this)

1. Set the interface as chosen in DEVICE, Apply, quit. Set the macOS default input to the built-in mic. Launch: the header must read `In: <interface>`. Check the device's rate did not change at launch.
2. Quit, unplug the interface, launch. `! In: <built-in>` in amber, one status line. Arm and press RECORD: the question names both devices. Return must cancel.
3. Plug it back in while idle. Within a few seconds the header returns to `In: <interface>` with no click on playback, since nothing is rolling.
4. Start a take on the built-in device (answer Record), plug the interface in during the take. Nothing may glitch or stop. At STOP it reopens within about a second.
5. Rename an aggregate in Audio MIDI Setup while the app is closed. Launch: it opens under the new name (UID match) and settings hold the new name.
6. Two identical interfaces: choose the second, swap their USB ports, reboot. It must come back to the same unit.
7. Remove one member of an aggregate. The header goes amber if the input count fell, and RECORD asks. Record what the aggregate really does: whether it stays listed, and how its channels renumber, is macOS behaviour nothing here has seen.
8. With the capture daemon on and a take rolling, force-quit the window and relaunch. The take must not glitch, and the interface's rate must not move.
9. A device slow to appear (a Dante or Thunderbolt device): the first reopen may fail. Confirm the stand-in stays open and the retry lands.

### A device that stops

10. Pull the clock from an interface that stalls rather than disappears, or suspend its driver, during a take. `!! NO AUDIO FROM THE DEVICE` within about 2 s, take still open, and the time stays on the banner once it recovers.
11. A device pair (separate input and output devices): unplug the output during a take. JUCE's combiner reports an error and stops. Confirm the alarm rises, and record what the take then does.
12. The same during a daemon take, with the window closed: the report must carry `deviceStallMs`.

### Trim-Follow

13. On a desk pushing preamp values: patch strip 3 from In 7, move the gain on desk channel 7, and hear strip 3 follow while the strip at position 7 stays put.

### Main window

- [ ] **Banner.** With an armed spare channel at digital zero and the twin link pulled, the banner goes red, TWIN LOST appears, and the pages turn every 3 s with a count. Nothing in the window moves, and STOP takes a click through all of it.
- [ ] **Banner at a distance.** Read "1 of 3" and the text from the mix position, at the smallest window.
- [ ] **DEVICE > Test** on a Dante or MADI rig: confirm the tone arrives on the monitor pair only and on no desk return. Press master MUTE during the tone. Press RECORD during the tone.
- [ ] **Remote RECORD during an autosave** with playback rolling, from a desk macro and from the twin leader: nothing starts, and the leader shows a refusal.
- [ ] **Backup drive pulled** at idle and during a take: BACKUP reads `BACKUP !` within a few seconds and pre-flight opens within a few seconds, including on a stalled network share.
- [ ] **Record time running out.** Drive a take to under 30, then under 10 minutes of record time: amber, then red. The clock keeps saying RECORD TIME LEFT in the last minute.
- [ ] **PATCH.** Click the last row with a stereo strip on an 8-input and a 32-in/2-out device. Link a strip on the last input and confirm R's dot and the status line agree with what records.
- [ ] **GRID at the smallest window:** faders usable, Mute present, vertical scroll reaches the last row.

### Capture copies and the capture daemon

Force-killing can wedge an audio device, so keep a recovery plan and stop if the interface becomes unstable.

- [ ] **Daemon device.** With daemon mode on and the interface picked, pull its cable, plug it back, pick it in DEVICE, press RECORD. Confirm the take holds the interface's channels, or that RECORD was refused naming both devices. Then launch with daemon mode on and press RECORD with no change in DEVICE.
- [ ] **Helper start.** On the mini with the HDSPe, time a cold launch with daemon mode on. Confirm no "never answered" message and no wedged device.
- [ ] **Copies after a strip move.** With a backup and a mirror on real drives, record, move a strip, unplug the backup, move another strip, plug it back, press RECORD. Confirm each copy's track holds one channel from start to end. Note how long the first seconds of that take held the copy's ring on the slowest drive: the renames happen then, with the ring holding audio, and this is the case where a very slow drive could cost that copy audio.
- [ ] **Pre-roll.** 56 channels, three copies, 30 s pre-roll, on the slowest drive that keeps real time. Press RECORD and confirm no hole after the pre-roll.
- [ ] **Recovery.** Daemon rolling, force-quit the window, turn off daemon mode in settings, relaunch. Confirm the rolling session is not listed.
- [ ] **Crash repair.** Kill the app mid-take on WAV past the first 5 s, and once past 4 GiB. Confirm the reopened take runs to the last seconds and opens in another DAW.

### Sessions and tools

- [ ] **Drive pulled with unsaved work.** Open a session on an external drive, change a fader, pull the drive. Press Cmd+Q. Expect *Session folder is missing* naming the folder, with Return cancelling. Reconnect, confirm the path comes back at the same mount point and that Save works. Repeat with File > Open and Close Session.
- [ ] **Soak monitor on a real take.** Run `tools/soak_monitor.sh` through a real take with a custom session root in FLAC and with Track 1 unarmed. Quit the window with the daemon rolling and check for the `APP NOT RUNNING` line. A real crash report's file name on this macOS should match `Zynforge Recording-*.ips`.

### Punch, reads and long jobs

- [ ] **Pull the drive during an export.** Export Audio from a USB drive to another drive and unplug the source mid-file. Expect "Export INCOMPLETE" or "Export failed", and no finished file for the track in progress. Repeat with a FLAC session: this is the case a truncated file cannot stand in for, EIO from a real device, and it must end rather than hang.
- [ ] **Pull the drive during a punch stop.** Punch, then pull the session's backup drive during finalization. Expect the copy reported failed, and the primary intact.
- [ ] **Punch on an edited show.** Ripple pre-show out of a real take and press RECORD with a selection, with the cursor, while rolling, and in PUNCH mode. Each refuses, naming a strip. Confirm RECORD with the cursor still offers Record After Take.
- [ ] **Cancel from the File menu during a 3-hour conversion.** Expect it to stop within a second or two, leave no partial file, and let RECORD work at once.

### EDIT on a real session

- [ ] With two saved sessions of the same channel count and stereo layout, open one, then the other, in EDIT: names, R I S M, routing and meters all follow the new session.
- [ ] Patch a strip to In 7 / Out 7, arm it, choose → Master in EDIT, press RECORD: it records In 7, and the status line shows no auto-routing.
- [ ] Pull the session drive during finalization with EDIT on screen: the window keeps responding and the take reports.
- [ ] At the minimum window and on a micro row, right-click the channel name: the menu opens, and the MARKERS LANE tag is readable on a row set to Markers.
- [ ] Eyeball the Fade tool's squares and the range band on a long take at fit zoom.

### Desk read-back (X32 / M32)

- [ ] **STAGE read-back.** SOUNDCHECK, then back to STAGE. The status says "confirmed by the desk" and the chip goes out. Confirm routing on the desk. Pull the network cable, press back to STAGE: within about 2 s it says the desk did not confirm, and the chip stays. Plug in, press again: it clears.
- [ ] **Routing enum.** With show patches using AES50 A and B and the card, confirm SOUNDCHECK reaches the desk. A read-out-of-range value would read as a lost reply: a timeout on every SOUNDCHECK press means the desk reports a value past 19, and the profile needs it.
- [ ] **Read straight after write.** Confirm STAGE never reports a wrong block on a healthy network. The query follows the writes in the same burst; if the desk answers before applying them, this reports a false failure, and a short wait belongs before the read.
- [ ] **Head-amp read-back.** Capture gains, change two, Restore Gains. The status says confirmed. Confirm a restored value reads back within one 0.5 dB step.
- [ ] **Put back by hand.** With the app on SOUNDCHECK, reload the show file on the desk. Console: check the desk's patch clears the chip and writes nothing.
- [ ] **Relaunch.** Force-quit on SOUNDCHECK, relaunch, connect. The chip stays if the desk is on card returns and clears within a couple of seconds if it isn't.

### Desk link health (each desk you have)

- [ ] Connect. The menu says "desk answering" within 4 s, or "no answer from the desk yet" if the dialect's probe gets no reply. Note which, per desk: an SD5 that never answers its probe is a dialect finding, not a failure.
- [ ] Pull the desk's network cable during a take. Within about 15 s, an amber CONSOLE NOT ANSWERING notice shows and the take is untouched. Plug it in: the notice clears and the status says it is answering again.
- [ ] On the SD5, confirm no DESK ON SOUNDCHECK ever shows, before or after using the desk's own VSC, and pre-flight says the patch is not visible here.

## 2026-11-02 design pass — not yet performed

Tests and rendered images can't judge these. They need the real app on a real display, at the mix position, in a dark room.

| Step | Do | Pass | Fail |
|---|---|---|---|
| ☐ D.1 | From the mix position, read the channel names in MIXER at XS, S, M and L. | Every name reads at every size. | A name is cut or too small. |
| ☐ D.2 | Roll a take on a disposable session. Stand back from the screen. | REC, the time and the red plate read from across the room. | You have to walk up to see that it is rolling. |
| ☐ D.3 | With a take rolling, pull the session drive or force another alarm. | The banner is the first thing you see, clearly louder than the plate. | Banner and plate blur together. |
| ☐ D.4 | Play a session through the master. Read the status panel. | LUFS M and I and TP move, and TP turns red within 1 dB of full scale. Readable from your normal seat. | Figures stay at `--`, or are too small to use. |
| ☐ D.5 | Look at EDIT in the dark for a minute. | Clips sit back, waveforms read, nothing glares. | A clip or a control is the brightest thing on screen for no reason. |
| ☐ D.6 | Arm every channel, then roll. | Idle, the deep red R buttons sit back. They lift when the take rolls. | The armed column shouts, or armed and off look alike from your seat. |
| ☐ D.7 | Force an alarm with a take rolling, with the mouse on a fader. | The banner rises along the top, loud enough from your seat, and nothing moves. | You miss it because it is at the top of the window, or a long alarm is cut off. |
| ☐ D.8 | Press VSC on a session you can check. Say Repatch, then press VSC ON and Restore. Quit and reopen in between once. | It asks both ways, reads VSC ON between, and the old output patch comes back. | The outputs differ from before, or VSC ON is gone after the reopen. |
| ☐ D.9 | With a console link on a disposable show file, go to the SOUNDCHECK patch. | DESK ON SOUNDCHECK shows beside the small clock and in pre-flight, and clears on the way back to STAGE (on `main`, once the desk has read the patch back). | Either place stays wrong after the desk moves. |
| ☐ D.10 | Read the strip meters against the desk's meters with tone at -18 and -6 dBFS. | The meter's own scale agrees with the desk. | The scale is off, or its numbers are too small to use. |

## 2026-11-02 cut to recorder and virtual soundcheck — not yet performed

None of this has been tried on real hardware. It is in the development Mac's `eca170e`, not in the mini's `281c6c0`. Use disposable sessions.

- [ ] Open Backup & Mirror Drives from File and from the BACKUP button. It is one dialog: backup first, with a format box, then the mirrors.
- [ ] Set a FLAC backup on a WAV session and record, once locally and once in daemon mode. The backup is FLAC, the primary is WAV, and the format is still set after a relaunch. Pre-flight reports the backup as it did.
- [ ] Leave a new install's backup format alone: it reads WAV 24.
- [ ] Run File ▸ Export ▸ Export Audio three times: Raw takes in FLAC, Edited tracks and Stereo mix. Ticks apply to the first two, all on by default. The last two say plainly they are 24-bit WAV, and the files are.
- [ ] Change a strip's Input or Output in the EDIT row. MIXER and PATCH show both sides moved, on both halves of a pair. The Output combo refuses during a take.
- [ ] With a loop range set, Ctrl+Cmd+C opens the Crop prompt. Cmd+C only copies.
- [ ] Press Tab outside a text field: nothing happens, and focus stays off the routing combos.
- [ ] Open a session from an earlier build with several takes. It plays the active take, and a save keeps them all.
- [ ] Open a session from an earlier build with automation, a VCA, a bus strip, a tempo map or a click row. It opens, plays and saves. The status line names `Session File Backups/Before Feature Removal`, and the folder holds the session's original documents. Opening it again says nothing and changes nothing.
- [ ] Open a session with none of that data: no folder, no status line.
- [ ] Recall a cue: nothing about the session changes but the playhead and the mix.
- [ ] Start a long waveform scan, then run Save As and Show Handoff while it runs. Both finish. The worst case is a rescan in the copy. Make any other file in the session unreadable: the copy stops and says so.

## 2026-11-01 fix-pass acceptance — not yet performed

None of this has been tried on real hardware. These fixes are in the development Mac's `eca170e`, not in the mini's `281c6c0`. Use disposable sessions. Automated coverage is in [testing.md](testing.md).

### Punch safety (needs the real app and a disposable session)

- [ ] With a take recorded, click Loop on and off, stop, press RECORD. RECORD must append (status says a new part), the transport must not roll, and the old take must be intact on primary, backup and mirror.
- [ ] Open a session saved by `281c6c0` after Loop was used. No range may be drawn on the ruler, and RECORD must append.
- [ ] Select a range, RECORD, let the punch finish, press RECORD again. The second press must append; the range stays drawn.
- [ ] Place an EDIT cursor inside a take with the transport stopped and press RECORD. *Replace audio?* must appear. Return appends and clears the cursor. Replace From Cursor punches. Esc leaves everything as it was.
- [ ] With that question open, start playback from a remote, then answer. Nothing may record, and the banner must ask for RECORD again.
- [ ] Turn on PUNCH mode with two strips armed, disarm one, arm another, roll through a selected range. Exactly the strips lit at punch-in get punched, on every copy.
- [ ] With PUNCH on, open another session. The pill must go out and the menu tick must clear.
- [ ] From the desk, send RECORD with a cursor placed, with a range selected and while rolling. Each must be refused, with the reason in the main window and at the remote.
- [ ] Read the PUNCH pill from the mix position.

### Main window

- [ ] With LOCK on, right-click a strip and the setlist bar; click GRID and try a fader; try to edit in EDIT. Nothing may change.
- [ ] Pull the backup drive during a disposable take. The red banner must appear along the top of the window, readable from the mix position, and STOP must still work with it up.
- [ ] Press RECORD with nothing armed. The banner shows why, then clears itself.
- [ ] Start a QC scan on a long session, then press RECORD. The take starts at once, the status says the scan stopped, and no report opens over the take.
- [ ] Open Session Settings, set pre-roll to 5 s, record a fresh take, and confirm the audio before RECORD is there.
- [ ] Start Generic OSC from Control Surfaces, copy the token from the panel, and drive RECORD from an OSC app.
- [ ] Put the meterbridge on a second display, click in the main window, and confirm it stays. Open another session with the same track count and confirm its meters move.
- [ ] First launch on a 44.1 kHz device: no sample-rate alarm, and Welcome shows after the tour.

### Capture copies, crash recovery and the capture daemon

Force-killing can wedge an audio device, so keep a recovery plan and stop if the interface becomes unstable.

- [ ] **Stalled copy.** Run a NAS mirror and a USB backup, pull the NAS network cable mid-take for 30 s and restore it. Expect the banner to name the mirror, no gap on primary or backup, silence on the mirror only, sample-aligned. Repeat with the cable left out through STOP: STOP finishes within about 30 s, the report names the mirror, and the next RECORD leaves it out and names it.
- [ ] **Hung copy.** With a take rolling, make a NAS mirror stop answering without returning an error (block its traffic at the switch rather than pulling the cable). Expect the window to stay alive, STOP to work, the time-left label to read PRIMARY TIME LEFT and the banner to name the copy. Repeat on the primary: the figure reads STALE rather than a number.
- [ ] **Sleeping backup.** Let a USB hard drive spin down, then press RECORD. Expect the take to start at once on the primary, the backup to catch up in step, and silence with a named report line only if it took longer than its buffer lasts.
- [ ] **Punch through a stall.** Stall the primary for over 6 s during a punch on a disposable take. Expect the original to stay on every copy, the punch audio in `Session File Backups/Unspliced Punch`, and the banner and report to say so.
- [ ] **Dying drive at STOP.** Yank a USB backup during finalization. The primary must finish and the report must flag the backup.
- [ ] **AIFF crash.** Record AIFF 24 for 10 minutes on the real rig, power-cut the Mac, reopen. Expect a whole take less at most the last 5 s, and on a drive with power-loss protection nothing missing. Check a take from `281c6c0` the same way.
- [ ] **FLAC crash.** Same with FLAC 24 on 56 tracks: session open should take seconds.
- [ ] **Daemon kill.** `kill -9` the daemon mid-take in daemon mode: the window ends the take, says so, and RECORD brings a new daemon. Quit the GUI mid-take and relaunch: it reattaches with no dialog over it.
- [ ] **GUI crash during a daemon take.** Daemon mode on. Open a session with named strips and a cue. RECORD. Force-quit the GUI only, never the daemon. Relaunch. Expect a REATTACHED status, with strips, names, routing and cue back while the take rolls. STOP twice. Reopen the session: all still there, and the audio is complete.
- [ ] **The same on a session never saved.** As above with a brand new session and no Save before RECORD. Same result. Confirm `session_mix.json` is in the take folder before the force-quit.
- [ ] **Device loss.** Pull the interface during a punch on a long take: the UI stays alive and the button returns to RECORD when the take has closed.
- [ ] **Long pre-roll.** 30 s pre-roll on 56 tracks with a slow USB backup and a NAS mirror: `missedSamples` 0 on the primary.
- [ ] **Ring memory.** Watch RSS on a 64-channel take with 3 copies and no pre-roll: at most about 200 MB of ring. On `main` a long pre-roll adds to that, up to 1 GiB of ring in all.

### Sessions

- [ ] **Recovery dialog left open.** After an app crash during a disposable take, relaunch and leave the recovery dialog open past the auto-save interval. Status says auto-save skipped, and `session_mix.json` is unchanged on disk.
- [ ] **Drive pulled while idle.** Open a session on an external drive, unplug it, wait one auto-save interval. A red AUTO-SAVE FAILED names the folder. Cmd+S and Save As refuse. Replug: the next auto-save works.
- [ ] **Trash on exFAT.** Record a short take on an exFAT drive, then Remove Last Capture. The session goes to the Trash on that drive. If the volume has no Trash, nothing is removed and the status says so.

### Editing and export

- [ ] Export a long session at another sample rate and note the time per track; compare the file by ear and in a DAW against the same export from `281c6c0`. For scale, a 3-hour 56-channel file set took roughly 18 minutes under load.
- [ ] Run Show Handoff to an exFAT destination with Finder open on it.
- [ ] Fill a real disk during a bounce and confirm the status reports failure and no finished file appears. Only `RLIMIT_FSIZE` proved this; a full volume has not been tried.

### EDIT on a real session

- [ ] Open a session with a stereo strip and an edit group. Shift+click a clip on the stereo row, press Delete, and play the region: nothing may be left on either side. Cmd+Z.
- [ ] Press D on a stereo clip and nudge the copy with Alt+arrows. Solo the strip with a pan hard left, then hard right: both sides move together.
- [ ] Drag a Range, press Cmd+X: the prompt must say audio, not channel. The channel stays.
- [ ] Move a strip, close and reopen the session: every lane shows its own waveform.
- [ ] On a session of 64 channels and 3 hours, watch the UI while a take stops and while the session opens: meters keep moving while the cache saves and loads. Note how long the lanes stay blank on open.
- [ ] Leave a clip's right-click menu open, start a take from the desk or a remote, then choose Mute: the status line refuses and the clip is unchanged after STOP.

### Network and console

- [ ] **Yamaha SCP scene notifications (UNVERIFIED).** On a CL/QL/RIVAGE: connect, recall a scene during a take, and check that a marker drops. If no marker appears, the desk needs a read-only subscription we couldn't confirm from public documentation. Don't add `ssrecall_ex` back: it recalls scenes. Save Console Log and send it in.
- [ ] **X32 show patch.** Connect, open a session, press SOUNDCHECK and confirm the desk moves to card returns. Force-quit the app. Relaunch, open the session, connect, and confirm the menu offers STAGE and restores the real patch.
- [ ] **Console address pin.** With a real X32, confirm names and scene recalls still arrive. If the desk answers from a different address than the one dialled (a second network port), the log will show "ignored: not from the address this link is pinned to". Use the address the desk answers from.
- [ ] **MCU bank.** On a real surface, mute a strip and bank away: the button must go dark and a press there must toggle the strip now shown.
- [ ] **Daemon + twin report.** On a short linked daemon take, confirm `verify_take.sh` passes and the report has both hashes and the `twin` block.
- [ ] **Leader restart.** With a daemon take rolling, quit and relaunch the leader, open Twin Recorder, type the follower's code, press Connect, and confirm it re-links without touching the take.

### Outputs, playback and sync

- [ ] 1:1 patch on the real interface with every output a console return, master at default Out 1-2. Console channels 1 and 2 carry only their own tracks. Master strip reads MIX OFF, status line says why. Faders down first.
- [ ] Same rig, master set to a spare pair: the mix is on that pair only. Master set to Off: nothing on any output from the mix, meters still move.
- [ ] Laptop or 2-output interface: strips patched to Out 1-2, the whole mix still heard.
- [ ] Trim-Follow on a desk (X32 first): take with the OSC listener on, reopen, listener on, Trim-Follow on, move a preamp 1 dB. The recorded channel moves 1 dB. Then a take recorded with the listener off: move a preamp, the channel does not move, and the status line names it. Faders down first.
- [ ] Trim-Follow past the limit: raise a preamp more than 12 dB over its captured value. The channel stops at +12 and the status line says so.
- [ ] Loop a region longer than 3 s on a 64-track session, on the show drive, for ten passes. No gap, no track late. Then FLAC.
- [ ] Locate by marker during playback on 64 tracks, on the show drive. Measure the silence before the tracks enter, and confirm they enter together.
- [ ] MTC from the real source, joined mid-stream at a minute boundary: the label never jumps back. LTC or MTC starting at 01:00:00:00 with the start timecode set: chase rolls from the right place. Without it: the status line says chase is holding, and locates stay put.
- [ ] Solo a strip during a take with StereoMix on: the file keeps the full mix.

---

## Market-moves acceptance — not yet performed

Everything in this section needs hardware or a person. Use disposable sessions. Automated coverage is in [testing.md](testing.md).

### DiGiCo desk test kit (SD5, hardware only)

Our DiGiCo addresses are UNVERIFIED. This one session settles what they should be. Use a disposable session on a desk that isn't running a show. Nothing below changes the desk beyond adding one External Control device.

**Before you start, write down:**

- SD5 software version (Master screen / System info) and the console's IP address on the network the Mac is on.
- The Mac's IP address, and this app's build.

**Set up the recorder:**

1. Session ▸ Network & Surfaces ▸ Control Surfaces ▸ DiGiCo ▸ Protocol Settings. Tick **Debug: log OSC traffic**. Note the Listen Port (default 8000). Enter the Console IP. Close.
2. Tick DiGiCo in the list so the listener runs. Optionally also Session ▸ Console ▸ Console: connect... with the DiGiCo profile, so both paths run. Write down which you used.
3. Session ▸ Console ▸ desk macro control. Enter the desk's IP and tick **Listen for desk macros**. Note the port (default 9301). Copy the addresses.

**Set up the desk:**

4. Setup > External Control: add a Macro OSC device pointing at this Mac, on port 9301. Then add or edit any OSC target for the app's Listen Port if the desk offers one.
5. Create a macro for `/zynforge/macro/marker` with an integer message of 1, and another for `/zynforge/macro/stop`. Do NOT add any special handling for a third-party device type on the desk.

**Run the tests, in order, noting the wall-clock time of each:**

6. Fire three different snapshots, then one of them again.
7. Rename one channel.
8. Fire the marker macro once. Press it again quickly (under a quarter of a second).
9. Create a session with a marker named exactly like a snapshot, turn on Session ▸ Console ▸ follow scenes to markers, and fire that snapshot.
10. In a disposable take, fire the stop macro once, wait a second, and fire it again.
11. Session ▸ Console ▸ Save Console Log. Keep the file.

**What each result proves:**

| In the log | It proves |
| --- | --- |
| `osc <- <desk ip>:<port>  /Console/Snapshots/recall ,is 5 "Verse"` | Our address and argument order are right. Mark the DiGiCo dialect's snapshot parse as verified for that software version. |
| An `osc <-` line for a snapshot with a different address | The real address: fix `handleDiGiCo` and `digicoDialect`, then re-run. |
| A snapshot line with a number and no name | Follow can only match "Scene N" markers on this desk. |
| No `osc <-` line for a snapshot, but other desk traffic appears | This OSC path doesn't carry recalls on this software. Scene markers and Follow can't work from it. |
| A rename shows `/Console/Channels/N/name ,s "..."` | Live name following works. |
| `[bundle, not acted on]` lines | The desk sends bundles, which neither console path acts on. That is a code change, and the log shows the addresses it needs. |
| `refused by the parser` lines | The desk sends a type we refuse. The line shows the address and type tags. |
| `macro <- <desk ip>:<port>  /zynforge/macro/marker ,i 1` followed by `macro: marker dropped` | Desk macros work, and integer messages arrive as integers. |
| `macro <-` with `,f` or `,s` and no action | The desk sends another type. The listener accepts only none or integer; change the macro's message type. |
| `macro: repeat inside the debounce window, ignored` after one press | The desk double-fires. The debounce is doing its job. |
| The panel shows dropped datagrams and a different "latest" address | The desk sends from another interface. Put that address in the panel. |
| Nothing at all from the desk | Network or port: check the Mac's firewall and the desk's target address first. |
| Step 10: first press leaves the take running with "STOP armed"; second ends it cleanly | The two-tap guard holds from the desk. |

**Send back:** the saved log, the SD5 software version, and the wall-clock notes.

### Scene Follow and X32 stereo (X32 / M32, hardware only)

1. Connect the link. Fire a scene outside a take: no marker appears. Turn Follow on with a marker named "Scene N", fire scene N: the playhead jumps and the transport doesn't change. Confirm the desk's `/-show/prepos/current` fires once per recall. With a desk talking to both the link and the OSC listener, a scene with duplicate markers must step once: the two reports count as one inside 250 ms.
2. On the desk, link channels 1-2. In Control Surfaces, press Create Session from Connected Console. Confirm one stereo strip in MIXER, EDIT and PATCH, and that `/config/chlink/1-2` appears in the saved log.

### Capture metadata and dead-channel alarm (real hardware)

Nothing below has been tried on hardware. The recorder side is proven only against synthetic input and a Mac's built-in devices.

1. **Clock source.** On the RME/MADI rig, open pre-flight with the interface on internal, then on word clock and MADI. Confirm each row names the source and that internal warns. Note what a Dante interface reports; if it reports nothing the row must say so without a failure.
2. **Clock in the report.** Record a take; `clockSource` and `clockSourceKind` must match the row.
3. **Timecode stamp.** With LTC locked, record, stop, and import into Reaper or Pro Tools using the BWF time. Measure against a hardware recorder or a slate on the same timecode. Record the offset: the label is read at RECORD and can be up to a frame old (two on MTC), and the code does not compensate for converter or driver latency, so expect up to a frame plus converter delay. Repeat with MTC, and with no chase (the Mac's clock).
4. **Spotting.** Import a take into Pro Tools, Reaper and Cubase with "use BWF time". Confirm each spots to the stamped time and that the channel name appears where the DAW shows sTRK or description.
5. **Real stream loss.** While recording from the MADI/Dante rig, pull the stream for 5 s. Expect one INPUT STREAM LOST alarm, on the status line, clearing on restore, and one stream entry plus each channel in `session.report.json`. Then confirm what the driver delivered during the drop: the alarm only sees exact zeros, so a driver that repeats its last buffer or outputs a non-zero value will not alarm.
6. **Single channel.** Pull one mic or stagebox line. An analog input has a noise floor, so it should NOT alarm unless the path delivers digital zero. Then mute a channel's direct out at the console and note whether that alarms. If the console's direct outs are post-gate, expect a channel fault whenever a gate closes for 3 s.
7. **RF64.** The patch has run on a synthetic RF64 file with JUCE's layout. On a real take over 4 GiB, run `tools/verify_take.sh` and check the stamp in the file.

### DAW handoff

- **Open the `.dawproject` in Cubase, Studio One and Bitwig.** UNVERIFIED: no DAW that reads DAWproject is installed on the development machines. It is valid against the format's `Project.xsd` and `MetaData.xsd` and follows its reference layout for clips in seconds with external files. Confirm: every track present with its name and colour, stereo tracks stereo, clips at the right time and source offset, fades present, audio online from the relative path, markers in place, fader and pan plausible. Note how each DAW treats overlapping clips and which fade curve it uses.
- **Open a real show's `.RPP` in REAPER on the owner's own configuration**, from an external drive, and play across a part boundary. The fixture proof uses a private configuration and seconds of audio; a many-gigabyte RF64 take on a show drive has not been tried.
- **Hand a Verified Show Handoff to another machine** and open both project files from the handoff folder, confirming the audio comes online without relinking.
- **Export Songs on a multi-hour show:** confirm memory stays flat, the status line updates, Cancel Song Export stops within a moment, and the first and last samples of each song line up with a marker on a whole-session bounce in a DAW.

### Twin Recorder (not yet performed)

Use the owner's two Macs on the show network: **MacBook Pro 192.168.68.81** (leader) and **Mac mini 192.168.68.72** (follower). Use disposable sessions only. Both Macs must run the same build. The Recording app is ad-hoc signed, so macOS may ask for Local Network or firewall permission the first time the follower listens or the leader connects: allow it, and confirm in System Settings ▸ Privacy & Security ▸ Local Network and ▸ Network ▸ Firewall. If the app was denied, the leader stays on CONNECTING and nothing else fails.

1. **Pair.** On the mini: Session ▸ Network & Surfaces ▸ Twin Recorder, role Follower, Listen for leader. Note the code and the address shown. On the MacBook: role Leader, enter 192.168.68.72 and the code, Connect. Expect LINKED on both and TWIN OK in both headers. Confirm the panel's warning text is visible and reads as "not an independent recorder".
2. **Wrong code.** Disconnect, enter a wrong code, Connect. Expect a refusal on the leader, one refused attempt counted on the follower, and no further attempts in the follower's count over 30 seconds.
3. **Mismatch warnings.** Give the Macs different armed counts and session names. Expect warnings on both, and that RECORD is not refused. Set them equal and confirm the warnings clear.
4. **Start and stop.** With matching setups and the same test tone on both inputs, press RECORD on the leader. Expect both rolling within a second. Press STOP once on the leader: the leader arms, the follower keeps rolling. Press STOP again: both finalize. Expect a compare line on the leader and a `twin` block in both `session.report.json` files, then run `tools/verify_take.sh` on both sessions.
5. **Follower refusal.** Disarm all tracks on the follower and press RECORD on the leader. Expect the leader to roll and to show the follower's reason. Repeat with LOCK on at the follower: expect "session is locked". Stop the leader.
6. **Pull the network cable mid-take.** On a disposable session, start both and let them roll for two minutes. Pull the mini's network cable. Expect TWIN LOST on both within about 3 seconds, both takes still rolling, and both clocks advancing. After a minute, plug it back in. Expect the link back within a few seconds, both still rolling, and nothing restarted. Stop from the leader. Both reports must show one link-loss interval of about the right length.
7. **Pull it, then stop.** Start both, pull the cable, press STOP twice on the leader. Expect a warning that the follower did not stop. Plug it back in. The follower must still be rolling, and the leader must say so. Stop the follower by hand with its own two taps, and confirm the leader's take did not change.
8. **Interface failure.** Start both, then unplug the leader's audio interface. Expect the leader's take to finalize with its device-lost warning, and the follower to keep rolling and report it. Replug, press RECORD on the leader, then stop from the leader. Expect the follower to stop.
9. **Follower dies.** Start both, force-quit the follower. Expect TWIN LOST on the leader within about 3 seconds and the leader's take unaffected.
10. **Quit.** With a take rolling on the leader, choose Quit ▸ Stop & Quit. The leader must finish quitting without delay. The follower must keep rolling with TWIN LOST. Stop it by hand.
11. **Marker.** During a take, drop a marker on the leader. Confirm it lands on the follower at that Mac's capture position, which will differ slightly from the leader's.
12. **Daemon mode.** Enable the capture daemon on one Mac and repeat step 4.

Record the clock offset each panel shows; on a healthy LAN the two should be opposite in sign and within a few milliseconds of each other in size.

### Notarized build (needs an Apple Developer ID)

1. Store the credentials once (`xcrun notarytool store-credentials`), then run `tools/notarize.sh --check` and fix what it reports.
2. Run `tools/notarize.sh`. Only a reported status of `Accepted` counts.
3. Mount the DMG on a Mac that has never seen this app, install it, and confirm Gatekeeper opens it without a warning, the microphone prompt appears, and the Local Network prompt shows the app's own reason.
4. Confirm `LICENSE` and `THIRD-PARTY-NOTICES.md` are in the DMG root.

## October 5 follow-up acceptance — `443c769`, not yet performed (repeat on the installed `281c6c0`)

Run these checks on disposable sessions; retain measured results and leave
unchecked items open. [The repair register](docs/history/REVIEW-REPAIRS-2026-10-05.md)
contains automated evidence for all 21 defects.

| Check | Expected result |
| --- | --- |
| ☐ Record 27 tracks with the intended interface and external drive; operate MIXER/EDIT and STOP. | Measure command response, STOP-to-capture-ended and STOP-to-finalized separately; no missing audio or falsely clean report. |
| ☐ During a pending save, stop existing playback/capture with Space. | STOP remains available; normal capture still requires its second confirmation and LOCK still applies. |
| ☐ Change one fader, undo/redo, reset a 27-track session, then reopen. | Values round-trip, controls respond and reset defaults are correct. |
| ☐ Open import/bounce chooser, start capture before selecting a file/destination. | Completion refuses the operation without importing or rendering into a live take. |
| ☐ Record/export/punch 32-bit float AIFF-C with primary and configured copies; open it in the intended DAW. | Float data, frame counts and channels match; any destination/finalization error stays visible. |
| ☐ Run the post-show QC report on stereo audio with signal only on the right and on an unreadable test file. | Right-channel issues are reported; the unreadable file is reported as incomplete, never as a clean result. |
| ☐ Test the actual console connection, including incomplete gain replies/reconnect. | Partial gains are not restored. |

## Long-run waveform check — record the installed build for each Mac

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ L.1 | On the updated Mac mini, start a disposable multichannel take and keep FOLLOW enabled. Observe the live waveform at the recording edge after three hours and again near five hours, including after H+/H- and V+/V- changes. | Playhead continues to scroll and the recent waveform keeps visible detail; zoom controls respond. | Scrolling stalls, waveform becomes blocky near the edge, or controls cease to respond. |
| ☐ L.2 | Stop normally, wait for final hashing, and retain the session. Compare the final file waveform with the last live view; inspect `session.report.json` and verify file lengths, hashes, missed samples and failure flags. | Final waveform duration and detail agree with the capture; no missing frames or capture errors. | Live/final mismatch beyond expected rendering differences, short file, unfinished hash or capture warning. |

The first five-hour Mac mini run on `d2c5858` was interrupted around 2:43 by a macOS PCIe `lan-1gb` kernel panic. A separate 55-channel run on 2026-10-02 completed 5:00:11 without a restart. Its final report and all 55 primary WAV frame counts agree, with zero missed samples or reported capture/write failures. The user said everything looked fine, but no timed live waveform/FOLLOW/zoom observation, live-to-final comparison or independent SHA-256 comparison was recorded. Keep L.1 and L.2 open for those remaining checks; see [the evidence](docs/history/FIELD-TEST-2026-10-01.md). The user requested no app or routing changes for the earlier host incident.

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
| ☐ 2.6 | Set capture format to FLAC/24 via Format & Recording dialog. Record. | New files are `.flac`, smaller than equivalent WAV. | Format ignored OR app crashes on switch. |

---

## 3. Audio path 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 3.1 | Play back a recorded session. Listen at moderate level. | Clean playback, no clicks, no dropouts. PerfDashboard CPU < 5 %. | Any audible click, glitch, or dropout. |
| ☐ 3.2 | While playing, move a fader. | Audio level follows instantly, no zipper noise. | Stepped / zippered audio. |
| ☐ 3.4 | While playing, press M to toggle a strip's mute. | Smooth fade to silence (not a hard cut). | Hard mute cut = pop. |
| ☐ 3.5 | Stop playback. Switch sample rate via Format & Recording dialog. | Warning dialog appears if SR mismatches the loaded session. | Silent switch with no warning. |

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

## 7. Visual / design polish ⬜

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 7.1 | Hover over a channel strip. | ~6 % brightness lift on the strip wash + brighter edge. | No hover state. |
| ☐ 7.2 | At XS strip width (24 per page), look at the REC/MON/MUTE/SOLO buttons. | Stacked vertically as a 1×4 column. | Cramped 2×2 grid = touch targets too small. |
| ☐ 7.3 | Clip a meter (drive a hot input). | Top PEAK tally bar pulses brand-red. Click clears it. | Tally doesn't latch / doesn't clear on click. |
| ☐ 7.4 | Open every dialog: New Session, Add Tracks, Session Format & Recording, Session Info, Audio Device, Markers list, Backup & Mirror Drives, Export Audio, Session Recovery. | All have the same orange-stripe DialogChrome top + flat solid body + footer divider. | Any dialog looks "raw JUCE" = chrome regression. |
| ☐ 7.5 | Look at any saturated chip (Edit Group badge, PatchPage strip number, active routing dot, active EditTools icon). | Text on coloured background reads cleanly (dark on bright, light on dark — `onSignal` does the right thing). | White-on-yellow or similar low-contrast pairs. |

---

## 8. Stretch — only if everything above passed 🟨

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 8.1 | Connect a console dialect via OSC. Change a channel name and recall a scene. | Strip name updates and a named marker drops; transport/arm/mute remain unchanged. | Missing metadata/marker, or unauthenticated state change. |
| ☐ 8.3 | Cue a setlist of 5+ cues. Press next-cue 5 times during playback. | Soft-takeover fade between each, no clicks. | Hard cuts = ramp engine broken. |

---

## 9. Session integrity + authenticated remote control 🟥 / 🟧

| # | Gesture | Expect | Failure indicator |
|---|---|---|---|
| ☐ 9.1 | Make unsaved mixer + clip + cue changes, then invoke New, Open, CSV import-as-session, Create from Console, New from Template, and double-click another `.zfproj`. | Each replacement offers Save & Continue / Continue Without Saving / Cancel. Cancel preserves everything; a deliberately unwritable save cancels the replacement. | Current session disappears, or switch continues after save failure. |
| ☐ 9.2 | Save As to an existing non-empty session folder, then to a new folder on another volume. | Non-empty target is refused. New target copies in the background; source remains active until complete; clone opens with audio, mix, clips, cues and layout intact. | Folders merge, UI freezes, destination opens early, or state is missing. |
| ☐ 9.3 | Start a large cross-volume Save As, then quit once and allow cancellation. | App reports cancellation and stays open until the owned worker finishes; source is unchanged and a newly-created partial target is removed. | Quit UAF/crash, partial clone remains, or source becomes the target. |
| ☐ 9.4 | Relocate a session across volumes; separately simulate inability to remove the old folder after a complete copy. | New folder opens and is authoritative. Cleanup failure explicitly names the old folder instead of reporting total move failure. | Active path still points old, or successful copy is discarded/misreported. |
| ☐ 9.5 | During Save As, relocation and Export Audio's Edited tracks and Stereo mix, try Record, Play, +CH, delete/reorder, and a second file operation. | Every competing mutation refuses with a wait/busy message; the running job finishes or cancels cleanly. | Structure changes under the worker, crash, corrupt/short output. |
| ☐ 9.6 | Save a default template. Create once from Welcome and once with File ▸ New Session; also choose New Session from Template while a show is open. | Default layout appears in both new sessions. Explicit template asks about the current show and creates a different session; it never overwrites the show. | Empty default session or template destructively changes current session. |
| ☐ 9.7 | Open a 24-strip session, then a 4-strip session; repeat with missing and malformed `session_mix.json`. | Valid mix shrinks exactly to 4. Invalid/missing mix falls back to the audio count. No names/stereo/cues from the first session survive. | Extra ghost strips or previous-show state leaks. |
| ☐ 9.8 | Build a continued take through `_part10`, `_part11`, and `_part100`; play/export/bounce/QC it. Then temporarily remove the last part. | Complete take is numerically ordered and sample-complete. Missing trailing part refuses instead of producing a plausible shorter result. | `part100` plays before `part11`, or truncated output is accepted. |
| ☐ 9.9 | Start Generic OSC and note the shown token. Send a state-changing command without it, with a wrong token, then with the token as the final string argument. | First two do nothing; authenticated command works. Console-specific dialect messages may update names/markers/trim but cannot arm/mute/transport. | Tokenless UDP changes the session or console dialect starts recording. |
| ☐ 9.10 | Generic OSC: send Record with no armed live input, then with a valid armed input. | The first does not roll and the status line says why; the second starts a real take. | A reported start with no take, or an unusable zero-input take. |
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

The helper checks WAV, AIFF and FLAC files, including continued takes, but does not certify the recording or backup drives. It exits nonzero while hashing is pending and does not prove channel mapping or that the reported duration equals the planned performance. See [helper limitations](SHOW-READINESS.md#verification-helper-limitations). After stopping a disposable session, run:

```bash
tools/verify_take.sh "/path/to/Session"    # use the exact session under test
# With no path, the helper verifies the capture the app last recorded, then its active session.
# It never guesses from folder dates.
```

Its implemented checks include:

- every WAV, AIFF and FLAC take opens, and WAV files over 4 GiB have an `RF64` + `ds64` header;
- a continued take has its base file and an unbroken `_partNN` sequence;
- primary-file frame totals match the report and every take file is listed exactly once;
- `session.report.json` exists, hashing has finished, `missedSamples` is zero and capture-failure flags are clear;
- every listed primary file's on-disk SHA-256 matches the report.

Still do by hand: (1b) open in another DAW, and (1d) the hard-kill-mid-take
crash-safety check on disposable data. A survivor without a clean-stop report will not pass the helper even when audio is recoverable. Requires `ffprobe`, `xxd`, `shasum`, `jq`, `python3`; these are not all stock macOS tools. A pending-hash report exits nonzero and must be checked again when hashing finishes.

## October regression acceptance — repeat on the intended installed build

Use disposable sessions and copies. These rows are not pre-passed by the automated suite.

| Check | Expected result |
| --- | --- |
| ☐ Open a session from an earlier build whose generated click bed is longer than its takes; record, reopen, then continue. | Continuation follows recorded media rather than the bed. Explicit cursor/rolling punches retain the chosen position. |
| ☐ Link stereo channels, try an overlapping pair, then test with one physical input unavailable. | Overlap is refused before writing. The available stereo side records with silence on the missing side and equal frame counts. |
| ☐ Rearm a track on a later take with primary and verified backup/mirror destinations; exercise pre-roll. | All copies align to the common capture start; pre-roll channels share a boundary. Inspect every copy independently. |
| ☐ Select a foreign or legacy nonempty replica folder; try a layout/sample-rate change against an existing take. | Capture refuses without modifying existing audio; use a new empty destination or restore the original setup. |
| ☐ Interrupt a local device while optional StereoMix is active; separately test daemon device loss. | Files finalize to the interrupted endpoint, errors remain visible, completed media reloads once, and repeated STOP preserves later edits. |
| ☐ Save nonzero capture gain, reopen, and test a take whose reference is zero dB. | Capture and live references restore without a playback gain jump; a zero read from the desk is retained as a real reference. A take recorded with no desk reading shows no reference and does not follow. |
| ☐ Enable input auto-arm during playback, then feed only the right input of a stereo pair. | Playback alone never arms; live right input arms the pair together when stopped/unlocked. |
| ☐ Disconnect/reconnect a desk after entering virtual soundcheck. | The saved stage patch survives; restoration remains available when desk state is unknown. Direct re-entry does not replace the saved patch. |
| ☐ Open delete confirmation, attempt capture/session replacement, then cancel or confirm on disposable material. | Session operations stay reserved; completion rechecks recording and the target session before deletion. |
| ☐ Open mixed root/Audio Files media; paste adjacent cross-track clips, trim to EOF, and drag a group against timeline zero. | Modern files win per track, legacy tracks remain playable, rendered clips retain their source, and shared bounds preserve group spacing. Missing media makes export fail visibly. |
| ☐ Exercise grouped cut/paste, locked consolidation and healing. | Locks, peer offsets, source channels and fades survive; incompatible healing or rendering fails without destructive replacement. |
| ☐ Lock the session and send OSC/MCU commands, including a fader move on either stereo half. | LOCK blocks prohibited commands; when unlocked, paired gain changes remain linked. |
| ☐ Switch between sessions with different routing state and disconnect/reconnect a configured mirror. | No prior-session state leaks; explicit input None and configured offline destinations survive restore. |

Also reopen the five-hour take and zoom to a few-minute span to observe the detailed waveform path. Repeat L.1/L.2 for live behavior.

## September regression acceptance

### 2026-09-30 capture-integrity delta (previous `672456d` package)

- [x] Install and verify source build `c563b00` and matching helper on both Macs (2026-10-04). Hashes, signatures and startup checks passed; old app backups/installers were removed as requested. See [INSTALL.md](INSTALL.md). This completes installation only; the hardware checks below remain open.
- [ ] During a disposable daemon continuation, send START again while rolling. It must refuse without moving the live playhead, changing the take's base position, or clearing any already-latched capture warning. STOP must remain responsive while a status client is slow or disconnected.
- [ ] Finish a clean continuation after a take with a known capture warning. The final `session.report.json` must list and hash all earlier and new parts and retain the earlier warning. Run `verify_take.sh` on a clean continued session and on a deliberately gapped disposable copy; only the complete, warning-free copy should pass.

### 2026-10-29 bug-hunt delta (`281c6c0` and later; disposable sessions only)

- [ ] During a selection punch and a manual punch while rolling: press cue digits, Cmd+digit, numpad +/- and the Loop button. The playhead must not move, Loop must not turn on, and the status line must say the playhead is locked. After STOP, compare the punched region against where it was played.
- [ ] Manual punch while playback rolls on a 56-track session: the punched audio must line up with the backing at the moment RECORD was pressed (no early splice).
- [ ] Continue a long take with one newly armed track and with an empty backup drive added: RECORD must start at once (no freeze); after STOP the lead-in files appear and the take plays aligned. Quit the app hard only on a disposable copy mid-take, reopen, and confirm the lead-in is completed.
- [ ] Open a session prepared on a laptop (fewer inputs) on the show interface: every strip must record its saved SD5 input, not a wrapped one.
- [ ] LTC chase at 25 fps from the real source: LOCK only with timecode present, correct position, LOCK drops when timecode stops; program audio on the LTC strip must never LOCK.
- [ ] A remote RECORD sent twice during a take must not stop it. A Yamaha/A&H console link unplugged mid-session must show as lost.
- [ ] Split a clip inside a fade-in and inside a fade-out, play and bounce: no audible change at the split.

### 2026-09-24 punch and import delta (historical build; repeat applicable checks on the current DMG)

- [ ] On a disposable session, start with loop playback enabled and select a very short punch range. RECORD must pass through playback pre-roll, replace only the selected samples, continue post-roll and stop; the loop must not wrap before punch-out, and its former enabled state must return afterward.
- [ ] While punching a known existing take, listen to both master and any configured direct outputs at safe level: hear the old take before/after and the live input **instead of old + live together** inside the punch. Manually punch in/out with RECORD or STOP once, reopen the same session, and compare before/new/after audio and original file path.
- [ ] Punch and continue with a newly armed track after an existing take. The new track must be silent before its start and sample-aligned on playback/export. Since 2026-10-29 RECORD must start immediately even on a long take: the silence is written after STOP as `Track_NN` and the new audio is `Track_NN_part02`; check both exist and that `Audio Files/Pending Lead-ins/` is gone after STOP.
- [ ] On disposable primary + backup/mirror copies, attempt a punch with a newly configured copy missing the original base. Capture must refuse without changing the primary. After populating the copy, punch again and inspect every result independently.
- [ ] Simulate an interrupted punch only on a disposable copy: retain `.punchbase`, leave a partial replacement, reopen, and confirm the original is restored and the partial is archived under that session's `Session File Backups/Interrupted Punch*`. Never perform this test on show material.
- [ ] Import a known four-channel source into a disposable session. Confirm four separate mono tracks, correct channel order, sample rate, playback routing and export, with no channels silently dropped.

- [ ] With primary/backup roots aliased or overlapping, recording refuses before primary files are touched.
- [ ] During local and daemon takes, arm/input/session replacement attempts leave capture unchanged; STOP completes only after acknowledgement.
- [ ] Two daemon takes preserve the first audio and create correctly routed continuation parts; status/time and session path agree after GUI reattachment.
- [ ] Move/delete mixed mono/stereo tracks; reopen and verify media, UUID-linked cues and clips/takes. Removed audio remains archived, and old undo/clipboard state is cleared.
- [ ] Import into an edited session; verify splits/fades and intentionally empty tracks survive. Paste a stereo source channel to an unrecorded track beyond the original duration; verify playback/export and missing-source silence.
- [ ] Check locked split/ripple/crop behavior, sorted marker naming and analysis menu commands.
- [ ] Reopen capture settings/sample rate and check recovery file counts. Exercise loop boundaries for audible gaps.
- [ ] Follow the exact-rig rehearsal and backup checks in SHOW-READINESS.md. Record results; no checkbox is pre-passed by the automated suite.

### 2026-09-20 reliability follow-up

- [ ] Put an existing `Track_01.wav`, `.flac`, `.aif`, or `.aiff` in a disposable session whose project metadata cannot be read. Press RECORD as a fresh take: it must refuse without changing the file. Explicit Continue must create a continuation instead.
- [ ] Configure a missing/unwritable backup and one valid mirror. RECORD must either refuse an unsafe primary or roll with the valid primary while showing the unavailable copy. The warning must remain visible after STOP; the selected backup path must survive relaunch.
- [ ] During a disposable take, disconnect a redundant drive. Primary capture must continue, the runtime failure must latch, and STOP/report must not claim clean redundancy. Reconnect and confirm the next take resets health only after writers open.
- [ ] Put primary, backup, and mirrors on combinations of the same and separate physical volumes. Confirm minutes remaining reflects aggregate byte rate per volume and does not count failed/skipped copies.
- [ ] Locally arm StereoMix, assign stream sends, leave physical stream outputs unassigned, and capture known signal. The StereoMix file must be audible. Remove all stream sends and confirm RECORD refuses the empty mix. Daemon mode must refuse StereoMix with a clear explanation.
- [ ] While daemon recording is active, try Play from the toolbar, keyboard, MCU, OSC and timecode chase. Every path must leave playback stopped. Consolidate must also refuse.
- [ ] Force a daemon configuration error, correct it, then record. The callback must be restored and the valid configuration must capture. STOP must distinguish a completed take with a finalization warning from a take that may still be rolling.
- [ ] Run Consolidate on long material, then change session or source state before completion. Stale results must not apply. Pre-create `_999`; Consolidate must create `_1000` or later without replacement.
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
   names/Trim-Follow update, and, during a take, a marker drops. Outside a take
   no marker drops. Console dialect input is
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
