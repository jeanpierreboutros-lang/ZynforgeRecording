# Decisions Log

This file records key technical and product decisions for ZynForge Recording as lightweight Architecture Decision Records (ADRs). Each entry captures the context, decision, rationale, and consequences so future-you (or another engineer joining the project) can understand *why* something is the way it is — not just *what* it is.

When making a non-trivial decision, add a new entry below using the template at the bottom. Keep entries short and concrete. Link related ADRs by title rather than by anchor — anchors rot when sections are reordered.

---

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

Two documented rules were broken by that one line ("Text on a saturated accent: `brand::onSignal(bg)`. Never hardcode black or white", and the bare-`Colours::white` ban). `Tools/design_audit.sh` enforces both — and **excludes `Source/Theme/`**, because `Theme/` is where the sanctioned helpers are defined.

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

**Consequences.** Ten checks now gate every commit, and `Tools/invariants_audit.sh` is the file to add to when an audit finds a class rather than an instance. New device-touching code must guard or the build goes red. The cost is real: a rule mis-written is worse than no rule (two of the original seven were blind — one matched a commented-out call, one was satisfied by an unrelated comment in the same file), so **a new rule is not landed until it has been watched go red against an injected regression**. That step is now part of the ritual, not an optional nicety.

**Also fixed in the same pass**, all smaller, all real: `compactPath` labelling `/Volumes/RECORD/...` as `~/Volumes/RECORD/...` on the control that tells the engineer which drive the take lands on; raw `this` captured in `MarkerListDialog`'s and `TimelineStrip`'s async menu/modal callbacks (the SafePointer convention every other dialog already followed); a sub-44.1k device displaying as "44.1 kHz"; and `NoiseReportDialog`'s comparator not flooring non-finite keys — unreachable today because the analyzer clamps, but the comparator shouldn't *depend* on a caller's clamping when an unfloored NaN is the hardened-`std::sort` SIGABRT that already crashed the recovery dialog once.

---

## Ship console families you can't test, safely — 2026-08-13

**Context.** The console layer was OSC-only, so four of the six console families in the picker were honest stubs. Meanwhile a direct competitor shipped DiGiCo + Allen & Heath + SSL integration at $39.99. Going up-market means supporting desks nobody here owns — which collides head-on with the standing rule that untested desk control is the project's largest liability.

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

**Decision.** Bug classes get a **grep that fails the build**. `Tools/invariants_audit.sh` runs in CI (before the build, so a violation fails fast) and in a pre-commit hook, alongside the existing design gate. Seven rules today, each traceable to at least one shipped defect. The rule set is enumerated over **consumers** of a hazardous pattern, not over the call sites that trigger it — that inversion is what the `condemnAllStrips` story cost us four times.

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

**Consequences.** A few genuinely deep, pre-existing, or intentional items are DEFERRED with reasons in `tasks.md`: the capture-daemon `SetTrackCount` resize under the live audio callback and the companion worker's `getTrack` race vs `setStripCount` (both need a recorder-vector lock/snapshot; one is behind the experimental daemon flag), the reorder-across-a-mono/stereo-boundary *block rotation* (we refuse the pair-splitting step for now rather than corrupt the layout), and a couple of intentional-behaviour Lows (remote RECORD → fresh session, lossy audition stream).

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

**Context.** JP asked to re-record a section of an existing take and keep the audio before/after (classic punch-in). The naive implementation — have the recorder seek into the existing file and overwrite the punched region in real time — would put destructive, position-offset writes on the audio thread, against the file the player may be reading, with RF64/backup/mirror finalisation all needing to stay consistent. That is the single most dangerous thing this app could do; a glitch loses or corrupts a take.

**Decision.** The real-time capture path is **unchanged**: a punch records a clean fresh `Track_NN` from sample 0, exactly like any take. The punch is an **offline splice on stop**. In `startRecording`, each punch-armed track's existing copy (primary + backup + every mirror) is moved aside to a `.punchbase` sidecar *before* the writer truncates. In `stopRecording`, after the writers close but before the async SHA thread, `splicePunchFile` (`Source/Audio/PunchSplice.h`) writes `base[0,punchIn) + freshTake + base[after]` to a temp file and atomically swaps it over the take. The sidecar is the original; on any splice failure the take reverts to it. The splice runs for every copy, so all drives stay byte-identical (JP decision). Because it lands before the SHA hashing + report build, the report's `sha256` + `totalSamples` describe the spliced files with no JSON surgery. `servicePunch` punches into the **current** session (it used to record a throwaway new session from 0). Multi-part (auto-split) bases are refused — the single-file splice can't represent them.

**Rationale.** Keeping the audio thread out of it means punch-in cannot regress capture integrity — the worst case is a clean fresh take plus an untouched original. The splice is ordinary read-only offline DSP (same class as Consolidate/Bounce), unit-tested in isolation (`PunchSpliceTests`) and end-to-end (`PunchRecordTests`). Splicing every copy honours the redundancy guarantee the backup/mirror feature exists for.

