# Decisions Log


## Cut to recorder and virtual soundcheck — 2026-11-02

**Status:** Accepted. The owner's call, after a feature census of everything the app had grown.

**Context:** The app's job is to record a show and play it back through the same outputs for virtual soundcheck. Around that it had grown VCAs, buses and mix automation, a tempo and click system, comping and analysis tools, and several network services. Each extra feature was more code near the audio callback and the session files, and more to prove on a real rig before a show.

**Decision:** Cut the app to a recorder and virtual soundcheck.

What went: AAF export, Noise Report, spectral auto-name and the mini spectrum, cloud upload, Session Mirror, NDI, the companion server (state, confidence page, `/stream.wav`, `tools/auto_stop.sh`), MP3 export, the Session Info text fields, the MIDI surface Device Type, comping and take actions, Strip Silence, Normalize, Tab-to-transient, the Scrub tool, VCAs, buses and aux sends, mix automation, the tempo map, Tempo lane, TempoBar, time signature, cue tempo curves, Cue Fade, the click, MIDI clock, the phase meter, TimelineStrip, dead buttons and dead dialog controls.

What stayed: capture, the capture daemon, MTC/LTC chase, VSC, PATCH, Loop, faders, pan, mute, solo and master, edit groups, STREAM sends and StereoMix capture, the setlist and cue mix snapshots, markers, Export Songs, Detect Songs, the QC report, Write soundcheck report, Reaper and DAWproject export, Show Handoff, CSV export, templates, the Meterbridge, LUFS readout, LOCK, the alarm banner, pre-flight, Split, Heal, Crop, Ripple Delete, Clear Range, Consolidate, clip gain, fades, crossfades, clip mute, lock and rename, snap, Mackie and control surfaces, generic OSC, desk macros, the console link and soundcheck, Scene Follow, Trim-Follow and Twin Recorder.

Merged rather than cut: Backup and Mirror Drives are one dialog with a real backup format box. Backup's default stays WAV 24, because a capture default never changes to suit a dialog. Four export items are one Export Audio dialog that calls the render paths already there.

Details that follow from the cut:

- **The playlist model stays.** Session files carry takes, and Punch, continue, undo, Crop, Ripple and export read the model. Only the UI that made and switched takes went.
- **`referenceMedia` stays for the recorder only.** The take length skips such a strip. Without that, RECORD after STOP on a session with a generated click would start at the end of the bed and pad every take with silence, and this work must not change capture.
- **Write soundcheck report keeps its band energies,** so the FFT feed in the recorder stays for that alone.
- **Tab stays consumed with nothing behind it.** Left to JUCE it walks focus onto a routing combo or a transport button, where the next arrow or Return changes it mid-show.
- **EDIT row routing joins the linked rule.** Its combos call `setTrackLinkedRouting` on both sides, like MIXER and PATCH, so the three views stay linked.
- **A session from an earlier build keeps its originals.** Nothing loads the removed data, so the first save would drop it for good. On open, once, the session's documents are copied to `Session File Backups/Before Feature Removal/`, before anything can overwrite them. Detection reads values, not keys: earlier builds wrote these keys on every save at their defaults, so a key alone would flag a session that never used these features. A save is refused when the copy fails, because a failed copy followed by a save would lose the data the copy exists to keep.
- **A session copy takes the session.** Save As, Move and Show Handoff skip writers' staging files, which can vanish at any moment, and a waveform cache that can't be read no longer fails a copy, since it costs only a rescan. Any other file that can't be copied still aborts, and the comparison in Show Handoff skips the same files.

**Consequences:** Sessions from earlier builds open, play and save, and lose nothing on disk, but their removed data no longer does anything. The overnight soak needs a manual stop. DAW projects carry a default 120 BPM in 4/4 and no notes about what used to be left out. None of this has been installed or run on real hardware: see [FIELD-TEST.md](FIELD-TEST.md).

## A copy never refuses a take, and a punch commits only if complete — 2026-11-01

**Status:** Accepted.

**Context:** RECORD opened every destination before go-live, so a sleeping backup drive cost the opening seconds on every copy, a dead mount could hang RECORD, and a bad copy could refuse the show. Separately, a punch that lost audio was spliced in anyway and the original deleted.

**Decision:** The primary is the take. Only the primary must open before go-live; each copy claims and opens on its own thread afterwards. A copy that fails any rule, including holding another session's audio, sits the take out untouched and is named in the banner and report. Identity rules are unchanged: an unverifiable copy is never adopted. A punch commits only if its capture is complete: a hole in the primary rolls every copy back and archives the punch audio; a hole in one copy leaves that copy on its pre-punch take.

**Consequences:** A misconfigured backup no longer stops the engineer at RECORD, so the warning has to be seen: it is a standing red banner and a line in the report. An engineer who wants a copy of every take must read that banner. A punch can be lost to a stall, but the original never is.

## RECORD punches only what the engineer chose — 2026-11-01

**Status:** Accepted. Amends the Trigger paragraph of *Punch-in is an offline splice on stop* (2026-06-14).

**Context:** A punch is the one thing RECORD can do that destroys audio: the replaced span is gone from every copy once the splice commits, and recording is not undoable. RECORD chose to punch from state nobody had chosen. The Loop button stored a whole-take region and left it behind, and `session_settings.json` kept it across reopens. PUNCH mode held a private per-index copy of the arms that ignored the R buttons, did not follow a strip move and survived a session switch. A remote RECORD followed the same punch rules as a local press. A stopped playhead left inside the take also punched, although the ADR only ever named a rolling playhead or a placed cursor.

**Decision:**

- The loop region does three jobs: loop range, EDIT selection, punch range. Only the third is dangerous, so it needs a mark of its own. `SessionPlayer::selectRange` is the only way to arm a range, and only a gesture that sets both edges calls it: a Selector drag, or moving an edge of a range that already exists. `setLoopRegion` makes a region that loops and edits but never punches. New code that forgets the difference fails safe: RECORD appends.
- The Loop button makes no region at all. Loop-play with no region wraps the whole session, so the implicit region cannot count, cannot be saved and cannot outlive Loop, because it does not exist. Every `hasLoopRegion` caller (ruler, rows, edit commands, menus) is right without being touched.
- A saved range comes back through `setLoopRegion`: it keeps working for Loop and edits and never arms a punch until the engineer touches it. A saved region that spans the whole timeline is dropped on open, because the earlier builds' Loop button is what wrote those. A region written before a take was continued no longer spans the timeline, so it comes back as an ordinary unarmed range, which is safe but may look like a stale selection.
- Half a range arms nothing. Range In or Range Out alone, Set as Loop In/Out with no range, and the default 2 s range all use `setLoopRegion`.
- One selection, one punch. A selection punch disarms its range when it starts, because the next RECORD may be the one meant to carry on with the show. The range stays visible for Loop and edits. A refused start keeps it armed.
- A parked playhead never punches. Only a rolling playhead or a placed EDIT cursor does; anything else appends, which never touches the take. Without this, disarming the range would only have moved the danger, because a selection punch leaves the playhead mid-take.
- A stopped cursor asks. A cursor takes one click to place and can be left anywhere, so with the transport stopped, RECORD at a placed EDIT cursor asks first (*Replace audio?*). Return is Record After Take, which clears the cursor and appends. Replace From Cursor punches. Esc cancels. The answer is a fresh press through every refusal, and does nothing if the transport moved while the question was open. A punch during playback and a selected range do not ask: both are deliberate. A remote press is refused, so it never reaches the question (`MainComponent::cursorPunchNeedsAnswer`).
- PUNCH mode has no arm state of its own. It punches the armed tracks at punch-in, as the R buttons show, and `startRecording` freezes them as in any take. It holds loop-play off from inside `AudioEngine::setPunchModeOn`, it ends in `clearSessionState`, and only a take it opened can be ended by crossing out of the range or by turning the mode off. A live take keeps its two-tap STOP. In PUNCH mode the mode itself is the arm, so each pass through the selected range punches until the mode goes off.
- A loop that is already on stays inert for a punch take, because `SessionPlayer` will not wrap while the seek lock holds.
- The main window says so: a pill beside the transport time shows PUNCH MODE, or which RECORD will replace audio (range, cursor, rolling playhead), using the same rules RECORD uses. Daemon mode shows nothing because a daemon take is always a fresh take, which cannot punch.

**Alternatives considered:** Keeping an implicit region with a flag (rejected: every `hasLoopRegion` caller would have to learn to ignore it, and edit commands would still see a whole-take selection). Leaving a selection armed after a punch for quick retakes (rejected: the cost of re-dragging a range is seconds, the cost of a stale arm is a missed show; PUNCH mode keeps the retake workflow, visibly). Confirming every punch with a dialog (rejected: a modal in the transport path, and a rolling punch and a selected range are deliberate; only the stopped cursor asks).

**Consequences:** RECORD can now append where it used to punch, for example at a marker reached without placing a cursor. That is the safe direction and the status line says what happened. Re-punching the same range means selecting it again, or using PUNCH mode. A deliberate cursor punch takes one more click, and Return never replaces audio.

## No thread is killed — 2026-11-01

**Status:** Accepted.

**Context:** JUCE kills a thread that outlives a `stopThread` timeout (`0` kills at once), with its locks held and its writes half done. `~TimeSliceThread` does that after 2 s and `juce::ThreadPool` after 500 ms, so a thread that was merely slow at shutdown could take the process down.

**Decision:** Every `stopThread` in `Source` takes `-1`. Every `juce::TimeSliceThread` is stopped explicitly before its destructor runs: recorder writers, player reader, mix writer, EDIT scan threads. A job that needs a worker uses `Source/UI/SerialWorker.h`, which always joins, not a `ThreadPool`. The gate *no thread can be killed by force* enforces all of it.

**Consequences:** A thread stuck on a dead drive now makes quit wait rather than crash.

## LOCK covers quitting — 2026-11-01

**Status:** Accepted.
**Context:** File > Quit refused under LOCK, but Cmd+Q and the window's close box went straight to `confirmAndQuit`, where Return means Stop & Quit, so two keys ended a locked take.
**Decision:** `confirmAndQuit` refuses under LOCK, idle or not, the same as the menu item, and says to unlock.
**Consequences:** A locked app also refuses a logout's request to quit, so macOS reports that it would not close. The engineer unlocks first. Under LOCK the system cannot end a take behind the engineer's back.

## Report scans have their own reservation, and a take cancels them — 2026-11-01

**Status:** Accepted. A deliberate second instance of the `sessionJobs` pattern.

**Context:** The noise, QC and song scans ran on detached threads and read every take file, which should not continue under a take. CLAUDE.md puts unbounded session-file jobs on `sessionJobs`, but that runner shares `sessionIoBusy`, and RECORD refuses while `sessionIoBusy` is set. A scan on it would make RECORD wait for a report, which is worse than the bug.

**Decision:** Scans run on `analysisJobs`, a second `SessionJobRunner` with its own `analysisBusy` flag: owned, joined, cancellable and structurally released, without touching `sessionIoBusy`. RECORD never waits for one. `cancelAnalysisForTake` runs on the 10 Hz timer, because a take can start from the button, a remote, a daemon or a punch window; the scan stops at its next read and writes no report. `analysisResultWanted` is the single gate at delivery: no modal over a take or finalization, and none for a session that is no longer open.

**Consequences:** A scan can overlap up to about 100 ms of a starting take. A session job can start during a scan, which only reads. Leaving a session cancels any scan.

## Each copy of a take has its own thread and ring — 2026-11-01

**Status:** Accepted (supersedes *Single writer thread for capture*)
**Context:** All copies were written in sequence on one thread with one FIFO per channel. A NAS mirror or dying USB backup that blocked a write for about 5.5 s put silence on every channel of every copy, the healthy primary included, so a redundant copy could damage the copy it was added to protect. STOP blocked on the same call.
**Decision:** The shard thread becomes a distributor that does no disk work. Each destination has its own thread and its own ring. A copy that falls a ring behind overwrites its own oldest audio and writes silence for it when it returns. STOP waits for the primary without limit and for a copy while it makes progress, leaving behind one that stalls for 30 s. All of a copy's state is shared, so its thread can outlive the recorder.
**Why it is safe to have more than one writer thread again:** the 2026-05-25 ADR could not find the race behind the white-noise channels. It is in that commit's parent: `drainShard` ended with `for (auto& w : writers) { w.writer->flush(); ... }`, run from whichever shard crossed the five-second mark. JUCE's `flush` seeks to the header and back, so it moved the file position under another thread's write. That landed audio at misaligned byte offsets, which read as full-scale noise, worse with more channels. That loop is gone. A writer now belongs to exactly one thread, and that rule is the fix. ThreadSanitizer runs clean on *Capture copies*, including a stalled copy on each destination, a copy left behind, and 32 tracks on three copies at live pace.
**Why a distributor, not a tee from the primary's thread:** a stalled primary would then hold up the copies. Why not several rings written by the audio thread: more work and memory on the real-time path for no gain. The audio thread's path is untouched.
**Why overwrite-oldest under a mutex, not a lock-free ring:** neither side is real-time. Both hold the lock for a memcpy of one chunk, never across a disk call, so the distributor waits on memory at most. It makes a lag a single number instead of a gap list and is clean under TSan.
**Physical volumes:** the code groups by volume only to estimate time, so writers are per destination, not per volume. Two copies on one drive compete for it, as they did.
**Consequences:** Ring memory is per take: 1 MiB per channel per copy at 48 kHz, cut down to stay under 1 GiB total, and pages are touched only as used. Two copies can drift by up to a ring while healthy; they line up by sample in the files. A stuck primary still holds STOP, deliberately: the primary is the take. Hard limits: a thread stuck in a write can't be killed, so an abandoned one lives until the kernel returns. Roll parts stamp from the estimate taken at start; the exact stamp at STOP replaces it, and a crash survivor keeps it, good to milliseconds.

## A lost capture daemon: three answers, and only proof ends a take — 2026-11-01

**Status:** Accepted
**Context:** After the daemon died or the link dropped, `lastStatus()` said recording forever, so RECORD, STOP, the toggle and Quit all refused and only a relaunch cleared it.
**Decision:** A reattach attempt (attach only, never launch) gives three answers. It answered: adopt what it says. Nothing listening: no daemon, so no take; end it and load what is on disk. Something holds the port and will not prove itself: unknown. Unknown keeps the guard and STOP says it could not confirm. Quit and turning daemon mode off work by asking, and say that nothing was stopped. Nothing kills a daemon on stale status.
**Consequences:** Retries back off to 30 s and each can hold the message thread for the 1.5 s handshake, only while something on that port does not answer. A refused connect is taken as proof. On loopback a live daemon's listener only refuses if the socket itself is gone.

## Whose mixer is in memory is tracked explicitly, and only that session is written — 2026-11-01

**Context.** The engine restores the active session's folder at launch so Save stays lit, but the recorder has no strips until the session is opened. Nothing connected the two, so `serviceAutosave`, which only checked that the folder existed, wrote the empty recorder over the real `session_mix.json` under the crash-recovery dialog, after a failed reopen, and after a daemon reattach. `trackCount: 0` is valid JSON, so the audio-count fallback never ran on the next open, and ten more autosaves pushed every good backup out of retention. The `.zfproj` and `session_settings.json` were overwritten the same way.

**Decision.** `AudioEngine` records which session the strips belong to (`mixerSession`), set only by paths that really loaded or created that mixer. Every writer of mixer-derived state goes through a guard at the write, not at its callers: `sessionmix::refusal` for the mix file, and `writeSessionMetadata` for the whole snapshot before any file changes. Separately, a zero-strip mix can never replace a file that has strips, whoever owns it. Retention keeps the newest snapshot with strips when empty mixes would otherwise fill the window. Reattach loads the session's mixer and documents, and a daemon take writes its mixer into its folder before it rolls.

**Why positive identity.** The alternative, flagging known-unloaded states, is safe only until someone adds a new way to pin a session. Here a new path fails closed: its save is refused and says so.

**Why RECORD is not refused on an unloaded session.** Capture outranks tidiness. A take in that state goes to a new folder, so nothing is lost and the unloaded session is untouched.

**Removing every strip from a saved session is refused (owner review welcome).** The rule "a zero-strip write can never replace a populated file" collides with one deliberate action: selecting all strips and deleting them. Two ways out were weighed. An exception for that action keeps a rarely used gesture, but the guard is then no longer absolute, and any future bug that reaches the same path is unguarded. Refusing is a small loss, since Close and New Session both start over, and it keeps the rule true without exceptions. Refused, before any file moves, with the message "A saved session keeps at least one strip; nothing was removed". A session that has never saved a populated mix can still go to zero.

**Consequences.** Tests and new code that pin a session and then save must claim the mixer. A session already emptied by an older build now opens with one strip per audio track. Offering its newest good backup is covered in *Damaged session files move aside at open*.

## Session copies keep their buffer on the heap — 2026-11-01

**Context.** `copyDirectoryCancellable` held a 1 MiB `std::array` on the stack and runs on a session job's `std::thread`, which has 512 KiB on macOS, so Save As, Show Handoff and a move between drives could crash the app. The code dates from 2026-08-18 and is in the installed `281c6c0`. The fault was reproduced in the test app; the installed app was not launched to confirm it. Nothing ran these copies end to end, so no test saw it.

**Decision.** The buffer is on the heap, a gate refuses a megabyte buffer on a stack, and `completeSaveAs`, `completeRelocate` and `completeShowHandoff` were split out of their file chooser callbacks so the copies now run headless in tests.

## Damaged session files move aside at open — 2026-11-01

**Context.** A truncated `session_mix.json` or `markers.json` loaded as defaults, and the next save destroyed the repairable bytes. A damaged `.zfproj` opened with no warning and then blocked every save.

**Decision.** Opening moves each unreadable file to a dated sibling before anything loads, tells the engineer, and offers the newest backup that holds a good copy. Restoring reopens the session. The name keeps the original extension out of the end of the file so nothing treats it as live.

**Why move rather than copy.** Moving a damaged `.zfproj` is what unblocks saving: the writer refuses an unreadable project on purpose, and that refusal stays. On read-only media the move fails, the engineer is told, and the file stays.

## Save As, Move and Handoff from a session that cannot be saved — 2026-11-01

**Context.** All three aborted when the source could not be saved, so a read-only or damaged session could only be copied in Finder.

**Decision.** They ask, then continue from the files on disk. Save As and Move write the in-memory state into the new copy afterwards. A handoff does not, because it is verified file by file against its source.

## Remove Last Capture moves to the Trash — 2026-11-01

**Decision.** It targets the recorded capture by path and identity, names it, and moves the whole session to the Trash. If the Trash refuses, nothing is removed; it never falls back to a permanent delete. JUCE polls up to 8 s on the message thread when a volume has no Trash. That is accepted: it runs only on an explicit, confirmed action with no take rolling.

## Cloud upload runs through a shell — 2026-11-01

**Note, 2026-11-02:** Cloud upload was removed. See *Cut to recorder and virtual soundcheck*.

**Decision.** The dialog promises a shell command, and engineers write `~` and `&&`, so `/bin/sh -c` runs it with Homebrew's folders on PATH, because launchd's PATH has neither. The session path is `$1`, never text pasted into the script.

**Consequence.** A template that relied on one word holding spaces with no quotes now gets shell splitting, as any shell command would.

## Crop and Ripple Delete move the whole timeline under one rule — 2026-11-01

**Note, 2026-11-02:** Automation and the tempo map were removed, so the rule no longer moves rides or tempo. It covers clips on every take, markers and cues.

**Context:** Both changed only the active take's clips. Rides, cues, the tempo map and other takes stayed on the old timeline, and the undo step restored only the clips.
**Decision:** One mapping (`timelinecut::Cut`) applied to clips on every take, automation, markers, tempo and cues, as one undo step holding a snapshot of each. A locked clip that would move refuses the edit (it used to stay put while everything slid under it), and so does Automation Safe on a lane with points. A cue or marker in the removed audio goes, and undo brings it back. A ripple on a subset of strips moves those strips' takes and automation and leaves markers, tempo and cue positions: most of the session is where it was.
**Consequences:** Marker and cue edits are not on the undo stack, so a step's undo keeps ones added since, at their own position, even though a position made after the cut no longer means the same place. Edits to an existing cue or the tempo map made after the step are replaced by undo. A shaped automation segment cut in two keeps its end values but only approximately its shape. Cue positions are dropped rather than clamped: piling them at the join would keep cues that point at nothing.

