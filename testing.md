# Testing Strategy — ZynForge Recording

## Philosophy and Goals

ZynForge Recording is a live-stage tool. The cost of a regression discovered in front of an audience is qualitatively higher than the cost of a regression in a typical desktop app. The testing strategy reflects that: **catch crashes and data loss before the audience does, even at the cost of some manual effort.**

The strategy is **build + unit + smoke + field**. The unit-test harness is in place (139 test groups and counting; see *How to Run Tests*) and covers the audio callback, recorder, player, automation, markers, clip-edit persistence, and accessibility paths headlessly. Smoke-test and field rehearsal still backstop it for anything the headless harness can't see (paint, real CoreAudio, live VoiceOver, **macOS menu enablement**, the session reopen-on-launch flow).

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
ps -p $PID -o pid,rss,etime,%cpu                                            # expect ~49 MB, <1% CPU
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