**Trigger (Pro Tools-modelled).** The RECORD button is the single entry point and branches on context: (1) **a selection on a loaded take → punch in/out** — `onRecordClicked` rolls the transport from a pre-roll lead-in, and `servicePunch` (position-windowed) auto-punches in at the selection start and out at its end, with `servicePunchSession` playing a post-roll tail before teardown (matches PT's selection + pre/post-roll); (2) **no selection on a loaded take → continue** at the edit cursor, or append at the end (PT's "pick up where a good take left off"); (3) **no session → fresh take** (new session). `servicePunch` arms the splice at the *actual* playhead where recording begins (one tick past the selection start), so captured audio sits at the right timeline position rather than being nudged to the nominal start. PUNCH mode can still be toggled manually for a hands-free position window.

**Consequences.** Stop does a synchronous splice (copies the base once) — sub-second for song-length takes; noticeable only on very long bases, which are also the multi-part case we refuse. RECORD on a loaded session now overdubs rather than overwrites — a true "redo from scratch" means New Session (or clearing the take); the status line names the punch point so the behaviour is visible. A newly-armed EMPTY track recorded during a continue lands at 0 (no base to offset against) — fine for the common "re-arm the same tracks" overdub, a known v1 gap for mixed arming. **Monitoring v1 gap:** there's no input-monitoring switch across the punch — the player keeps playing the existing (about-to-be-replaced) audio of the punched track in the punch window rather than passing your input, so you perform against the old part; the *recording* is correct, and switching monitor to input at the in-point is the next pass. Pre-roll/post-roll are playback lead-in/out, not the recorder's history-buffer pre-roll (which is a separate knob, reused here for the lead-in length). See *Read-only offline DSP is allowed; real-time signal-modifying DSP is not* (punch fits the offline rule).

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

**Status:** Accepted (in progress — Phase 1 landed)
**Context:** Real DAW interchange (a session opening as an editable timeline of clips → source WAVs) needs AAF: it's the one format Pro Tools — the dominant target in this market — imports. AES31/EDL are text and verifiable here, but Pro Tools doesn't ingest them, so they miss the #1 DAW. AAF, however, is a binary Microsoft-Compound-File container with a fragile object model, and neither a DAW nor the reference library (pyaaf2 / AAF SDK) is reachable from the build machine to validate output.
**Decision:** Build AAF **natively in C++**, layered + phased, and **de-risk the binary with a write→read→assert oracle** (an independent reader in the test suite) rather than trusting the writer blind. Phases: (1) MS-CFB container writer + round-trip validator [**done** — `Aaf/CompoundFile.h`, `CompoundFileTests`]; (2a) object-model primitives — AUID/MobID/storedForm/value-encoders + the `properties`-stream codec [**done** — `Aaf/AafTypes.h`, `Aaf/AafProperties.h`, `AafTests`]; (2b) object-graph→CFB assembler — object = storage + `properties` stream + child sub-storages [**done** — `Aaf/AafObject.h`, `AafTests`]; (2c) the real AAF composition graph (Header/ContentStorage/Mobs/Slots/Sequence/SourceClip → WAVE descriptor + URL locator to the existing WAVs, external essence) **[BLOCKED — needs a reference `.aaf`]**; (3) fades-as-effects + embedded-essence; (4) real-DAW import field-check.
**Phase 2c blocker — 2026-06-09:** The verifiable foundation (phases 1–2b, all round-trip-tested) is complete. Phase 2c requires the **SMPTE class/property AUID registry** (~15 classes, ~40 PIDs) and an **in-file meta-dictionary** that maps the `properties`-stream PIDs to their definitions — both byte-exact. These cannot be reconstructed reliably from memory, and **no validation reference is reachable here** (PyPI/pyaaf2 blocked, no AAF SDK, no DAW, no sample `.aaf` on disk). Building them blind would be the "looks done, silently fails" artifact this ADR exists to avoid. **Unblock:** one reference `.aaf` exported from Pro Tools (any small session) lets us reverse-engineer the exact AUIDs/PIDs/meta-dictionary and complete 2c with confidence; OR a connected machine where `pip install aaf2` works (use pyaaf2 as the live oracle).
**Rationale:** The valuable format and the hard format are the same (AAF), so avoiding it would ship interchange that misses the main target. Re-deriving a fragile binary standard without the reference impl is risky, so we build our own oracle for everything verifiable here and treat "does Pro Tools specifically accept it" as a **documented field-check, exactly like the RF64 soak** — not a silent assumption. Phasing keeps each increment proven before the next; no new runtime dependency (the AAF SDK is heavyweight + unreachable offline).
**Consequences:** Real-DAW import remains a one-time human verification step before the feature is trusted. The CFB writer is a reusable, fully-tested foundation. If field import ever needs the canonical SDK, the phased boundary makes swapping the lower layer tractable.
**Related Documents:** `Source/Audio/Aaf/`, `Source/Tests/CompoundFileTests.cpp`, `tasks.md` (Full AAF/OMF interchange), this report's market analysis.

---

## No plugin hosting — 2026-04-12

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

## No Harrison LiveTrax / Waves Tracks Live attribution — 2026-04-18