## Recording is kept by undo, not by clearing the undo stack — 2026-11-01

**Context:** Undo snapshots hold every track, and clearing the stack at each STOP would lose every edit's undo across a show.
**Decision:** Each snapshot records the media length it knew; `restorePlaylistSnapshot` adds what was recorded since. It is keyed on length, so any growth of a track's media counts as recorded, including a click regenerated longer.
**Consequences:** A span deleted by hand after the take stays deleted through its own undo and redo, because that step's snapshots already know the longer length.

## Offline renders ignore master MUTE and follow solo — 2026-11-01

**Context:** Engineers mute the master to keep playback out of the PA; that is monitoring. Solo also changes what is heard, but soloing to print something is a normal use of a bounce.
**Decision:** Master MUTE is out of the render; the master fader stays in. Solo stays in, with a note on the status line. A mix with no signal at all warns.
**Consequences:** `MixKernel` stays the one definition: the render passes `muted = false` to `mix::masterGain`. Live and offline deliberately differ here, alongside the differences already listed in CLAUDE.md. The DAW exports still carry master mute, since they describe the session rather than render it.

## Scene markers cut songs only when nothing else does — 2026-11-01

**Context:** A scene recall can land inside a song, but some shows run one scene per song and have no other markers.
**Decision:** Scenes are excluded when a session has any user or song marker, and used when it has none.
**Consequences:** Adding a single user marker to a scene-run show changes what Export Songs offers. The dialog shows the rows before anything is written.

## Hum gate at -70 dBFS — 2026-11-01

**Note, 2026-11-02:** The Noise Report was removed, and this gate with it.

**Context:** The `-40 dBFS` gate read an unnormalised FFT, so it really sat near -100 dBFS. Normalising alone would have put it at a true -40 dBFS, hiding clearly audible hum.
**Decision:** Normalise, and set the gate at -70 dBFS: above that, hum is audible in a quiet passage at ordinary gain.
**Consequences:** A judgment value, not a measured one. It should meet a real rig before anyone relies on it.

## A native windowed-sinc converter — 2026-11-01

**Context:** `juce::ResamplingAudioSource` is linear interpolation plus a biquad. No new dependency is allowed.
**Decision:** `SincResampler`: Kaiser window, 48 zero crossings a side, aliases about 100 dB down. The kernel is worked out once for 1024 positions between samples, each output blends the nearest two, and the sum runs through vDSP.
**Consequences:** The phase table made it about 7.5 times faster than the first version, which computed every coefficient per sample. A 3-hour 56-channel file set still takes roughly 18 minutes under load. Same-rate exports are unaffected: they stay bit-exact.

## EDIT clip commands resolve their targets in one place, and a selection follows the clip — 2026-11-01

**Status:** Accepted.
**Context:** The right-click clip menu resolved a stereo clip's R half and edit-group peers, but Shift+Delete, D and Shift+click took one (track, index). Because a row paints only L's list, a half-edited pair looked right while it played out of time, and bounces carried the error. Selections were indices, so anything that shifted a list silently moved them to a neighbour: D, Cmd+Z, Delete removed the clip after the original.
**Decision:** All clip commands go through `Source/UI/ClipEditTargets.h`. A locked clip anywhere in the targets refuses Delete, Duplicate, Ripple and Cut whole, because skipping it leaves its peers changed and it behind. Selection identity is a runtime `Clip::selectionId`, never saved, and every read resolves it. Clearing the selection after each edit was rejected: it needs a hook at every mutation, several of which are in other files, and one missed hook is the original bug. Comparing clip contents was rejected because a Move or trim changes them while the clip stays the same clip.
**Consequences:** A selection drops on undo, since undo rebuilds clips. A multi-selection containing one locked clip now refuses as a whole, where it used to delete the rest. Ripple on an edit group removes the clicked clip's span from every member, so a member split differently is cut at that span's edges.

## Cmd+X on a range clears it — 2026-11-01

**Status:** Accepted.
**Context:** There is no range clipboard. With no clip selected, Cmd+X fell through to the strip cut, and a Range drag selects its strip, so a range cut removed the channel with no prompt.
**Decision:** In EDIT a range outranks the strip selection, as it does for Delete, so Cmd+X runs the Delete-range confirmation and clears the range. A strip cut always goes through `confirmDeleteChannels`. Copying a range to the clipboard is a new feature and was left out: the prompt and status say the audio was cleared, and nothing claims it can be pasted.
**Consequences:** Cmd+X on a range does not fill the clipboard.

## Rows wait for saved peaks; the cache runs on its own thread — 2026-11-01

**Status:** Accepted.
**Context:** `WaveCache.wfm` can reach hundreds of megabytes and was read and written on the message thread, which the rule against blocking it forbids. It cannot use `sessionJobs`: a reservation holds `sessionIoBusy`, and a cache save must never refuse RECORD.
**Decision:** One owned background worker on `EditPage` (`SerialWorker`, see *No thread is killed*), joined in the destructor, with a cancel token polled per megabyte. A session's rows stay blank until its peaks load, because they must be in the caches before thumbnails ask for them, or every file rescans.
**Consequences:** Lanes on a large session are blank for the moment the cache takes to read, instead of the app freezing for it. The quit-time flush stays synchronous, as it was, and runs only when the timer has not already saved.

## Long clip jobs publish only into the layout they started from — 2026-11-01

**Note, 2026-11-02:** Normalize and Strip Silence were removed. Consolidate keeps the check.

**Status:** Accepted.
**Decision:** Normalize, Consolidate and Strip Silence record the track generation at launch and publish nothing if it changed. Row Normalize is a session job on the host, so reorders from the UI are refused while it runs, and the generation check covers what bypasses the UI.
**Consequences:** A job overtaken by a strip move fails with a status line and no undo step.

## A console's show patch is saved before the desk moves — 2026-11-01

**Context.** The stash lived in memory until someone captured gains, and reloading the session's file erased it. A crash in SOUNDCHECK stranded the desk on card returns, and the next SOUNDCHECK stashed those card returns as the show patch. The state also outlived the session, so Restore could write the last show's preamps to this one.

**Decision.** `console_state.json` gains `deskPatch` (`soundcheck` or `stage`). The stash is written before the first card-return message, and a failed write leaves the desk where it is. SOUNDCHECK refuses with no session open. A read where every block is already on card returns is refused. The link's state follows the active session through `setSessionDir`, polled from the 10 Hz timer and called before every console action, instead of a hook in each session-swap path. The one exception is a desk this run moved: its stash is about the desk, not the session, so it survives the change until STAGE restores it.

**Consequences.** A session opened after a crash offers STAGE straight away. Reopening session A after STAGE was restored under session B still says `soundcheck` in A's file; going to STAGE then writes the same show patch again, which is harmless if the rig hasn't changed. A file with no `deskPatch` behaves as before (Unknown plus a stash offers STAGE). `Reconnect preserves stage patch` now sets a session first, because the old behaviour it exercised (moving a desk with no session open) was the bug.

## Network OSC is bounded and takes turns — 2026-11-01

**Context.** Every parseable datagram became a message on JUCE's unbounded FIFO queue, so a sender faster than the message thread put timers, meters and STOP behind an ever longer line.

**Decision.** Packets wait in a per-receiver backlog (2048 packets, 4 MB) and drain 4 ms at a time, then requeue. Overflow is dropped on the reader thread, counted, and logged 10 times a second at most. Markers from the network are held to 8 at once, then one a second.

**Consequences.** A burst past 2048 packets while the message thread is stalled loses the excess, as UDP may anyway. The largest real burst we know of is a desk's name dump of a few hundred short packets. A real engineer can't reach the marker limit by hand, but a desk programmed to fire more than 8 scenes in under a second would lose markers past the eighth, and the status line says so.

## `session.report.json` is locked across processes — 2026-11-01

**Context.** The GUI's twin merge and the daemon's hash publish are separate processes doing read-modify-write on one file, so either could undo the other within a window of milliseconds.

**Decision.** `flock` on a file in the per-user capture folder, one lock for all sessions, held alongside `publication`, with a 3 s bounded wait. `juce::InterProcessLock` was rejected: it uses `fcntl` locks, which don't exclude within a process (so it can't be tested without a second process), and which a process drops when it closes any descriptor on the file.

**Consequences.** One lock for all sessions is fine: each hold is one small read and write. Past the 3 s wait the writer goes ahead, so a stuck holder brings back the old race rather than losing the report.

## A mix bus stays off patched outputs — 2026-11-01

**Note, 2026-11-02:** The click was removed. The rule stands for the master and stream buses.

**Context.** Strip N patched to output N is the standard virtual soundcheck, and this app makes that patch from the strip combos, PATCH and import (`setTrackLinkedRouting`). The master pair defaults to Out 1-2. Direct routes and the master sum add into the same channels, so on a rig whose outputs are all console returns, console channels 1 and 2 carried their own track plus the whole mix at master level. There was no way to give the master no output. The app cannot tell a console return from a speaker.

**Decision.** On a device with more than two outputs, the master bus and the click stay off their pair when any strip is patched to either output of it, and the stream bus likewise. It is one decision for the pair: holding only the shared channel would leave half a mix on the other console channel. It reads the patch, not what is audible, so a momentary mute cannot release the mix. A two-output device is exempt: there the pair is the monitor, and it has nowhere else. The master can be set to Off. An opt-in setting, default off, restores summing for a rig whose first outputs are monitors. Master MUTE still kills the selected pair: that guard is not weakened.

**Consequences.** A multi-output interface used for prep with speakers on Out 1-2 and strips patched 1:1 now hears only strips 1 and 2 until the engineer moves the master pair or turns on the setting. Import makes that patch, so this will be met. It is the safe failure: too little signal on a monitor, against the whole mix on a PA channel, and the master strip and status line say what happened and what to do. On a held pair there is no click anywhere until a spare pair is chosen. Meters, LUFS, the companion stream and NDI carry on from the monitor mix.

**Rejected.** Warning only: the wrong signal still reaches the PA. Moving the master off when VSC is pressed: PATCH and import make the same patch by other paths. Keying on routes beyond the device's outputs: a 16-out rig with 32 strips would still put the mix on channels 1-2.

## Trim-Follow follows a move, never a value — 2026-11-01

**Context.** The offset was live minus capture, with 0 as both "the desk read 0 dB" and "the desk never reported". A take with no reading left capture at 0, and the first push on reopen became the offset: a 1 dB nudge moved the channel by the preamp's whole value, on direct outputs, master and stream.

**Decision.** Explicit Known flags on both values, saved with the session. No reference: 0 dB offset, and the UI names the channels. A take started with no reading on a strip clears that strip's reference, even on a continue: the new audio's gain is unknown, and not following is the safe failure. A session from before the flag reads a stored 0 as no reference, so a real 0 dB reference in an old session stops following until the next take. The offset is limited to +12 dB up and -30 dB down. Louder is the PA risk, so it gets the tighter limit; a real move past it under-follows and the status line says so. The step is not ramped: a preamp move on the desk is not either, and the limit bounds it.

A real zero read from the desk is still a real reference; only a file from before the flag cannot prove one.

## Solo stays out of the recorded mix — 2026-11-01

**Note, 2026-11-02:** The stream rule stands. VCAs and the Mute lane were removed, so only the strip mute applies.

**Decision.** The stream bus and StereoMix.wav ignore strip and VCA solo; mutes and the Mute lane still apply. The file is a record of the show, and an engineer checking a mic must not empty it without warning. The earlier code comment calling the send post-solo described the code, not a decision.

## The player owns its disk reader — 2026-11-01

**Context.** `juce::BufferingAudioReader` buffers ahead of the last read and notices a new position only when a read has missed, with an idle reader polled every 100 ms. Each loop wrap, locate and far edit lost up to 100 ms per track, by a different amount on each.

**Decision.** `PlaybackReader` replaces it: the same RAM-block design, plus a cue, two read positions and `isBuffered`, lock-free on the audio side. The player cues known jumps ahead of time and holds unknown ones, silent and with the position still, until every track has the target. A hold is capped at 2 s, after which the jump plays and a dead source reads as silence. A seek-locked player never holds.

**Consequences.** A locate is a short silence, all tracks entering on one sample, rather than an immediate staggered entry. RAM per reader rises from 3 blocks to 5 (32768 samples each, at 48 kHz). Idle readers poll every 10 ms rather than 100. Timecode chase and the clock see the position stand still for the hold; a slave clock gets its Continue before the audio enters, by the length of the hold.

**Rejected.** Rendering ahead on the timeline: a rewrite of clip playback, and every edit would have to invalidate it. Waking the reader from the callback: `TimeSliceThread` takes a lock to do that.

## Chase runs from the session's start timecode — 2026-11-01

**Decision.** A start timecode saved in `session_mix.json`, typed in Transport Sync or taken from the first take's BWF stamp, which lines a session up with the same timecode it was recorded against. Timecode outside the session parks: no locate, playback chase started is stopped once, and a status line says why. Manual locates survive. A hold is not an alarm: chase resumes when timecode comes back inside.

## macOS only — 2026-10-31

**Status:** Accepted.
**Context:** A Windows port was started during the market review work and stopped the same day at the owner's request.
**Decision:** The product stays on macOS, with no Windows or Linux build. The started port is kept only on the local branch `archive/windows-port-local` and was never merged or pushed to `main`.
**Consequences:** CoreAudio stays assumed, `Source/Audio/ClockSource.cpp` needs no other platform, and the market comparison on platform reach stands as a known gap.

## Distribution: all rights reserved, Developer ID, and the JUCE licence — 2026-10-31

**Status:** Accepted for the repository; the licence purchase is the owner's open decision.
**Context:** The repo had no licence file while its DMGs were public, and the app was only ad-hoc signed. JUCE is AGPLv3 or commercial.
**Decision:** `LICENSE` reserves all rights, `THIRD-PARTY-NOTICES.md` lists every shipped component, and `tools/notarize.sh` produces a Developer ID build from a copy of the app. Distribution now rests on a JUCE licence rather than the AGPLv3: the two cannot coexist with a closed product.
**Consequences:** At JUCE's free Starter tier nothing is owed while revenue stays under $20,000; [docs/SELLING-CHECKLIST.md](docs/SELLING-CHECKLIST.md) lists the tiers and what has to be true before selling. Nothing has been bought or accepted for the owner. Notarization has not run, because it needs the owner's Developer ID.

## Desk macro control: a pinned source address, despite the earlier rejection — 2026-10-31

**Status:** Accepted. Narrows, for this one listener, the alternative rejected in *Inbound remote control is authenticated or read-only* (2026-08-18: "Source-IP allowlist (rejected: spoofable/brittle across DHCP)"), and the sentence there that console transport control "is intentionally removed" from inbound listeners.

**Context:** Generic OSC needs a per-start text token as its final argument. A DiGiCo Macro OSC device can send only address-only or integer messages, so a desk macro can't reach it, and the console dialects deliberately can't drive transport. Engineers want RECORD and marker keys on the desk.

**Decision:** Add an opt-in listener with its own UDP port (default 9301) that accepts datagrams from one configured IPv4 source, checked on the reader thread before parsing. It takes only eight exact addresses, address-only or integer. It is off on every launch. It refuses under LOCK. Every transport command goes through `performRemoteTransport`, so RECORD runs full pre-flight and STOP keeps the two-tap guard. Identical commands inside 250 ms count once.

**Residual risk, stated plainly:** UDP has no handshake, so a source address is not an identity. Any host on the LAN can forge a datagram that appears to come from the desk, and an attacker who can see the traffic gets the address list too; the messages are plaintext and carry no secret. While the listener is on, such a host can do what a desk macro can: start a take that pre-flight allows, play, pause, drop markers, move the playhead outside a take, and STOP (needing two sends more than 250 ms and less than 2 s apart, which a deliberate attacker can do). It can therefore end a live take. It cannot arm, mute, patch, rename, delete or overwrite anything: a fresh take never replaces a take, a remote RECORD that would punch is refused (see *RECORD punches only what the engineer chose*), a remote PLAY is refused while PUNCH mode is on, and a stopped take is finalized normally.

**Correction, 2026-11-01:** this paragraph used to say that without the remote punch refusal behind it, and it was not true: a remote RECORD followed the same punch rules as a local press. Since then `handleRemoteTransport` marks the press as remote and `runRecordPlan` refuses a punch from it, on every remote surface.

**Why that is acceptable here:** the 2026-08-18 rejection was about making an allowlist THE authorization for the whole Generic command set, on by default for any OSC client, with arm, mute and channel control behind it. This listener is different in each of those: the engineer opts in on every launch, it is reachable only from one named address, and its command set is the short list above, each behind an entry point that already refuses for itself. A show network is normally a closed console network, and an attacker on it can already send a desk anything the desk accepts. A DHCP change fails closed: the desk stops being heard, and the setup panel shows the count of dropped datagrams and the latest sender.

**What would invalidate this:** adding any command that can arm, mute, patch or touch files; accepting a subnet, hostname or wildcard; or starting the listener automatically. Each needs a new decision. An engineer on a shared or venue network should leave it off.

**Alternatives considered:** A token in the address path (rejected: it would have to be typed into a console macro, then sits in the show file, and does not change per start). Treating integer arguments as a PIN (rejected: a static number over plaintext UDP, security theatre). Using the console link's connected socket (rejected: DiGiCo macro devices send to a configured target and we cannot verify they reply to source ports).

**Related documents:** `Source/Network/DeskMacro.{h,cpp}`, `Source/Network/OscPacketReceiver.h`, `Source/Tests/ConsoleVscTests.cpp`, README *Desk macro control*.

## Scene recalls: a shared rule, and LOCK left as each path had it — 2026-10-31

**Context:** A recall always dropped a marker, so a virtual soundcheck left strays and never moved the playhead. The rule existed in two places.

**Decision:** One decision function (`scenefollow::decide`) behind `RemoteControl::onConsoleScene`, called by both paths. A take marks; otherwise no marker; Follow is opt-in. LOCK refuses Follow on both paths, but the OSC listener's existing gate also stops a recall from marking a take under LOCK, and the console link's never did. Both were left alone.

**Rationale:** Opening the listener's gate weakens a guard. Adding one to the link would silently lose a locked show's markers, and a locked show is the normal state during a show. The difference is pinned by a test.

**Consequences:** Engineers on the OSC listener who LOCK during a show still get no scene markers, as before. If the owner wants scene markers through LOCK on that path, that is a decision to open the gate for one message.

## The EDIT cursor is cleared by a remote locate — 2026-10-31

**Context:** PLAY and RECORD prefer a placed EDIT cursor when stopped, so a Follow or macro locate would be undone by the next PLAY.

**Decision:** `RemoteControl::locate` clears the cursor rather than moving it. A moved cursor at sample 0 would read as a deliberate punch from the start; a cleared one at 0 appends a new part, which never touches a take. Cmd+1..9 marker jumps are unchanged.

## 2026-10-31: One post-close write, 8 bytes, compare-and-swap

**Context.** A BWF time reference must name the file's first sample, but JUCE fixes the bext block at writer creation and writers open before the first live block. The rule until now was that a closed capture file is never touched.
**Decision.** Open with an estimate, and after close patch only the 8 TimeReference bytes, in place. The patch walks RIFF/RF64 chunks, requires a whole bext block, and writes only if the bytes hold what this recorder stamped at open. It runs before hashing. A refusal is reported and never marks the audio bad.
**Alternatives.** Rewriting the file through JUCE (`replaceMetadataInFile` can fall back to copying the audio, which is unacceptable); an offset in the report only (a DAW would never see it); keeping one writer open past stop (the header flush would rewrite the block).
**Consequences.** One narrow exception to "closed capture files are immutable", and it stays the only one. A crash leaves the estimate, not the exact value: for a take that was stamped from a timecode hint that estimate is still timecode-derived.

## 2026-10-31: The dead-channel alarm keys on exact zero, not on silence

**Context.** Silence is normal in a show, so a level threshold would cry wolf all night, and an alarm nobody trusts is worse than none.
**Decision.** Only exact digital zero counts. A mic or DI always has a noise floor; a lost path delivers zeros.
**Consequences.** A converter or console that sends a stuck non-zero value is not detected. A console that sends real digital zero on purpose (post-gate or muted direct outs) raises a false channel fault. The alarm observes and never touches the take, so a false one costs attention, not audio.

## 2026-10-31: Clock-source reads go through a bounded probe

