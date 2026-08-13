# Testing Strategy — ZynForge Recording

## Philosophy and Goals

ZynForge Recording is a live-stage tool. The cost of a regression discovered in front of an audience is qualitatively higher than the cost of a regression in a typical desktop app. The testing strategy reflects that: **catch crashes and data loss before the audience does, even at the cost of some manual effort.**

The strategy is **build + unit + smoke + field**. The unit-test harness is in place (**293 test groups** as of 2026-08-13, and counting; see *How to Run Tests*) and covers the audio callback, recorder, player, automation, markers, clip-edit persistence, accessibility, the measured pre-flight probes, post-show QC, song detection, crash-report scanning, the console link (+ profile capabilities), stereo-pair export, the EDIT ruler↔lane scroll alignment, the live-wave continue path, the session-recovery sort comparator, and the `EngineStatus` boundary — all headlessly. The 2026-07-10 deep audit + its re-audits added five regression tests in `RecordingIntegrityTests`: *swapTracks preserves clip edits* (a reorder must move splits/comps with the audio, never wipe them), *splitClipAt fade geometry* (hard cut at the seam, inherited fade clamped to the new clip length), *Continue-record grows the take* (a continue must refresh the default clip to the grown length, never leave the appended audio silent), *swapTracks renames continuation parts* (a reorder must move a take's `_partXX` parts + FLAC/AIFF files, not just `Track_NN.wav`, or they end up on the wrong channel), and *marker during a continue lands on the timeline* (a marker dropped mid-continue must land at `recordBaseSamples + offset`, not the new part's 0-based length). The 2026-08-11 whole-codebase bug hunt added **`Source/Tests/AuditFixTests.cpp`** (9 groups), one per headlessly-reachable fix: the stereo-mix bounce staying in bounds when the player out-counts the mixer, `seedDefaultClips` preserving clips above the last recorded take, mirrors counting toward the disk-rate estimate, a continue adopting the existing take's container, `takeIsMultiPart` ignoring `.punchbase` sidecars, the FLAC 32-bit clamp, same-rate export length, `removeStripAt` shifting stereo flags + strip UUIDs, and the transient scan covering FLAC. The remaining fixes from that pass are message-thread behaviour (threaded export / Save-As, the async SMART poll, the pre-flight probe guard, punch arm restore) and are smoke-test territory. The 2026-08-12 EDIT-view audit added **`EditViewFixTests.cpp`** (3 groups) against the shared logic its fixes were extracted onto — `EditTimeline.h`'s notional-span and peer-clip-mapping helpers — plus the sorted-lane invariant that makes the automation point-drag re-resolution necessary. `EditPage::TrackRow` is now a **public** nested declaration precisely so it can be constructed directly: **`EditTrackRowTests.cpp`** covers the meter-condemn contract (a condemned row must stop dereferencing its `TrackState`), the stale-index guard, and odd/even/unrouted stereo routing display. Reverting either fix makes the suite fail — the stale-index one by *crashing the test binary* in `TrackRow::updatePollState`, which is the production UAF reproducing under test. The remaining EDIT fixes (wave-cache ordering, the mouse-handler drag paths) are still smoke-test territory. **CI** (`.github/workflows/ci.yml`, macos-14) builds and runs the full suite on every push and PR to `main`. Smoke-test and field rehearsal still backstop it for anything the headless harness can't see (paint, real CoreAudio, live VoiceOver, **macOS menu enablement**, the session reopen-on-launch flow).

## Testing Pyramid / Approach

```
        ╱╲          field rehearsal     — every change touching Source/Audio/
       ╱  ╲         smoke-test         — every change, automated by Claude
      ╱────╲        unit tests         — `--run-tests`, headless, every change
     ╱──────╲       build              — every change, blocking
```

- **Build** — `cmake --build build --config Release`. Must succeed. No warnings-as-errors yet, but new warnings should be addressed.
- **Unit tests** — `juce::UnitTest` groups in `Source/Tests/`, run via `--run-tests` (or `ZYNFORGE_RUN_TESTS=1`). Headless, no CoreAudio device, no message thread. Add a test with every bug fix and every new audio-thread / persistence path.
- **Smoke-test** — Launch the built app, exercise the changed surface, verify RSS / CPU / no new crash report. Documented in `CLAUDE.md` under *Build + smoke test recipe*.
- **Field rehearsal** — User-driven. Anything touching the audio callback, the recorder, or the player must be exercised in a real (or simulated) session before being declared shippable. The user is the only authority for this stage.