**Status:** Accepted
**Context:** Both products occupy adjacent positioning (live multitrack recorder + virtual soundcheck). Question: does the product page / commit history / code comments credit them as inspiration?
**Decision:** No. ZynForge Recording is its own product. Do not reference, name, or credit Harrison LiveTrax or Waves Tracks Live in any artefact — code, comments, docs, commit messages, marketing.
**Rationale:** The product has its own design language and architectural choices (no plugins, per-cue tempo curves, OSC dialect parity across five consoles). Comparisons would mislead users about feature parity and dilute the brand.
**Consequences:** Engineers familiar with those products won't see explicit translation guides. Documentation has to teach the workflow on its own merits.
**Alternatives Considered:** Mention as a comparison reference in marketing only — rejected to keep the message clean.
**Related Documents:** `CLAUDE.md` workflow rule 5.

---

## Companion server is loopback-only with a per-session access token — 2026-05-24

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

**Status:** Accepted
**Context:** Until 2026-05-24 there were zero tests. Every change shipped on "the app launched at 49 MB and didn't crash for 9 seconds." A separate test target was discussed but adds CMake complexity (link-graph surgery, JUCE module duplication) and slows the dev loop.
**Decision:** The GUI app binary doubles as a test runner. Setting `ZYNFORGE_RUN_TESTS=1` (or passing `--run-tests`) at launch makes `Main.cpp` instantiate a `juce::UnitTestRunner`, run every registered `juce::UnitTest`, print results to stderr AND `~/Library/Logs/Zynforge/test-report.log`, then `quit()` with the failure count as exit code. `AudioEngine::setTestModeSkipAudioInit(true)` lets tests construct the engine without opening a 256-channel audio device.
**Rationale:** Lowest friction -- no extra target to maintain, the test binary always has the latest engine code, and adding a new test is one file (`Source/Tests/*.cpp`). Exit-code semantics let CI / shell scripts gate on test pass/fail.
**Consequences:** Tests can't run in parallel with the real app. The single binary is slightly larger. Test-only state (the `s_testSkipAudioInit` static) lives in production code. The first test pass found and fixed a real correctness bug (write paths silently no-op'd on uninitialised lane storage) -- which justifies the investment.
**Alternatives Considered:** Separate `juce_add_console_app` target -- rejected to avoid duplicate compilation of the audio engine + link-graph complexity.
**Related Documents:** `Source/Main.cpp`, `Source/Tests/AutomationTests.cpp`.

---

## Read-only offline DSP is allowed; real-time signal-modifying DSP is not — 2026-05-24

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
**Consequences:** Long sessions emit `Track_01.wav` + `Track_01_part02.wav` + ... numbered sequentially. Mix engineers consolidate by re-importing in order (Pro Tools' "Import Audio" + numerical sort, or any DAW's equivalent). The `session.report.json` does not currently list every part file individually — a future improvement would be to enumerate parts there. Backup writers roll independently to keep the mirror layout consistent.
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

**Status:** Accepted
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

**Status:** Accepted
**Context:** On launch the engine restored the active-session *folder* (so Save stayed enabled) but never loaded its *content*. The app came up with 0 channels and a Welcome dialog; combined with the menu-cache bug above, the whole UI read as dead. A session recorded but never explicitly **Saved** has no `session_mix.json`, so even when opened the mixer wasn't sized → "No channels yet".
**Decision:** `showStartupWelcome` auto-reopens the last session if it's still on disk (skipping the dialog). All open paths (File ▸ Open, Welcome `onOpen`, auto-reopen) go through one `openSessionFolder()` that pins the active dir, restores setlist + UI layout + full mixer state, loads the audio, and — when there's no `session_mix.json` — **sizes the mixer from the loaded audio track count** as a recovery fallback.
**Rationale:** Matches DAW expectation (come back to your work). Consolidating the three open paths fixed a real divergence (Welcome `onOpen` previously called only `loadSession`, leaving the mixer + Export grey). The recorder-from-audio fallback recovers recorded-but-unsaved sessions instead of showing an empty mixer.
**Consequences:** Relaunch lands straight in the last session. A fresh first run (no prior session) still shows Welcome. A session with neither `session_mix.json` nor `Audio Files/` is treated as "not a real session" and skipped.
**Related Documents:** `Source/UI/MainComponentSessionIO.cpp` (`openSessionFolder`), `Source/UI/MainComponentHelp.cpp` (`showStartupWelcome`).

---

## Aux sends are per-session in `session_mix.json`, not global appProps — 2026-06-04

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

**Status:** Accepted
**Context:** Generic OSC listened on UDP and accepted transport/arm/mute with no sender identity or secret, so any host able to reach the port could start or alter a take. Console dialect messages cannot carry a ZynForge application credential. The companion server had authentication, but returned success before marshalled transport commands actually ran.
**Decision:** Generic OSC generates a per-start token and requires it as the final string argument on every state-changing command. Console-specific inbound dialects retain read-only recorder value (names, scene markers, gain/trim) but cannot arm, mute, or drive transport. Companion transport commands marshal to the message thread, wait up to five seconds, and return the real engine result; record additionally requires a usable armed live input.
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