**Context.** `AudioObjectGetPropertyData` is a synchronous call into coreaudiod and a driver with no timeout.
**Decision.** The HAL is only read on the probe's own thread; callers wait a bounded time or not at all. One read at a time, so a stalled driver costs one parked thread. It deliberately cannot be joined: the thread owns only `shared_ptr` state and may outlive the probe.
**Consequences.** A report can say `unavailable` for a take stopped before the read finished. This is the one thread in the capture path that may outlive its owner, and it touches nothing else.

## DAW interchange: capture, resolve, write; automation stays out — 2026-10-31

**Note, 2026-11-02:** "Automation stays out" no longer applies: there is no automation. The capture, resolve and write steps stand, and projects carry a default 120 BPM in 4/4.

**Context.** The only DAW handoff was a CSV. Two formats had to describe the same session, and a project file that mis-describes a show is worse than none: a mix engineer would trust it.

**Decision.** A shared model, built in two steps: plain values captured on the message thread, then a worker resolve that opens take headers, cuts clips at part boundaries, collapses stereo pairs and refuses any missing media. Both writers read only the resolved model. Audio stays by reference in both formats. Each export reports what it approximated or left out, in a summary and inside the file.

**Automation is not exported.** Lanes interpolate in dB with tension and S-curves, a strip holds its fader before the first point, stereo pairs keep a pan lane per side, and the lanes interact with VCA and trim. Neither format can hold that exactly: both would need densified curves, and the DAWproject side could not be checked in any DAW here. It ships without it, and every export says so. The fader, pan and mute carried are the static values.

**Clip gain in DAWproject.** The format has no clip gain attribute. A gain expression lane is schema-valid, but no importer was available to test and one public converter logs it as unsupported, so it is reported as left out rather than written unverified.

**Stereo width in REAPER.** Dual-pan mode (`PANMODE 6`, side pans in `VOLPAN` fields 4 and 5) with the -3 dB law reproduces the session's per-side pans exactly, so pairs carry their real image. A pair linked with both pans at centre is a mono sum in the session and stays one in REAPER. DAWproject has one pan per track and carries the balance, with a note.

**Consequences.** Automation-heavy sessions need a stem bounce to keep their moves. A session with hand-drawn tempo changes keeps only its base tempo in the project.

## Twin Recorder link: authenticated, not encrypted; a dead link never acts — 2026-10-31

**Context.** The owner runs two Macs that can each receive the same console feed. A second machine that starts and stops with the first protects against the failures a single machine cannot survive: a computer, a drive, an interface. It does not protect against a bug in this app, because both Macs run it, so it must never be presented as an independent recorder.

**Decision.** A leader connects over TCP to a follower (default port 9310, bound only while enabled). The roles are local choices. Pairing uses a code of 80 bits from OS entropy, typed once, with mutual HMAC-SHA256 challenge/response and fresh nonces per connection, reusing `CaptureIdentity.h`. Role labels in the transcript stop a proof being reflected. The session key derives from the code and both nonces.

Every frame after the handshake carries a sequence number and its own HMAC under the session key, tagged by direction. Authenticated traffic is the requirement, and a handshake alone would not give that: anyone on-path could inject a `stop` into an authenticated TCP stream. With per-frame MACs an injected, replayed or reflected frame ends the connection, which is safe because a dropped link changes nothing on either Mac.

**Traffic is authenticated, not encrypted.** Anyone on the network can read session names, track counts and transport state, and can see when a take starts. Keep the link on a private show network. There is no TLS because it would mean a new dependency or an in-app crypto stack for metadata that is not secret; the companion ADR reached the same view. The pairing code is not stored, because no Keychain helper exists, so an app restart means pairing again.

Safety rules, in priority order: link loss never stops, starts or alters a take; the leader starts first and only queues; `stop` is sent only after the leader's confirmed two-tap STOP ended the take; the follower runs its own full pre-flight; the follower cannot command the leader.

**Smaller choices, and why.**
- *A follower take started by hand stays rolling on leader STOP.* Leaving a take rolling is the recoverable outcome; ending one the link did not start is not, and the leader says so.
- *A link RECORD that would punch on the follower is refused.* A stale EDIT cursor would otherwise replace audio in the follower's take with nobody at that Mac.
- *A leader punch does not relay.* It edits that Mac's take.
- *Stop & Quit does not stop the follower.* It is not a two-tap STOP, and the follower carries on with TWIN LOST.
- *Addresses are IPv4 literals.* `StreamingSocket::connect` resolves names with `getaddrinfo`, which has no timeout, and that would make quit unbounded.
- *A wrong code stops the leader's retries.* Otherwise it would knock once a second for the rest of the show. Re-enabling tries again.
- *One connection at a time, and a new caller is refused rather than superseding.* After a real loss the old connection times out within 3 s and the retrying leader gets in. An attacker can hold the single handshake slot for 1.5 s at a time and so delay pairing, which affects no take.

**Consequences.** An operator can be left with a follower still rolling after the leader stops: a STOP pressed during link loss is deliberately never delivered later, so the leader warns and the follower is stopped by hand. Comparison is arithmetic on two reports, with no audio comparison or gap repair. The wall-clock offset is an NTP-style estimate from millisecond timestamps, for reading two reports side by side; it is not a sample-accurate alignment, and nothing here locks the two Macs' clocks. Widening exposure: this is a new listening port while enabled, which is why it is off by default and bound only while enabled.

## Follow-up capture, persistence and control contracts — 2026-10-05

**Note, 2026-11-02:** Session Mirror was removed, and its schema checks with it. Noise analysis and spectral classification went too.

**Context.** The follow-up review reproduced 21 defects after S6, including
misordered capture gaps, stale dialog writes, unnecessary UI settings I/O and
false completion/error handling. Runtime evidence corrected the AIFF diagnosis:
JUCE rejected 32-bit AIFF creation rather than writing integer samples.

**Decision.** Preserve samples and silence in one bounded capture timeline;
allocate daemon state before callbacks; write float AIFF-C with checked final
flush; patch descriptive metadata against the latest project; record already-
applied undo without replay; batch reset transactions; and recheck capture at
chooser completion. Require distinct requested console replies, byte-correct
MIDI framing, separate OSC credentials and validated mirror schemas. Report
analysis failures explicitly and read FFT data only after publication. Auto-stop
requires an acknowledged finalization outcome before verifying same-session idle.

**Consequences.** Existing public format selections and `.aif` paths remain;
float data now uses standard AIFF-C `fl32`. Malformed metadata, incomplete remote
state and failed finalization refuse success while preserving earlier data.
The installed `443c769` pair passed all three full 563-group suites and six
operator-script fixtures. Startup observation does not certify physical capture
or interaction latency. [The repair register](docs/history/REVIEW-REPAIRS-2026-10-05.md)
contains each defect's regression and limits; [INSTALL.md](INSTALL.md) owns
installation state. Earlier decisions below retain their original evidence;
this amendment supersedes their weaker completion rules.


This file records key technical and product decisions for ZynForge Recording as lightweight Architecture Decision Records (ADRs). Each entry captures the context, decision, rationale, and consequences so future-you (or another engineer joining the project) can understand *why* something is the way it is — not just *what* it is.

When making a non-trivial decision, add a new entry below using the template at the bottom. Keep entries short and concrete. Link related ADRs by title rather than by anchor — anchors rot when sections are reordered.

---

## Owned finalization and truthful persistence completion — 2026-10-05

**Context.** A live trace placed the UI in metadata snapshot copies on an external volume. Controlled regressions then reproduced both that barrier and the recorder's drain/splice/read-open barrier blocking message processing. Recording becoming false did not establish that required metadata had saved; remote STOP and cue updates could report success after a failure.

**Decision.** End capture once at the recorder boundary and retain a finalization lease while an engine-owned worker completes media I/O and prepares reader state. Install live playback/clip state on the message thread and retire displaced readers on owned work. MainComponent holds the session guard through immutable metadata persistence on a separate serial writer. Explicit saves keep their outcomes; UI-only layout requests coalesce with bounded storage. Publish durable success after required writes/snapshot completion, retain failures, and compare current content before marking clean. A confirmed remote STOP returns a pending explanation until a later retry can report the completed result. Preserve existing recording safety confirmation and persistence formats.

**Consequences.** UI navigation can continue while capture/session mutations remain guarded. Closing or replacing a session reserves edits and waits for a successful continuation; queued callbacks cannot dereference a destroyed host. Synchronous compatibility helpers and the daemon host acknowledgement/load path remain identified residuals. Metadata snapshots are not audio backups. All three full S6 suites pass 519 groups without failures; no sanitizer reports were emitted. Final bounded disk-full verification passes; at that checkpoint the verified S6 pair was installed and left closed, its startup smoke was microphone-consent-gated. Physical-rig acceptance remains separate; [the audit register](docs/history/AUDIT-2026-10-05.md) records defect/test mappings, results and residuals.

## October audit contracts — 2026-10-04

**Note, 2026-11-02:** `referenceMedia` stays for the recorder only: nothing creates it now, and no generated click is regenerated.

**Context.** The October audits found data-alignment, session-identity and asynchronous-state failures that were not covered by the earlier capture checks. A backing bed could move the capture endpoint, unknown replica ownership could overwrite another take, and reconnect or delayed confirmation could act on stale state.

**Decision.** Persist reference-media and capture-gain identity separately from playback length/live gain. Validate stereo topology and replica provenance before writer creation, align each continuation destination, and switch pre-roll histories at one callback boundary. Reserve session I/O throughout delete confirmation and recheck the target at completion. Preserve the saved stage patch until restoration, finalize device loss once, and propagate failed source reads through export. Use one source-bound delta for grouped editing and input-only detector peaks for stereo auto-arm. Keep all preference writers in the same process-scoped test isolation mode.

**Consequences.** Unverifiable nonempty legacy replicas and incompatible capture layouts are refused without modifying their audio. Users may need a new empty replica destination or a new session. Synthetic tests cover these contracts; real device/desk recovery and the full rig remain field checks. See [the October audit record](docs/history/AUDIT_FIXES_2026-10-03.md).

## Installation retention — 2026-10-04

**Context.** Both Macs held old app bundles, rollback copies and installer images that made it easy to launch an obsolete build.

**Decision.** Install and verify the exact `c563b00` GUI/helper pair on both Macs, then honor the user's request to delete all inventoried old Recording versions. Keep the current installer and development Release output. Preserve source, recordings, session recovery data, settings and other products.

**Consequences.** No local old-version rollback bundle remains. Recovering an earlier app now requires obtaining or rebuilding that revision; Git history is retained. [INSTALL.md](INSTALL.md) is the authoritative package and installation record.

## Continued-session integrity report — 2026-09-30

**Context.** Recording another pass in the same session replaced `session.report.json` with only the latest part list and current-pass warnings. A later clean pass could make an earlier failed pass look clean, while handoff and `verify_take.sh` saw unreported audio.

**Decision.** On stop, enumerate all on-disk supported take parts for each track, report their actual frame totals, and hash every listed file. Accumulate previous report counters and capture-failure flags across passes; keep the report's immediate metadata write and asynchronous final SHA-256 pass. Publish `sha256Failed` when hashing cannot complete and surface final write/hash failure locally. A verifier checks physical part numbering and frame counts as well as hashes.

**Consequences.** One session report describes all continued passes. A hash only proves that a file matches its report; it cannot independently establish the intended performance duration, channel routing or health of external backups. A pending report is incomplete evidence. A new hash pass can recheck earlier files after an interrupted prior pass.

## STOP must not wait for status I/O — 2026-09-30

**Context.** The daemon held its command mutex while publishing status to a socket. A connected client that stopped reading could block a status write and delay a STOP that needed the same mutex.

**Decision.** Build status snapshots while holding the mutex, then release it before socket writes. Refuse a duplicate START while rolling before arming a continuation. The unattended soak helper pins the authenticated live session and performs the host's two-tap STOP. Amended October 5: require a successful finalization acknowledgment and then `recording:false` for that same session; idle alone is insufficient.

**Consequences.** A slow status subscriber cannot keep the recording state lock while STOP closes audio. A wedged network connection can still prevent a remote acknowledgement; the helper must report that uncertainty rather than claim the take stopped. Field testing remains necessary on real devices and network conditions.

---

## Observer truth and portable handoff — 2026-09-23

**Note, 2026-11-02:** The companion server and its confidence page were removed. The portable handoff stands.

**Audit clarification, 2026-09-23.** A UI timer copying a cached daemon snapshot is not a new observation. Status age is measured from the supervisor's last received push; lost links invalidate the snapshot and retain a guarded unknown-take state until the daemon's actual outcome is known. Host-controlled channel labels and mute/solo/colour overlay daemon meter data. The capture report persists skipped mirror destinations so handoff warns even when no mirror writer ever opened.

**Context.** A remote observer needs recording-health data without the risk of transport controls. During daemon capture the existing companion read the GUI's local recorder metrics, which could look healthy while the daemon was writing elsewhere. A copied show folder can also appear deliverable before the copy or source hash report finishes.

**Decision.** Serve a separate token-protected `/confidence` page with no command controls. Feed `/state.json` the cached daemon `EngineStatus` whenever external capture is active, including source and age so stale status alarms. For handoff, copy only to a new/empty folder on the owned export worker, retain an incomplete marker until every source/copy file hash matches, and publish a manifest last. The manifest distinguishes verified transfer from original-capture evidence and explicitly excludes external backup disks.

**Consequences.** A phone remains a supplementary observer, not a failsafe alarm path; sleep or network loss can prevent warnings. The handoff may consume substantial storage and time and must not be run during recording. A clean transfer cannot certify channel mapping, dropped samples, power-loss survival or separate backups; the exact-rig acceptance plan remains authoritative.

---

## Capture truth and background-edit decisions — 2026-09-20

**Amended 2026-11-01. Recover saves before clearing the crash marker.** The marker asks an older project to rebuild its clips, so it goes only once that rebuild is on disk. A save that fails leaves the session on the recovery list.

### Redundancy health is latched truth, not current object shape

**Context.** Backup and mirror failures disappeared when failed writers were cleared, and a configured path was enough to make the UI/report call a copy active. A STOP reply could therefore look clean after losing redundancy or failing final metadata.

**Decision.** Separate requested, opened, active, failed, and completed states. A destination becomes active only after directory and writer creation. Open/runtime failures latch for the whole take and cross `EngineStatus`/protocol v3. STOP reports command acceptance and capture completion separately, then lists finalization failures. Disk endurance groups byte rates by physical volume rather than by configured destination.

**Consequences.** The UI can remain alarming after capture stops; that is intentional until the next take resets the latches. New status fields require matching GUI/helper builds. A redundant copy on the same physical disk consumes throughput and capacity but is not independent endurance.

### Existing media requires explicit continuation

**Context.** The UI inferred whether to continue from a successfully loaded session. If media existed but was unreadable, the recorder opened the base filenames as a fresh take and replaced them.

**Decision.** The recorder is the final guard: before opening any fresh writer it scans all supported base-container names and refuses on collision. Only explicit continue or punch paths may write alongside an existing take. The UI treats unreadable media as protected, not absent.

**Consequences.** A damaged session may require repair or a new session before recording can resume; it will not trade existing audio for convenience. The guard is intentionally below the UI so daemon/direct callers cannot bypass it.

### Analysis edits are optimistic background transactions

**Note, 2026-11-02:** Strip Silence, Normalize and the transient analysis were removed. Only Consolidate of that list remains.

**Context.** Strip Silence, Normalize, Consolidate, and transient analysis could monopolize the message thread and could be invoked while daemon capture was rolling. Strip Silence also analyzed raw files, which no longer represented the audible arrangement after clip edits.

**Decision.** Snapshot the session identity and source clip lists on the message thread, render the arrangement on a worker, then apply only if the same session and inputs still exist. Local or external recording and another session operation block destructive work. Strip Silence analyzes the rendered arrangement (source channel, multipart/cross-track media, gain and fades), preserves locked clips, and treats all-silent output as an explicit empty arrangement.

**Consequences.** Results can be discarded if the engineer edits while a worker runs; this is safer than applying stale geometry. Callbacks must use the engine invalidation handle / component `SafePointer`. Temporary Consolidate files are removed when stale and final names are allocated without a numeric ceiling.

### Autosave success is transactional

**Context.** Autosave advanced its timing and clean marker even if a metadata write or backup snapshot failed, then waited a full interval before trying again. A snapshot taken too early could preserve a mixed generation of files.

**Decision.** Write every live metadata file first, then create the snapshot. Advance autosave timestamps and undo baselines only after both phases succeed; otherwise warn and retry after 15 seconds.

**Consequences.** Repeated storage failure produces repeated bounded warnings instead of silent false cleanliness. Snapshot creation must check every copy and remove a partial snapshot.

---

## September recording-integrity decisions — 2026-09-12

For the current September additions, see the following decisions; older entries retain their historical context.

### Journal track topology changes and retain removed audio — 2026-09-12

**Context.** Independent swap/delete paths could mismatch audio, stereo topology, UUIDs, edits and automation, and unchecked renames could leave partial state.

**Decision.** Use one `reorderTracks` permutation and `TrackFileTransaction` for media and metadata. Stage/check moves, retain recovery journals and archive removed media. Remap index-based consumers; clear old undo and clipboard state after reorder.

**Consequences.** Deleted-track recordings remain recoverable on disk and consume space. Recovery folders must not be swept as caches. Reorder is not exposed as an ordinary undoable edit. Simulated rollback coverage does not certify physical power-loss durability.

### Explicit capture configuration and acknowledged transport — 2026-09-12

**Context.** A separate recorder is unsafe if it uses different device/routing settings, overwrites prior takes or reports STOP before capture stops.

**Decision.** Protocol 2 transfers capture configuration and orders status with command acknowledgement. Freeze take arms, create continuation parts, include external capture in engine guards and require an accepted Quit before shutdown escalation.

**Consequences.** Install GUI and daemon together. Process isolation does not replace an independent recording system or hardware rehearsal.

### Software validation is not show acceptance — 2026-09-12

**Context.** Even the current 350-group Release and sanitizer passes cannot exercise the user's SD5, RME AoX-D, chosen clock/network/storage path or two-hour workload.

**Decision.** Keep software completion and rig acceptance separate. Require a three-hour, 56-input rehearsal at 48 kHz with the selected redundancy and recorded results; maintain an independent recorder for important shows.

**Consequences.** That build was rehearsal-ready. Unselected hardware/routing/storage remain open decisions; no show sign-off is implied by commit, installation or a green test report. See [SHOW-READINESS.md](SHOW-READINESS.md).

## A run flag decides whether to work, never whether to join — 2026-08-14

**Context.** The Network audit's worst finding was a process abort. `TextLineTransport` / `MidiTcpTransport` (Yamaha, Allen & Heath) run a reader thread that clears `running` **itself** when the peer closes. `disconnect()` began `if (! running.exchange(false)) return;` — so once the desk dropped the link, teardown skipped the join, the destructor destroyed a joinable `std::thread`, and `std::terminate()` took the process out. No crash dialog, no chance to save. Trigger: a desk power-cycle, a switch reboot, an idle TCP timeout — then the next Connect, profile change, or quit.

`CaptureLink::disconnect` carries a comment describing this exact bug being fixed in that file. The transports written later repeated it.

**Decision.** Join unconditionally. **The flag decides whether to do WORK; it must never decide whether to JOIN.** Encoded as invariants **rule 12**: an `exchange(false)) return` within 15 lines above a `.join()` fails the build.

**Rationale.** The seductive part is that the early return looks like correct idempotence — and in `CompanionServer::stop()` and `CaptureServer::stop()` it genuinely *was* safe, because only `stop()` ever clears those flags. That is precisely why prose didn't hold the line: the pattern is fine until someone adds a second writer of the flag, and the failure mode when they do is not a bug report, it's `SIGABRT`. Rule 12 found both of those "safe" sites the moment it existed; they're now unconditional too, so the rule is enforceable rather than aspirational.

**Consequences.** Twelve checks gate every commit. Verified both directions: with the bug reintroduced the test run dies at `exit=134` after 209 of 302 groups, leaving an `.ips` whose trace reads `std::terminate` → `TextLineTransport`; restored, 302/302. Writing the rule also reproduced the rules-1-7 blindness in mirror image — it matched the anti-pattern quoted in my own explanatory *comment* — so candidate lines are comment-stripped before they count, same as the window.

---

## Cross-thread reads of the track vector need a lock, not a convention — 2026-08-14

