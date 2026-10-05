# Testing Strategy — ZynForge Recording

## October 5 isolated repair verification

The clean audit baseline built successfully; its original 440 groups had one capture-deletion failure. Historical green counts below do not validate the current patch. [The audit report](docs/AUDIT-2026-10-05.md) owns current red/green and sanitizer evidence.

The frozen S6 source passed **519 groups / 0 failures** in each full optimized
Release, TSan and ASan/UBSan/float-cast-overflow run; no sanitizer reports were
emitted. The universal GUI/helper and separate sanitizer builds, signatures and
gates pass. S6 no-device TERM/INT repeats and the final bounded ENOSPC export
repeat (5/0) pass. Normal startup smoke is waiting on macOS microphone consent
and is not passed. The S6 pair is installed, hashes/signatures verified and left
closed; physical-rig acceptance remains pending.

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

Test mode uses the distinct application identity `Zynforge Recording Tests`, preserving single-instance exclusion between test processes without forwarding arguments to an already-running production app. Run only one suite/process at a time. `--test-filter` selects registered suite names case-insensitively; zero matches exit nonzero. Omit the filter for the full suite. `--test-report` must be an absolute log path. The authoritative build and filtered LaunchServices commands live in the [audit register](docs/AUDIT-2026-10-05.md#reproduction-commands-and-final-verification-still-required).
Inspect each report's total failures and sanitizer logs; LaunchServices' own exit code alone is not the test outcome. For an instrumented bundle launched with `open`, pass sanitizer configuration using `open --env`, for example `--env TSAN_OPTIONS=halt_on_error=1:log_path=/private/tmp/zynforge-tsan`; a shell-only environment assignment may not reach the launched app. Build TSan separately from ASan/UBSan. See the audit report for exact compiler flags and actual run logs. Never run failure-injection fixtures against the user's session or current capture app.

## Philosophy and Goals

**Current validation — 2026-10-04:** Source build `c563b00` passed the universal Release GUI/helper build, **438 test groups with zero failures on Apple Silicon**, both static gates, strict/deep bundle-signature checks and a 30-second isolated startup/quit smoke. Both production settings files retained their hashes. Intel slices built successfully; this run does not establish native Intel execution results.

At the October 4 checkpoint, the same app and matching protocol-v3 helper were installed on both Macs; S6 now supersedes the development-Mac installation. Installed executable hashes and signatures match; both apps launched without a new crash report. The mini's two exits were manual user quits. The current DMG passed checksum/image verification on both Macs and a read-only bundle comparison locally. Old app backups and installers were removed at the user's request. [INSTALL.md](INSTALL.md) owns the current package, hashes and installation evidence.

The October regression groups, together with daemon, companion and existing integration suites, cover the 28 initial October findings and 14 follow-up findings. See [the audit fix record](AUDIT_FIXES_2026-10-03.md). Disposable physical device removal, console reconnect, storage failure, detailed waveform/FOLLOW interaction and the planned 56-input rehearsal remain open. The older five-hour primary-drive take is historical evidence for `d2c5858`, not hardware acceptance of `c563b00`; see [field evidence](FIELD-TEST-2026-10-01.md).

**2026-09-30 capture-integrity validation:** A repeated daemon START is refused without resetting a continued take's base or clearing an existing recovery warning. A continued session report lists base and later parts, accumulates frame/time counters and retains earlier capture-failure flags. Handoff flags a failed punch or hash pass for review. Disposable verifier fixtures pass complete WAV/AIFF/FLAC and continued takes, while failed capture flags, a short file, a missing middle part and a rolled-back punch fail. A mocked unattended-stop run confirms the companion's two-tap STOP and a nonzero result when it is unreachable. These are source and fixture tests; slow-client behavior, real disk failure and the actual device path still need field checks. The [fix record](AUDIT_FIXES_2026-09-30.md) maps each failure to its change.

The current harness covers **438 test groups**, including capture provenance/alignment, reference media, stereo topology, device-loss finalization, session state, console restoration, routing, grouped editing, long-take waveforms and settings isolation. Historical paragraphs below retain the counts that were accurate for their dated audit passes.

### Earlier validation snapshots

**2026-09-29 follow-up validation:** disposable verifier fixtures cover valid WAV, AIFF and FLAC takes plus an unlisted WAV that must fail. The daemon suite now simulates audio-device loss during a rolling take and checks that the file closes, a latched alarm reaches the client, and the take report records the interruption. On the target Macs, verify the full capture-warning text and METERS/BACKUP controls at the normal and minimum supported window sizes; exercise a stereo pair whose interleaved left file coexists with an older right file; interrupt a disposable daemon take by disconnecting its device and inspect the saved audio and report. A physical long-take waveform scroll remains necessary.

**Prior source/package (2026-09-24):** the universal Release build with the long-take EDIT and codebase-audit fixes and its exact installed copy each passed **397 test groups / 0 failures**. That installed app had the matching helper and was later preserved for rollback. [GitHub CI run 36017363562](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/36017363562) applies only to older commit `b2c1991`. Native UI geometry warnings, physical devices, actual power loss and the planned three-hour/56-input rehearsal remained unverified. See [SHOW-READINESS.md](SHOW-READINESS.md).

**September regression coverage:** `SeptemberRegressionTests.cpp` covers backup collision, frozen capture arms, empty arrangements, extended cross-track playback, missing sources, locks/takes, replacement automation snapshots, session UUID round-trip, reorder/delete persistence, mixed stereo topology, rollback/interrupted transactions, seamless loops, canonical project lookup and concurrent/failing atomic writes. `AuditFixTests.cpp` additionally covers cross-container fresh-take protection, explicit continue, unavailable/persisted backups, edited-arrangement and all-silent Strip Silence, daemon-record playback refusal, and consolidation beyond `_999`. Capture protocol/link tests cover completion state and command rejection before Hello; audio-callback tests cover StereoMix without physical stream outputs. Dedicated import and click-render suites cover cancellation, collision and transactional installation. Companion tests cover malformed pre-auth input; AAF tests cover both 16-bit limits; packaging is gated by strict code-sign verification. The September 20 set of 16 findings is mapped in [the 2026-09-20 fixes](AUDIT_FIXES_2026-09-20.md); earlier passes remain in [the 2026-09-15 report](AUDIT_REPORT_2026-09-15.md) and [the 2026-09-12 report](AUDIT_FIXES_2026-09-12.md).

The punch/recovery/import regressions now cover an interrupted `.punchbase` restore and partial-take archive, an occupied sidecar refusing capture, a newly enabled backup without the base refusing punch, pre-roll not shifting the splice, newly armed punch/continue tracks aligning to the session, a sub-block selection punch that records exactly its window and monitors only the live take, and a four-channel import retaining all four inputs. Loop suspension, post-roll continuity and physical output switching still need disposable real-device interaction checks; headless tests alone cannot certify a live show.

The 2026-09-24 final source smoke launch started the built bundle and remained at approximately 95 MB RSS / 0.3% CPU with no new `Zynforge*.ips` crash report. The UI automation bridge and AppleScript quit both timed out, so this was **not** a visual or interactive acceptance check; the exact smoke-process PID was terminated after confirming it was the newly launched idle build. The installed app was not replaced.

The 2026-09-24 DMG packages that same `b2c1991` source build, not a separately tested installation. `hdiutil verify` passed, a read-only mount contained the expected Applications shortcut plus the matching GUI/helper bundle, `diff -qr` matched the Release app, and both Mach-O executables were `arm64` + `x86_64`. No installed-bundle or second-Mac smoke test has been performed for this DMG. The older `0.2.0` DMG was untouched.

The manual-punch regression covers a rolling playhead overriding a stale edit cursor, one-press punch-out with session metadata saved, format-changed punch into the original file, and refusal of a missing fresh insert. A disposable real-device take must still verify the whole RECORD/play/punch-out interaction and backup/mirror consistency before show use.

The final 2026-09-24 punch build opened Welcome and reached the idle transport with no session loaded; no new Zynforge crash report appeared. Its audio-device initialization on this development Mac was slow, so this idle launch is not a capture-performance result.

The live EDIT-navigation regression checks that manual horizontal pan/time zoom pauses playhead follow, automatic page-scroll and vertical-only scroll do not, FOLLOW resumes, a new transport pass resets follow, and H+/H- return exactly to fit-to-take. Native smoke and a disposable recording must still verify actual wheel/trackpad, minimap, H/V controls and UI performance while rolling; headless state tests alone do not certify that interaction or capture integrity.

The long-take regression checks that three hours of peak bins retain actual sample time within bounded memory, a long continuation prefill stays bounded, a three-hour live timeline has scrollable width, and recording read-only mode leaves zoom buttons enabled. Additional tests assert that isolated transients remain as thin maxima beside a smoother overview body, sustained levels remain visible, an audio file newer than `WaveCache.wfm` invalidates the cache, H+ can pass the old 16× cap, and deep zoom draws only viewport grid ticks. The completed 2026-10-02 hardware take confirms saved primary-audio duration and a clean report; an earlier screenshot exposed blocky post-STOP display. Screenshots of `98a645e` show the thin-body overview and narrow peaks after reopening, but the user confirmed H+ could not pass a 16-minute span. The zoom fix is installed on both Macs in `c563b00`; a few-minute detailed-zoom screenshot is still needed; live motion/navigation at timed long-run checkpoints still require direct observation.

The 2026-09-24 audit follow-up adds file-backed regressions for a pure end trim, a renamed/locked clip, playback of newly appended audio after a split, refusal of an orphan continuation part, fresh/stale daemon marker positioning and LOCK refusing remote playback. Disposable `verify_take.sh` fixtures exercise pending hashes, complete hashes, missing hashes, valid continuation parts and an orphan part. Native interaction still needs a disposable recording and export test.

Native empty-session smoke on 2026-09-24 confirmed the rebuilt app opens EDIT, shows full H/V labels, and H+ followed by H- removes the minimap at 1x. This did not exercise a rolling waveform or physical capture.

ZynForge Recording is a live-stage tool. The cost of a regression discovered in front of an audience is qualitatively higher than the cost of a regression in a typical desktop app. The testing strategy reflects that: **catch crashes and data loss before the audience does, even at the cost of some manual effort.**

The strategy is **build + unit + smoke + field**. The unit-test harness is in place (see the current validation count above and *How to Run Tests*) and covers the audio callback, recorder, player, automation, markers, clip-edit persistence, accessibility, the measured pre-flight probes, post-show QC, song detection, crash-report scanning, the console link (+ profile capabilities), stereo-pair export, the EDIT ruler↔lane scroll alignment, the live-wave continue path, the session-recovery sort comparator, and the `EngineStatus` boundary — all headlessly. The 2026-07-10 deep audit + its re-audits added five regression tests in `RecordingIntegrityTests`: *swapTracks preserves clip edits* (a reorder must move splits/comps with the audio, never wipe them), *splitClipAt fade geometry* (hard cut at the seam, inherited fade clamped to the new clip length), *Continue-record grows the take* (a continue must refresh the default clip to the grown length, never leave the appended audio silent), *swapTracks renames continuation parts* (a reorder must move a take's `_partXX` parts + FLAC/AIFF files, not just `Track_NN.wav`, or they end up on the wrong channel), and *marker during a continue lands on the timeline* (a marker dropped mid-continue must land at `recordBaseSamples + offset`, not the new part's 0-based length). The 2026-08-11 whole-codebase bug hunt added **`Source/Tests/AuditFixTests.cpp`** (9 groups), one per headlessly-reachable fix: the stereo-mix bounce staying in bounds when the player out-counts the mixer, `seedDefaultClips` preserving clips above the last recorded take, mirrors counting toward the disk-rate estimate, a continue adopting the existing take's container, `takeIsMultiPart` ignoring `.punchbase` sidecars, the FLAC 32-bit clamp, same-rate export length, `removeStripAt` shifting stereo flags + strip UUIDs, and the transient scan covering FLAC. The remaining fixes from that pass are message-thread behaviour (threaded export / Save-As, the async SMART poll, the pre-flight probe guard, punch arm restore) and are smoke-test territory. The 2026-08-12 EDIT-view audit added **`EditViewFixTests.cpp`** (3 groups) against the shared logic its fixes were extracted onto — `EditTimeline.h`'s notional-span and peer-clip-mapping helpers — plus the sorted-lane invariant that makes the automation point-drag re-resolution necessary. `EditPage::TrackRow` is now a **public** nested declaration precisely so it can be constructed directly: **`EditTrackRowTests.cpp`** covers the meter-condemn contract (a condemned row must stop dereferencing its `TrackState`), the stale-index guard, and odd/even/unrouted stereo routing display. Reverting either fix makes the suite fail — the stale-index one by *crashing the test binary* in `TrackRow::updatePollState`, which is the production UAF reproducing under test. The remaining EDIT fixes (wave-cache ordering, the mouse-handler drag paths) are still smoke-test territory. **CI** (`.github/workflows/ci.yml`, macos-14) builds and runs the full suite on every push and PR to `main`. Smoke-test and field rehearsal still backstop it for anything the headless harness can't see (paint, real CoreAudio, live VoiceOver, **macOS menu enablement**, the session reopen-on-launch flow).

**2026-08-18 remediation coverage.** The suite proves exact mixer shrink/reset, complete state clearing, corrupt-mix fallback, primary-writer fail-closed behaviour with punch-stash restoration, multipart numeric ordering and missing-trailing-part refusal, player reader lifetime/prebuffering, persisted MIDI running status, Generic OSC authentication, companion commands returning their real message-thread result, and wrap-safe remote time conversion. Remote RECORD now uses the full host preflight (2026-09-23). Save As/relocation cancellation, native `.zfproj` launch, save-failure modal flow, and the installed bundle remain smoke-test responsibilities because they require macOS UI/filesystem integration.

## Testing Pyramid / Approach

```
        ╱╲          field rehearsal     — every change touching Source/Audio/
       ╱  ╲         smoke-test         — every change, automated by Claude
      ╱────╲        unit tests         — `--run-tests`, headless, every change
     ╱──────╲       build              — every change, blocking
```

- **Build** — `cmake --build build --config Release`. Must succeed. No warnings-as-errors yet, but new warnings should be addressed.
- **Unit tests** — `juce::UnitTest` groups in `Source/Tests/`, run via `--run-tests` (or `ZYNFORGE_RUN_TESTS=1`). No physical CoreAudio device; UI regressions use the real message thread and bounded native-loop pumping. Add a test with every bug fix and every new audio-thread / persistence path.
- **Smoke-test** — Launch the built app, exercise the changed surface, verify RSS / CPU / no new crash report. Documented in `CLAUDE.md` under *Build + smoke test recipe*.
- **Field rehearsal** — User-driven. Anything touching the audio callback, the recorder, or the player must be exercised in a real (or simulated) session before being declared shippable. The user is the only authority for this stage.

**Menu / session-load smoke checks (can't be unit-tested — native menu + message thread).** macOS caches the menu's greyed states until `menuItemsChanged()` fires, so after any change near menu enablement, manually verify: launch reopens the last session with its channels; with a session loaded the Edit/Track/Export items light up; after a clip edit, Undo lights up; selecting a strip lights up Track ▸ Cut/Copy/Solo. Export reads from `Audio Files/` — verify Export Individual Track actually writes a file.

**Session-transition smoke checks.** With unsaved mixer/clip/cue changes, exercise New, Open, CSV, Console, New from Template, Finder `.zfproj`, Close and Quit. Verify all replacement paths offer Save / Don't Save / Cancel; failed saves leave the session open. Save As must reject a non-empty target, leave the source active during copy, and open only the complete clone. Cancel a cross-volume Save As/relocation and verify there is no partial destination. While a copy/move/bounce is active, Record/Play/strip mutation and a second session job must refuse without changing state.

**Click-track generation smoke check (device-manager + file I/O, not unit-tested).** In a fresh session, set a tempo and press Generate Click Track: it must create a "Click" strip and write its `Track_NN.wav` on the **first** press (a regression once made the guard abort the first press with "Click slot isn't a Click strip"). Press again to regenerate — it must overwrite the same strip, never a recorded channel that merely happens to be named "Click".

**Audio Device panel single-instance smoke check (message thread + modal, not unit-tested).** With no audio device present, both the toolbar DEVICE button and the PATCH tab's "Audio settings…" placeholder open the device panel. Open one, then trigger the other — it must surface the existing panel, not stack a second (two live panels each snapshot the device state, so cancelling both would double-restore and fight over the channel counts). Cancelling the single panel must still restore the boot channel counts (meters/recording keep working).

## Frameworks and Tools in Use

- **CMake / Xcode** — build orchestration.
- **JUCE's built-in `juce::UnitTest`** — the test groups in `Source/Tests/` register themselves as static instances and run via `--run-tests` inside the normal app binary (no separate test target). `AudioEngine::setTestModeSkipAudioInit(true)` + `prepareForTests(sr, blockSize)` let tests construct an engine and drive `audioDeviceIOCallbackWithContext` without a real audio device.
- **macOS unified log** + `~/Library/Logs/DiagnosticReports/` — primary post-launch signal source. `log show --process "Zynforge Recording" --predicate 'messageType == error or messageType == fault' --last 1m` surfaces runtime errors and faults.
- **`ps` / Activity Monitor** — RSS and CPU snapshots during smoke-test.

## When to Write Tests

The harness exists and every change should keep it green (and grow it where the change adds a testable path). Write a test:

- Whenever fixing a bug. The test should fail on the pre-fix code and pass on the post-fix code.
- For every pure function in `Source/Audio/` that does non-trivial maths (clip rendering, fade interpolation, sample-rate conversion, timecode decoding).
- For every persistence path (`.zfproj` round-trip, `appProps` reload).
- For every soft-takeover ramp + VCA gain calculation — these are easy to regress and silent when they break.

Do not write a test for paint code. Visual regression is caught by smoke-test + screenshot review.

## How to Run Tests

### Build
```bash
cmake -B build -G Xcode
cmake --build build --config Release
```

### Smoke-test

Stop takes, save, and quit the GUI and helper gracefully before launching a second instance. Use temporary preferences for a development smoke:

```bash
open -n "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" --args --isolated-settings
```

Wait through startup, identify the exact new process and its executable path, then sample its PID with `ps -p <pid> -o pid,rss,etime,%cpu` at 10, 20 and 30 seconds. Check the visible window, runtime errors and newly created `~/Library/Logs/DiagnosticReports/Zynforge*.ips`, then quit through the app. Record the actual CPU/RSS and UI state rather than treating old measurements as fixed pass thresholds. The October 4 isolated smoke settled near 120 MiB RSS / 3% CPU at 30 seconds on this Mac. An installed-app launch with saved preferences is a separate check; see [INSTALL.md](INSTALL.md).

### Unit tests

The audit-follow-up regressions cover stale daemon status, remote STOP confirmation/current-session RECORD preflight, snapshot-journal retention, hostile multipart filenames/read failures, refused companion commands, skipped-mirror report/handoff evidence, legacy root-level handoff and duration-scaled MP3 timeouts. A live rig is still required to validate daemon disconnect and remote-control ergonomics during a real take.
The 2026-09-23 idle smoke launch showed a normal window and no new `.ips` crash report (about 118 MB RSS / 5.9% CPU on this development machine), but AppKit logged transient negative-view-geometry runtime faults. Treat that as an open native-UI follow-up, not a fully clean runtime log.

```bash
# Stop takes and quit GUI + daemon gracefully before this isolated test run.
# Never run process-name-wide kill commands on a recording workstation.
APP="build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
open -W -n "$APP" --args --run-tests
# Launch the bundle through LaunchServices. On newer macOS, invoking the raw
# GUI executable can abort in NSApplication registration before tests start.
# Results land in ~/Library/Logs/Zynforge/test-report.log. Check its mtime and
# final summary; a stale report means the run did not actually fire.
tail -1 "$HOME/Library/Logs/Zynforge/test-report.log"   # "[zynforge tests] N test groups, 0 failure(s)"
```

### Static gates (pre-commit, and the cheapest tier of all)

```bash
tools/design_audit.sh       # 7 brand rules   -- raw colours/fonts/alphas/radii/tints, spacing ratchet
tools/invariants_audit.sh   # 27 correctness rules -- recurring bug classes
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

**Settings isolation:** `Source/Audio/SettingsFile.h` provides `makeSettingsFile()` for `AudioEngine` and all four `Strip*` settings writers. `--run-tests` and `--isolated-settings` enable process-wide isolation before construction; once enabled, later audio-initialization changes cannot restore production preference writes. Each process gets a unique temporary settings file. Tests that construct an engine should set `AudioEngine::setTestModeSkipAudioInit(true)` first; suites that need callback initialization must still retain settings isolation. Shared state within a process still requires setup/teardown cleanup.

Notable suites: `CompanionServerTests` (loopback server end-to-end), `MenuDispatchTests` (id-collision + dispatch-range guard), `CompoundFileTests`/`FastHashTests`/`SessionBackupTests` (capture-side helpers), `AudioCallbackTests` (audio-thread integration, including RF64 policy, 64-channel throughput, StereoMix capture without physical stream outputs, and windowed offline-render equivalence), `AuditFixTests` (record collision, redundancy, Strip Silence, external-record transport, and filename boundaries), `CaptureLinkTests`/`CaptureDaemonTests` (protocol handshake, completion and daemon failure paths), `PreflightTests` (measured disk-speed/writability/headroom), `QcAnalyzerTests` (peak/clip/floor against synthesized WAVs), `SongDetectorTests` (multi-track quorum, incl. an always-hot ambient mic detecting nothing alone), `CrashScanTests` (.ips filter + summary), and `ConsoleLinkTests` (full X32 query→stash→flip→restore state machine through a transport seam, plus a real connect→disconnect→reconnect socket-rebind regression).

`ShowHandoffTests` checks the portable sidecars, exact source/copy hashes, changed-copy rejection, and pending-capture-report warning. `CompanionServerTests` also checks that `/confidence` requires the token, exposes no command endpoint, and shows daemon status rather than local writer metrics. Manual checks remain necessary: open the confidence page through a TLS tunnel on a phone, interrupt its network, verify alerts and the no-controls surface, then run a real multi-hour handoff on a separate volume and inspect its files/report in another DAW. Do not run the handoff during a live take.

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

`OctoberRegressionTests.cpp` exercises the 14 additional findings listed in `AUDIT_FIXES_2026-10-03.md`: stereo-layout rejection, reference-bed recording alignment, reconnect stage restoration, capture deletion guards, device-loss finalization, persisted trim references, missing-left stereo capture, mixed legacy media, pasted-clip export, source-bounded trims, grouped movement bounds, live aux, VCA send mute and input-only auto-arm. Run the full `--run-tests` suite with isolated settings, then an isolated normal-UI startup smoke. Desk replies and device-stop callbacks are simulated; repeat those two cases on a disposable hardware session before field deployment.

Validation completed 2026-10-04: universal Release GUI/helper build, 438 passing test groups, design/invariant gates, strict bundle signatures and normal startup/quit smoke. No new crash report; saved settings hashes unchanged.

## STOP responsiveness regression — 2026-10-04

The development Mac's live PID 70029 sample found the message thread in the synchronous session snapshot copy on external ExFAT PJ, concurrent with eight hash readers and four high-priority thumbnail readers. The source fix serializes report hashing process-wide, yields 50 ms between 4 MiB chunks, cancels obsolete reads, and lowers thumbnail scans to background priority. `FastHashTests` checks yielding hash equivalence across a chunk boundary and cancellation within a file without returning a partial digest.

Validation: universal Release GUI/helper build; **440 test groups, zero failures**; both audit gates; deep/strict bundle signatures. Reopened the affected 58-track, 77:36 session with isolated preferences: EDIT/MIXER switching and save/quit completed, with no new crash report. At about 61 seconds the process used 452,592 KiB RSS and 49.7% CPU while scanning/painting. The session's mix/settings/report JSON stayed semantically unchanged; only project `updatedAt` changed during save. All 58 WAV headers matched the report's primary frame totals at 48 kHz. Hashes from the original stopped take remained pending; this is not full audio-content verification.

This smoke verifies loading, view commands and saving on the affected drive, not a repeat of the entire 77-minute capture/STOP under the fixed build. Repeat a disposable recording on the target interface/drive before the next long take.
