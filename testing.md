# Testing Strategy — ZynForge Recording

## 2026-11-11 fix round

The review's 82 findings were fixed on six branches and merged on `integrate/fix-2026-11-11`. Each fix has a regression test, and each test was built and run on the unfixed code first and seen failing there, except ENG-08 (a lock in the callback, which no headless test can observe) and the tour's wording (MAIN-10), checked by eye. Nothing here has run on real hardware; the hardware steps are *2026-11-11 fix round* in [FIELD-TEST.md](FIELD-TEST.md).

### Build on this Mac since Xcode 27.2

Xcode was updated to 27.2 on 2026-11-06. From then on every `cmake -G Xcode` configure hung in xcodebuild / SWBBuildService at 0 % CPU, because `clang-stat-cache` sticks in `stat()`. Two ways round it, both used for this round:

```bash
# Xcode generator: turn the SDK stat cache off for configure and build
echo 'SDK_STAT_CACHE_ENABLE = NO' > /path/outside/repo/no-stat-cache.xcconfig
export XCODE_XCCONFIG_FILE=/path/outside/repo/no-stat-cache.xcconfig
cmake -B build -G Xcode
cmake --build build --config Release

# Or Unix Makefiles, in a separate build folder (never a half-configured Xcode one)
cmake -S . -B build-make -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/build/_deps/juce-src"
cmake --build build-make --target ZynforgeRecordingTests -- -j2
cmake --build build-make --target ZynforgeRecording -- -j2
cmake --build build-make --target ZynforgeCapture -- -j2
open -W -n "build-make/ZynforgeRecordingTests_artefacts/Release/Zynforge Recording Tests.app" \
     --args --test-report=/private/tmp/zynforge-tests.log
```

CI on GitHub's runners is unaffected. A checked-in xcconfig is suggested in [tasks.md](tasks.md). Never commit `build-make/`.

### New test files

- `Source/Tests/EngineDeviceTests.cpp`, *Engine device fixes 2026-11-11*, 18 groups; 51 failures on the unfixed code. Device ride-through (a pair losing its output, a CoreAudio restart in place, a silent stop with no error, the 5 s grace, a device back at another rate, punches), RECORD against a device stop, PLAY at a rate the session is not at, 256 outputs, the chosen buffer, device ids read again, `StereoMixWriter` write errors and gaps, the DEVICE panel's close and input meter, and the master meter after the fader. Tests drive `serviceDeviceWatch` with their own clock.
- `Source/Tests/UiFixTests.cpp`, *Main window, MIXER and PATCH fixes (2026-11-11)*, 28 groups; 48 failures on the unfixed code, from a tests-only commit made first. Prompts closed for a take as Cancel, the shortcut sheet against the key handler (about 400 combinations pressed in MIXER and EDIT), toasts, PATCH's off-device and shared-output rows, batch dialogs, missed samples after STOP, edit groups in mixer undo, and the punch-out restart. The test `MainComponent` runs no timers, so tests call `serviceModalsAroundTakes()` themselves; each window test starts with no modal open, lets a modal's answer run before checking it, and switches views before waiting. A suite earlier in the run left a "Welcome" dialog open; the fixture closes it and logs it (tasks.md).

### New groups in existing files

- *Capture fixes*, *Capture daemon link*, *Capture copies* (`CaptureFixTests.cpp`): lead-in silence ticks progress (`silenceBlockDelayMsForTests` slows it), the drain limit from a copy's open, hash passes paused while a take captures and resumed after a refused start, empty lead-ins dropped, the disk estimate with a copy still opening, "intact" with a failed write or a pulled drive, a strip whose lead-in is owed.
- *Timecode chase*: drop-frame by frame count both ways and across midnight; the label's age in the chase target. *Engine mix safety*, *Playback jumps*, *Audio callback*: K-weighting (a 997 Hz full-scale sine reads -3.01 LUFS within 0.02 at 48 kHz, 0.05 at 44.1 and 96) and cue release.
- *OSC remote*: a valueless STOP refused and said once a second, momentary MARKER and SCENE, scene 0 as a release, names by patch, junk packets. *Network state*, *Console transports + dialects*, *Network fixes*: Yamaha bare paths and ERROR, a TCP link lost and latched, head-amp capture saved on completion, New Session from Console restoring OSC, To stage. *MCU protocol*: touch notes and the -60 dB floor.
- *Twin recorder link* / *Twin recorder host*: a new link starts clean, an adopted take is compared, STOP during this Mac's own STOP. *Capture supervisor*, *Capture helper device and exit*, *Capture link transport*: the helper spawned with nothing of the window's, starting without a default input, the exit code's reason, a reconnecting window greeted during a long STOP.
- *Session fixes* (`SessionFixTests.cpp`), *Session backup*, *Stereo-pair export*, *Audio import worker*, *Song detection*, *Marker store*: snapshots refused and retention, crash-added strips, session prompts closed for a take, file panels that reserve nothing while open, folder names, 200-byte names, Welcome results, read-only autosave, raw export BWF and names, poly import read once, Detect Songs naming an unreadable take, marker names per session, template menu binding and ticks, the capture-daemon toggle, Remove last capture. Seam: `launchSessionPicker` / `sessionPickerForTests`.
- *EDIT row fixes* (`EditRowFixTests.cpp`), *EDIT ruler scroll align*, *Timeline edits*, *SetlistBar accessibility*: EDIT prompts closed for a take, Memory Locations during a take, ruler hit-test, Crop and Ripple undo with later cue and marker edits, continued-take peaks, clip prompts finding their clip, zoom around the playhead, the quit-time cache save, clip copy during a take, lanes from memory while peaks load, group shading, "(pick a cue)". 33 failures on the unfixed code.
- `tools/test_verify_tools.py`: `verify_take.sh` runs Python isolated (the test plants a `json.py`).

### Changed tests

- *A chosen interface that comes back opens at its own rate and buffer* (*Engine repairs*) now picks 256 and expects the chosen 256, not the stand-in's buffer.
- *OSC remote*: the old "an address with no argument is a press" checks now expect a valueless STOP to be refused.
- *MCU protocol* round-trips from -60 dB, the new floor.
- *Review persistence repairs*: Session Info shows the folder name, not the project's.
- *Strip lifetime and mixer fingerprint*: the field list has `editGroup`.

### Gate

The invariants gate has a 40th rule, *every modal closes for a take started behind it*: any `enterModalState`, `launchAsync`, `launchAsynchronously`, `showAsync` or `showMessageBoxAsync` in `Source/UI` needs `closesForTake` / `launchClosesForTake` / `askClosesForTake` nearby, or a `modal-over-take-ok:` comment, and `showAsync` / `showMessageBoxAsync` always count as unmarked. Native file panels are left out. It was seen failing with the Recover sessions list unmarked and with an allow comment removed.

### Results

Each branch alone, before the merge, Release, built with Unix Makefiles (`main` had 1701):

| Branch | Full suite | Notes |
|---|---|---|
| engine | 1719 / 0 | *Engine device fixes* 18 / 0 |
| capture | 1716 / 0 | |
| remote | 1722 / 0 | *Twin recorder host* 20 / 20 under seven busy CPU loops; `tools/test_verify_tools.py` OK |
| session | 1736 / 0 | |
| EDIT | 1717 / 0 | |
| main window | 1729 / 0 | *Main window, MIXER and PATCH fixes* 28 / 0; one earlier full run had 3 twin-host failures under load, passed alone 20 / 0, before the remote branch's fixture fix |

Both gates were clean on every branch, and the app and helper built.