**Menu / session-load smoke checks (can't be unit-tested — native menu + message thread).** macOS caches the menu's greyed states until `menuItemsChanged()` fires, so after any change near menu enablement, manually verify: launch reopens the last session with its channels; with a session loaded the Edit/Track/Export items light up; after a clip edit, Undo lights up; selecting a strip lights up Track ▸ Cut/Copy/Solo. Export reads from `Audio Files/` — verify Export Individual Track actually writes a file.

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
```bash
pkill -x "Zynforge Recording" 2>/dev/null
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"
sleep 10
PID=$(pgrep -f "Zynforge Recording")
ps -p $PID -o pid,rss,etime,%cpu   # ~49 MB / <1% with NO session; ~115 MB / ~5% with a session
                                   # auto-reopened (live input meters repaint by design --
                                   # measured 2026-06-12 after the idle-repaint fixes; 14%+ means
                                   # a blanket-repaint regression, go sample the process)
ls -lt ~/Library/Logs/DiagnosticReports/Zynforge*.ips 2>/dev/null | head -1 # expect no new entries
log show --process "Zynforge Recording" \
    --predicate 'messageType == error or messageType == fault' --last 1m    # expect empty
```

### Unit tests
```bash
# Quit any running GUI instance first -- LaunchServices will otherwise
# forward the launch to it and the flag is dropped.
pkill -f "Zynforge Recording" 2>/dev/null; sleep 1
BIN="build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app/Contents/MacOS/Zynforge Recording"
"$BIN" --run-tests
# Results stream to stderr AND ~/Library/Logs/Zynforge/test-report.log.
# Check the report's mtime before trusting the pass count -- a stale
# report means the run didn't actually fire (e.g. LaunchServices intercept).
tail -1 "$HOME/Library/Logs/Zynforge/test-report.log"   # "[zynforge tests] N test groups, 0 failure(s)"
```

### Static gates (pre-commit, and the cheapest tier of all)

```bash
Tools/design_audit.sh       # 7 brand rules   -- raw colours/fonts/alphas/radii/tints, spacing ratchet
Tools/invariants_audit.sh   # 10 correctness rules -- the bug CLASSES that repeat audits kept re-finding
Tools/install_hooks.sh      # once per clone: wires both as a pre-commit hook (hooks aren't in git)
```

These catch a class of defect no unit test will: a hazard correctly handled at nine call sites and missed at the tenth. They run in under a second, so run them before the suite, not after.

**Adding a rule: watch it go RED before you land it.** A rule that passes on a broken tree is worse than no rule, because it reads like coverage. Two of the original seven were blind — one matched a *commented-out* call, one was satisfied by an unrelated comment in the same file — which is why the rules now strip comments (`sed 's,//.*,,'`) before grepping. The procedure is: write the rule, inject the regression it's supposed to catch, confirm the gate fails, restore, confirm it passes. Rule 10 found three real unguarded sites on the day it landed that a careful read of the same files had missed.

**Test isolation — BETWEEN tests, not just from your settings.** Every test-mode `AudioEngine` shares ONE throwaway `zynforge-test.settings`, so any suite that writes per-strip state (`setTrackStereo` → `strip_stereo_N`, routing → `strip_in_N`, gains, colours) **leaks into every later suite** that constructs an engine and calls `applyPersistedStripState()`. `EditTrackRowTests` broke *Player maps files by Track_NN index* exactly this way. A suite that mutates per-strip state must call `engine.clearAllStripOverrides()` on setup AND teardown — see that file's `Host` struct.

**Test isolation:** the suite is safe to run on your own machine — a test-mode `AudioEngine` points its `appProps` at a throwaway `zynforge-test.settings` in the temp dir, so recording/pref-mutating tests can't corrupt your real `.settings` (`activeSessionDir`, recent list). This was a real bug (tests left the app reopening a deleted scratch session); `EngineStateTests` asserts the test engine's settings file lives under the temp dir. Tests that touch the engine should set `AudioEngine::setTestModeSkipAudioInit(true)` before constructing it.

Notable suites: `CompanionServerTests` (loopback server end-to-end), `MenuDispatchTests` (id-collision + dispatch-range guard), `CompoundFileTests`/`FastHashTests`/`SessionBackupTests` (capture-side helpers), `AudioCallbackTests` (audio-thread integration, incl. RF64 policy + 64-ch throughput + the windowed offline-render equivalence), `PreflightTests` (measured disk-speed/writability/headroom), `QcAnalyzerTests` (peak/clip/floor against synthesized WAVs), `SongDetectorTests` (multi-track quorum, incl. an always-hot ambient mic detecting nothing alone), `CrashScanTests` (.ips filter + summary), and `ConsoleLinkTests` (full X32 query→stash→flip→restore state machine through a transport seam, plus a real connect→disconnect→reconnect socket-rebind regression).

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
- **Tests run headless.** No paint, no Component, no message thread. If a test needs a message-thread pump, it belongs in the smoke-test tier, not the unit tier.

## Common Pitfalls to Avoid

- **Don't smoke-test in Debug.** Release-only optimisations sometimes mask UB; Debug-only assertions sometimes mask logic bugs. Always Release.
- **Don't compare crash-report counts naively.** The pre-existing 13:04 crash from the original session is in the report directory. Compare timestamps, not counts.
- **Don't trust LSP `juce undeclared identifier` errors.** They are stale because the LSP's include path doesn't see the JUCE module headers. The build is authoritative.
- **Don't assume Apple Silicon and Intel behave identically.** Universal builds means both must work. If a NEON path is added, verify the SSE / scalar fallback at least builds.
- **Don't rely on the field rehearsal as a unit-test substitute.** A rehearsal catches obvious regressions; it doesn't exhaustively explore edge cases.
- **Don't smoke-test for two seconds.** Watch the process for at least 10 seconds — slow leaks and timer-driven crashes don't show up immediately.
- **Launch the smoke-test with `open "...app"`, not the raw binary, if you'll need to quit it (learned 2026-06-09).** A binary launched directly (`.../MacOS/Zynforge Recording &`) is NOT LaunchServices-registered, so System Events can't see it (`get name of every process` omits it → "Invalid index") and you cannot answer its save-on-quit modal or `osascript ... to quit` by name. `open` registers it; only then is graceful quit (`osascript -e 'tell application "Zynforge Recording" to quit'` + Return to confirm Save) possible. Force-killing is the device-wedge hazard — see the *quit gracefully* memory.
- **Phantom "the app keeps dying" is usually harness reaping, not a crash.** A backgrounded GUI process gets reaped when the spawning shell/tool call returns. Launch + sample + quit **in one** call (or via `open`, which detaches under launchd). If you see a clean `exit status 0` with no `.ips` and no `quit()` in your own backtrace, suspect the harness, not the app. The app has a **7 s splash** before the main window opens (`Main.cpp` `callAfterDelay(7000)`), so don't sample CPU/RSS until ~10 s in; first launch on a large session also rebuilds `WaveCache.wfm` (transient CPU spike) before settling.