**Context.** `condemnAllStrips()` is the codebase's answer to "a `TrackState` can be freed while something else is reading it". It works by detaching UI **components** before the recorder's vector shrinks. The Network audit found three readers it cannot help: the companion server's `/cmd` handler and `captureStatus()` (both on worker threads, the latter once per client every 500 ms), and the OSC receiver. Each range-checked `getNumTracks()` and then called `getTrack()` — a TOCTOU against `removeLastTrack()`'s `tracks.pop_back()`, which frees the `TrackState`. The vector had no lock at all.

The invariants gate did not fire, correctly: rules 1-2 enumerate consumers that **cache** a `TrackState&`. These take one transiently. The hazard is the same; the shape isn't.

**Decision.** `MultitrackRecorder` gets a `structureLock` (a recursive `juce::CriticalSection`) taken by every structural mutator and by every off-audio-thread reader. The **audio thread does not take it** and its relationship to the vector is unchanged — this fixes the network race specifically, and pretending otherwise would be a much larger claim than the change supports.

**Rationale.** The alternative — marshalling every remote command to the message thread — is worse for `captureStatus()`, which has to return a value to an HTTP response and would need a blocking round-trip, i.e. a new deadlock surface. A short lock around a bounded walk is cheaper than that and easier to reason about.

**The second half of the same fix.** Routing those callers through the engine also closed a separate documented violation: *"mute / solo / arm mirror across the pair automatically — never half-update"* was implemented **only in `ChannelStrip`**, via its `pairState` pointer. So a mute from a phone, an OSC message, or an MCU surface left one leg of a stereo pair in the opposite state. There are now `setTrackMuted/Soloed/Armed` + `toggle*` on `AudioEngine` that do both jobs — pair mirroring and the lock — and remote surfaces have no business doing either by hand. This is the third time a rule written for the UI turned out not to cover the network entry points; the durable form is an engine method, not a note.

---

## A redundancy feature must prove the copy is somewhere else — 2026-08-14

**Context.** Reading the seven files the previous pass had only grepped turned up the most serious defect of the whole audit series, in the feature whose entire purpose is not losing audio. Mirrors write to `root/<sessionName>/Audio Files/Track_NN.<ext>`. The primary writes `sessionDir/Audio Files/Track_NN.<ext>`. Those are the same path exactly when the mirror root is the session's parent — and the mirror picker opens at `~/Music`, sessions live in `~/Music/Zynforge Sessions`, and a fresh mirror row defaults to WAV 24-bit, the same as the default capture format. So picking "my sessions folder" as a mirror — a reasonable thing to think you want — opened two `AudioFormatWriter`s on every take file and left them there for the show. Nothing checked. `applyAndClose` filtered exactly one thing: rows where no folder had ever been picked.