**Merged on `main` (all six branches + the lead's gate commit), built with the Xcode generator (`XCODE_XCCONFIG_FILE` workaround above):**

- **Release (universal):** **1834 test groups, 0 failures**, launched through `open`, at `66e7fee`. The two four-branch and six-branch Makefile runs before it were 1795 / 0 and 1834 / 0.
- **ASan/UBSan (Debug, arm64):** the first full run was 1834 / 0 but UBSan logged a signed overflow in `EditTimeRuler::idleSignature` (summing marker-name hashes; it existed before this round, the new marker tests reached it), fixed in `66e7fee`. The second full run stopped on a stack-use-after-scope in a *Capture path* test: a daemon reader thread ran a test hook after the test's local release flag was gone, which the HELP-04 change (a superseded reader is no longer joined before the next client is served) made possible. The three wait-loop hooks now own their flag (`5c41cbf`, test-only). Final full run at `5c41cbf`: **1834 / 0, no sanitizer reports**.
- **TSan (Debug, arm64):** full run at `d9461a7`: 1834 groups, 1 failure, no reports. The failure was the new 64 KB Latin-1 OSC timing check (38 ms against a 25 ms bound in the slower Debug build); Debug and sanitizer builds now get 150 ms (`66e7fee`). After that, *OSC remote* 20 / 0 and every *Capture* suite (244 groups) 0 failures, no reports. The full TSan suite was not re-run after those two small commits.
- Release *Capture* suites at `5c41cbf`: 244 / 0. Both gates are clean (40 invariants rules, the new modal rule among them). The app and helper built universal.

### Not covered headless

- A real device pair losing its output, a real CoreAudio restart and a real HAL error: the tests drive the engine's device callbacks and `serviceDeviceWatch` directly.
- ENG-08, the lock the callback took: nothing headless can see a mutex taken; the code no longer has one.
- Mackie touch on a real surface, Yamaha SCP on a real desk, a real twin pair, real file panels.
- Toast wrapping, PATCH rows and the shortcut sheet at real size: checked by eye (FIELD-TEST.md).

## 2026-11-04 fix round

The whole-codebase review's 75 findings were fixed on four branches and merged. Each fix has a regression test, and every regression test was built and run on the unfixed code first and seen failing there, with the exceptions listed below. Nothing here has run on real hardware; the hardware steps are *2026-11-04 fix round* in [FIELD-TEST.md](FIELD-TEST.md).

### New test files

- `Source/Tests/CaptureHelperTests.cpp`, *Capture helper device and exit*. A test-only device type, `RigType` (an interface plus a separate headphone output, named after the output as CoreAudio's combiner names it), goes in through `CaptureDaemon::useDeviceTypeForTests`, so the helper's real device paths run without CoreAudio. It covers input-only opening with "Headphones" unplugged mid-take, an arm change that keeps the device and its pre-roll, device work on the helper's main thread whichever thread asked, the helper staying up until STOP's reply has gone out, a status built before STOP never arriving after STOP's reply (added at the merge, after a twin-host test failed three times in full runs; seen failing first), and the exit policy in `HelperExit.h` (a take finishes at once only when the system is ending, or on a second signal). *Twin recorder host* gains *A daemon STOP whose "stopped" status arrives before its reply still reaches the follower as a STOP*, which holds STOP's reply while the window ticks; it passed before the window-side change too and is kept as a guard. Under seven busy CPU loops the twin group failed 3 of 12 runs before the two merge fixes and 1 of 12 after; see tasks.md.
- `Source/Tests/CaptureEngineRepairTests.cpp`, two suites, *Capture repairs* and *Engine repairs*, both friends of `MultitrackRecorder`. On the unfixed code they failed 55 and 18 times. They cover copy lines after STOP, the float AIFF flush, root-level takes, a session folder moved mid-take, the report's clip figures, refusal texts, the probe on a pulled drive, crashes inside a punch install, copy hashes, undo after a continue, close cuts in one file, the device coming back, markers through a helper STOP, loudness, pre-roll across layout changes and StereoMix alignment.
- `Source/Tests/MixerFixTests.cpp`, *Mixer and main window fixes*, 32 groups; 98 failures on the unfixed code. Seams: friend declarations, `PreflightDialog::getRows`, `BigClockPanel` getters and the `backupdlg::rootCheckStallForTests` hook. `--test-filter` takes the registered name, not the class name.

### New groups in existing files

- *Capture path* (`CapturePathTests.cpp`): a lost helper that still holds its take is not taken for gone; a helper told to quit mid-take stays up for STOP and answers it (the real helper binary); Quit waits while a take its device ended is still closing; resumed hashes reach the primary first, and a backup that is gone is owed, not a failed take; a session another process is hashing is not hashed again.
- *OSC remote*: a momentary STOP button gives one STOP tap per press; a momentary RECORD button never stops a take. The timed seam is `OscRemote::dispatchForTest (msg, nowMs)`.
- *Timecode chase*: LTC at 29.97 non-drop and at 23.976 told from 30 and 24; MTC at 29.97 non-drop told from 30 after the master has run a while; a full frame between quarter-frame runs never keeps a stopped master running.
- *Twin recorder link*: a follower still closing its last take refuses RECORD and says why; a follower whose window restarted during a linked helper take still stops with the leader; a take a restarted follower can't tie to the link is refused with the real reason. `FakeHost` gains `daemonStop` and the two link-memory overrides.
- *EDIT integrity*: scans stop reading the take's drive during a take; on-screen reads use the page cache and whole-take scans do not; a new session's saved peaks hold none of the last session's; quitting while saved peaks load leaves `WaveCache.wfm` as it was; a strip move during the load; imports into freed and live names; held takes through add, link and delete; a one-file stereo pair scans once.
- *Session fixes*: Save As and Move keep mixer edits made during the copy; Export Audio, Export Songs and New Session confirmed after another session opened; Export Songs after the songs moved; Restore Backup under another name; RECORD with the Local Storage drive gone; raw export of an unlinked stereo take; Save As waiting for hashes. Test hooks: `launchForTests` on `ExportAudioDialog`, `ExportSongsDialog` and `NewSessionDialog`.
- *Song detection*: Detect Songs hears each strip's own channel of a shared stereo file.
- `tools/test_verify_tools.py`: a copy that could not be hashed is a note for a whole primary; a copy flagged on its own is a note; a primary that could not be hashed still fails; a pending report says how to finish it; the fallback says it is the open session, not the recorded one.

### Exceptions to "seen failing first"

- *A helper told to quit mid-take stays up for STOP and answers it* passed before the fix too: the race is narrow on an empty test take. It is an integration guard; the red test is *A helper stays up through STOP until STOP's reply has gone out*.
- `test_a_primary_that_could_not_be_hashed_still_fails`, *Off stays off through a mode flip* and *Clearing the LTC source drops LOCK at once, and the decoder relocks on its own* are guards and pass before and after. The LTC-source race itself shows only under ThreadSanitizer.
- *Add Tracks while a STOP is still finalizing adds nothing and says so* passed on the unfixed code: that finding was not a fault as described, since every async STOP already holds the session-I/O reservation. It stays as a guard.
- Three tests needed a second try before they failed: the strip-move-during-load test first passed because the real reorder handler ran a second, corrective rescan, so it now stands in for that handler; the Local Storage test lost its setting to a settings reload, and with `saveIfNeeded()` added it was seen failing with the fix disabled by hand rather than on the unfixed code; the page-cache test calls a new class, `scanio::ScanFileInput`, and was seen failing against it written without the page-cache switch.

### Changed tests

- *OSC transport goes through the host interception boundary* sent `/zynforge/record 0` as STOP; it now sends `/zynforge/stop`.
- An MTC full frame no longer makes chase live, so *MTC full-frame message publishes the timecode immediately, as a cue, not a running clock* (which asserted the old behaviour), *A locked MTC source stamps the take through the engine* (*Capture metadata*), five fixtures in *Network state* and *Chase liveness follows the selected source* (*Engine review repairs*) now feed a quarter-frame run, through `ltctest::mtcQuarterFrames` in `Source/Tests/LtcTestSignal.h`.
- A *Network fixes* twin test that disconnects during a take now presses Disconnect twice.

### Results

Each branch alone, before the merge: helper 1610, engine 1627, EDIT 1612 and mixer 1623 test groups, 0 failures (main had 1591). `tools/test_verify_tools.py` 28 OK, `tools/test_notarize.py` 16 OK. Both gates are clean. The app target builds; the built app was not launched.

On the merged branch:

- **Release:** **1701 test groups, 0 failures** in the final run, launched through `open`. Earlier runs of the merged tree had one failure each in the twin-host test that is still flaky under load (see tasks.md).
- **ASan/UBSan:** 1699 test groups ran with one failure, a test expectation the merge then corrected (a backup whose drive is gone is owed, not failed); *Capture path* re-run under ASan after the correction: 51 groups, 0 failures. No sanitizer reports.
- **TSan:** **1699 test groups, 0 failures**, no reports, before the two merge-time helper/window fixes; after them *Capture helper device and exit*, *Capture daemon*, *Twin recorder host* and *Capture supervisor*: 53 groups, 0 failures, no reports.

*Twin recorder host / A daemon take found rolling offers a re-link, and a confirmed STOP reaches the follower* failed once in a full run on two branches while the machine was loaded ("This take ended without STOP"), and passed alone and in every later full run. A likely cause is in tasks.md (*Bug audit, 2026-11-04*).

### Not covered headless

- The OS's SIGTERM-then-SIGKILL at logout, and a signal's sender pid: the exit policy is tested as a pure rule.
- A real CoreAudio combined device losing its output; `RigType` stands in.
- A real full JUCE clone from GitHub: the pin was checked in the existing checkout and in an offline scratch project.
- The new CI steps: CI is blocked by billing.

## Post-STOP rescan on the exFAT drive — 2026-11-04

Measured on the external exFAT SSD (FSKit, 128 KiB clusters, macOS 26.6.2), by the drive's own *Bytes (Read)* (`ioreg`) against the bytes asked for, and the app's reads by `proc_pid_rusage`.

- **Layout:** 16 files of 384 MiB written side by side in 32 KiB writes, the same preallocated in 64 MiB steps (`F_PREALLOCATE` works on that volume), extended by `ftruncate`, and one written alone: all read back uncached at 1.00× (512 KiB reads, one file or four at once). Ruled out.
- **One file at a time:** cached 512 KiB reads cost 1.65–1.75× (read-ahead past where the reader stopped), uncached 1.00×, read-ahead off 1.00×.
- **55 files in turn, four threads, 512 KiB cached reads (the thumbnail pattern):** 66.3×, 39 MiB/s of data. Read-ahead off: 0.96×, 1537 MiB/s. Uncached: 1.00×, 1675 MiB/s.
- **The app (`eca170e`) on that take:** its own reads matched the drive's 2.7 GB/s, with the four thumbnail threads in `read()`.
- **Timers:** a 1 ms sleep took 1.5 ms normally, 7.5 ms under a utility clamp and 64 ms under a background clamp; a 50 ms sleep took 60, 116 and 249 ms.
- **Uncached reads see fresh writes:** a file written through a buffered stream and read back uncached before and after a header-style rewrite, on that drive and on the internal APFS disk: identical.
- **Real take, 4 h 10 min, 55 tracks, 114 GB, cache moved aside, the built app in `--isolated-settings`:** uncached reads at background priority: all waveforms in 24 min 16 s, the drive at 1.00× and 85–170 MiB/s. Uncached with an explicit throttled disk policy (the thread then left QoS): 1 min 48 s at about 1.28 GiB/s. Shipped, uncached at utility priority: in 3 min 9 s. The old build took 3 h 10 min after STOP for the 85 GB three-hour take. Quit took about a second in each run.
- **Playback pattern (not changed):** 55 streams read at real-time speed through the cache cost 195× (7 of 88 rounds late); with read-ahead off 1.08×, uncached 1.16×, none late.

New groups: *Whole-file passes read past the page cache and see every byte* (*Fast hash*: byte-exact reads across odd chunk sizes, seeks, the end of the file, a file still growing, the scan's buffered stream, no folder or missing file) and *Waveform scan threads run at utility priority, not background* (*EDIT integrity*: the thread's QoS is utility and it has no explicit disk policy). The invariants gate fails if a scan or hash opens a cached stream, if the scan threads stop starting through `scanio::startScanThread`, or if those files call `setiopolicy_np`. **Release: 1590 test groups, 0 failures**, launched through `open`; ASan/UBSan and TSan were not re-run.

## One waveform shape — 2026-11-04

*Zoomed-out waveform is one shape* (in *EDIT view fixes*) replaces the median-and-peak check. `buildWaveformOverview` now returns one level per column: a lone loud window among quiet ones gives 0.28 after smoothing, not its 0.95 peak; a sustained 0.70 stays 0.70; alternating 0.8 / 0.1 windows give their quadratic mean, 0.57. **Release: 1588 test groups, 0 failures**, launched through `open`; the invariants gate is clean and the build adds no new kind of warning. ASan/UBSan and TSan were not re-run for this display-only change. The look itself is checked by eye (*One waveform* in [FIELD-TEST.md](FIELD-TEST.md)).

## After the first long take — 2026-11-03

The take is described under *First long take* in [FIELD-TEST.md](FIELD-TEST.md). What was measured on it, on `5c7a3e4`: five minutes after STOP the rows on screen showed the first 155 s of the take, and the drawn shape matched the first 155 s of `Track_02.wav` (correlation 0.87) rather than its last 155 s (0.19). The lanes then grew slowly. The app read 2.7 GB/s from the drive while its file offsets moved about 9 MB/s, and a sample showed all four scan threads in `read()` under `readMaxLevels`. A test reader making the same 1.5 KB reads on the same files, at background priority and across 14 files in turn, cost only 1.3–2.1× the bytes it asked for.

Two new groups in *EDIT integrity*:

- *A stopped take keeps its live waveform until its own rescan finishes*: a held row with no source is painted after STOP has settled, then rescanned. With the old release condition it fails, as it should: the first paint dropped the hold.
- *Waveform scans read through a buffer and keep their cache keys*: `scanio::FileSource` has `juce::FileInputSource`'s hash and a buffered stream, a buffered thumbnail draws the same peaks as a bare one, and `ConcatReader::createForScan` returns the same samples as `createUnchecked`.

**Release, ASan/UBSan and TSan:** 1588 test groups, 0 failures each, launched through `open`, with no sanitizer reports. The tool checks pass and both gates are clean; the invariants gate now also keeps `scanio::` to the drawing code. The built app launched with `--isolated-settings` and quit by itself after 35 s with no crash report.

**On the real take, after install.** `577592c` reopened the session: the app read 3.06 GB/s from the drive while its WAV file offsets moved 18 MB/s (`5c7a3e4`: 2.7 GB/s and about 9 MB/s), and a sample found its threads waiting, not reading. So the buffer did not fix the waste. A plain Python reader on the take's files, with the app closed and at background priority, cost the drive 14.7–14.8× the bytes it read with 512 KiB reads (one, two or four threads), 11.0× with 4 MiB reads and 2.0× with 1.5 KB reads from four threads. The drive is exFAT mounted through FSKit (`mount` shows `fskit`).

## JUCE 8.0.17-security — 2026-11-03

On JUCE 8.0.17-security: **1586 test groups, 0 failures** in Release and under ASan/UBSan, each launched through `open`, with no sanitizer reports. Under TSan the full run had one failure, in *LOCK stops the right-click menus on strips and the setlist*: a menu from the check before it could still be closing on that slow run and counted as the strip's. The strip itself refuses a right-click under LOCK. The test now measures only its own click. After that change: Release 1586 / 0, and *Main window guards* 87 / 0 under TSan, four runs in all.

**Before install, at `5c7a3e4`:** **1586 test groups, 0 failures** in Release, under ASan/UBSan and under TSan, each launched through `open`, with no sanitizer reports. The full TSan run took 16 minutes. `tools/test_notarize.py` and `tools/test_verify_tools.py` pass. The built app launched with `--isolated-settings`, stayed near 109 MiB and 2% CPU, quit by itself after 35 s and left no crash report. The development Mac has this build (see [INSTALL.md](INSTALL.md)); nothing here has run on real hardware.

Two tests changed with the framework. The test audio format overrides the new `createWriterFor`. The depth-guard test keeps its coverage floors on well-formed gaps, because since JUCE 8.0.16 a malformed UTF-8 byte between JSON tokens no longer decodes as a space; malformed gaps are still checked for the guard never under-counting.

## `9a40f54` — 2026-11-03, the round after `d4a9a57`

Three tests each failed once while seven branches shared the machine: *A hole in the primary reaches the window from the daemon*, *session.report.json includes SHA-256 per part file* and a *Capture supervisor* quit. The first was a race in the test: the audio FIFO can overflow before the copy's ring does, so it now waits for the copy's own report. The other two have not come back in any later run, and their cause was not proven.

**Final pass on `main`, built at `7221295`:** **1586 test groups, 0 failures** in Release, under ASan/UBSan and under TSan, each launched through `open`, with no sanitizer reports. Release also passed three full runs in a row just before that. Both gates are clean, and `tools/test_notarize.py` and `tools/test_verify_tools.py` pass. The built app launched, quit by itself in 182 s (the rest is the microphone wait on a new ad-hoc build) and left no crash report.

Nothing in this section has run on real hardware. The development Mac has this build as `9a40f54`, the Mac mini has `281c6c0`. The hardware steps are under *2026-11-03 fix round* in [FIELD-TEST.md](FIELD-TEST.md).

### New suites

- *Main window fixes* (`Source/Tests/MainWindowFixTests.cpp`), on the shared silent device (`Source/Tests/SilentAudioDevice.h`).
- *Capture path* (`Source/Tests/CapturePathTests.cpp`): daemon device, copies following strip moves (offline, crash mid-move, rollback), pre-roll ring, live helper sessions, crash remains, helper lifecycle, disk figures, refused threads. *A helper asked to exit closes down cleanly* needs the built `ZynforgeCapture` binary beside the test app and is skipped without it. *A take past 4 GiB* writes a sparse file, so its temp volume must support sparse files (APFS does; exFAT would write 4 GiB of zeros).
- *Session fixes* (`Source/Tests/SessionFixTests.cpp`). It drives the handoff, Save As, Move, Close, Quit and replacement paths on a real `MainComponent`, with a folder rename standing in for a pulled drive. A real short write comes from `RLIMIT_FSIZE`, as in *Session safety*: a session file under the stream's 32 KiB buffer goes out in the flush whose result the stream ignores.
- `Source/Tests/TakeSafetyTests.cpp` holds three suites. *Punch only where the timeline is the file* runs the rule, then every way a punch starts on a real session: stopped cursor, answered cursor, rolling, selection, PUNCH mode, the engine's own door, remote, twin and daemon. *A failed read is a failure* gets its failures from real short reads, by cutting real files with truncate(2) both before and while they are open, and from a real errno with read(2) on a directory; it covers each consumer. *Edits and exports keep their word* covers strip paste, continues, session length, exact exports, overs and job cancellation.
- *EDIT row fixes* (`Source/Tests/EditRowFixTests.cpp`): real session swaps and a refused delete through `MainComponent`, the MIXER and PATCH routing clicks, and the row's mouse gestures. *A strip's routing holds through a take and its finalization* is in *EDIT view audit 2026-10-29*, which can reach the recorder's finalization hook.
- *Network state* (`Source/Tests/NetworkStateTests.cpp`): cues through VSC, every profile's soundcheck state, read-backs (the fake desk answers later, loses single writes, ignores writes, or says nothing), the state across links, sessions and relaunches, the check, damaged files, reads landing under a take, the punch range for every remote, desk health, and the small remote fixes.
- *Engine fixes* (`Source/Tests/EngineFixTests.cpp`) runs the real `AudioDeviceManager` and RECORD's whole path on a fake rig (`Rig`, `FakeType`, `FakeDevice`), and the device rules as pure data.
- `tools/test_verify_tools.py` now covers `verify_take.sh` on real WAV files (needs ffprobe, jq, xxd, shasum), `soak_monitor.sh` with a private HOME and a fake daemon in a fake bundle, and `verify_reaper_export.py`'s verdict. The soak tests skip themselves if the real app is running, because `pgrep -x` would find it.

### Gate changes

`tools/invariants_audit.sh` now has 38 rules. New: "the 10 Hz tick reads no take length", "noting a capture reads nothing back from the take's volume", "the device test goes through the engine" and "whatever writes or reports from audio opens it checked". "a take's status path reads no disk" now also covers `EditPage::timerCallback`, red-checked by putting the old stat back there.

### Changed tests

- Tests that pinned STAGE or Restore Gains as finished on the send now answer the read-back, and the count of messages each sends doubled.
- `useSilentDevice` in `Source/Tests/SilentAudioDevice.h` calls `noteChosenDevice`, because a fixture device is a chosen one.

### Writing tests here

- Never put a side effect inside `jassertquiet`: it is not evaluated in a Release build, which is how a first version of the truncate fixture quietly cut nothing.
- Under heavy machine load a `MainComponent` teardown can take minutes: `~EditPage` waits for the waveform scan threads, which run at utility priority. Sample the process before calling it a hang.
- *Capture supervisor* uses port 49745 in every worktree, so two suites running at once may answer each other's daemon. It failed once on "quit send failed" with seven branches loading the machine and passed on every later full run. The shared port is the likely cause, not a proven one.
- Test hooks: `MainComponent::leaveMissingSessionPromptForTests` answers the missing-folder prompt, and `templatesDirForTests` points the template list at a test folder.

### Not covered headless

- PATCH's dot drawing (the real-R dot, parted dots) and the banner's "1 of 3" counter are paint and need an eye. Pagination, order, turns and glyph fit are tested. Pills and GRID are tested through bounds.
- `ScanRepaint` is tested as logic, not as a real paint. The MARKERS LANE tag and the grid during a take were read, not rendered.
- A real EIO from a pulled drive: truncated files and a read on a directory stand in for it.

## `d4a9a57` — 2026-11-02, design critique fixes

`d4a9a57`: **1236 test groups, 0 failures** in Release, under ASan/UBSan and under TSan, each launched through `open`, with no sanitizer reports. CI passed Debug and Release on the same commit. Both gates are clean. The built app launched, quit by itself in 87 s and left no crash report. The commit after it changes only the window title and these docs: it passed CI, was not smoke-tested and is not installed.

- *Main window guards* gained: an alarm moves nothing and covers no header button at three window sizes; VSC asks, shows it is on, survives a save and reload, restores, and undoes; a strip added after the soundcheck keeps its output; a new or another session starts with VSC off; the desk's soundcheck chip; a pre-flight problem's button; mixer controls hidden in EDIT; the small clock following a take; every fader level at every strip width.
- *Edit track row* gained: a strip on the master reads the same as in the MIXER.
- *Display text* is new: counted plurals, typeset dashes, decimal units, the record-time format, R button contrast against the alarm red.
- Red check: the test for a new session failed until `clearAllStripOverrides` forgot the saved patch.
- PATCH's two dots, the meter scale, and the Welcome storage line were checked in rendered images only, not by a test.

## The cut and the design pass — 2026-11-02

Current source, after the cut to recorder and virtual soundcheck and the design pass: **1219 test groups, 0 failures** in Release, under ASan/UBSan and under TSan, each launched through `open`, with no sanitizer reports and no forced thread stops. `tools/design_audit.sh`, `tools/invariants_audit.sh`, `test_notarize.py` and `test_verify_tools.py` pass. One later fix, which limits the crash notice to the recorder and its helper, brings it to 1220 groups in Release and CI; the sanitizers were not re-run for that string check. Nothing here has run on real hardware. It is installed on the development Mac only; see [INSTALL.md](INSTALL.md).

Nothing in this section has run on real hardware, and it is installed on the development Mac only. The hardware steps are under *2026-11-02 cut* in [FIELD-TEST.md](FIELD-TEST.md).

### The cut to recorder and virtual soundcheck

- Suites that went with their features: *Automation*, *Automation toolbar accessibility*, *VCA panel accessibility*, *Click track renderer*, *Punch mode* (it held only the automation punch range), *Transient detector*, `CompanionServerTests`, `CompoundFileTests` and `AafTests`.
- *Session safety* gained the tests for sessions holding removed data: a fixture for each removed key, the session without any of it, once only, a failed copy refusing the save, and the bus strip that records as an ordinary strip. It also holds *A staging file or a busy waveform cache cannot fail a session copy*, for Save As and Show Handoff.
- *Dialog audit fixes* covers Backup & Mirror Drives and Export Audio.
- *Audio callback*'s parity test runs on fader, pan, stereo pair, mute, solo and master.
- `Source/Tests/SavedTakes.h` is how a test gets a session with several takes, now the app makes none.
- Red checks: taking the open call out of the old-session copy failed 102 checks. Offsetting the offline gain failed the parity test at all four compare points. Removing the save guard, with a key counted as data, failed 8 checks.
- Gate changes in `tools/invariants_audit.sh`, now 34 rules: "detached uploads do not inherit short-lived output pipes", "engine-reference members are nested inside AudioEngine lifetime", "external inputs fail bounded and validated" and "offline renders read automation under a blocking lock" went with their subjects. "remote transports share the host interception boundary" lost its companion clause and gained one for the desk macro. "long renders are cancellable and transactionally installed" keeps the track export and bounce lines. "mix rules live only in MixKernel.h" now covers the pan law, the solo gate, the fader floor and master gain, and each pattern was seen to go red against an injected copy. "session_mix.json is written only through the mixer guard" lists `SessionRemovedData.h` among the files that only read or copy it.
- Stage measurement, not the current figure above: at `5573c18` the `ZynforgeRecordingTests` app passed 1216 groups, 0 failures in Release and under TSan, each launched directly, the full TSan run with no sanitizer reports (the base commit, measured the same way: 1259 groups, 0 failures, no reports). Run alone under TSan, launched directly, these suites passed with no sanitizer reports: *Mix kernel*, *Audio callback*, *Engine mix safety*, *Playback jumps*, *Engine review repairs*, *Session safety*, *Recording integrity*, every *Capture* suite, *Audit fixes*, *October audit regressions*, *Dead-channel alarm*, *Session jobs*, *Song export*, every *Punch* suite and the *Concurrency* suites. *Clip split + crop* passed its 10 groups, but its run ended in a TSan report at process exit. The base commit gives the same report, so it does not come from this stage.

## Fix pass — 2026-11-01

At that pass: **1298 test groups, 0 failures** in Release, under ASan/UBSan and under TSan, each launched through `open` with no sanitizer reports and no forced thread stops (Release and TSan at `58a7267`; ASan/UBSan after a test-only fix to `CaptureFixTests.cpp`, where a detached probe thread outlived the event it waited on). `tools/design_audit.sh`, `tools/invariants_audit.sh`, `test_notarize.py` and `test_verify_tools.py` all passed. CI passed the same 1298 groups on the macOS 15 runner, Debug and Release, at `f2df45f`. CI moved from macOS 14 because GitHub retires that runner on 2026-11-02.

Nothing in this section has run on real hardware, and it is installed on the development Mac only. The hardware steps are under *2026-11-01 fix-pass acceptance* in [FIELD-TEST.md](FIELD-TEST.md).

### New suites

- *Capture copies* also pins, each seen failing on the old code: a drive that stops answering costs only its own reading; a punch with a hole keeps the original on every copy; a copy that keeps writing slowly still has a total limit at STOP; a copy stuck in post-take work costs the primary nothing; a hole in the primary reads as missed samples; RECORD waits for no copy; a copy that opens later than its ring pads silence in step; a dead mount can hang neither RECORD nor STOP. Stalls come from test seams: no real hung NAS or sleeping drive has been tried. A one-off interposer logging every main-thread `stat`, `statfs`, `open` and `rename` on a take's folders, across a daemon take and a local take with backup and mirror, logged no call on a destination while the take rolled or finalized.
- *Main window guards* (`Source/Tests/MainWindowGuardTests.cpp`) runs RECORD's whole path, pre-flight included, on `SilentDevice` (now in `Source/Tests/SilentAudioDevice.h`), a fake `AudioIODevice` registered with the engine's device manager. It never reaches CoreAudio, so there is no consent prompt and no real interface is touched. Use it for anything that needs `prepareRecordStart`. It shares one `MainComponent` across its groups.
- *Capture fixes*, *Capture daemon link* and *Capture copies* (`Source/Tests/CaptureFixTests.cpp`). *Capture daemon link* is a real `MainComponent` over a real socket to an in-process `CaptureDaemon`, on a random port, so branches can run it side by side; `MainComponent::captureDaemonPort` is the seam and the default is still 17890. *Capture copies* holds one copy's thread in a write through `beforeCopyWriteForTests`. Run *Capture copies* under TSan after touching `CaptureCopy.h`.
- *Session safety* (`Source/Tests/SessionSafetyTests.cpp`) covers mixer ownership on every save path, reattach, retention and failed reopen.
- *Timeline edits* (`Source/Tests/TimelineEditTests.cpp`) covers undo across a take, Crop and Ripple Delete, cues, render warnings, the final-flush check, the size refusal, sample-rate conversion, song detection on the timeline, marker types, Heal and QC, including its noise floor.
- *EDIT integrity* (`Source/Tests/EditIntegrityTests.cpp`) drives MainComponent's real commands and the row's mouse handlers on a real session folder. `MainComponent::confirmForTests` stands in for the delete prompts.
- *Network fixes* (`Source/Tests/NetworkFixTests.cpp`). A failure in "Hostile nesting on a worker's stack" or "A worker survives random text" may show up as the test app dying with SIGBUS, with no summary line in the report. That is the failure, not a harness fault: the guard let a line through to the real parser on a 512 KB stack. The flood test sleeps 1 ms per packet in its listener and takes a few seconds.
- *Engine mix safety* (28 groups) and *Playback jumps* (10 groups), in `Source/Tests/EngineMixSafetyTests.cpp`. *Timecode chase*, *Mix kernel* and *Engine review repairs* gained groups. *Playback jumps* holds one rule per block: every sample the playhead moved past reached every track. It is deliberately not timing-sensitive: on a slow or loaded machine a jump holds for longer and the rule still has to hold, so a failure is a real loss of audio, never a late disk. It ran green at load average 149.
- `python3 tools/test_verify_tools.py` covers which session `verify_take.sh` judges and whether `verify_reaper_export.py` catches a short render, without REAPER.

### Red evidence

- Red seen by a deliberate-break build for L-only sends, raw live-minus-capture, solo through `channelAudible`, nothing held off, and the jump hold and cue pass removed.
- `PlaybackReader` under ThreadSanitizer: a standalone harness (one reader jumping about, a cue thread, the background thread refilling the pool), 8 s, 4.36 M samples checked, no reports, no misplaced audio.

### Changed tests

- `RecorderPlayerTests` lost *Per-track punch-arm toggle* and `AudioCallbackTests`' punch test now arms tracks directly, because the per-track punch state they pinned no longer exists.
- *Punch splice*: "punch past the end extends the take" pinned the clamp that was the bug. It is now an append case and a gap case.
- *October regressions*, "Device loss finalizes the take once": finalization is asynchronous now, so the test waits for `onTakeInterrupted`.
- *Main daemon transport regressions* call `stopActiveCaptureAsync`: the synchronous helper no longer has a daemon branch.
- *Capture daemon link*, "Missed samples and copy problems reach the banner", checks `alarmBanner` text and level for each condition alone, and that it clears. It cannot show the launch alert (`s_testConstruct` skips it); it pins the status line and `lastConnectError` instead.
- *Capture copies*, "A stuck drive shows in the readings": the drive must show real throughput before it stalls, or a frozen reading passes.

### Writing tests here

- **Tests that pin a session and then save must claim its mixer** (`claimMixerFor`), or the save is correctly refused. `recorder.startRecording` called directly does not claim; `AudioEngine::startRecording` does.
- **The private test HOME can be on exFAT, which has no permissions, and JUCE's temp folder lives under HOME.** `chmod` silently does nothing there. Any test that needs a read-only folder must use a real Mac volume and assert the folder refuses a write, as *Session safety* does with `/tmp` and the read-only copy test does with `/private/tmp`.
- A test that calls `switchView` must wait for `sessionIoBusy` to clear before sending keys: the view change queues a layout save, and `keyPressed` swallows keys until it lands.
- The final-flush test sets `RLIMIT_FSIZE` and ignores `SIGXFSZ` for the length of one export, and restores both. Nothing else in the process may write a large file meanwhile; suites run one at a time, so nothing does.
- On a heavily loaded machine, EDIT's background scan threads can take seconds to wake, and `~EditPage` waits for them for as long as that takes.
- **Hash timing under load.** A failed `session.report.json includes SHA-256 per part file`, or a 30 s poll timing out in *Punch record* ("session report length + SHA describe the SPLICED file"), under heavy load is a timing failure of an async hash, not a persistence failure. Both passed when re-run alone.

### Not covered headless

- Crop dialog text and its recheck on press, the status lines in `MainComponent`, the analysis cancel from the 10 Hz timer, clip-menu Consolidate, and `startExportTracksTo`'s unlinked-pair and uncreatable-folder paths.
- The modal rename dialogs (only `MarkersManager::indexOfId` is tested) and the marker list's popup.

## Market moves — 2026-11-01

At `d69acee` the `ZynforgeRecordingTests` app passed **938 groups / 0 failures** in Release, under ASan/UBSan (once in the standard setup, once from a fresh home) and under TSan, with no sanitizer reports, and in CI (Debug and Release). That is 689 before this work, plus 249 new groups: *Console VSC* 75, *Twin recorder link* 50, *Capture metadata* 40, *Dead-channel alarm* 30, *Session interchange* 23, *Twin recorder host* 18 and *Song export* 13. Both static gates pass. `tools/test_notarize.py` passes 16 tests. `tools/test_auto_stop.py` passes.

The merged Release app launched with `--isolated-settings`, passed the microphone prompt and quit through the normal path with `--smoke-quit-after`, with no crash report. Nobody looked at its windows. None of the new dialogs, panels or native menu ticks were operated by hand, and nothing here has run on a console or between two Macs: see *Market-moves acceptance* in [FIELD-TEST.md](FIELD-TEST.md).

**A hang found by this suite.** Stereo pairs inside a stream fault spun `channelalarm::Monitor::live` forever: a short-circuited `||` skipped the loop's own increment. In the app that froze the status timer on the message thread, on exactly the fault the alarm exists for. *Dead-channel alarm* now pins it twice, through the Monitor and through the callback. A hang fails no assertion, so run a suite under a time limit when it can loop; this one spun for almost three hours before anyone looked.

**Run the final pass through `open`.** A directly launched test app is never frontmost, so it has no real keyboard focus. A host's Welcome window takes focus in its name field when the app is frontmost, and a focused text field keeps Cmd keys, so one test failed only through `open`. A filtered run or a direct launch cannot show this kind of failure. Run sanitizers with a fresh home as well: CI starts from one, so the first-run tour and Welcome appear there.

**Wait for a result, not a time.** A test that needs a status-loop tick gave it a fixed 400 ms and failed in CI on both jobs while passing here, because the runners are slower than this Mac. It now waits for what the loop latched, and still fails when the loop's path is disabled.

**Capture rules were broken on purpose.** Five were reverted in one build and each failed in its own test: the stamp's compare-and-swap, stamping before hashing, removing spliced files from the stamp list, a stale clock reading, and watching the right side of a pair. The hang above was red before its fix.

### Console and virtual soundcheck

- *Console VSC* (`Source/Tests/ConsoleVscTests.cpp`) drives both scene paths through one assertion set on a real `MainComponent`: Follow on and off, stopped and playing, duplicates, no match, seek lock, `sessionIoBusy`, LOCK, and daemon and local capture. It covers macro parsing, source filtering over a real UDP socket, integer 0, debounce, the two-tap STOP on a real local take and RECORD pre-flight; X32 chlink encode/parse, pair linking, the session-file round trip and an end-to-end create against a simulated desk; and log capture, cap and token redaction over real sockets.
- Result on `feature/console-vsc` at e2ebc6a, Release arm64: 762 groups, 0 failures (baseline 689, plus 73 in *Console VSC*). Each of the 73 was seen failing in one of three deliberate-break builds. ASan, UBSan and TSan were not run on this branch.
- The create-session end-to-end test waits out the real 3 s collection delay.
- Not covered headlessly: the native menu tick, the Save Console Log file chooser (`writeConsoleLog`, after it, is covered), and the look of the Desk Macro panel and the hub's new button. Neither panel has been opened in a running app.
- The two-path echo rule (`scenefollow::RecallEchoFilter`) was red-checked: with the filter disabled, *Console VSC* failed on the extra step (expected 240000, actual 720000).

### Capture metadata and dead-channel alarm

New suites: `Capture metadata` (`Source/Tests/CaptureMetaTests.cpp`) and `Dead-channel alarm` (`Source/Tests/ChannelAlarmTests.cpp`). `Source/Tests/LtcTestSignal.h` now holds the LTC generator, shared with `Timecode chase`.

`Capture metadata` runs `tools/verify_take.sh` against a real stamped take and skips it (logged) when ffprobe, jq or xxd are missing. It also runs the HAL reader against every input device on the machine: it passes with none, and the log shows what each device reports.

### DAW handoff and song export

Suites added: *Session interchange*, *Song export*. 

**REAPER proof (not part of the suite, needs REAPER installed):**

```bash
ZF_EXPORT_FIXTURE_DIR=/tmp/fx "build/.../Zynforge Recording Tests" --test-filter="Session interchange"
tools/verify_reaper_export.py /tmp/fx --work /tmp/fx-work --schemas <folder with Project.xsd, MetaData.xsd>
```

It renders the fixture session with `REAPER -renderproject`, compares the result with the engine's own mix sample for sample, reads every track, item, source and marker back through REAPER's scripting API, and validates the `.dawproject` with `xmllint`. REAPER runs in the background from a private resource folder and quits itself.

Run it after any change to `ReaperProject.h`, `DawProject.h` or `SessionInterchange.h`.

Result on REAPER 7.48/OSX64 (x86_64, under Rosetta), 2026-10-31, fixture of 13 strips over 10 s (mono edits, stereo pairs, a take in two parts, FLAC, 24-bit and float AIFF, float WAV, unlinked stereo, a cross-track clip, a muted track, an enclosed overlap, a narrow pair, clip gain, fades of both shapes, pans, master fader):
- Script-driven render against `renderStereoMix`: 480000 frames both ways. Identical to five decimals in every second except the two holding the equal-power fades, where the peak difference is 0.00039 (69 dB below the mix). That is REAPER's shape 7 against the session's sine.
- Read back: 11 tracks, 15 items, 2 markers, all sources online; names with quotes, ampersand, non-ASCII and edge spaces intact; colours, gains, pans, fades, shapes, CHANMODE, sample rate, tempo, master fader and notes as written. REAPER's own save keeps ITEMMIX 1 and TIMELOCKMODE 0.
- `project.xml` and `metadata.xml` validate against the format's XSDs with `xmllint`.

`-renderproject` itself works on tiny projects but races REAPER's start-up audio-device prompt on anything larger, so the tool renders from a script (action 42230). The private configuration names an audio device that does not exist and no plug-in folders.

Not covered: a real show's size, REAPER on the owner's own configuration, and any DAW that reads DAWproject.

### Twin Recorder

- **Twin Recorder**: `--test-filter="Twin recorder"` runs *Twin recorder link* (two real `TwinLink` endpoints over loopback, a fake host, hand-driven sockets for hostile peers, and a relay that stops forwarding to act as a pulled cable) and *Twin recorder host* (the link wired into the real `MainComponent`, with a hand-driven follower). About 26 s per run on this machine. A 12-run loop found one real flake: a 5 s wait on the report hash pass, which runs at background priority and starved while other branches were building. That wait is now 30 s, and it returns as soon as the pass runs. After that change the suite passed 20 of 20 runs, then 12 of 12 on the branch's final commit, on a machine also building other branches. The full suite is 757 groups, 0 failures, in Release; ASan, UBSan and TSan were not run on this branch.
- Red evidence: deliberate regressions were applied to the committed code and each went red in the named test, including link loss stopping a take, a wrong code accepted, a stale `stop` crossing onto a new connection, any take end stopping the follower, a follower commanding a leader, a report written during a take, and the two report-carry paths. Removing the pre-auth depth guard crashes the process on 12,000 `[`, which is the crash the guard exists to prevent.
- The link's timing is configurable (`Config::linkTimeoutMs` and friends) so the tests run at a tenth of the real periods. The 3 s link timeout itself is exercised only in the field.
- What these tests cannot show: real network behaviour, macOS permission prompts, or two real audio devices. The host suite has no audio device, so `prepareRecordStart`'s device checks are not reached with a passing device; the record stage after them (`runRecordPlan`) is driven directly.

### Mac distribution

`tools/test_notarize.py` drives `tools/notarize.sh` against stand-in tools: signing order, entitlements, refusing a rejected submission and refusing to overwrite a published DMG. A real notarization has not run, because it needs the owner's Developer ID.

## October 29 structural-debt pass (source after `69aab9f`)

At `abb83bf` the `ZynforgeRecordingTests` app passed **689 groups / 0 failures** in Release, under ASan/UBSan and TSan (no reports) and in CI, after the split-fade merge (11 new groups in *Editing fixes* and *October audit regressions*, red against the old clip model by up to 0.499). Before that it passed **678 groups** (675 before the deferred lead-ins; the three lead-in groups failed against the previous recorder: silence written at start, no recovery, no split), under **ASan/UBSan/float-cast-overflow** and under **ThreadSanitizer** (run on `12188dd`, arm64 Debug, no sanitizer reports); CI passed on `967f721`. That covers the lead's punch/session fixes and the merged network, editing, EDIT-view and engine branches. Two interim Release counts recorded during the merges (623, 666) were short. After each merge that added a test file to CMakeLists, the first incremental Xcode build did not compile that file, so those runs lacked the *Editing fixes* (12) and *Engine review repairs* (9) groups. Both suites ran and passed in later runs. After a CMakeLists change, check that the expected suite names appear in the log, not just the total. The engine branch adds *Engine review repairs* (`EngineReviewRepairTests.cpp`) and 14 LTC groups in *Timecode chase* (synthesized SMPTE 12M at 24/25/29.97 DF/30 fps; tones, program audio and random biphase bits must never LOCK); its locate-race test lost 228/300 locates on the old player. The EDIT-view branch adds the *EDIT view audit 2026-10-29* suite (in `EditTrackRowTests.cpp`; red against the unfixed code for every finding, including a counting reader that saw 200,000 thumbnail reads for a 400 px window at deep zoom). The bug-hunt fixes added *Punch and session guards* (19 groups: stale punch/continue arms, the punch playhead/loop lock, the capture-origin splice, per-copy splice rollback and archiving, empty punches, refusal messages, New Session under a take, undo on New Session, main-window guards, AppleDouble project lookup). Red check: a build with every lead fix's behaviour reverted (arm64, `red_mutations` patch) failed each of those tests, and the stale-export test crashed on the out-of-range index, which is the reported bug. That crash stopped the run, so a second build reverted only the analysis, linked-patch and click-refresh guards: all three of those checks failed. The AppleDouble lookup test passed on the old code too: macOS's directory enumeration already hides `._` files, so that finding did not reproduce and the filter is defence in depth. The network branch adds 10 groups to *OSC remote*, *Companion server* and *Network audit fixes*, and the editing branch adds *Editing fixes 2026-10-29* plus groups in *Channel CSV*, *Timeline export* and *Transients*. Each was shown red by its author against the unfixed code; the nested-OSC and nested-JSON tests crashed the test process (SIGBUS) before the fix. Before them, 587 groups passed under **ASan/UBSan/float-cast-overflow** and under **ThreadSanitizer** (run on `2615b65`: mix kernel, recorder stages and `RemoteControl` included) (arm64 Debug builds; no sanitizer report files; runtimes confirmed linked with `otool -L`). Both static gates are clean (the invariants gate has 31 checks). *Mix kernel* (5 groups) tests each shared mix rule on its own; *Audio callback* gained "Live playback and the offline bounce mix the same session identically", which plays a session through the callback and compares it with `renderStereoMix` sample for sample. New suites: *Strip reorder* (6 groups, drives the EDIT row's real mouse handlers), *Session jobs* (6 groups, including a strip removal in the middle of a stereo-mix render) and *Strip lifetime and mixer fingerprint* (3 groups; ASan reports a heap-use-after-free in it when the keep-alives are removed). Each new test and gate rule was shown to fail against an injected regression before landing. Nothing from this pass is installed on either Mac.

Sanitizer recipe used for this pass (separate build trees, never combined; reuse the Release tree's JUCE checkout):

```bash
cmake -S . -B build-asan -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/build/_deps/juce-src" \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined,float-cast-overflow'
cmake --build build-asan --config Debug
open -n -W --env ASAN_OPTIONS=halt_on_error=1:log_path=/tmp/zf-asan \
  'build-asan/ZynforgeRecordingTests_artefacts/Debug/Zynforge Recording Tests.app' \
  --args --test-report=/tmp/zf-asan-tests.log
# TSan: same with build-tsan, -fsanitize=thread, TSAN_OPTIONS=halt_on_error=1:log_path=/tmp/zf-tsan
```

Run one suite with `--test-filter="<suite name>"` on the test-runner app. After adding a test file to `CMakeLists.txt`, re-run `cmake -B build -G Xcode` explicitly before building -- the Xcode build's automatic regeneration missed one such edit.

## October 5 follow-up verification

All 21 follow-up findings are repaired. Full Release, ThreadSanitizer and
ASan/UBSan/float-cast-overflow each passed **563 groups / 0 failures**, with no
sanitizer reports emitted. The 44 new C++ groups include capture gap ordering,
startup ordering, persistence, network framing, UI settings cost, true float
AIFF and final-header failure propagation. Six mocked auto-stop tests and both
static gates pass. Every repaired path has recorded pre-fix failing evidence.

See [the follow-up repair register](docs/history/REVIEW-REPAIRS-2026-10-05.md) for the
21-item test mapping, commands, evidence location, startup observation and
runtime limits. That `443c769` pair has since been superseded on both Macs by
`281c6c0` (2026-10-29); see [INSTALL.md](INSTALL.md) for hashes and rollback. Historical
S6 results below remain evidence for the previous build. The isolated launch
reached main-component rendering with unchanged production settings. The later
installed launch with normal preferences also reached rendering, produced no new
crash report and was left open. UI interaction and normal quit were not verified. Physical recording and long-take STOP acceptance remain pending.

## Earlier October 5 S6 repair evidence

The clean audit baseline built successfully; its original 440 groups had one capture-deletion failure. This paragraph and the S6 results below describe that earlier audit. [The audit report](docs/history/AUDIT-2026-10-05.md) retains its red/green and sanitizer evidence; the follow-up section above is current.

The frozen S6 source passed **519 groups / 0 failures** in each full optimized
Release, TSan and ASan/UBSan/float-cast-overflow run; no sanitizer reports were
emitted. The universal GUI/helper and separate sanitizer builds, signatures and
gates pass. S6 no-device TERM/INT repeats and the final bounded ENOSPC export
repeat (5/0) passed. At S6, normal startup smoke waited on macOS microphone
consent; the pair was installed, verified and left closed. Later `443c769`
startup reached rendering, as recorded in INSTALL.md; physical-rig acceptance
remains pending.

The audit ledger retains every prior checkpoint, including S5 Release's deadline
failure and its measured 712.261 ms blocking native send. S6 explicitly sets
O_NONBLOCK and passes the unchanged optimized deadline oracle as well as both
sanitizer suites. Instrumented formatting can expire before entering that kernel
path, so optimized Release coverage remains necessary. Preference reset/range/
growth reloads are 5/5/10 after the earlier four-group/38-failure red. The
17-group/six-failure frame-boundary red also has green S6 regression coverage.

Apple leaks reported the same 288 allocations / 18,816 bytes in three framework
connection cycles for repaired Main (28 groups) and baseline full (440 groups).
The telemetry workload reported 281 allocations / 18,448 bytes in three Apple
cycles. These nonzero residuals and differing scopes remain explicit; no
exclusions or zero-leak claim apply. The final S6 Main repeat reproduced the
same 28/0 assertions and 288-allocation / 18,816-byte / three-cycle result.

`Main daemon transport regressions` uses a no-device daemon plus disposable session folders. Its metadata gate blocks the real snapshot I/O path while a native macOS message-loop sentinel checks responsiveness; a bounded watchdog prevents a broken implementation from hanging the runner. Coverage includes a synthetic 27-track STOP, duplicate STOP during pending finalization, latched remote failure, cue write failure, immutable revisions, non-undo dirty state, navigation during persistence, 128 coalesced layout requests, forbidden session replacement and host teardown. These controlled fixtures do not establish real-device capture timing or external-volume latency. `Session backup` covers newest contents, legacy collision names, clock rollback, linked external sentinels, journals and invalid retention.

Test mode uses the distinct application identity `Zynforge Recording Tests`, preserving single-instance exclusion between test processes without forwarding arguments to an already-running production app. Run only one suite/process at a time. `--test-filter` selects registered suite names case-insensitively; zero matches exit nonzero. Omit the filter for the full suite. `--test-report` must be an absolute log path. Current build and sanitizer LaunchServices commands live in the [follow-up register](docs/history/REVIEW-REPAIRS-2026-10-05.md#reproduction-commands). The earlier audit retains commands for its isolated worktrees.
Inspect each report's total failures and sanitizer logs; LaunchServices' own exit code alone is not the test outcome. For an instrumented bundle launched with `open`, pass sanitizer configuration using `open --env`, for example `--env TSAN_OPTIONS=halt_on_error=1:log_path=/private/tmp/zynforge-tsan`; a shell-only environment assignment may not reach the launched app. Build TSan separately from ASan/UBSan. See the audit report for exact compiler flags and actual run logs. Never run failure-injection fixtures against the user's session or current capture app.

## Philosophy and Goals

**Historical validation — 2026-10-04:** Source build `c563b00` passed the universal Release GUI/helper build, **438 test groups with zero failures on Apple Silicon**, both static gates, strict/deep bundle-signature checks and a 30-second isolated startup/quit smoke. Both production settings files retained their hashes. Intel slices built successfully; this run does not establish native Intel execution results.

At the October 4 checkpoint, the same app and matching protocol-v3 helper were installed on both Macs; `443c769` now supersedes the development-Mac installation. Installed executable hashes and signatures match; both apps launched without a new crash report. The mini's two exits were manual user quits. The current DMG passed checksum/image verification on both Macs and a read-only bundle comparison locally. Old app backups and installers were removed at the user's request. [INSTALL.md](INSTALL.md) owns the current package, hashes and installation evidence.

The October regression groups, together with daemon and existing integration suites, cover the 28 initial October findings and 14 follow-up findings. See [the audit fix record](docs/history/AUDIT_FIXES_2026-10-03.md). Disposable physical device removal, console reconnect, storage failure, detailed waveform/FOLLOW interaction and the planned 56-input rehearsal remain open. The older five-hour primary-drive take is historical evidence for `d2c5858`, not hardware acceptance of `c563b00`; see [field evidence](docs/history/FIELD-TEST-2026-10-01.md).

**2026-09-30 capture-integrity validation:** A repeated daemon START is refused without resetting a continued take's base or clearing an existing recovery warning. A continued session report lists base and later parts, accumulates frame/time counters and retains earlier capture-failure flags. Handoff flags a failed punch or hash pass for review. Disposable verifier fixtures pass complete WAV/AIFF/FLAC and continued takes, while failed capture flags, a short file, a missing middle part and a rolled-back punch fail. These are source and fixture tests; slow-client behavior, real disk failure and the actual device path still need field checks. The [fix record](docs/history/AUDIT_FIXES_2026-09-30.md) maps each failure to its change.

The harness at `443c769` covered **563 test groups** (575 in the source of that time), including capture provenance/alignment, reference media, stereo topology, device-loss finalization, session state, console restoration, routing, grouped editing, long-take waveforms and settings isolation. Historical paragraphs below retain the counts that were accurate for their dated audit passes.

### Earlier validation snapshots

**2026-09-29 follow-up validation:** disposable verifier fixtures cover valid WAV, AIFF and FLAC takes plus an unlisted WAV that must fail. The daemon suite now simulates audio-device loss during a rolling take and checks that the file closes, a latched alarm reaches the client, and the take report records the interruption. On the target Macs, verify the full capture-warning text and METERS/BACKUP controls at the normal and minimum supported window sizes; exercise a stereo pair whose interleaved left file coexists with an older right file; interrupt a disposable daemon take by disconnecting its device and inspect the saved audio and report. A physical long-take waveform scroll remains necessary.

**Prior source/package (2026-09-24):** the universal Release build with the long-take EDIT and codebase-audit fixes and its exact installed copy each passed **397 test groups / 0 failures**. That installed app had the matching helper and was later preserved for rollback. [GitHub CI run 36017363562](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/36017363562) applies only to older commit `b2c1991`. Native UI geometry warnings, physical devices, actual power loss and the planned three-hour/56-input rehearsal remained unverified. See [SHOW-READINESS.md](SHOW-READINESS.md).

**September regression coverage:** `SeptemberRegressionTests.cpp` covers backup collision, frozen capture arms, empty arrangements, extended cross-track playback, missing sources, locks/takes, session UUID round-trip, reorder/delete persistence, mixed stereo topology, rollback/interrupted transactions, seamless loops, canonical project lookup and concurrent/failing atomic writes. `AuditFixTests.cpp` additionally covers cross-container fresh-take protection, explicit continue, unavailable/persisted backups, daemon-record playback refusal, and consolidation beyond `_999`. Capture protocol/link tests cover completion state and command rejection before Hello; audio-callback tests cover StereoMix without physical stream outputs. Dedicated import suites cover cancellation, collision and transactional installation. Packaging is gated by strict code-sign verification. The September 20 set of 16 findings is mapped in [the 2026-09-20 fixes](docs/history/AUDIT_FIXES_2026-09-20.md); earlier passes remain in [the 2026-09-15 report](docs/history/AUDIT_REPORT_2026-09-15.md) and [the 2026-09-12 report](docs/history/AUDIT_FIXES_2026-09-12.md).

The punch/recovery/import regressions now cover an interrupted `.punchbase` restore and partial-take archive, an occupied sidecar refusing capture, a newly enabled backup without the base refusing punch, pre-roll not shifting the splice, newly armed punch/continue tracks aligning to the session, a sub-block selection punch that records exactly its window and monitors only the live take, and a four-channel import retaining all four inputs. Loop suspension, post-roll continuity and physical output switching still need disposable real-device interaction checks; headless tests alone cannot certify a live show.

The 2026-09-24 final source smoke launch started the built bundle and remained at approximately 95 MB RSS / 0.3% CPU with no new `Zynforge*.ips` crash report. The UI automation bridge and AppleScript quit both timed out, so this was **not** a visual or interactive acceptance check; the exact smoke-process PID was terminated after confirming it was the newly launched idle build. The installed app was not replaced.

The 2026-09-24 DMG packages that same `b2c1991` source build, not a separately tested installation. `hdiutil verify` passed, a read-only mount contained the expected Applications shortcut plus the matching GUI/helper bundle, `diff -qr` matched the Release app, and both Mach-O executables were `arm64` + `x86_64`. No installed-bundle or second-Mac smoke test has been performed for this DMG. The older `0.2.0` DMG was untouched.

The manual-punch regression covers a rolling playhead overriding a stale edit cursor, one-press punch-out with session metadata saved, format-changed punch into the original file, and refusal of a missing fresh insert. A disposable real-device take must still verify the whole RECORD/play/punch-out interaction and backup/mirror consistency before show use.

The final 2026-09-24 punch build opened Welcome and reached the idle transport with no session loaded; no new Zynforge crash report appeared. Its audio-device initialization on this development Mac was slow, so this idle launch is not a capture-performance result.

The live EDIT-navigation regression checks that manual horizontal pan/time zoom pauses playhead follow, automatic page-scroll and vertical-only scroll do not, FOLLOW resumes, a new transport pass resets follow, and H+/H- return exactly to fit-to-take. Native smoke and a disposable recording must still verify actual wheel/trackpad, minimap, H/V controls and UI performance while rolling; headless state tests alone do not certify that interaction or capture integrity.

The long-take regression checks that three hours of peak bins retain actual sample time within bounded memory, a long continuation prefill stays bounded, a three-hour live timeline has scrollable width, and recording read-only mode leaves zoom buttons enabled. Additional tests assert that isolated transients remain as thin maxima beside a smoother overview body, sustained levels remain visible, an audio file newer than `WaveCache.wfm` invalidates the cache, H+ can pass the old 16× cap, and deep zoom draws only viewport grid ticks. The completed 2026-10-02 hardware take confirms saved primary-audio duration and a clean report; an earlier screenshot exposed blocky post-STOP display. Screenshots of `98a645e` show the thin-body overview and narrow peaks after reopening, but the user confirmed H+ could not pass a 16-minute span. The zoom fix is included in `443c769` on the development Mac and `c563b00` on the mini; a few-minute detailed-zoom screenshot is still needed; live motion/navigation at timed long-run checkpoints still require direct observation.

The 2026-09-24 audit follow-up adds file-backed regressions for a pure end trim, a renamed/locked clip, playback of newly appended audio after a split, refusal of an orphan continuation part, fresh/stale daemon marker positioning and LOCK refusing remote playback. Disposable `verify_take.sh` fixtures exercise pending hashes, complete hashes, missing hashes, valid continuation parts and an orphan part. Native interaction still needs a disposable recording and export test.

Native empty-session smoke on 2026-09-24 confirmed the rebuilt app opens EDIT, shows full H/V labels, and H+ followed by H- removes the minimap at 1x. This did not exercise a rolling waveform or physical capture.

ZynForge Recording is a live-stage tool. The cost of a regression discovered in front of an audience is qualitatively higher than the cost of a regression in a typical desktop app. The testing strategy reflects that: **catch crashes and data loss before the audience does, even at the cost of some manual effort.**

The strategy is **build + unit + smoke + field**. The unit-test harness is in place (see the current validation count above and *How to Run Tests*) and covers the audio callback, recorder, player, markers, clip-edit persistence, accessibility, the measured pre-flight probes, post-show QC, song detection, crash-report scanning, the console link (+ profile capabilities), stereo-pair export, the EDIT ruler↔lane scroll alignment, the live-wave continue path, the session-recovery sort comparator, and the `EngineStatus` boundary — all headlessly. The 2026-07-10 deep audit + its re-audits added five regression tests in `RecordingIntegrityTests`: *swapTracks preserves clip edits* (a reorder must move splits with the audio, never wipe them), *splitClipAt fade geometry* (hard cut at the seam, inherited fade clamped to the new clip length), *Continue-record grows the take* (a continue must refresh the default clip to the grown length, never leave the appended audio silent), *swapTracks renames continuation parts* (a reorder must move a take's `_partXX` parts + FLAC/AIFF files, not just `Track_NN.wav`, or they end up on the wrong channel), and *marker during a continue lands on the timeline* (a marker dropped mid-continue must land at `recordBaseSamples + offset`, not the new part's 0-based length). The 2026-08-11 whole-codebase bug hunt added **`Source/Tests/AuditFixTests.cpp`** (9 groups), one per headlessly-reachable fix: the stereo-mix bounce staying in bounds when the player out-counts the mixer, `seedDefaultClips` preserving clips above the last recorded take, mirrors counting toward the disk-rate estimate, a continue adopting the existing take's container, `takeIsMultiPart` ignoring `.punchbase` sidecars, the FLAC 32-bit clamp, same-rate export length, and `removeStripAt` shifting stereo flags + strip UUIDs. The remaining fixes from that pass are message-thread behaviour (threaded export / Save-As, the async SMART poll, the pre-flight probe guard, punch arm restore) and are smoke-test territory. The 2026-08-12 EDIT-view audit added **`EditViewFixTests.cpp`** (3 groups) against the shared logic its fixes were extracted onto — `EditTimeline.h`'s notional-span and peer-clip-mapping helpers. `EditPage::TrackRow` is now a **public** nested declaration precisely so it can be constructed directly: **`EditTrackRowTests.cpp`** covers the meter-condemn contract (a condemned row must stop dereferencing its `TrackState`), the stale-index guard, and odd/even/unrouted stereo routing display. Reverting either fix makes the suite fail — the stale-index one by *crashing the test binary* in `TrackRow::updatePollState`, which is the production UAF reproducing under test. The remaining EDIT fixes (wave-cache ordering, the mouse-handler drag paths) are still smoke-test territory. **CI** (`.github/workflows/ci.yml`, macos-15) builds and runs the full suite on every push and PR to `main`. Smoke-test and field rehearsal still backstop it for anything the headless harness can't see (paint, real CoreAudio, live VoiceOver, **macOS menu enablement**, the session reopen-on-launch flow).

**2026-08-18 remediation coverage.** The suite proves exact mixer shrink/reset, complete state clearing, corrupt-mix fallback, primary-writer fail-closed behaviour with punch-stash restoration, multipart numeric ordering and missing-trailing-part refusal, player reader lifetime/prebuffering, persisted MIDI running status, Generic OSC authentication and wrap-safe remote time conversion. Remote RECORD now uses the full host preflight (2026-09-23). Save As/relocation cancellation, native `.zfproj` launch, save-failure modal flow, and the installed bundle remain smoke-test responsibilities because they require macOS UI/filesystem integration.

## Testing Pyramid / Approach

```
        ╱╲          field rehearsal     — every change touching Source/Audio/
       ╱  ╲         smoke-test         — every change, automated by Claude
      ╱────╲        unit tests         — `ZynforgeRecordingTests`, headless, every change
     ╱──────╲       build              — every change, blocking
```

- **Build** — `cmake --build build --config Release`. Must succeed. No warnings-as-errors yet, but new warnings should be addressed.
- **Unit tests** — `juce::UnitTest` groups in `Source/Tests/`, run by the `ZynforgeRecordingTests` app (`--test-filter=<substring>`, `--test-report=<absolute path>`). No physical CoreAudio device; UI regressions use the real message thread and bounded native-loop pumping. Add a test with every bug fix and every new audio-thread / persistence path.
- **Smoke-test** — Launch the built app, exercise the changed surface, verify RSS / CPU / no new crash report. Documented in `CLAUDE.md` under *Build + smoke test recipe*.
- **Field rehearsal** — User-driven. Anything touching the audio callback, the recorder, or the player must be exercised in a real (or simulated) session before being declared shippable. The user is the only authority for this stage.

**Menu / session-load smoke checks (supplement native-loop regression fixtures).** macOS caches the menu's greyed states until `menuItemsChanged()` fires, so after any change near menu enablement, manually verify: launch reopens the last session with its channels; with a session loaded the Edit/Track/Export items light up; after a clip edit, Undo lights up; selecting a strip lights up Track ▸ Cut/Copy/Solo. Export reads from `Audio Files/` — verify Export Audio actually writes a file.

**Session-transition smoke checks.** With unsaved mixer/clip/cue changes, exercise New, Open, CSV, Console, New from Template, Finder `.zfproj`, Close and Quit. Verify all replacement paths offer Save / Don't Save / Cancel; failed saves leave the session open. Save As must reject a non-empty target, leave the source active during copy, and open only the complete clone. Cancel a cross-volume Save As/relocation and verify there is no partial destination. While a copy/move/bounce is active, Record/Play/strip mutation and a second session job must refuse without changing state.


**Audio Device panel single-instance smoke check (message thread + modal, not unit-tested).** With no audio device present, both the toolbar DEVICE button and the PATCH tab's "Audio settings…" placeholder open the device panel. Open one, then trigger the other — it must surface the existing panel, not stack a second (two live panels each snapshot the device state, so cancelling both would double-restore and fight over the channel counts). Cancelling the single panel must still restore the boot channel counts (meters/recording keep working).

## Frameworks and Tools in Use

- **CMake / Xcode** — build orchestration.
- **JUCE's built-in `juce::UnitTest`** — the test groups in `Source/Tests/` register themselves as static instances in the separate **`ZynforgeRecordingTests`** app (`Source/Tests/TestMain.cpp`), which links the same `ZynforgeCore` static library as the recorder. The recorder itself carries no test code (CI checks its symbols). `AudioEngine::setTestModeSkipAudioInit(true)` + `prepareForTests(sr, blockSize)` let tests construct an engine and drive `audioDeviceIOCallbackWithContext` without a real audio device.
- **macOS unified log** + `~/Library/Logs/DiagnosticReports/` — primary post-launch signal source. `log show --process "Zynforge Recording" --predicate 'messageType == error or messageType == fault' --last 1m` surfaces runtime errors and faults.
- **`ps` / Activity Monitor** — RSS and CPU snapshots during smoke-test.

## When to Write Tests

The harness exists and every change should keep it green (and grow it where the change adds a testable path). Write a test:

- Whenever fixing a bug. The test should fail on the pre-fix code and pass on the post-fix code.
- For every pure function in `Source/Audio/` that does non-trivial maths (clip rendering, fade interpolation, sample-rate conversion, timecode decoding).
- For every persistence path (`.zfproj` round-trip, `appProps` reload).
- For every soft-takeover ramp calculation — these are easy to regress and silent when they break.

Do not write a test for paint code. Visual regression is caught by smoke-test + screenshot review.

## How to Run Tests

### Build
```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

On Xcode 27.2 the Xcode configure can hang; export `XCODE_XCCONFIG_FILE` pointing at a file with `SDK_STAT_CACHE_ENABLE = NO`, or build with Unix Makefiles in `build-make/` (see *2026-11-11 fix round* above).

### Smoke-test

Stop takes, save, and quit the GUI and helper gracefully before launching a second instance. Use temporary preferences for a development smoke:

```bash
open -n "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" --args --isolated-settings
# Unattended: also quit through the normal shutdown path after N seconds
# (skips the quit prompt; honoured only with --isolated-settings).
open -n -W "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" \
     --args --isolated-settings --smoke-quit-after=35
```

The app is single-instance: a smoke launch while another copy is open is forwarded to that copy and exits. Each rebuild is ad-hoc signed, since 2026-11-04 with the hardened runtime and audio-input entitlement a release has (the post-build seal in `CMakeLists.txt`). A build that changed still gets a new ad-hoc identity, so macOS may ask again for microphone consent: until that prompt is answered, the main thread sits inside `AudioEngine()` -> `AudioDeviceManager::initialiseDefault` -> CoreAudio `AudioDeviceStart` and no window appears (observed 2026-10-29 with the built-in mic/speakers as defaults; `sample <pid>` shows it). `--smoke-quit-after` fires only once startup completes. Do not force-kill a stuck smoke instance; answer the prompt. The quit prompt is drawn by JUCE, so scripted keystrokes only reach it when the controlling process has macOS Accessibility permission; `--smoke-quit-after` avoids needing it.

Wait through startup, identify the exact new process and its executable path, then sample its PID with `ps -p <pid> -o pid,rss,etime,%cpu` at 10, 20 and 30 seconds. Check the visible window, runtime errors and newly created `~/Library/Logs/DiagnosticReports/Zynforge*.ips`, then quit through the app. Record the actual CPU/RSS and UI state rather than treating old measurements as fixed pass thresholds. The October 4 isolated smoke settled near 120 MiB RSS / 3% CPU at 30 seconds on this Mac. An installed-app launch with saved preferences is a separate check; see [INSTALL.md](INSTALL.md).

### Unit tests

The audit-follow-up regressions cover stale daemon status, remote STOP confirmation/current-session RECORD preflight, snapshot-journal retention, hostile multipart filenames/read failures, skipped-mirror report/handoff evidence and legacy root-level handoff. A live rig is still required to validate daemon disconnect and remote-control ergonomics during a real take.
The 2026-09-23 idle smoke launch showed a normal window and no new `.ips` crash report (about 118 MB RSS / 5.9% CPU on this development machine), but AppKit logged transient negative-view-geometry runtime faults. Treat that as an open native-UI follow-up, not a fully clean runtime log.

```bash
# The test runner is its own app with a separate identity and isolated
# preferences. Run one suite at a time, away from live capture; never kill a
# user app/daemon to run tests.
APP="build/ZynforgeRecordingTests_artefacts/Release/Zynforge Recording Tests.app"
open -W -n "$APP" --args --test-report=/private/tmp/zynforge-tests.log
# Launch the bundle through LaunchServices. On newer macOS, invoking the raw
# GUI executable can abort in NSApplication registration before tests start.
# Inspect the freshly written explicit report; open's exit code is insufficient.
tail -1 /private/tmp/zynforge-tests.log   # "[zynforge tests] N test groups, 0 failure(s)"
```

### Static gates (pre-commit, and the cheapest tier of all)

```bash
tools/design_audit.sh       # 7 brand rules   -- raw colours/fonts/alphas/radii/tints, spacing ratchet
tools/invariants_audit.sh   # 40 correctness rules -- recurring bug classes
tools/install_hooks.sh      # once per clone: wires both as a pre-commit hook (hooks aren't in git)
```

These catch a class of defect no unit test will: a hazard correctly handled at nine call sites and missed at the tenth. They run in under a second, so run them before the suite, not after.

**Adding a rule: watch it go RED before you land it.** A rule that passes on a broken tree is worse than no rule, because it reads like coverage. Two of the original seven were blind — one matched a *commented-out* call, one was satisfied by an unrelated comment in the same file — which is why the rules now strip comments (`sed 's,//.*,,'`) before grepping. The procedure is: write the rule, inject the regression it's supposed to catch, confirm the gate fails, restore, confirm it passes. Rule 10 found three real unguarded sites on the day it landed that a careful read of the same files had missed.

**A failing run can leave a test daemon behind.** Capture-daemon suites launch a binary and bind a port. After an abort, identify the exact test process, command line and port before cleanup; do not terminate a production daemon or use process-name-wide kills. A port conflict and an intermittent race require different diagnoses. Preserve the original failure report and rerun only after confirming isolation.

**`juce::StreamingSocket::waitUntilReady` takes the socket's readLock even for a WRITE check** (`juce_Socket.cpp`). Polling writability on a socket whose reader thread is parked in `waitUntilReady(true, 200)` starves the writer on that lock and every send fails — it broke all 32 capture-daemon assertions when tried as a fix for a blocking-write wedge. Bound a wedged write with a timed write MUTEX plus a `close()` from another thread instead.

**A NEW test file needs a cmake RECONFIGURE, not just a build.** Adding a `.cpp` to `CMakeLists.txt` and running `cmake --build build --config Release` reported **BUILD SUCCEEDED** while silently not compiling it: the project was regenerated but the build used the stale target, so the suite ran with the old test list. The pass count is the tell — it didn't move. Run `cmake -B build -G Xcode` first, then build, then confirm your suite is actually in the binary:

```bash
strings "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app/Contents/MacOS/Zynforge Recording" \
    | grep -c "<your beginTest name>"      # 0 means it never got linked
```

This is the same failure mode as the stale test-report mtime below, one layer down: a green run that didn't run what you think it ran.

**Test isolation — BETWEEN tests, not just from your settings.** Within one process, all five settings writers share one uniquely named temporary `zynforge-tests-<UUID>.settings`, so any suite that writes per-strip state (`setTrackStereo` → `strip_stereo_N`, routing → `strip_in_N`, gains, colours) **leaks into every later suite** that constructs an engine and calls `applyPersistedStripState()`. `EditTrackRowTests` broke *Player maps files by Track_NN index* exactly this way. A suite that mutates per-strip state must call `engine.clearAllStripOverrides()` on setup AND teardown — see that file's `Host` struct.

**Settings isolation:** `Source/Audio/SettingsFile.h` provides `makeSettingsFile()` for `AudioEngine` and all four `Strip*` settings writers. The test runner and `--isolated-settings` enable process-wide isolation before construction; once enabled, later audio-initialization changes cannot restore production preference writes. Each process gets a unique temporary settings file. Tests that construct an engine should set `AudioEngine::setTestModeSkipAudioInit(true)` first; suites that need callback initialization must still retain settings isolation. Shared state within a process still requires setup/teardown cleanup.

Notable suites: `MenuDispatchTests` (id-collision + dispatch-range guard), `FastHashTests`/`SessionBackupTests` (capture-side helpers), `AudioCallbackTests` (audio-thread integration, including RF64 policy, 64-channel throughput, StereoMix capture without physical stream outputs, and windowed offline-render equivalence), `AuditFixTests` (record collision, redundancy, external-record transport, and filename boundaries), `CaptureLinkTests`/`CaptureDaemonTests` (protocol handshake, completion and daemon failure paths), `PreflightTests` (measured disk-speed/writability/headroom), `QcAnalyzerTests` (peak/clip/floor against synthesized WAVs), `SongDetectorTests` (multi-track quorum, incl. an always-hot ambient mic detecting nothing alone), `CrashScanTests` (.ips filter + summary), and `ConsoleLinkTests` (full X32 query→stash→flip→restore state machine through a transport seam, plus a real connect→disconnect→reconnect socket-rebind regression).

`ShowHandoffTests` checks the portable sidecars, exact source/copy hashes, changed-copy rejection, and pending-capture-report warning. Manual checks remain necessary: run a real multi-hour handoff on a separate volume and inspect its files/report in another DAW. Do not run the handoff during a live take.

## Code Coverage Expectations

No formal coverage target today. The bar for new code:

- `Source/Audio/` — 70% line coverage on pure-function paths (excluding RT callbacks, which require a host harness).
- `Source/Theme/` — coverage not meaningful (mostly tokens and inline helpers).
- `Source/UI/` — coverage not pursued; smoke-test covers it.
- `Source/Network/` — 50% line coverage; HTTP endpoints and OSC parsers are good candidates.

## Guidelines for Writing Good Tests

- **One test, one behaviour.** A test that asserts five things is five tests in a trench coat.
- **Test names describe the behaviour:** `playerLoadsClipAtCorrectSamplePosition`, not `testPlayer1`.
- **Set up the minimum state required.** Construct objects directly when possible; don't load a `.zfproj`.
- **Compare floats** with `juce::approximatelyEqual` or `std::abs(a - b) < eps`. Never `==`.
- **Tests must not write** to `~/Music/Zynforge Sessions/`. Use `juce::File::createTempFile` inside a scoped `TempDir`.
- **Automated tests must not require operator input or production hardware.** Component and message-thread tests are supported by the existing UI fixtures; keep event pumping bounded and use disposable media. Native visual interaction and physical-device behavior still belong in smoke/field checks.

## Common Pitfalls to Avoid

- **Don't smoke-test in Debug.** Release-only optimisations sometimes mask UB; Debug-only assertions sometimes mask logic bugs. Always Release.
- **Don't compare crash-report counts naively.** The pre-existing 13:04 crash from the original session is in the report directory. Compare timestamps, not counts.
- **Don't trust LSP `juce undeclared identifier` errors.** They are stale because the LSP's include path doesn't see the JUCE module headers. The build is authoritative.
- **Don't assume Apple Silicon and Intel behave identically.** Universal builds means both must work. If a NEON path is added, verify the SSE / scalar fallback at least builds.
- **Don't rely on the field rehearsal as a unit-test substitute.** A rehearsal catches obvious regressions; it doesn't exhaustively explore edge cases.
- **Don't smoke-test for two seconds.** Watch the process for at least 10 seconds — slow leaks and timer-driven crashes don't show up immediately.
- **Launch the smoke-test with `open "...app"`, not the raw binary, if you'll need to quit it (learned 2026-06-09).** A binary launched directly (`.../MacOS/Zynforge Recording &`) is NOT LaunchServices-registered, so System Events can't see it (`get name of every process` omits it → "Invalid index") and you cannot answer its save-on-quit modal or `osascript ... to quit` by name. `open` registers it; only then is graceful quit (`osascript -e 'tell application "Zynforge Recording" to quit'` + Return to confirm Save) possible. Force-killing is the device-wedge hazard — see the *quit gracefully* memory.
- **An app exit is not automatically a crash.** Check the exit status, new crash reports, logs and whether the user quit it. Shell-launched background processes can also be reaped when their harness exits. Use LaunchServices for native smoke, record the exact process, and allow splash/session startup and waveform scanning to settle before interpreting idle metrics.


## October 3 follow-up regression coverage

`OctoberRegressionTests.cpp` exercises the 14 additional findings listed in `docs/history/AUDIT_FIXES_2026-10-03.md`: stereo-layout rejection, reference-bed recording alignment, reconnect stage restoration, capture deletion guards, device-loss finalization, persisted trim references, missing-left stereo capture, mixed legacy media, pasted-clip export, source-bounded trims, grouped movement bounds and input-only auto-arm. Run the full `--run-tests` suite with isolated settings, then an isolated normal-UI startup smoke. Desk replies and device-stop callbacks are simulated; repeat those two cases on a disposable hardware session before field deployment.

Validation completed 2026-10-04: universal Release GUI/helper build, 438 passing test groups, design/invariant gates, strict bundle signatures and normal startup/quit smoke. No new crash report; saved settings hashes unchanged.

## STOP responsiveness regression — 2026-10-04

The development Mac's live PID 70029 sample found the message thread in the synchronous session snapshot copy on external ExFAT PJ, concurrent with eight hash readers and four high-priority thumbnail readers. The source fix serializes report hashing process-wide, yields 50 ms between 4 MiB chunks, cancels obsolete reads, and lowers thumbnail scans to background priority. `FastHashTests` checks yielding hash equivalence across a chunk boundary and cancellation within a file without returning a partial digest.

Validation: universal Release GUI/helper build; **440 test groups, zero failures**; both audit gates; deep/strict bundle signatures. Reopened the affected 58-track, 77:36 session with isolated preferences: EDIT/MIXER switching and save/quit completed, with no new crash report. At about 61 seconds the process used 452,592 KiB RSS and 49.7% CPU while scanning/painting. The session's mix/settings/report JSON stayed semantically unchanged; only project `updatedAt` changed during save. All 58 WAV headers matched the report's primary frame totals at 48 kHz. Hashes from the original stopped take remained pending; this is not full audio-content verification.

This smoke verifies loading, view commands and saving on the affected drive, not a repeat of the entire 77-minute capture/STOP under the fixed build. Repeat a disposable recording on the target interface/drive before the next long take.

## Documentation and soak validation

Documentation-only changes validate local links/anchors, referenced source APIs,
commands and current-versus-historical claims. They do not change the app binary
or re-establish runtime performance. Avoid rebuilding or interrupting the installed
app solely to edit prose.

An overnight soak has no unattended stop: someone stops the take with the usual
two presses. `tools/soak_monitor.sh` only watches the app and the growing take,
and never signals the app. Final SHA hashing and `verify_take.sh` come after STOP.
