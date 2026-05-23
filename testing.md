# Testing Strategy — ZynForge Recording

## Philosophy and Goals

ZynForge Recording is a live-stage tool. The cost of a regression discovered in front of an audience is qualitatively higher than the cost of a regression in a typical desktop app. The testing strategy reflects that: **catch crashes and data loss before the audience does, even at the cost of some manual effort.**

Today the strategy is **build + smoke + field**. A formal unit-test harness is on the roadmap (`tasks.md`) but is not yet in place.

## Testing Pyramid / Approach

```
        ╱╲          field rehearsal     — every change touching Source/Audio/
       ╱  ╲         smoke-test         — every change, automated by Claude
      ╱────╲        build              — every change, blocking
     ╱──────╲       unit tests         — roadmap, not yet implemented
```

- **Build** — `cmake --build build --config Release`. Must succeed. No warnings-as-errors yet, but new warnings should be addressed.
- **Smoke-test** — Launch the built app, exercise the changed surface, verify RSS / CPU / no new crash report. Documented in `CLAUDE.md` under *Build + smoke test recipe*.
- **Field rehearsal** — User-driven. Anything touching the audio callback, the recorder, or the player must be exercised in a real (or simulated) session before being declared shippable. The user is the only authority for this stage.

## Frameworks and Tools in Use

- **CMake / Xcode** — build orchestration.
- **JUCE's built-in `juce_unit_tests` module** — not yet wired into the build. Adding it requires opting in via `CMakeLists.txt`.
- **macOS unified log** + `~/Library/Logs/DiagnosticReports/` — primary post-launch signal source. `log show --process "Zynforge Recording" --predicate 'messageType == error or messageType == fault' --last 1m` surfaces runtime errors and faults.
- **`ps` / Activity Monitor** — RSS and CPU snapshots during smoke-test.

## When to Write Tests

Today: no automated tests are written. Each change is validated through the three-tier process above.

When the test harness lands (`tasks.md`), write a test:

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

### Future: unit tests
```bash
# Not yet wired up. Expected interface once added:
cmake --build build --config Release --target ZynforgeRecording_Tests
./build/ZynforgeRecording_artefacts/Release/ZynforgeRecording_Tests
```

## Code Coverage Expectations

No coverage target today. When the test harness lands, the initial bar is:

- `Source/Audio/` — 70% line coverage on pure-function paths (excluding RT callbacks, which require a host harness).
- `Source/Theme/` — coverage not meaningful (mostly tokens and inline helpers).
- `Source/UI/` — coverage not pursued; smoke-test covers it.
- `Source/Network/` — 50% line coverage; HTTP endpoints and OSC parsers are good candidates.

## Guidelines for Writing Good Tests

Once the harness is in place:

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