**Decision.** A mirror destination must be *proven to be somewhere else* before it is accepted. `MultitrackRecorder::mirrorRootRejection` refuses a root that holds the session, is inside the session, duplicates another mirror, or is the backup root, and it is applied in **two** places: the picker (so the engineer finds out at the desk, with the reason on the offending row) and record start (so a bad config restored from settings, or a session opened somewhere new after the mirror was configured, can't bite either). Skipped mirrors are counted and the recording banner reports them.

**Rationale.** Validating only in the dialog would have been the same mistake as guarding a dialog's launch instead of its mutation: the config outlives the dialog. It's persisted, and the *session* moves — a root that was fine when configured becomes the session's parent the moment the engineer opens a session from that folder. The check has to run where the writers are opened. The dialog check exists for the message, not for the safety.

**The corollary is the more general lesson.** Two of the other findings in this pass were the same shape — a component quietly disagreeing with the engine about whether something happened:

- `AudioEngine::setMirrors` persisted a mirror list the recorder had *refused* mid-take, so the settings file claimed a redundancy that wasn't running, and a restart made it appear to fix itself. **An engine setter that can refuse must report the refusal, and a caller must not persist what was refused.** `setMirrors` now returns `bool` and is `[[nodiscard]]`.
- `ClickSettingsDialog` switched the live click engine off, saved that, and closed *before* calling the generate path — which refuses mid-take. Pressing "Generate click track" during a show silenced the drummer's click and produced no file. **Do the thing first, then commit to it.** `generateOrRefreshClickTrack` now returns whether it rendered.

**Consequences.** `setMirrors` is `[[nodiscard]]`; new mirror-shaped destinations must go through the rejection rule. Five regression tests pin it, and the injected-regression check showed 6 failures with the rule neutered.

---

## The design gate's blind spot is `Theme/` itself — 2026-08-14

**Context.** `ZynForgeLookAndFeel::drawToggleButton` painted the ON-state letter with `Colours::white.withAlpha(0.95f)` over whatever accent the call site had set as the fill. That is correct for record red and wrong for everything else in the palette: solo `#FFD64D`, monitor `#4AD878`, mute `#FF7733` and the dialog toggles `#5DD87A` all have a perceived brightness above `onSignal`'s 0.55 threshold, so they want a **black** letter. White on solo yellow is roughly 1.2:1 — an effectively invisible **S** on one of the few chips an engineer scans mid-show in a dark room.

Two documented rules were broken by that one line ("Text on a saturated accent: `brand::onSignal(bg)`. Never hardcode black or white", and the bare-`Colours::white` ban). `tools/design_audit.sh` enforces both — and **excludes `Source/Theme/`**, because `Theme/` is where the sanctioned helpers are defined.

**Decision.** That exclusion is right for the *definitions* and wrong for the *call sites*. Invariants **rule 11** now bans raw `Colours::white` / `Colours::black` everywhere in `Theme/` except `BrandColors.h`, which is where `gloss()` and `onSignal()` legitimately produce them.

**Rationale.** A LookAndFeel paints for every call site in the app, which makes it the most expensive place in the codebase to hardcode a colour: the call sites did the right thing (`ChannelStrip` sets a per-function accent; `ClickSettingsDialog` even calls `onSignal` for its own buttons) and the LAF overrode all of them. Generalising: an exclusion added so a gate can express a rule becomes the one place the rule isn't enforced. Verified by reintroducing the original line and watching rule 11 go red.

**Consequences.** Eleven checks now gate every commit. Adding a colour constant to `Theme/` outside `BrandColors.h` fails the build.

---

## Guard the show at the MUTATION, not at the dialog — 2026-08-13

**Context.** The dialogs + `Theme/` pass was the last unread surface. It found `AudioDeviceDialog` — a **floating, non-modal** panel — calling `setAudioDeviceSetup(setup, true)` from three combo handlers with zero `isRecording` references in the file. A device restart runs `audioDeviceStopped()` -> `recorder.release()` -> `stopRecording()`, so nudging the buffer-size combo mid-show ended the take. `MainComponent::applySessionSettings` had guarded the identical operation for months. This is the same shape as the `condemnAllStrips` UAF and the `.wav`-only take globs: the hazard was understood, the guard was written once, at the site where it was first noticed.

**Decision.** Two things, together:

1. **Guard at the mutation, never at the entry point.** The instinct is to refuse to *open* the DEVICE panel while recording. That is wrong here and would have shipped a false sense of safety: the panel is non-modal, so the engineer can leave it open and then hit RECORD. Enablement computed once in a constructor is stale the moment the show starts. Every mutating path checks live state, the controls grey out from a 4 Hz change-gated watch, and the Cancel/destructor revert — which *also* restarts the device — is skipped while rolling.
2. **Encode it as invariants rule 10** rather than as a paragraph in `CLAUDE.md`. The rule enumerates every caller of `setAudioDeviceSetup()` / `initialise()` and demands a recording guard within 40 lines above it.

**Rationale.** Point 2 paid for itself the same day. Careful reading of the dialog files found one violation; the rule found **four** — the three combo handlers plus `applySessionSampleRate` and both new-session device pushes in `MainComponentHelp`. That is the argument for the whole gate, restated: a rule that enumerates the *consumers* of a hazardous pattern finds instances that reading misses, because reading follows the call graph you already have in your head.

**Consequences.** Ten checks now gate every commit, and `tools/invariants_audit.sh` is the file to add to when an audit finds a class rather than an instance. New device-touching code must guard or the build goes red. The cost is real: a rule mis-written is worse than no rule (two of the original seven were blind — one matched a commented-out call, one was satisfied by an unrelated comment in the same file), so **a new rule is not landed until it has been watched go red against an injected regression**. That step is now part of the ritual, not an optional nicety.

**Also fixed in the same pass**, all smaller, all real: `compactPath` labelling `/Volumes/RECORD/...` as `~/Volumes/RECORD/...` on the control that tells the engineer which drive the take lands on; raw `this` captured in `MarkerListDialog`'s and `TimelineStrip`'s async menu/modal callbacks (the SafePointer convention every other dialog already followed); a sub-44.1k device displaying as "44.1 kHz"; and `NoiseReportDialog`'s comparator not flooring non-finite keys — unreachable today because the analyzer clamps, but the comparator shouldn't *depend* on a caller's clamping when an unfloored NaN is the hardened-`std::sort` SIGABRT that already crashed the recovery dialog once.

---

## Ship console families you can't test, safely — 2026-08-13

**Correction, 2026-11-01:** Yamaha SCP sends no subscription. The earlier `ssrecall_ex "scene"` was the scene-recall verb, in a section that promises never to change a desk. Whether a Yamaha desk pushes scene notifications to a connected client without being asked is UNVERIFIED; see FIELD-TEST.

**Context.** The console layer was OSC-only, so four of the six console families in the picker were honest stubs. Going up-market means supporting desks nobody here owns — which collides head-on with the standing rule that untested desk control is the project's largest liability.

**The resolution is to separate two things that were conflated.** "Is this address model correct?" and "is the desk on the other end who we think it is?" are different questions with different answers and different blast radii.

- **Risk-tier the capabilities.** For a RECORDER, nearly all the console value is read-only: channel names label the session, scene recalls drop markers that make the show navigable next morning. Neither can change a desk. Repatch and head-amp gain restore — the features that can wreck a show — are the *least* differentiating. So the read tier generalises across every family and the write tier stays narrow.
- **Gate writes on a handshake, not on ownership.** `canWrite()` requires `dialectTrusted || dialectConfirmed`. `dialectTrusted` is a claim about the address model — documented protocol plus encode/parse pinned by tests, true only for the X32/M32 reference. `dialectConfirmed` is set when the console answers the connect-time probe the way the dialect predicts. A dialect written from published documentation therefore ships **usable read-only immediately** and **cannot write until the desk identifies itself**.

**Decision.** Transport seam (`ConsoleTransport` + OSC/UDP, SCP text/TCP, MIDI/TCP), dialect seam (`ConsoleDialect`), risk-tiered capabilities, handshake-gated writes. Adding a console family is now a dialect table plus tests, not a protocol rewrite.

**What this does NOT buy.** The plumbing is tested; the dialects are guesses. Framing, tokenising, encode/parse round-trips and the refusal itself are covered by `ConsoleTransportTests`, but no test can tell us whether Yamaha's parameter path or A&H's SysEx enquiry is spelled correctly. First contact with each desk will find things. The gate means those findings are "it stayed read-only" rather than "it wrote nonsense to my preamps" — and the tests make the *plumbing* not the suspect when it happens.

**Consequences.** X32/M32 behaviour is unchanged (it's trusted, so it writes immediately as before). The four new families are read-only until proven. A caveat worth repeating: `dialectTrusted` is not "hardware-verified" — the X32's routing enum indices and AES50 head-amp mapping still want a real desk before anyone leans on them at a gig.

---

## Enforce bug CLASSES in CI, not in prose — 2026-08-13

**Context.** Five audit passes over two months. Every one found defects in classes a previous pass had already "fixed" somewhere else: `condemnAllStrips` was extended four times across MIXER call sites while the identical hazard sat open in the EDIT rows and the meterbridge; `.wav`-only globs were fixed in the player, then found again in the transient cache, the timeline CSV and the Crop guard; the unlocked `TrackState::name` write was fixed in two rename editors and still present in a menu item. Each fix was correct. Each was applied only where the bug was *found*.

**Why documentation didn't work.** Every one of those classes was already written up in `CLAUDE.md` — often with the exact failure mode spelled out. The prose was read, agreed with, and then not applied to the file being edited two weeks later. A rule that depends on someone remembering it at the right moment is not a control.

**Decision.** Bug classes get a **grep that fails the build**. `tools/invariants_audit.sh` runs in CI (before the build, so a violation fails fast) and in a pre-commit hook, alongside the existing design gate. Seven rules today, each traceable to at least one shipped defect. The rule set is enumerated over **consumers** of a hazardous pattern, not over the call sites that trigger it — that inversion is what the `condemnAllStrips` story cost us four times.

**A gate must be proven to fail.** Two of the original seven rules passed a deliberately injected regression: one matched a *commented-out* call, the other was satisfied by an unrelated comment elsewhere in the same file. Both were found only because the gate was self-tested by breaking the code on purpose. **Adding a rule without watching it go red is adding a rule that does nothing** — this is the same lesson as the 2026-07-10 ADR about regression tests needing to reproduce the precondition, one level up.

**Consequences.** The sweep that produced the gate found five more open sites (meterbridge condemn, punch-sidecar loading, the Crop container guard, a second unlocked name write, a hardcoded sessions root). Ongoing cost is one CI step and the discipline of adding a rule when a class is identified. The limits are real and worth stating: it is grep, so it cannot tell code from comment without help, it only guards classes we already know about, and every rule is a maintenance liability if the pattern legitimately changes.

---

## EDIT-view audit — a fix is only fixed where it was applied — 2026-08-12

**Context.** The 2026-08-11 hunt deferred the EDIT view (a 4,087-line row class) as a follow-up. Reading it produced 16 defects, and the two HIGHs both come from the same root cause the previous audits kept circling.

**The lesson, again, sharper.** `condemnAllStrips()` exists to stop timer-driven components reading a `TrackState&` that is about to be freed. It was written for the MIXER, then extended three separate times as new MIXER call sites were found. Nobody ever asked the other question: *what else caches a `TrackState&` on a timer?* The EDIT view does — every `TrackRow` owns a `LedMeter` built from `getTrack(index)` — and `LedMeter::detach()` even carries a comment saying it's "called from `ChannelStrip::invalidate()`". So the whole time the MIXER hole was being closed and re-closed, the identical hole sat open one view over. **Decision:** the rule in `CLAUDE.md` is no longer "call `condemnAllStrips()` from every shrink site" but "**every surface that caches a `TrackState&` must be wired into `condemnAllStrips()`**" — the enumeration is over *consumers*, not call sites. Wiring EDIT in was three lines once the question was asked.

**Second theme: duplicated constants drift.** The empty-session timeline span existed as four independent literals — 300 s in the ruler, 300 s at the edit cursor, 60 s in `laneTimelineSamples()`, `48000 * 60` in the tempo lane (wrong at any rate but 48 k). `laneTimelineSamples()`'s own comment warned that a paint-vs-hit-test mismatch "makes freshly-placed automation points un-clickable" — and the function was itself one of the mismatched copies. Same shape in the edit-group broadcasts, where a clip INDEX (meaningful only inside one track's list) was passed to peers. **Decision:** both now live as named helpers in a new `Source/UI/EditTimeline.h` that the ruler, the lanes and the tests all include. Extracting them is also what made them testable — `EditPage::TrackRow` is a private nested class, so logic left inside it can only ever be smoke-tested.

**Consequences.** 281 groups / 0 failures (278 → 281). Three of the sixteen are locked by `EditViewFixTests.cpp`; the rest live in mouse handlers on a private nested class and remain smoke-test territory, which is the standing argument for keeping the smoke test in the workflow rules rather than trusting the suite alone.

---

## Whole-codebase bug hunt — the message thread is a show-critical resource — 2026-08-11

**Context.** A single-reader pass over the audio core, recorder, player, clip/bounce engine, session IO, UI control paths, network layer and capture daemon found 36 defects (4 blockers, 7 high, 13 medium, 12 low). Unlike the 2026-07 audits — which were dominated by *data-loss* bugs — the severe findings here clustered around two themes that hadn't been treated as first-class hazards.

**Theme 1: blocking the message thread is a show-stopping bug, not a polish item.** Four separate paths could freeze the app for seconds to forever while a take was rolling: the SMART poll (`readAllProcessOutput()` has no timeout and the `waitForProcessToFinish` after it was dead code), the pre-flight write-speed probe (16 MB written synchronously to the take's own volume), track export (per-file resample + a 120 s `lame` wait), and Save As (a multi-GB folder copy). None of them lose audio — the capture thread keeps rolling — but to an engineer at FOH a beachballed transport *is* the emergency. **Decision:** unbounded file or subprocess work goes on an owned, cancellable, destructor-joined worker (the existing `bounceThread` pattern, now joined by `exportThread`); engine state is resolved on the message thread first and only plain values cross the boundary; and nothing writes to the session volume mid-take. Written up as a top-level rule in `CLAUDE.md`.

**Theme 2: "max of two counts" is an anti-pattern when the two counts mean different things.** `forEachStereoMixWindow` bounded its loops on `jmax(recorder.getNumTracks(), player.getNumTracks())` and then dereferenced *recorder* TrackStates — an unchecked `*tracks[i]` past the end whenever a session had more `Track_NN` files on disk than strips in the mixer. The player's count comes from the filesystem; the recorder's is the mixer. **Decision:** the mixer is authoritative for anything reading mixer state (a file with no strip has no fader, so it isn't in the mix — matching the stems bounce). `jmax` stays legitimate purely as a *range check* on an index, which is how the sibling entry points use it. `getTrack()` gained a debug bounds assert so the next unchecked caller trips in testing.

**Also worth recording.** Two correctness-of-deliverable fixes follow the same shape as earlier ADRs: the offline bounce now reads automation under a **blocking** lock (`automationValueAtOffline`) because the RT try-lock-with-fallback — correct for the audio thread, which must never block — was silently baking static fader values into rendered files; and a **continue-record adopts the existing take's container** rather than the current capture format, because scanning for only the current extension forked the take into two files claiming one track index when the engineer changed format between takes.

**Consequences.** Nine of the fixes are locked by `Source/Tests/AuditFixTests.cpp` (278 groups / 0 failures, up from 269). The rest are message-thread behaviour and remain smoke-test territory — which is itself the argument for keeping the smoke test in the workflow rules. One behaviour change needs JP's confirmation: **master mute now mutes the click** (it was written into the output buffers after the master fader, so "kill the monitors" left the metronome audible).

---

## Third re-audit — the fix passes keep regressing; the discipline that held — 2026-07-10

**Context.** The re-audit below itself landed regressions, so a third, focused six-agent pass re-read *only the diff the second pass produced*. The pattern held for a third time: the fix pass introduced new regressions AND left one of its own fixes incomplete.

**What it caught (and we fixed):**

- **Two fresh regressions from the second pass.** (1) The marker-position fix read `getSamplesSinceStart()` (the new file's 0-based length) instead of `getRecordTimelineSamples()` (`recordBaseSamples + offset`), so a marker dropped during a continue/punch landed `recordBaseSamples` too early. (2) The click-track "defence in depth" guard required the click strip to be playback-only *before* the code that marks it playback-only ran, so **Generate Click Track aborted on the first press** for a freshly-created strip. Both fixed; the marker fix is locked by *marker during a continue lands on the timeline*.
- **A pre-existing HIGH the second pass made visible.** `swapTracks` renamed only `Track_NN.wav`, stranding a continued/FLAC/AIFF take's `_partXX` parts at the old index — a reorder stitched them onto the wrong channel. The second pass's waveform re-scan surfaced it. Fixed to move every file of each take (all parts, all containers) via collision-safe temp staging; locked by *swapTracks renames continuation parts*.
- **The `condemnAllStrips()` fix was itself incomplete.** The second pass added it to some TrackState-freeing sites but missed three: File ▸ New Session (`launchNewSessionDialog`), New from Template (`applySessionTemplate`), and New from CSV (`createSessionFromCsv`) all `setStripCount`-shrink without a preceding condemn — the exact ~100 ms meter/spectrum-timer UAF window the fix set out to close. Added the condemn to all three. (A dedicated agent verified the *new* strip-lifetime code is otherwise correct and that every growth-only `setStripCount` site correctly needs no condemn.)

**Decision / rule.** Two reinforcements of the existing rule. (1) **A regression test must be proven to fail on the pre-fix code, and its setup must actually reproduce the bug's precondition.** The marker test first went red because its synthetic setup produced `recordBaseSamples == 0` (a fresh dir has no take to continue, so `startRecording` scans length 0) — it needed a *real* prior take on disk for the base to be nonzero. A test that can't reach the buggy state proves nothing. (2) **When a fix introduces a helper for a class of hazard (here `condemnAllStrips`), grep for EVERY site in that class and wire them all in the same change** — the three missed free-sites are the same lesson a third time.

**Consequences.** After three passes the regression rate is finally what forces the discipline: design the fix, grep all siblings, add a test that reproduces the precondition and fails pre-fix, then re-audit the diff. The two documented trade-offs are unchanged and intentional: the end-trim-reloads-to-full behaviour (correct for a recorder — audio is never lost) and the reorder-across-a-mono/stereo-boundary refusal.

---

## Re-audit after the fix pass — regressions + the incomplete-fix lesson — 2026-07-10

**Context.** After the 10-area fix pass, an 11-agent re-audit (the 10 areas + a cross-cutting concurrency slot) re-read the whole codebase specifically to catch regressions the pass introduced. It found **2 Blockers, both regressions of my own fixes**, plus a run of Highs/Mediums that were mostly *incomplete* fixes — the pass fixed a bug on one path and missed its siblings.

**What the re-audit caught (and we fixed):**

- **Two Blocker regressions.** (1) `seedDefaultClips`'s "edited" test compared clip length to file length, so a continue-record (file grows) misclassified an unedited default clip as edited and preserved it SHORT — the appended audio played silent. Fixed by dropping the length clause (a plain full-from-zero clip is always refreshable). (2) The PatchPage "Audio settings…" device dialog used `launchAsync` (modal) but closed via `setVisible(false)`, leaving an invisible modal that froze the app. Fixed by branching the close on a `selfOwned` flag → `exitModalState`.

- **The incomplete-fix pattern.** The pass repeatedly fixed one path and missed a sibling: the multi-part `_part` skip reached the noise + song analyzers but not QC; the meter-UAF `detach()` reached the per-strip delete paths but not `setStripCount`-shrink / console rebuild (fixed with `condemnAllStrips()`); LOCK gated the keyboard but not the menu bar; the disk-headroom `bytesPerSample` fix reached the display but not the RECORD-time guard; the FLAC/`.wav` glob fix reached session load but not QC / the recovery + new-session dialogs; the `SessionMirror` schema fix made it apply state but with no change-detection (settings-file thrash); the `ConsoleLink` gain clamp used the wrong domain (dB on a 0..1 value); the companion beachball fix polled writability but didn't close worker sockets on stop.

**Decision / rule.** When a fix targets a *class* of bug (a glob, a guard, a lifetime hazard, a clamp), **grep for every sibling call site and apply it to all of them in the same change** — a partial fix reads as "handled" and hides the rest from the next reviewer. And **re-audit after a large fast fix pass**: the two Blockers here were both introduced by that pass and would not have surfaced without a second read. New regression tests lock the two riskiest engine changes (*Continue-record grows the take*, *swapTracks preserves clip edits*).

**Consequences.** A few genuinely deep, pre-existing, or intentional items were deferred with reasons in `tasks.md`: the capture-daemon `SetTrackCount` resize under the live audio callback and the companion worker's `getTrack` race vs `setStripCount` (subsequently addressed by structure locking), the reorder-across-a-mono/stereo-boundary *block rotation* (we refuse the pair-splitting step rather than corrupt the layout), and the lossy audition stream. The former remote RECORD → fresh-session behaviour was removed in the 2026-09-23 audit follow-up; the host now enforces current-session preflight.

---

## Record-safety + data-loss decisions from the 10-area deep audit — 2026-07-10

**Context.** A ten-area re-audit of the whole codebase surfaced a cluster of defects whose fixes were load-bearing enough to record the *why*, not just the *what*. They share two themes: (a) same-session reloads and remote entry points were silently destroying edits or capture, and (b) one flag was doing double duty.

**Decisions.**

- **`seedDefaultClips` preserves edits on a same-session reload.** It now takes `preserveEdits`: `false` for a genuine session OPEN (wipe stale clips; the caller's `.zfproj` playlist restore repopulates), `true` for a same-session reload. `AudioEngine::loadSession` gained a `preserveEdits` param (default false); `stopRecording` and the click-track regen pass `true`; `swapTracks` now **swaps the engine's clip lists + comp playlists** for the two indices and republishes preserving edits, instead of reloading through the wiping path. A track counts as "edited" (untouchable) when it has >1 take or its single clip is shaped away from the plain full-range default. *Why:* stop-recording, strip reorder, and a stray OSC/console stop were all reloading through the wiping path and discarding every split/fade/comp on every track, then autosave persisted the loss.

- **`WriterChannel::active` gates the drain, not `writer != nullptr`.** A per-channel `active` flag (set when any destination opens at record start) decides whether the drain loop services a channel. *Why:* a failed primary writer nulls `writer`, which was indistinguishable from a deliberately-empty stereo-R slot, so the whole channel — backup, mirrors, FIFO drain — was skipped for the rest of the take while the report still read "backup OK". This directly contradicted the failover guarantee the backup/mirror feature exists for.

- **Record-safety guards live at the ENGINE, not just the UI.** `startPlayback` refuses while recording; `stopRecording` fails closed when not recording; `setTrackStereo` refuses while recording (and bumps the track generation so every view re-collapses the pair). *Why:* the UI buttons were guarded, but network/remote entry points (OSC, MCU, companion, timecode chase) reached the engine directly and bypassed them — starting playback over a live take, reload-wiping an idle session, or flipping the writer layout mid-take. Guarding at the engine covers every caller.

- **The recovery dialog owns the next launch step.** `offerSessionRecovery` returns whether it showed a dialog and defers the Welcome/auto-reopen to the dialog's close (a destructor `onClosed` hook). *Why:* the auto-reopen ran *underneath* the recovery dialog and loaded the orphan being recovered, so "Delete" then `deleteRecursively`'d the currently-loaded session.

- **Offline bounce runs on an owned, cancellable thread.** Stem/mix bounces use a `std::thread` member joined (with a cancel flag) in the destructor, not a detached `juce::Thread::launch`. *Why:* a detached thread dereferenced a freed engine when the app quit mid-bounce.

**Consequences.** `loadSession(dir, preserveEdits)` is the single knob for "reload same session vs open a different one" — pass `true` for any in-place reload. The failover fix means a channel keeps draining after a primary failure even if all destinations are dead (read-and-discard), so the FIFO can't overflow-cascade. New regression tests lock the two riskiest changes: *swapTracks preserves clip edits* and *splitClipAt fade geometry* (`RecordingIntegrityTests`). Deferred with reasons in `tasks.md`: X32 AES50 head-amp capture (needs hardware), noise-report semantics (analyzer redesign), SessionMirror LAN wiring (a feature). Full per-fix list in `CHANGELOG.md` (three *10-area deep audit* entries).

---

## Idle CPU left at ~5%; hero panels made opaque, real floor is the timer swarm — 2026-07-05

**Context.** After the hardening pass, a smoke test showed ~5–7% CPU while "idle" (documented baseline is <1%). Profiling: the message thread was repainting `MainComponent` continuously because `BigClockPanel` (a 30 Hz armed-ready "breathe" pulse) and `PerfDashboard` (repaints as the live AUDIO% jitters) were **non-opaque** — every child repaint forced the parent's slice to redraw too.

**Decision.** Made both panels `setOpaque(true)` (they fill their full bounds with the body `bgDeep` first, so the look is identical) and dropped the pulse to 15 Hz + scoped its repaint to the border band. **Idle CPU is left at ~5% for now** (JP) rather than pursuing more.

**Rationale.** The opaque + pulse changes are correct and profile the two panels down to ~1 sample each — but they did **not** move the steady-state idle number. With the audio device actually running, the ~5% is diffuse: the CoreAudio device callback (on the dev machine an Avid **proxy** device, since no RME is connected — a native device should be lighter) plus the per-strip UI-timer swarm (LedMeter 30 Hz, VcaPanel 20 Hz, MasterStrip/transport/MainComponent 10 Hz), each waking briefly. No single hot spot. A transient "5%→0.2%" reading during the investigation was a measurement artifact — that instance had no session loaded and the device not started (Welcome screen + a pending mic-permission prompt).

**Consequences.** Keep `BigClockPanel`/`PerfDashboard` (and any hero panel over the flat `bgDeep` body) opaque — a non-opaque panel that repaints on a timer drags the parent. The real lever for idle CPU is a UI-refresh/meter-timer consolidation (one coalesced timer, signal-gated meter repaints); it's deferred in `tasks.md` and only worth doing if idle becomes a real constraint on the target rig.

---

## Shared `.settings` writers reload-to-REPLACE, not merge — 2026-07-05

**Context.** `appProps` and the four `Strip*` persistence modules (`StripColours`/`StripNames`/`StripGains`/`StripRouting`) each open their own `juce::PropertiesFile` pointing at the *same* on-disk `.settings` file. A `PropertiesFile` save rewrites the whole in-memory key map. During a 2026-07-05 audit this was found to lose updates: module A saved, module B (holding a stale snapshot) later saved and clobbered A's key. The first fix — reload before each save — used a bare `PropertiesFile::reload()`, which **merges** (it adds/updates keys from disk but never *removes* in-memory keys that are absent from disk). That introduced a worse bug: `clearAllStripOverrides` had `StripGains` delete `strip_pan_0`, but `StripNames` still held it in memory (merged in earlier), and its next whole-file save **resurrected** the deleted pan — so a cleared hard-pan came back on the next `setStripCount`, exactly the gig-prep "why is this panned?" class of bug.

**Decision.** Every shared-file writer reloads with **replace** semantics before a mutate+save: `clear()` + `reload()`, guarded on the file existing (so a first run with no file doesn't blank in-memory state). Implemented as `reloadReplace(*props)` in each `Strip*.cpp` and `AudioEngine::reloadAppPropsBeforeWrite()`.

**Rationale.** Replace both fixes the original lost-update (still picks up others' changes) and honours deletions (drops keys others removed), so no module can resurrect a just-deleted key. A single shared `PropertiesFile` instance would be cleaner still, but is a larger refactor across five modules; the replace-reload is the minimal correct fix. The residual risk — a genuinely corrupt on-disk file failing to parse would blank in-memory state — is no worse than a corrupt settings file already is, and these writes are all synchronous on the message thread so there's no partial-write window.

**Consequences.** Never use a bare `reload()` on these shared files before a save (see `coding-standards.md`). Locked by the `EngineStateTests` "clearAllStripOverrides wipes per-strip state" test (now asserts pan **and** gain stay wiped after clear + grow-back). If the modules are ever unified onto one `PropertiesFile`, this dance goes away.

---

## Punch-in is an offline splice on stop, not a real-time write into the take — 2026-06-14

**Amended 2026-11-01.** What RECORD may punch is now narrower; see *RECORD punches only what the engineer chose*. The splice itself is unchanged.

**Amended 2026-09-24.** The offline-splice decision remains. The trigger, recovery, multipart, alignment and monitoring details below supersede the original timer-driven selection trigger and v1 gaps; they remain part of the current capture implementation.

**Context.** JP asked to re-record a section of an existing take and keep the audio before/after (classic punch-in). The naive implementation — have the recorder seek into the existing file and overwrite the punched region in real time — would put destructive, position-offset writes on the audio thread, against the file the player may be reading, with RF64/backup/mirror finalisation all needing to stay consistent. That is the single most dangerous thing this app could do; a glitch loses or corrupts a take.

**Decision.** A punch records a fresh file; it never seeks into the original on the audio thread. Before opening any writer, `startRecording` confirms each active primary/backup/mirror has the required base and moves every existing part into `.punchbase` sidecars. `stopRecording` closes writers, splices `base[0,punchIn) + freshTake + base[after]` via `Source/Audio/PunchSplice.h`, and commits or rolls back all copies before the asynchronous SHA report. A multi-part base is read as one take and flattened into its base filename. Interrupted-punch sidecars are restored on session open or before another capture; a partial replacement is archived, not silently discarded. Missing/new backup or mirror bases refuse the punch rather than claiming a complete redundant copy. For newly armed tracks with no base, a silent lead-in puts their fresh audio at the current session position.

**Rationale.** Keeping the audio thread out of it means punch-in cannot regress capture integrity — the worst case is a clean fresh take plus an untouched original. The splice is ordinary read-only offline DSP (same class as Consolidate/Bounce), unit-tested in isolation (`PunchSpliceTests`) and end-to-end (`PunchRecordTests`). Splicing every copy honours the redundancy guarantee the backup/mirror feature exists for.

**Trigger.** RECORD branches by context: (1) a range the engineer selected in this run, on a loaded take, opens the punch writer before playback, rolls from a playback-only pre-roll lead-in, and lets the audio callback capture exactly `[in,out)` even within one device block; the UI timer finalizes and resumes post-roll, while loop wrapping is suspended; (2) a loaded take without an armed range punches at the rolling playhead or, after asking, at an explicitly placed stopped cursor, and otherwise appends a continuation at the end, wherever a stopped playhead happens to be parked; (3) no existing take starts a fresh recording. Manual punch ends with one RECORD or STOP press; normal live capture keeps its two-press STOP guard. PUNCH mode remains available for a hands-free position window: it punches the tracks armed at the moment of punch-in, into a range selected in this run, and it ends with its session.

**Consequences.** Stop still performs a synchronous offline splice, which can be noticeable for a long take and needs free space for its temporary result; a verified bundle is not proof of real-device throughput. The selected punch's exact capture window is distinct from the playback pre/post-roll and from the recorder's history buffer, which is included only on fresh takes. During a rolling punch, armed-track monitoring/direct routes use live input in place of the old take inside the captured span; the old take remains audible before/after. Silent lead-ins for newly armed tracks preserve alignment but add start-time I/O and disk usage on long sessions. A true redo from scratch still requires a new/cleared take. Crash recovery protects originals, but backup/mirror consistency and physical monitoring must be verified on disposable real-device sessions before show use.

## Waveform scanning is sharded across N parallel caches — 2026-06-14

**Context.** Opening a freshly-imported (un-cached) multitrack session felt slow: JUCE's `AudioThumbnailCache` scans on a single internal `TimeSliceThread`, so every WAV's min/max reduction ran serially on one core. For a 32–55-track session that's a multi-second first-paint, even on a fast SSD (the bottleneck is the single-threaded read+reduce, not raw I/O). Record-stop was already made instant via the held live envelope; this is the *load* counterpart.

**Decision.** `EditPage` owns **N=4 `AudioThumbnailCache` shards** (each with its own high-priority scan thread) instead of one. A track is assigned to a shard by its **physical index** (`Track_NN % N`), so files scan in parallel and each file always lands in the same shard (stable cache hits on reopen). N is a fixed constant, not CPU-derived, so the on-disk cache is portable between machines. `WaveCache.wfm` gains a shard-count header + one length-prefixed section per shard; the version revision bumped to 3 so pre-shard single-blob caches are dropped and re-scanned once.

**Rationale.** Sharding reuses JUCE's proven scanner and thread plumbing — no bespoke DSP/reader, no cross-thread writes into one cache (each cache fully owns its thumbnails + thread). Failure is graceful: a corrupt or mismatched cache section just re-scans that shard ("slow first paint, never wrong audio"). Fixed N keeps caches portable, which matters because sessions move between machines without their `WaveCache.wfm`.

**Consequences.** First-open of a cold session is ≈ up to 4× faster (multicore SSD); reopen stays instant. 4 high-priority reader threads can't preempt the CoreAudio callback (RT priority is higher), so capture integrity is untouched; load also typically happens pre-show, not mid-take. If profiling later shows disk-seek thrash on spinning media, N can drop to 2 without a format change (the shard-count header already guards mismatches). See *Waveforms appear instantly when a take stops* (the record-side counterpart) in CHANGELOG.

## Tints route through `brand::lift()/sink()`; raw `brighter()/darker()` is gate-banned — 2026-06-14

**Context.** A design-system audit found the token infrastructure strong (generated, in-sync, gated) but with two last-mile leaks the gate couldn't see: 60+ raw `juce::Colour::brighter()/darker()` tint calls with ad-hoc amounts, and 53% of `withAlpha(...)` values bypassing the named alpha scale. Tints were the single largest source of *uncontrolled* visual variance — and completely invisible to `design_audit.sh`.

**Decision.** (1) Every lighten/darken outside `Theme/` routes through `brand::lift(c, amt)` / `brand::sink(c, amt)` (pure pass-throughs over JUCE's `brighter/darker`), with named amounts in `brand::tint::{faint,hover,edge,deep}`; a 6th gate rule bans raw `.brighter()/.darker()` outside `Theme/`. (2) The most-repeated catalogued opacities were promoted to *named* alpha steps in the family token source (`tokens.json`): `faint` 0.10, `chrome` 0.12, `edgeSoft` 0.16, `wash` 0.30 — and `0.10f` dropped from the gate's ad-hoc catalog. (3) A `type::micro` (9.5 pt) rung was added so dense EDIT lanes stop re-sizing `caption()` off-scale.

**Rationale.** A token system that can't *see* tints isn't enforcing them. Making tints a single, gate-visible chokepoint means a future re-skin (or a high-contrast mode) can retune lift/sink in one place instead of hunting 60 magic numbers. Promoting alphas was the system telling us the scale was too small — when >half of usage can't find a named step, the scale is wrong, not the components. Keeping the values in `tokens.json` (not app-local) holds the whole ZynForge family on one source of truth.

**Consequences.** Tokens regenerated → `tokens.json` sha `857c0847a54e` (re-vendored into Recording only; other apps pick it up when they re-vendor). The migration is visually identical (pass-through helpers, same numeric amounts), verified by 245/0 tests + CLEAN 6-rule gate. New code must use `lift/sink` + named alpha/type rungs; the gate will reject raw tints. `NewSessionDialog` was also de-duplicated onto `DialogChrome` stylers as part of the same pass. See *Unified dialog chrome via `DialogChrome.h`*.

## AAF export built natively + de-risked with a round-trip oracle — 2026-06-08

**Note, 2026-11-02:** AAF export was removed before phase 2c, and with it the compound-file writer and its tests. Reaper and DAWproject export carry sessions to DAWs.

**Status:** Accepted (in progress — Phase 1 landed)
**Context:** Real DAW interchange (a session opening as an editable timeline of clips → source WAVs) needs AAF: it's the one format Pro Tools — the dominant target in this market — imports. AES31/EDL are text and verifiable here, but Pro Tools doesn't ingest them, so they miss the #1 DAW. AAF, however, is a binary Microsoft-Compound-File container with a fragile object model, and neither a DAW nor the reference library (pyaaf2 / AAF SDK) is reachable from the build machine to validate output.
**Decision:** Build AAF **natively in C++**, layered + phased, and **de-risk the binary with a write→read→assert oracle** (an independent reader in the test suite) rather than trusting the writer blind. Phases: (1) MS-CFB container writer + round-trip validator [**done** — `Aaf/CompoundFile.h`, `CompoundFileTests`]; (2a) object-model primitives — AUID/MobID/storedForm/value-encoders + the `properties`-stream codec [**done** — `Aaf/AafTypes.h`, `Aaf/AafProperties.h`, `AafTests`]; (2b) object-graph→CFB assembler — object = storage + `properties` stream + child sub-storages [**done** — `Aaf/AafObject.h`, `AafTests`]; (2c) the real AAF composition graph (Header/ContentStorage/Mobs/Slots/Sequence/SourceClip → WAVE descriptor + URL locator to the existing WAVs, external essence) **[BLOCKED — needs a reference `.aaf`]**; (3) fades-as-effects + embedded-essence; (4) real-DAW import field-check.
**Phase 2c blocker — 2026-06-09:** The verifiable foundation (phases 1–2b, all round-trip-tested) is complete. Phase 2c requires the **SMPTE class/property AUID registry** (~15 classes, ~40 PIDs) and an **in-file meta-dictionary** that maps the `properties`-stream PIDs to their definitions — both byte-exact. These cannot be reconstructed reliably from memory, and **no validation reference is reachable here** (PyPI/pyaaf2 blocked, no AAF SDK, no DAW, no sample `.aaf` on disk). Building them blind would be the "looks done, silently fails" artifact this ADR exists to avoid. **Unblock:** one reference `.aaf` exported from Pro Tools (any small session) lets us reverse-engineer the exact AUIDs/PIDs/meta-dictionary and complete 2c with confidence; OR a connected machine where `pip install aaf2` works (use pyaaf2 as the live oracle).
**Rationale:** The valuable format and the hard format are the same (AAF), so avoiding it would ship interchange that misses the main target. Re-deriving a fragile binary standard without the reference impl is risky, so we build our own oracle for everything verifiable here and treat "does Pro Tools specifically accept it" as a **documented field-check, exactly like the RF64 soak** — not a silent assumption. Phasing keeps each increment proven before the next; no new runtime dependency (the AAF SDK is heavyweight + unreachable offline).
**Consequences:** Real-DAW import remains a one-time human verification step before the feature is trusted. The CFB writer is a reusable, fully-tested foundation. If field import ever needs the canonical SDK, the phased boundary makes swapping the lower layer tractable.
**Related Documents:** `Source/Audio/Aaf/`, `Source/Tests/CompoundFileTests.cpp`, `tasks.md` (Full AAF/OMF interchange), this report's market analysis.

---

## No plugin hosting — 2026-04-12

**Note, 2026-11-02:** The decision stands. The fixed routing graph no longer has VCAs or aux sends.

**Status:** Accepted
**Context:** Early users asked whether ZynForge Recording should host AU / VST plugins for offline mixdown or VSC tone shaping. The decision had to be made before the routing graph was architected.
**Decision:** ZynForge Recording does not host plugins. The audio callback's routing graph is fixed: input → recorder, file → player → optional clip gain → mute/solo/VCA/aux → master.
**Rationale:** Hosting plugins forces the engine to support arbitrary parameter automation, plugin sidecar processes, plugin crash recovery, and a per-plugin UI surface. That is a different product (a DAW). The product positioning is *live recorder + virtual soundcheck*, and the engineer brings their own console for tone shaping. Keeping the engine plugin-free preserves real-time safety guarantees and keeps the live-show attack surface small.
**Consequences:** Cannot offer in-the-box EQ / compression. No automation lanes. No third-party effect ecosystem. Engineers expecting DAW-like behaviour will need to be told this upfront in `design.md`.
**Alternatives Considered:** (a) Host AU only, gated by an "expert mode" flag — rejected; the support burden is identical to full hosting. (b) Build a tiny first-party effect chain (HPF + gain trim) — rejected as scope creep.
**Related Documents:** `design.md`, `architecture.md` §7.

## Real-time safety is mechanical, not best-effort — 2026-04-15

**Status:** Accepted
**Context:** Crashes during a live show are existentially bad. Mid-show audio glitches are nearly as bad.
**Decision:** The audio callback is mechanically restricted: no allocation, no locks, no `Logger::writeToLog`, no file open/close, no message-thread calls. State transitions are atomics. Scratch buffers preallocate in `audioDeviceAboutToStart`. Background writers run on `TimeSliceThread`s.
**Rationale:** Best-effort RT discipline tends to drift as the codebase grows. Mechanical rules — enforced by code review and by the file layout itself (anything inside `Source/Audio/AudioEngine::audioDeviceIOCallbackWithContext` must obey them) — survive refactors.
**Consequences:** Every new feature touching audio needs explicit thinking about which thread allocates, which thread mutates, and how the data crosses. Some convenience APIs (e.g. logging from the audio thread for debugging) require detour through a lock-free ring buffer.
**Alternatives Considered:** Per-callback profiling + best-effort allocation avoidance — rejected as too easy to violate accidentally.
**Related Documents:** `architecture.md` §3 + §7, `coding-standards.md`.

## JUCE 8 via FetchContent, not a vendored submodule — 2026-04-20

**Status:** Accepted
**Context:** The project depends on JUCE. Two distribution options: vendored git submodule (pinned to a commit) vs CMake `FetchContent` (pinned to a tag).
**Decision:** Pull JUCE 8.0.4 via `FetchContent_Declare` in the top-level `CMakeLists.txt`. No submodule.
**Rationale:** FetchContent is JUCE's officially recommended path on the modern CMake project. Avoids a ~200 MB submodule in the repo. Upgrading JUCE is a single-line edit. The cost — slower first-time configure — is acceptable.
**Consequences:** First configure takes longer; subsequent builds are unaffected. Offline builds need a populated `_deps` cache.
**Alternatives Considered:** Vendor JUCE as a submodule — rejected on repo-size grounds. System-installed JUCE — rejected for reproducibility.
**Related Documents:** `architecture.md` §2, `CMakeLists.txt`.

## Persistence first: every user-edited state round-trips to disk — 2026-05-23

**Status:** Accepted
**Context:** A regression discovered during the 2026-05-23 audit: comp playlists (Takes) were stored in RAM only. Closing the app between rehearsal and show silently dropped them. Cue snapshots referenced strips by array index, so a strip reorder corrupted every cue.
**Decision:** Any state the engineer can edit through the UI **must** round-trip through `.zfproj` (per-session) or `appProps` (cross-session). Stable identities (`TrackState::stripId` — a `juce::Uuid`) are mandatory for anything that survives a reorder.
**Rationale:** A recorder app cannot lose user state. Trust evaporates after a single loss in front of an audience.
**Consequences:** Every new editable field needs a serialiser + deserialiser. `.zfproj` now carries `formatVersion` (currently 2) so future schema migrations are explicit. Slight added complexity at every edit site.
**Alternatives Considered:** Periodic autosave to a sidecar file — rejected because the recovery path is fragile.
**Related Documents:** `architecture.md` §6, `CHANGELOG.md` 2026-05-23, related ADR *Stable identity via UUIDs* (folded into this entry).

## Unified dialog chrome via `DialogChrome.h` — 2026-05-23

**Status:** Accepted
**Context:** Every dialog had its own paint method with slightly different gradient parameters, divider positions, and Apply/Cancel button colours. `juce::AlertWindow::showAsync(...)` rendered with default JUCE chrome that didn't match. Visual inconsistency across modals undermined the rest of the design system.
**Decision:** Extract dialog chrome into `Source/Theme/DialogChrome.h`. Every modal calls `dialog::paintChrome(g, *this, "TITLE")` and uses `dialog::stylePrimary` / `styleSecondary` for buttons. `ZynForgeLookAndFeel::drawAlertBox` paints the same chrome so `AlertWindow.showAsync(...)` calls match.
**Rationale:** A single helper means a single source of truth. Future visual changes happen once, not in nine places. Lowers the bar for a new dialog: write the content, call the helper.
**Consequences:** New dialogs must use the helper; bespoke modal paint code is now a smell. All existing dialogs refactored in one pass.
**Alternatives Considered:** Inheritance — a `ZynforgeDialogContent` base class. Rejected because dialogs already have varied root types (`Component`, `Component + ChangeListener`, etc.); free functions compose more cleanly.
**Related Documents:** `architecture.md` §5, `coding-standards.md` (brand + design system rules), `CHANGELOG.md` 2026-05-23.

## No competing-recorder attribution — 2026-04-18

**Status:** Accepted
**Context:** Both products occupy adjacent positioning (live multitrack recorder + virtual soundcheck). Question: does the product page / commit history / code comments credit them as inspiration?
**Decision:** No. ZynForge Recording is its own product. Do not reference, name, or credit any competing recorder in any artefact — code, comments, docs, commit messages, marketing.
**Rationale:** The product has its own design language and architectural choices (no plugins, OSC dialect parity across five consoles). Comparisons would mislead users about feature parity and dilute the brand.
**Consequences:** Engineers familiar with those products won't see explicit translation guides. Documentation has to teach the workflow on its own merits.
**Alternatives Considered:** Mention as a comparison reference in marketing only — rejected to keep the message clean.
**Related Documents:** `CLAUDE.md` workflow rule 5.

---

## Companion server is loopback-only with a per-session access token — 2026-05-24

**Note, 2026-11-02:** The companion server was removed, with its tunnels and `/stream.wav`.

**Status:** Accepted
**Context:** The HTTP companion (`/state.json`, `/cmd`, `/stream.wav`) used to bind `0.0.0.0` with zero auth. Anyone on the same Wi-Fi could arm tracks, mute strips, or hijack a stream. At a public-Wi-Fi venue this is a real attack class.
**Decision:** `startCompanionServer(port)` binds `127.0.0.1` by default. LAN exposure is opt-in via the explicit `startCompanionServerOnLan(port)` API. Every request requires a 32-hex-char access token (regenerated each start) via `?t=<token>` query or `Authorization: Bearer <token>` header. Token-less / wrong-token requests get 401.
**Rationale:** Default-deny matches how the engineer thinks ("I started a server, no one should be able to use it without me handing them the URL"). Token-in-URL is the cheapest UX -- the host copies the full URL to the clipboard on start so the engineer can paste it on a phone.
**Consequences:** Existing phone clients break -- they must include the token. The loopback default means LAN access takes an extra menu item. Token regenerates per server start so an old bookmark stops working after a restart (acceptable -- engineers re-launch the app between sessions).
**Alternatives Considered:** Per-session password (engineer-set) -- rejected for friction. WebSocket auth handshake -- rejected as over-engineering for an HTTP polling client.
**TLS / HTTPS resolution — 2026-06-05:** Decided **not** to build in-app TLS. JUCE has no server-side TLS, so it would mean bundling a TLS stack + shipping a **self-signed** cert (browser "not secure" warnings on the phone, extra attack surface to maintain) — a worse security/UX trade than the alternative. The on-path-attacker gap is closed instead by **fronting loopback with a tunnel** (Tailscale `serve`, Cloudflare Tunnel, or `ssh -L`), which terminates real CA-backed TLS and adds identity for free. The companion stays loopback-only; `startCompanionServerOnLan` remains an unwired API (no menu path), so plaintext is never served to the LAN by accident, and the host UI warns (`isCompanionExposedOnLan`) if it ever is. Documented in README "Companion server — Security & secure remote access".
**Audio-stream confidentiality — 2026-06-08:** The backlog item "wrap `/stream.wav` in TLS or migrate to SRTP" is **resolved by this same architecture, not a separate cipher.** `/stream.wav` is one more endpoint on the loopback server, so it inherits the loopback bind + per-session token + tunnel-TLS story above; a dedicated stream cipher would be redundant with the tunnel and would re-introduce the in-app-crypto burden we rejected. The one real gap was that the served page didn't *thread* the token onto its sub-requests (`/state.json`, `/cmd`, `/stream.wav`), so the client 401'd against its own server. Fixed: the page reads the token from its URL and appends `?t=<token>` to every request; the `<audio>` stream URL carries it too (an `<audio>` element can't send an `Authorization` header). Net: the stream is access-controlled (no token → 401) and confidential over a tunnel, with no bundled TLS/SRTP. Guarded by `Source/Tests/CompanionServerTests.cpp`.
**Related Documents:** `Source/Network/CompanionServer.{h,cpp}`, `AudioEngine::startCompanionServer{,OnLan}`, `README.md` (tunnel recipes), `Source/Tests/CompanionServerTests.cpp`.

---

## Bare digit jumps to cues only; Cmd+digit jumps to markers — 2026-05-24

**Status:** Accepted
**Context:** Pressing `1`..`9` used to jump to a cue if the setlist had one, else fall through to a marker jump. Engineer's muscle memory depended on session contents -- a cue list shipped to a venue without an expected cue silently fell through to "jumped to marker 1," which could mean rewinding to bar 0 mid-show.
**Decision:** Bare `1`..`9` is reserved for cue jumps. `Cmd+1`..`9` is the Pro Tools-style marker / Memory Location shortcut. The two never compete.
**Rationale:** Predictable keys beat clever fallbacks on stage. Engineers always know which list the digit will hit.
**Consequences:** Engineers who relied on the old fall-through have to retrain to `Cmd+N` for markers. The first time a digit press does nothing, the status bar surfaces the reason ("No cue N" or "No marker N -- drop one with M first").
**Alternatives Considered:** Keep the fallback but log it to the status bar -- rejected because the muscle memory of "this key always does X" is more important than the convenience.
**Related Documents:** `Source/UI/MainComponentKeys.cpp`.

---

## Test harness lives inside the GUI binary, behind `ZYNFORGE_RUN_TESTS=1` — 2026-05-24

**Status:** Superseded 2026-10-29 by *Tests run in their own app against a shared core library*
**Context:** Until 2026-05-24 there were zero tests. Every change shipped on "the app launched at 49 MB and didn't crash for 9 seconds." A separate test target was discussed but adds CMake complexity (link-graph surgery, JUCE module duplication) and slows the dev loop.
**Decision:** The GUI app binary doubles as a test runner. Setting `ZYNFORGE_RUN_TESTS=1` (or passing `--run-tests`) at launch makes `Main.cpp` instantiate a `juce::UnitTestRunner`, run every registered `juce::UnitTest`, print results to stderr AND `~/Library/Logs/Zynforge/test-report.log`, then `quit()` with the failure count as exit code. `AudioEngine::setTestModeSkipAudioInit(true)` lets tests construct the engine without opening a 256-channel audio device.
**Rationale:** Lowest friction -- no extra target to maintain, the test binary always has the latest engine code, and adding a new test is one file (`Source/Tests/*.cpp`). Exit-code semantics let CI / shell scripts gate on test pass/fail.
**Consequences:** Tests can't run in parallel with the real app. The single binary is slightly larger. Test-only state (the `s_testSkipAudioInit` static) lives in production code. The first test pass found and fixed a real correctness bug (write paths silently no-op'd on uninitialised lane storage) -- which justifies the investment.
**Alternatives Considered:** Separate `juce_add_console_app` target -- rejected to avoid duplicate compilation of the audio engine + link-graph complexity.
**Related Documents:** `Source/Main.cpp`, `Source/Tests/AutomationTests.cpp`.

---

## Read-only offline DSP is allowed; real-time signal-modifying DSP is not — 2026-05-24

**Note, 2026-11-02:** The rule stands. `NoiseAnalyzer.h` and `SpectralClassifier.h` were removed; `SpectrumBands.h` is what remains, for the soundcheck report.

**Status:** Accepted
**Context:** The 2026-05-24 audit flagged ambiguity: `Source/Audio/NoiseAnalyzer.h` runs FFT-based hum detection / bump counting / noise-floor analysis, and `SpectralClassifier` does spectral fingerprinting for auto-naming. Both are real DSP. The "no plugins, no effects" rule (`CLAUDE.md` workflow rule 6) is unambiguous about plugin hosting but doesn't draw a line for in-tree DSP. Without a written line, a future contributor could justify adding an EQ "just for the click track" or a noise gate "just for analysis preview" and the codebase would silently drift.
**Decision:** Three-tier rule.
  1. **Plugin hosting (AU / VST / AAX / LV2):** still NO. See *No plugin hosting* ADR above.
  2. **Real-time signal-modifying DSP** (EQ, dynamics, reverb, gating, saturation, gain reduction, anything that changes what the engineer hears vs what landed on disk): NO. The recorder + virtual-soundcheck mission is to transport bits unmodified between the desk and disk. Any in-the-box DSP breaks the "what you record is what you'd hear back" contract that engineers rely on for verification.
  3. **Offline read-only analysis** (FFT inspection, classifier output, level / phase / clip detection, hum / noise reporting): YES. These never touch the audio path; they produce metadata / reports, never modified audio. Examples in tree today: `NoiseAnalyzer` (post-record hum + noise + bump scan), `SpectralClassifier` (track auto-naming).
**Rationale:** The mission boundary isn't "no DSP." It's "no audio-path DSP." Analysis-that-emits-metadata is the same category as a meter or a peak indicator — already accepted. Surface a written line so the next contributor doesn't have to relitigate.
**Consequences:** Future analysis-style features (loudness measurement, polarity detection, click consistency report, automated phase-correlation history) are inside the line. Future signal-modifying features (per-strip EQ, headphone-mix processing, talkback ducking, summing reverb) are outside it -- regardless of how reasonable they sound in isolation.
**Alternatives Considered:** Strict "zero DSP" -- rejected because that bans even the existing peak meter computation. "DSP allowed if it's optional" -- rejected because optionality always erodes; once the toggle exists, the next request is "default it on."
**Related Documents:** `Source/Audio/NoiseAnalyzer.h`, `Source/Audio/SpectralClassifier.h`, `CLAUDE.md` workflow rule 6.

---

## Auto-split on chunk-size ceiling, not RF64 promotion — 2026-05-25

**Status:** Superseded for WAV by *RF64 for WAV via JUCE + periodic header flush* (below). Still in force for AIFF/FLAC.
**Context:** Standard RIFF WAV caps the data chunk's size field at 32-bit unsigned → 4 GiB. AIFF caps at 2 GiB (signed 32-bit). At 48 kHz / 24-bit / mono that's ~8.3 h per WAV, ~4.1 h per AIFF. A 24-hour install, an all-day festival capture, or a 96 kHz session can hit the wall mid-take and silently emit a malformed-header file. Two ways to handle this: (a) **RF64 promotion** — when the writer notices it's about to overflow, rewrite the header into the RF64 / WAV64 extended format that supports 64-bit sizes; (b) **Auto-split** — close the current file at the threshold and open `Track_NN_part02.<ext>`, continuing the recording across multiple files.
**Decision:** Auto-split, not RF64.
**Rationale:**
  1. JUCE's `WavAudioFormat` does not natively support RF64 writing. Implementing it would mean subclassing the writer or forking JUCE — significant code, hard to keep in sync upstream, and AIFF would still need its own solution (AIFF-C / AIFF64 are even less standard).
  2. Every tool that opens a Pro Tools / Logic / Reaper session reads RIFF WAV. RF64 support is uneven — older plugins, broadcast playout systems, and some hardware players reject it. Auto-split produces files every tool can read.
  3. The user mental model — "if a 10-hour record needs to be a 10 GB file, fine, but if it needs to be three 3.3 GB files, that's also fine and I'll just import them all" — matches engineers' existing experience (Pro Tools and many field recorders do exactly this).
  4. Implementation is small: a few hundred lines in `MultitrackRecorder` (byte counter per writer + a roll function called from the drain loop). The roll is on the writer thread, never the audio thread; no real-time concern.
**Consequences:** Long sessions emit `Track_01.wav` + `Track_01_part02.wav` + ... numbered sequentially. Mix engineers consolidate by re-importing in order (Pro Tools' "Import Audio" + numerical sort, or any DAW's equivalent). At the time of this decision, `session.report.json` did not list every part. The 2026-09-30 continued-session reporting decision above supersedes that limitation: reports now enumerate and hash every on-disk part. Backup writers roll independently to keep the mirror layout consistent.
**Alternatives Considered:** RF64 promotion (per above), accepting the limit and documenting it (rejected — silent file corruption is a real show-day failure mode), capping recording duration in the UI (rejected — it's the engineer's call, not the app's).
**Related Documents:** `Source/Audio/MultitrackRecorder.{h,cpp}` (`maxBytesForContainer`, `openWriterAtPath`, the drain-loop roll logic), `Source/Tests/AudioCallbackTests.cpp` (auto-split test).

---

## RF64 for WAV via JUCE + periodic header flush — 2026-06-05

**Status:** Accepted (supersedes the WAV half of *Auto-split on chunk-size ceiling*)
**Context:** The original auto-split ADR rejected RF64 mainly on the premise that *"JUCE's `WavAudioFormat` does not natively support RF64 writing."* That premise is **false**: JUCE's `WavAudioFormatWriter` reserves the `ds64` chunk slot at file creation and, on `flush()` / close, rewrites the header as **RF64** once `bytesWritten >= 4 GiB` (else a normal RIFF) — and `WavAudioFormat`'s reader handles RF64. The only reason takes were splitting is that `maxBytesForContainer` returned 3.9 GiB for WAV, so the recorder's own roll fired *before* JUCE ever reached RF64. Multi-part files are a real workflow wart (re-import + consolidate in order) and modern DAWs (Pro Tools, Logic, Reaper, Nuendo) all read RF64.
**Decision:** For **WAV**, stop auto-splitting: `maxBytesForContainer(WAV)` returns "no ceiling", so a WAV take is one continuous file — ordinary RIFF under 4 GiB, RF64 above it. AIFF + FLAC keep auto-splitting (no RF64 path here). To keep a single huge file crash-safe, the writer thread now calls `flush()` on every open writer **every ~5 s** (`flushOpenWriters`), so a crash mid-take leaves a self-describing, readable file (header valid up to the last flush) instead of one continuous file with a stale/zero-length header. This flush also benefits AIFF/FLAC.
**Rationale:**
  1. JUCE already does RF64 correctly — no fork, no custom writer.
  2. One continuous file is what engineers want for a long take; no consolidate step.
  3. Periodic flush removes the *new* risk a single file introduces (whole-take loss on crash) and is a strict crash-safety improvement for all formats. Flush is on the writer thread (a seek + small header write), never the audio thread.
  4. Files under 4 GiB stay plain RIFF/WAV (the `ds64` slot reads as a harmless `JUNK` chunk), so the common case is unchanged and universally compatible.
**Consequences:** A >4 GiB WAV take is a single RF64 file; tools that reject RF64 (rare, older broadcast/hardware) won't read it — acceptable given the target DAWs all support it, and sub-4 GiB takes remain plain WAV. The `Track_NN_part02.wav` path still exists for AIFF/FLAC. Actual >4 GiB promotion is JUCE-tested and verified by field test (a unit test can't write 4 GiB); the unit suite verifies WAV never rolls + the flushed-header-is-crash-readable guarantee.
**Related Documents:** `Source/Audio/MultitrackRecorder.{h,cpp}` (`maxBytesForContainer`, `flushOpenWriters`, drain-loop flush), `Source/Tests/AudioCallbackTests.cpp` (RF64 / crash-readable test), JUCE `juce_WavAudioFormat.cpp` (`isRF64`, `flush`, `writeHeader`).

---

## Single writer thread for capture (multi-shard parallelism disabled) — 2026-05-25

**Status:** Superseded by *Each copy of a take has its own thread and ring — 2026-11-01*. The cause in the Context below was found: see that ADR.
**Context:** The recorder drained its per-channel lock-free FIFOs to disk on a *pool* of `juce::TimeSliceThread`s — one shard per channel-range, round-robined across threads — to "parallelise" disk writes. In practice this corrupted recordings: under real multi-core parallelism whole channels came back as **white-noise garbage** (lag-1 autocorrelation ≈ 0 instead of ≈ 1 for a real signal), **non-deterministically**, and **worse with more channels**. Real takes also showed on-disk truncation (WAV header claiming the full length, far less data actually written). A new headless integrity test (`RecordingIntegrityTests`) pushes a known per-channel signal through the recorder and re-reads every WAV; it reproduces the garbage at **2+ writer threads** and passes with **0 failures at 1** across 8/16/48-channel configs. Per-FIFO single-producer/single-consumer usage, the JUCE `AbstractFifo` scoped API, and `writeFromFloatArrays` (channel-local buffers) were each verified correct in isolation — yet parallel draining still corrupts, so the race is in the multi-shard interaction and was not worth the risk to keep hunting while shipping a broken recorder.
**Decision:** Serialise all disk writing onto a **single** writer thread (`chooseShardCount()` returns 1 → one shard covering every channel). The shard machinery is retained so re-enabling parallelism is a one-line change once the race is found and a test proves it safe.
**Rationale:**
  1. **Correctness is non-negotiable for a recorder.** A reproduced data-corruption bug outranks a throughput optimisation.
  2. **The parallelism bought nothing real.** 48 tracks × 24-bit × 48 kHz ≈ 6.6 MB/s — one thread drains that with the multi-second FIFO never near full. SSDs do hundreds of MB/s.
  3. **Proven by test.** The same headless test that reproduces the bug at N>1 is green at N=1; it now guards against regressions.
**Consequences:** All armed tracks finalise at the same correct length and round-trip cleanly. If a future workload genuinely needs parallel disk I/O (very high channel/sample-rate counts on slow media), the sharding can be revived — but only behind a green `RecordingIntegrityTests` at the target thread count.
**Alternatives Considered:** Keep hunting the race while shipping broken recordings (rejected — unacceptable for a recorder); add per-FIFO mutexes (rejected — defeats the lock-free design and the FIFO usage is already correct); cap channel count (rejected — doesn't address the root cause).
**Related Documents:** `Source/Audio/MultitrackRecorder.cpp` (`chooseShardCount`, `drainShard`, `rebuildShards`), `Source/Tests/RecordingIntegrityTests.cpp`.

---

## macOS menu enablement refreshed via a polled state signature — 2026-06-04

**Status:** Accepted
**Context:** `MainComponent` is the `juce::MenuBarModel` driving the macOS system menu bar. macOS caches each item's enabled/greyed state and only re-queries `getMenuForIndex` after `menuItemsChanged()` is called — which the app never did. Result: every menu froze in the **empty launch state** (no session, no undo history, no selection), so Undo/Redo, all of Edit, Track, and Export stayed greyed even once a session was loaded, edits made, or strips selected. This presented to the user as "nothing works." Tell-tale: the transport clock read a real duration (player loaded) while the player-gated Edit items were still grey.
**Decision:** Build a cheap string signature of every condition that gates a menu item (`player.isLoaded`, `canUndo/canRedo`, track count, selection count, recording, loop region, active-session, punch, snap, cues-empty, clipboard) in `refreshMenuStateIfChanged()`, called from the existing 10 Hz UI timer; call `menuItemsChanged()` only when the signature changes.
**Rationale:** A `juce::ApplicationCommandManager` would be the "proper" command-driven alternative but would mean migrating ~50 ad-hoc `menuItemSelected` IDs to command IDs — large and risky. Polling a signature at 10 Hz is O(1), allocates one short string, and refreshes within ~100 ms of any state change. Diffing the signature avoids rebuilding the native menu every tick.
**Consequences:** Menu items light up the moment they're usable. New menu-gating conditions must be added to the signature or they won't trigger a refresh. (Documented in `coding-standards.md`.)
**Alternatives Considered:** ApplicationCommandManager migration (rejected for now — scope/risk); calling `menuItemsChanged()` at every mutation site (rejected — dozens of call sites, easy to miss one); unconditional `menuItemsChanged()` every tick (rejected — needless native-menu rebuilds).
**Related Documents:** `Source/UI/MainComponentMenu.cpp` (`refreshMenuStateIfChanged`), `Source/UI/MainComponentTimer.cpp`.

---

## Reopen the last session on launch; never start in an empty all-grey window — 2026-06-04

**Amended 2026-11-01.** Still true, but the restored pin no longer permits saving. Until the open succeeds the session is pinned but not loaded, and saves to it are refused.

**Status:** Accepted
**Context:** On launch the engine restored the active-session *folder* (so Save stayed enabled) but never loaded its *content*. The app came up with 0 channels and a Welcome dialog; combined with the menu-cache bug above, the whole UI read as dead. A session recorded but never explicitly **Saved** has no `session_mix.json`, so even when opened the mixer wasn't sized → "No channels yet".
**Decision:** `showStartupWelcome` auto-reopens the last session if it's still on disk (skipping the dialog). All open paths (File ▸ Open, Welcome `onOpen`, auto-reopen) go through one `openSessionFolder()` that pins the active dir, restores setlist + UI layout + full mixer state, loads the audio, and — when there's no `session_mix.json` — **sizes the mixer from the loaded audio track count** as a recovery fallback.
**Rationale:** Matches DAW expectation (come back to your work). Consolidating the three open paths fixed a real divergence (Welcome `onOpen` previously called only `loadSession`, leaving the mixer + Export grey). The recorder-from-audio fallback recovers recorded-but-unsaved sessions instead of showing an empty mixer.
**Consequences:** Relaunch lands straight in the last session. A fresh first run (no prior session) still shows Welcome. A session with neither `session_mix.json` nor `Audio Files/` is treated as "not a real session" and skipped.
**Related Documents:** `Source/UI/MainComponentSessionIO.cpp` (`openSessionFolder`), `Source/UI/MainComponentHelp.cpp` (`showStartupWelcome`).

---

## Aux sends are per-session in `session_mix.json`, not global appProps — 2026-06-04

**Note, 2026-11-02:** Aux sends were removed. Their keys in an older session are ignored.

**Status:** Accepted (extends *Persistence first*, 2026-05-23)
**Context:** Aux send routing was stored in global `appProps` keyed by strip index (`strip_send_<i>_<slot>_*`). Because appProps is app-global, opening a different session inherited the previous session's sends, and the routing never round-tripped through the authoritative `session_mix.json` — the same leak the per-index appProps mechanism was already deprecated for.
**Decision:** `saveSessionMixTo` / `loadSessionMixFrom` serialize each strip's 4 send slots (`{bus, dB, post}`). `setTrackSend` no longer writes appProps; `applyPersistedStripState` no longer reads sends from appProps (which would clobber the per-session values).
**Consequences:** Sends round-trip per-session and no longer leak. Old sessions open with no sends until re-saved. **`strip_isbus_*` and automation `safe`/`vTrim`/`pTrim` still live in global appProps and have the same latent leak** — tracked in `tasks.md` as the persistence-consolidation follow-up.
**Related Documents:** `Source/Audio/AudioEngine.cpp` (`saveSessionMixTo`, `loadSessionMixFrom`, `setTrackSend`, `applyPersistedStripState`).

---

## EDIT clips render as per-clip region blocks; channels default to grey — 2026-06-04

**Status:** Accepted
**Context:** The EDIT lane drew one continuous full-width thumbnail with yellow "cut-flag" markers at clip starts — so a moved/slip-trimmed clip's block showed the *wrong* audio, and it didn't read like a DAW. Separately, new channels defaulted to the per-index `personality` wash; the user asked for a neutral default they colour themselves.
**Decision:** Each clip is drawn as a discrete block (name-header bar + border) with **its own waveform** mapped to the clip's file region (`fileStart..fileStart+fileLen`), the proven comp-lane pattern; the continuous thumbnail is kept only as the no-clips fallback. The yellow cut-flag is gone (block borders mark boundaries). `brand::stripColour` now returns `stripDefaultGrey` for all indices; recolour via the hue×shade `StripColourPicker` gradient. (This overrides the per-index personality default for the **Recording** app; ZynForge **Live** is unchanged.)
**Consequences:** Edits read correctly and look like Pro Tools/Logic regions. The `personality` palette stays in `BrandColors.h` for reference but is no longer auto-assigned. A timeline grid (1-2-5 s) and trim/move snap-to-grid were added alongside.
**Related Documents:** `Source/UI/EditPage.cpp`, `Source/Theme/BrandColors.h`, `Source/UI/StripColourPicker.{h,cpp}`.

---

## Edit vs Track menu split — 2026-06-04

**Status:** Accepted
**Context:** A single Edit menu mixed timeline/audio editing (Separate, Crop, Range, Punch) with mixer-channel management (cut/copy/paste/delete strips, batch rename/colour, selection) — not how a DAW separates the two, and confusing.
**Decision:** Edit holds only timeline/audio editing + Undo/Redo. A new top-level **Track** menu holds channel management. Menu bar: `File · Edit · Track · Session · Help`. Item IDs and keyboard shortcuts are unchanged; only the `topLevelIndex` dispatch renumbered (Session 2→3, Help 3→4).
**Consequences:** Clearer separation. Any future menu-index-based logic must account for the inserted Track menu at index 2.
**Related Documents:** `Source/UI/MainComponentMenu.cpp` (`getMenuBarNames`, `getMenuForIndex`).

---

## Coalesced mixer undo (poll + settle), recording never undoable — 2026-06-04

**Status:** Accepted
**Context:** Undo only covered clip + automation edits; the everyday mixer gestures (fader, pan, mute, solo, rename, recolour, routing) had no undo wiring, so Cmd+Z did nothing after them and the Undo menu stayed greyed. Wiring an undo step into every strip callback is a large surface, and continuous controls (fader/pan) would spam one step per pixel.
**Decision:** A 10 Hz poll (`MainComponentEdit.cpp::pollMixerUndo`) snapshots the whole mixer and records ONE `MixerSnapshotAction` once a change has held steady ~300 ms — so a fader drag is a single undo step. `editUndo` flushes any pending change first (so an immediate Cmd+Z catches the latest move); the poll re-baselines after undo/redo and on session open, and **skips while recording** so a captured take is never undoable.
**Rationale:** Uniformly covers every mixer mutation without per-callback wiring or per-pixel spam; the "recording isn't undoable" rule matches the engineer's mental model (you delete a take explicitly + confirmed, you don't Cmd+Z it away).
**Consequences:** A snapshot is captured at 10 Hz (cheap for typical track counts; a bit wasteful at 256 tracks — optimise later if needed). Add/remove-channel is structural and re-baselines rather than recording a step (separate task).
**Related Documents:** `Source/UI/MainComponentEdit.cpp` (`pollMixerUndo`, `MixerSnapshotAction`).

---

## Keyboard shortcuts match on key code, not text character — 2026-06-04

**Status:** Accepted
**Context:** Cmd+Z did nothing from the keyboard while the Edit-menu Undo worked. Shortcuts were matched via `KeyPress::getTextCharacter()` under the Cmd modifier, which is unreliable on macOS. JUCE only assigns native menu key-equivalents to `ApplicationCommandManager` commands; this app uses ad-hoc `menuItemSelected` IDs, so the menu's "⌘Z" is display-only and `keyPressed` is the real handler.
**Decision:** Match Cmd-combos on the physical **key code** (`getKeyCode()`), handle undo/redo early so a focused combo/label can't shadow them, and skip while a `TextEditor` has focus. (A full migration to `ApplicationCommandManager` — which would also give native accelerators — was rejected for now as too large; ~50 IDs.)
**Related Documents:** `Source/UI/MainComponentKeys.cpp`.

---

## Console VSC control is profile-based, not one universal protocol — 2026-06-10

**Status:** Accepted (X32 reference shipped; other profiles are capability-declared placeholders pending hardware)
**Context:** The outbound virtual-soundcheck control (soundcheck⇄stage repatch + head-amp gain capture) shipped X32/M32-only. The goal is "works with all the mixers" — DiGiCo, Yamaha, SSL, Allen & Heath. But "universal" is NOT four more OSC dialects: (a) console control protocols differ — X32 is OSC/UDP, Yamaha is **SCP over TCP**, A&H is MIDI/TCP, DiGiCo/SSL have partial/version-specific OSC; and (b) crucially, the large-format desks **already have a native Virtual Soundcheck mode** that flips every input between the stage preamps and the Dante/MADI record card in one console action. So per-input repatch automation is mainly valuable for **X32-class desks that have no native VSC button** but do expose a clean OSC routing model.
**Decision:** Make `ConsoleLink` a transport + state machine driven by a pluggable **`ConsoleProfile`** (`Source/Network/ConsoleProfile.h`). A profile declares its capabilities honestly (`canRepatch`, `canCaptureGains`, `hasNativeVsc`) and, when applicable, the OSC address model. `ConsoleLink` guards every action on the active profile's capabilities. The X32/M32 profile is the full reference implementation; DiGiCo / Yamaha / SSL / A&H ship as capability-declared profiles (`hasNativeVsc=true`, repatch/gain over the wire = false) that point the engineer at the console's own VSC, until their control protocols are wired + verified on real hardware.
**Rationale:** Encodes the real-world truth instead of faking it — the app doesn't pretend to repatch a DiGiCo it can't talk to, and it doesn't need to (the desk's native VSC does it). The architecture is genuinely universal (any console plugs in as a profile), and the value-add automation lights up exactly where it's wanted (OSC desks without native VSC). UI surfaces a console picker; capability-gated menu items grey out for native-VSC desks.
**Consequences:** + Honest, pluggable, ready for per-console wiring. + The market story is "works with your console" (record/play into any card + native VSC), with deep automation on X32-class desks. − Full repatch/gain on Yamaha (SCP/TCP) or A&H (MIDI/TCP) needs a second transport layer, not just an address profile — tracked. − Each concrete non-X32 profile needs hardware to verify.
**Alternatives Considered:** Add OSC dialects like `OscRemote` does for inbound — rejected: most target desks don't expose repatch over OSC and several aren't OSC at all. Fake/stub repatch for all consoles — rejected: dishonest and dangerous (would claim to flip a patch it can't).
**Related Documents:** `Source/Network/ConsoleProfile.h`, `ConsoleLink.{h,cpp}`, `ConsoleLinkTests`; inbound `OscRemote` (5-dialect parser) is the analog for the receive side.

---

## Capture-process split (headless record daemon) — 2026-06-10

**Status:** In progress — Phase 0 mostly done; Phase 1 protocol contract built + tested (no live daemon yet).
**Context:** The audio callback, file writers, and the entire UI live in one process. A UI crash, a wedged modal, or a graphics-driver fault kills the take. The hardware this app competes with (dedicated live recorders) is appliance-grade precisely because capture shares nothing with presentation. We already survived one instance of the class (the companion-server SIGPIPE killing the app mid-take) — the lesson generalises.
**Decision:** Split capture into a minimal headless daemon owning the CoreAudio callback + `MultitrackRecorder` + integrity manifest, with the GUI as a client. Phased so every step ships independently:
1. **Phase 0 — boundary hygiene (prereq, cheap).** Everything the UI reads from the engine during recording goes through a narrow, serialisable status struct (transport, peaks, disk stats, missed samples); no UI code touches recorder internals directly. Pure in-process refactor, testable now.
2. **Phase 1 — daemon binary.** A `zynforge-capture` target reusing `MultitrackRecorder` + the device layer, controlled over a local socket with a versioned protocol (start/stop/arm/status — the companion server protocol is the starting point; it already proxies transport + state). The GUI launches and supervises it; if the GUI dies, the daemon finishes the take and writes the manifest.
3. **Phase 2 — supervision + reattach.** A restarted GUI discovers the running daemon and reattaches mid-take (the session folder + `recording.session` marker already encode enough to rehydrate). Watchdogs both ways: daemon records on without a GUI; GUI flags a dead daemon loudly.
**Rationale:** The biggest single reliability jump available — "app crashed mid-set" becomes a UI blip instead of a lost take. Phasing avoids a long-lived divergent branch.
**Consequences:** + Take survives any UI fault; enables headless/rack operation later. − A protocol surface to version and test; device hot-plug handling moves daemon-side; debugging spans two processes. Input monitoring stays daemon-side with the callback, so monitor changes must flow through the protocol — that is most of phase 1's surface.
**Alternatives Considered:** In-process hardening only (signal handlers, watchdog thread) — cheaper but can't survive SIGKILL or graphics faults; overlaps with phase 0 but rejected as the end state. XPC instead of sockets — macOS-idiomatic but couples to launchd and complicates a future headless build; sockets reuse the companion protocol work.
**Related Documents:** `architecture.md` (threading model); companion-server ADR; `tasks.md` (phased breakdown).

### Progress + Phase-1 design detail

**Phase 0 (status boundary) — mostly done.** `EngineStatus` (`Source/Audio/EngineStatus.h`) is the serialisable snapshot; `AudioEngine::captureStatus()` fills it; the companion `/state.json` and the header readouts (PerfDashboard + BigClock) consume it. Per-track meters + the record-status string still read `getTrack()` directly — deferred to land with Phase-1 component changes.

**Phase 1 wire protocol — built + tested (contract only).** `Source/Network/CaptureProtocol.h` defines the daemon↔GUI messages: **newline-delimited JSON**, each line tagged `type` = `cmd` / `reply` / `status`. `Command` carries the control vocabulary (Hello, StartRecording, Stop, StartPlayback, ArmTrack, SetCaptureFormat, SetSessionDir, Ping — a superset of the companion `/cmd` verbs); `status` wraps `EngineStatus`; a **Hello handshake** negotiates `kProtocolVersion` (v1 = exact match, so a mismatched GUI/daemon fails LOUD instead of silently mis-recording). Header-only + engine-free + transport-free, so it's tested in isolation (`CaptureProtocolTests`) before any socket exists — same approach proven on ConsoleLink. **Deliberately not wired to the recorder yet:** a half-built split that owns recording would *reduce* reliability, which defeats the purpose.

**Remaining Phase-1 chunks (each its own PR + soak):** (a) ✅ **DONE** — the local-socket transport (`Source/Network/CaptureLink.{h,cpp}`: `CaptureServer` + `CaptureClient`, persistent newline-JSON, auto Hello handshake, loopback-tested); (b) the `zynforge-capture` binary target (links `MultitrackRecorder` + the device layer, no UI); (c) GUI-side `CaptureClient` that launches + supervises the daemon and drives recording through `Command`s, rendering `status`; (d) the cutover — route recording through the daemon behind a flag, soak-test at 64 ch, then make it the default. **Phase 2** (mid-take reattach + bidirectional watchdogs) follows. Monitoring (input→output routing) stays daemon-side with the callback, so monitor changes flow through the protocol — that's most of Phase 1's control surface.

---

## Stereo tracks record as ONE interleaved file — 2026-06-13

**Note, 2026-11-02:** Normalize and Strip Silence were removed, so the "(normalize/consolidate/stripSilence)" consequence now covers Consolidate only.

**Status:** Accepted — implemented 2026-06-13
**Context:** A stereo track used to record as two mono `Track_NN.wav` files + an `isStereo` flag. Import/export/mixer already collapsed the pair, but the engineer rightly expects the *capture* to produce one stereo file — and every downstream consumer (player, clips, thumbnails, QC, export) was built on mono-per-channel files, so this touched the core.
**Decision (phased, all shipped):**
1. **Recorder** (`MultitrackRecorder`): the L `WriterChannel` opens ONE 2-channel writer (`openWriterAtPath(..., numChannels=2)`); the R slot is an empty `WriterChannel`. The L drain step reads BOTH per-channel FIFOs (min frames ready), stages them planar (`ShardClient::stageL/stageR`), and does one `writeFromFloatArrays(..., 2, n)`. Byte/roll/RF64 accounting × `numChannels`; pre-roll dumps both channels. The RT push path + meters + per-channel FIFOs are unchanged. A pair never straddles a shard (contiguous index slices, `perShard ≥ 2`).
2. **Player** (`SessionPlayer`): `loadSession` sniffs `reader->numChannels`; a 2-ch file expands into TWO `Track`s sharing ONE `BufferingAudioReader` — L reads ch0, R reads ch1 (`useLeft`/`useRight`). One output stream per physical channel → index→output mapping unchanged.
3. **Editor/bounce/import**: `AudioEngineClips::ArrangementSource::open` resolves the channel (own 2-ch file → ch0; no own file but prior slot's `Track_<track>` is 2-ch → ch1), so `bounceStereoPairToWav` reads both halves from the one file. EDIT `TrackRow` draws ch0 in the L lane and ch1 in the R lane from the same thumbnail (`stereoOneFile` → `drawChannel`). Import writes one interleaved 2-ch file (`writeStereo`).
4. **Compatibility**: legacy mono-pair sessions keep working everywhere — channel-sniffing makes both on-disk layouts transparent; `session_mix.json` unchanged (`isStereo` stays authoritative).
**Rationale:** Matches every engineer's mental model (drag one stereo stem into the DAW) and removes the two-file special-casing in capture.
**Consequences:** + the recorded/imported file IS a stereo WAV. − a native-stereo session has no `Track_(N+1).wav`; any path that resolves audio by physical index must sniff channel count (the resolver does this for player/bounce/EDIT). Per-R-index offline ops (normalize/consolidate/stripSilence) are unreachable from the UI (the R row is collapsed) so they were left index-direct; document if that changes. Tests: `AudioCallbackTests` "Stereo pair records ONE interleaved file + player routes both channels" (243 groups, 0 failures); legacy "bounceStereoPairToWav interleaves" stays green via the resolver.
**Related:** gig-one field report (tasks.md); stereo import/export collapse work of 2026-06-10.

## Orange means STATE, not chrome — 2026-06-14

**Status:** Accepted
**Context:** The Heated-Steel redesign reused bright brand orange and the meter's `meterHot` for *permanent* chrome — the strip spine, the fader-cap groove, the header seams — drawn at full intensity always. In a dark venue the eye couldn't separate "this channel exists" from "this channel is HOT": a record-armed strip and a peaking meter looked the same as idle UI.
**Decision:** Bright/hot orange (and `meterHot` specifically) is reserved for **state** — record-armed, peaking, recording. Permanent structural chrome uses `brand::structuralForge()` (the structural-identity orange) drawn **ember-subdued at rest** (low alpha), escalating to full only when the element's owner is active (e.g. the spine brightens + glows when the strip is armed). The fader-cap groove moved off `meterHot` onto `structuralForge()` so a hot meter is once again the brightest orange on screen. Identity marks (dialog badge, splash, forge-marks) keep full brand orange — they're brand, not state.
**Rationale:** "Cold steel that runs HOT where the signal lives" only works if *hot* is visually earned. Reserving the loudest accent for live state restores at-a-glance readability without losing the forge identity.
**Consequences:** + armed/peaking now pop; the resting wall is calmer. − any new always-on accent must consciously pick `structuralForge()` (subdued) over `meterHot`/`brandOrange`-at-full. Enforced by eye, not the token audit (both are valid tokens).

## ZynForge LookAndFeel is the app-wide default (prompts + fonts) — 2026-06-14

**Status:** Accepted
**Context:** `juce::AlertWindow::showAsync(...)` prompts (Delete channel, etc.) rendered as JUCE's stock alert — grey-blue chrome + a red warning triangle — because only `MainComponent` + a few hand-made `AlertWindow`s set the ZynForge LAF; `showAsync` boxes use the **default** LAF, which was never set. Separately, JUCE resolves every typeface through the default LAF, so custom-paint `Inter`/`JetBrains Mono` text fell back to system fonts.
**Decision:** `MainComponent` sets `juce::LookAndFeel::setDefaultLookAndFeel(&laf)` in its ctor (and resets to `nullptr` in the dtor, before the member `laf` is destroyed). `ZynForgeLookAndFeel::drawAlertBox` draws the forge-mark badge in JUCE's reserved 80px icon column for any non-`NoIcon` alert and the text at JUCE's own `textArea` (no re-flow / clip).
**Rationale:** One line makes every prompt first-party AND fixes font resolution app-wide — both were the same root cause (no default LAF).
**Consequences:** + every popup/menu/tooltip/alert is ZynForge chrome with the bundled fonts. − broad blast radius (governs all default-LAF components); the member-LAF lifetime must be reset in the dtor to avoid a dangling default on shutdown (done).

## Session replacement and copies are transactions — 2026-08-18

**Status:** Accepted
**Context:** Session operations crossed three kinds of state at once: live engine/strip objects, multiple persisted JSON files, and potentially multi-gigabyte audio folders. Open could inherit the previous session or restore playlists before the player erased them; Save As merged into existing folders and pinned incomplete copies; cross-volume relocation could report failure after a complete destination existed; save errors were ignored by switch/close/quit.
**Decision:** Every session replacement uses one confirmation funnel and one ordered open funnel. A requested save must succeed before replacement. Open clears session-scoped state, loads audio/default clips, applies exact mix topology (or audio fallback), then restores playlists/automation/UI. Save As and relocation are owned, cancellable, joined transactions: save source first, reject ambiguous targets, keep the source authoritative during work, and switch only to a complete destination. A transition flag blocks record/play/topology/bounce mutations while a worker owns session structure. A complete destination plus failed old-folder cleanup is success with a warning, not rollback or generic failure.
**Rationale:** The engineer must always know which complete folder is authoritative, and no action presented as Save/Open/Move may silently produce a hybrid session or discard the current one. One funnel prevents each menu/document/template entry point from drifting.
**Consequences:** Save As requires a new or empty folder. Long copies remain cancellable and the UI stays responsive, but conflicting actions temporarily refuse. Every future session entry point must call `confirmSessionReplacement`/`openSessionFolder`; every future background consumer of session topology must participate in the transition/structure-ownership protocol.
**Alternatives Considered:** Merge-by-filename (rejected: creates hybrid sessions and overwrites same-named files); pin destination before copy (rejected: partial folder becomes active); best-effort saves on close/quit (rejected: the requested safety action becomes data loss).
**Related Documents:** `MainComponentSessionIO.cpp`; `architecture.md` *Session transition funnel*; `FIELD-TEST.md` §9.

---

## Inbound remote control is authenticated or read-only — 2026-08-18

**Note, 2026-11-02:** The companion server was removed. Generic OSC and the console dialects remain under this rule.

**Status:** Accepted
**Context:** Generic OSC listened on UDP and accepted transport/arm/mute with no sender identity or secret, so any host able to reach the port could start or alter a take. Console dialect messages cannot carry a ZynForge application credential. The companion server had authentication, but returned success before marshalled transport commands actually ran.
**Decision:** Generic OSC generates a per-start token and requires it as the final string argument on every state-changing command. Console-specific inbound dialects retain read-only recorder value (names, scene markers, gain/trim) but cannot arm, mute, or drive transport. Companion transport commands marshal to the message thread, wait up to five seconds, and return the real host result; the host runs full active-session preflight and two-tap STOP confirmation. Engine-level record/stop without a host handler fail closed.
**Rationale:** UDP reachability is not authorization, and a recorder should fail closed when identity cannot be established. Read-only console metadata remains useful without giving an unauthenticated packet show-changing power. HTTP callers must not receive optimistic success for a command that later fails.
**Consequences:** Existing Generic OSC controllers must append the token displayed by the app. Console transport control through the inbound-dialect listener is intentionally removed; trusted desk control remains in the separately probed/gated `ConsoleLink`. Remote clients can now distinguish conflicts and timeouts.
**Alternatives Considered:** Source-IP allowlist (rejected: spoofable/brittle across DHCP); state-changing console dialects without a token (rejected: no application identity); asynchronous HTTP 200 (rejected: false operational feedback).
**Related Documents:** `OscRemote`; `CompanionServer`; README *Console integration (OSC)*; `FIELD-TEST.md` §9.

---

## Native sans-serif replaces invalid Inter assets — 2026-08-18

**Status:** Accepted (supersedes the Inter portion of *ZynForge LookAndFeel is the app-wide default*, 2026-06-14)
**Context:** `Inter-Regular.ttf` and `Inter-Bold.ttf` were GitHub HTML error pages saved with `.ttf` extensions, not fonts. They bloated the app and asked CoreText to parse invalid data. JetBrains Mono is a valid bundled font and remains useful for stable numeric widths.
**Decision:** Proportional UI roles request JUCE's native sans-serif; fixed-width roles resolve to bundled JetBrains Mono. Remove the invalid Inter assets from BinaryData. Keep all callers on named `brand::type::*` roles so the face decision stays centralized.
**Rationale:** Native macOS typography is valid, legible and stable without adding another binary dependency. The token/type-role system preserves consistent sizing and makes a future validated proportional font a single controlled change.
**Consequences:** Proportional glyph metrics may vary slightly by macOS version. Pixel specs name “native sans,” not Inter. Never add a font asset without validating its magic/type and license first.
**Alternatives Considered:** Re-download Inter during this fix (rejected: unnecessary dependency for a correctness release); keep the invalid assets unused (rejected: misleading and still shipped).
**Related Documents:** `CMakeLists.txt`; `Source/Theme/ForgeTokens.h`; `design.md` *Typography*.

---

## Session-file jobs run on one runner that owns the release — 2026-10-29

**Status:** Accepted (refines *Session replacement and copies are transactions*, 2026-08-18)
**Context:** Eight session-file jobs (track export, stem/mix bounce, import, Save As copy, relocation, show handoff, click render) each hand-wrote "claim `sessionIoBusy`, join the previous `std::thread`, launch, post a completion that clears the flag" onto two shared thread members. Releasing the flag was every worker's own job on every exit path. The track export's cancel path returned before posting, so a quit that cancelled an export left the session busy until restart. Separately, both bounce workers held the recorder's structure lock for the whole render while the 10 Hz UI timer's `captureStatus()` waited on it, freezing the app for the length of the bounce.
**Decision:** `SessionJobRunner` owns the one worker and the cancel flag. Callers `reserve()`, `release()` on pre-launch exits, and `launch (work)`; the work returns its completion and the runner clears the reservation on the message thread before running it, whatever the work returned or threw. Renderers snapshot every strip they read under a brief structure lock and render lock-free; UI code never takes that lock. Two invariant-gate rules pin both.
**Rationale:** A release that is structural cannot be skipped by a new early return. One worker matches reality: every job already excluded every other through the same flag. Snapshotting is both the fix for the freeze and the honest semantic for an offline render.
**Consequences:** A mix bounce renders the mixer as it stood when it began; fader/mute moves during the bounce no longer leak into the file. Metadata saves still share `sessionIoBusy` through their own pending count, which `enqueueMetadataSnapshot` keeps from overlapping a job.
**Alternatives Considered:** Patch the one early return (rejected: the class stays open for the next job); keep the structure lock and make `captureStatus` try-lock (rejected: other message-thread users of the lock would still block, and the render would still read live strips).
**Related Documents:** `Source/UI/SessionJobRunner.h`; `AudioEngine::forEachStereoMixWindow`; `Source/Tests/SessionJobsTests.cpp`; CLAUDE.md *Don't block the message thread*.

---

## TrackState lifetime is shared; views poll one generation — 2026-10-29

**Status:** Accepted (supersedes the "condemn before every free" obligation in the 2026-08 audit ADRs)
**Context:** `ChannelStrip`, `LedMeter` and `MiniSpectrum` cache a `TrackState&` and read it on their own timers. Any recorder shrink -- and a device restart that reshapes the track set in `MultitrackRecorder::prepare()` -- freed those objects while the views kept reading them until the next rebuild tick. Four audits closed it one call site at a time with `condemnAllStrips()` plus a grep gate. Separately, mixer undo built and stringified a JSON snapshot of every strip on every 10 Hz tick, including every tick of a take.
**Decision:** The recorder owns `TrackState`s through `shared_ptr` (`enable_shared_from_this`); each caching surface holds a keep-alive taken from the reference it is given. Every layout change bumps the recorder's track generation (now including `reorderTracks`), which both views already poll. Mixer undo detects change with an allocation-free fingerprint over the same field table as its snapshot.
**Rationale:** Ownership makes the use-after-free impossible rather than avoided; the audio thread only dereferences, so no refcount or deallocation reaches it. Engine-to-UI change notifications were considered but track sets also change on device restarts off the message thread, so a polled generation counter is the simpler robust signal. Command-based mixer undo would need per-gesture wiring in every mixer control; the fingerprint keeps the "catches every edit, including ones outside the UndoManager" property at negligible cost.
**Consequences:** A removed track's view can show a stale value for one tick (condemn still stops its timers immediately). Adding a mixer field means adding it to `MixerSnapshot.h`'s table. ASan now proves the lifetime contract (*Strip lifetime* suite).
**Alternatives Considered:** Keep condemn-only (rejected: correctness rests on every future caller remembering); `juce::ChangeBroadcaster` notifications (rejected for now, above); hashing the JSON string (rejected: still allocates the whole snapshot each tick).
**Related Documents:** `Source/Audio/TrackState.h`; `Source/UI/MixerSnapshot.h`; `Source/Tests/StripLifetimeTests.cpp`; CLAUDE.md *Real-time discipline*.

---

## Tests run in their own app against a shared core library — 2026-10-29

**Status:** Accepted (supersedes *Test harness lives inside the GUI binary*, 2026-05-24)
**Context:** By October the suite was 70 files and ~16k lines compiled into the shipping recorder, reachable through `--run-tests` / `ZYNFORGE_RUN_TESTS=1`, with the recorder's application identity switching on a command-line flag. The 2026-05 decision avoided a separate target because of "link-graph surgery, JUCE module duplication".
**Decision:** The recorder's code is a `ZynforgeCore` static library. JUCE modules link to it PRIVATE (their sources compile into it once) and its include paths / module definitions are re-exported to consumers. The shipping app is `Main.cpp` + the library; `ZynforgeRecordingTests` is `Source/Tests` + `TestMain.cpp` + the same library, embedding the capture helper the same way so daemon suites exercise the shipping discovery path.
**Rationale:** The module-duplication concern is solved by private linkage plus interface re-export; nothing compiles twice. The shipped binary no longer contains test suites or a test identity switch.
**Consequences:** Run tests from the test app (CLAUDE.md, testing.md). CI checks the recorder's symbols for `runTest`. `*ForTests` hooks and test `friend` declarations remain in production headers (inert in the recorder).
**Alternatives Considered:** An INTERFACE "shared sources" library (rejected: every source compiled twice); keeping the in-binary harness (rejected: ships test code and a mode switch).
**Related Documents:** `CMakeLists.txt`; `Source/Tests/TestMain.cpp`; `.github/workflows/ci.yml`; testing.md *How to Run Tests*.

---

## Mix rules are shared, mix loops are not — 2026-10-29

**Note, 2026-11-02:** The ADR stands. The VCA gate, the stereo-pair lane rule, lane resolution, the post-fader mute and send gain left with their features, and the live read of automation with them. The fader floor and master gain are now in the gate.

**Context:** The real-time callback and the offline stereo bounce each carried their own copy of the solo / mute / VCA gate, the stereo-pair lane rule, the mute-lane semantics, the pan law and the send gain. Fixes had to be found and repeated by hand in both, and nothing checked that a bounce matched what was heard.

**Decision:** Each RULE has one definition in `Source/Audio/MixKernel.h` (header-only, `noexcept`, allocation-free, no engine state), called by both paths. The two mix LOOPS stay separate: the callback also handles live input, punch replacement, routed outputs, the stream bus, a 64-bit accumulator and meters, while the bounce renders windows from disk with a 512-sample automation step. A parity test renders one session both ways and compares them sample for sample; an invariants rule refuses a re-derived copy of a rule.

**Consequences:** A rule change is made once and is tested against both paths. The parity test found the live path reading automation one block late, now fixed. Differences that remain are deliberate and listed in CLAUDE.md (no live input, trim-follow or mono fold-down in the bounce). Merging the loops themselves was rejected: it would put disk-render concerns into the real-time path for no rule-level gain.

## Remote control is a separate class the engine owns by value — 2026-10-29

**Note, 2026-11-02:** The companion server was removed. `RemoteControl` holds the console OSC listener, desk macros and the MIDI control surface, and the audio thread touches nothing in it.

**Context:** `AudioEngine` owned the companion HTTP server, the console OSC listener with its log and channel-name capture, and the MIDI control surface directly: 26 public methods and their state in a class that is mainly the audio hub.

**Decision:** Move them into `RemoteControl` (`Source/Network/`). Keep the ENGINE as the owner, as one by-value member: each remote holds `AudioEngine&` and calls back into it from its own thread, so its lifetime must nest inside the engine's (the same rule as `SessionMirror` in `MainComponent`). By value, not `unique_ptr`, because the OSC thread calls back through `engine.getRemotes()` and a `unique_ptr` is reset before its object finishes destructing. `performRemoteTransport` stays on the engine as the single host interception boundary; MTC input, LTC, NDI and the click stay with the engine because the audio callback feeds them.

**Consequences:** The engine header loses the network API and three forward-declared server types; callers say `engine.getRemotes().startOsc (…)`. The control surface is destroyed earlier in teardown than before (inside `remotes`, not after most engine members), which only shortens the window in which its MIDI thread can reach the engine. Preferences (`appProps`, about 150 uses in `AudioEngine.cpp`) were not moved: they are per-strip engine state, and moving them needs a persistence-layer design rather than a mechanical move.

## Lead-in silence is written after STOP — 2026-10-29

**Context:** A copy with less audio than the take's timeline position needs silence before this pass's audio: a newly armed track on a continue or punch, or a backup/mirror drive added mid-session. That silence was written synchronously inside `startRecording`, before capture went live and under `structureLock`. Two hours into a show, one new track on three copies is about 3 GB; an empty backup added after an hour with 56 tracks is about 29 GB. The UI froze and every track missed audio until the write finished. The pad also ignored the auto-split ceiling.

**Decision:** Capture starts at once. `openTakeWriters` reserves part slots for each such copy (enough to stay under its container's ceiling) and opens the new audio in the part after them. A JSON marker per copy and track records the reservation. During finalization, on the STOP worker, `completeLeadIns` writes the silence into the reserved slots from the real timeline start (a rolling punch's captured origin). It deletes the marker, and only then do the manifest and report enumerate the parts. `recoverInterruptedPunches` completes any marker left by a crash, at session open and at record start.

**Consequences:** RECORD never waits on a lead-in, and the silence write moves to the background finalization. Such a track's take is now two or more files (silence part, then audio part), like any continued take, so every reader must go through `findTakeParts`. After a crash during a rolling punch, recovery sizes the silence from the armed position rather than the captured origin (the origin was only in memory); this is off by the start latency, on a new track only. Alternatives rejected: writing the silence on a background thread during the take, which competes with live capture for the disk; going live first and padding from the writer thread, which overflows the ~5.5 s FIFO on a large pad; and flattening silence and audio into one file at STOP, which rewrites the whole new take and re-splits it at the ceiling.

## Template

```
## [Decision Title] — [YYYY-MM-DD]

**Status:** Accepted / Proposed / Deprecated
**Context:** Why was this decision needed?
**Decision:** What was chosen?
**Rationale:** Why this option over alternatives?
**Consequences:** Positive and negative impacts.
**Alternatives Considered:** Brief.
**Related Documents:** Links to other docs.
```
