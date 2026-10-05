# October 5 follow-up review repairs

This register tracks the 21 findings reviewed at `962cb42`, after the earlier
S6 audit. The user authorized all repairs. The installed application and user
recordings are outside this source/test repair; no physical-take acceptance is
implied.

## Evidence collected before repair

- Existing S6 Release suite: 519 groups, zero failures
  (`/private/tmp/zynforge-review-baseline.log`).
- New C++ regression snapshot: 28 groups, 88 failed assertions
  (`/private/tmp/zynforge-review-red-tests.log`). Positive creation/reference
  controls passed; every reviewed C++ defect had a failing assertion.
- Initial auto-stop fixture: three failures; after repair all three passed.
  Expanded retry/session/abort fixtures: two additional failures, then six
  passing tests (`/private/tmp/zynforge-review-autostop-{red,green,red2,green2}.log`).
- Finalization fault injection then exposed 17 failed assertions in a 44-group
  run: capture flags/reports for primary, backup and mirror, three export paths
  preserving existing destinations, and unpublished punch-output cleanup. Those
  regressions remained unchanged for the successful 563-group Release run.
- The original AIFF review described integer output. Runtime reproduction
  corrected that detail: JUCE's public AIFF factory rejects 32-bit creation
  altogether. The selected format fails to record/export. The repair supplies
  standard AIFF-C `fl32`, keeping the persisted format identifier and `.aif`
  extension. Existing integer AIFF remains readable.

## Defect-to-regression map

| # | Defect | Repair and regression suite |
|---|---|---|
| 1 | Ordinary recorded Click channel overwritten | Require explicit reference-media ownership; Review persistence repairs |
| 2 | FIFO gap placed before retained audio | Bounded timeline gap descriptors; Review capture repairs, exact mono/stereo sample ordering |
| 3 | Daemon callback enabled before track allocation | Allocate requested storage before callback registration; Review capture startup |
| 4 | Properties overwrites a newer save | Queue only descriptive fields and merge current project; Review persistence repairs |
| 5 | AIFF 32-bit float creation fails | IEEE float AIFF-C writer shared by capture/export/punch; Review UI and format repairs |
| 6 | Unrequested headamp completes gain capture | Require every requested index; Review network repairs |
| 7 | Partial gain capture restored | Refuse incomplete snapshots; Review network repairs |
| 8 | Mixer undo bookkeeping rewrites all settings | Initial action captures without replaying an applied gesture; Review UI and format repairs |
| 9 | Session reset performs per-field XML I/O | Batch defaults by settings domain; Review capture repairs |
| 10 | Deferred import/bounce admits recording | Recheck capture at actual chooser completion; Review persistence repairs |
| 11 | Saving swallows Space STOP | Preserve stopping transport through busy gate and existing lock/two-tap rules; Review UI and format repairs |
| 12 | Failed project parse becomes destructive save | Refuse before sibling writes; Review persistence repairs |
| 13 | Mixed legacy/modern export omits tracks | Resolve media per track, modern precedence; Review persistence repairs |
| 14 | Noise analysis omits stereo right | Analyze all file channels; Review capture repairs |
| 15 | Unreadable analysis appears clean | Explicit error in report/UI; Review capture and persistence repairs |
| 16 | Classifier reads unpublished FFT memory | Honor producer publication handshake; Review capture repairs |
| 17 | MIDI realtime byte becomes scene data | Byte-wise framing including fragmented messages and SysEx; Review network repairs |
| 18 | OSC token treated as payload | Separate credential envelope from argument count; Review network repairs |
| 19 | Invalid mirror response clears layout | Validate layout before mutation; Review network repairs |
| 20 | Auto-stop success before finalization | Require successful acknowledgment and pinned state throughout retries; tools/test_auto_stop.py |
| 21 | Auto-stop persists token in shared temporary file | Remove unused token file; tools/test_auto_stop.py |

## Reproduction commands

Run from this repository on the development Mac. Reuse the already pinned JUCE
8.0.4 checkout; no dependency update is part of this patch. The Release bundle
contains both arm64 and x86_64; the sanitizer bundles execute arm64.

```sh
cmake -S . -B build -G Xcode \
  -DFETCHCONTENT_SOURCE_DIR_JUCE="$PWD/build/_deps/juce-src"
cmake --build build --config Release --parallel 3
open -n -W "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app" \
  --args --run-tests --test-report=/private/tmp/zynforge-review-release-full.log
python3 tools/test_auto_stop.py
bash tools/design_audit.sh
bash tools/invariants_audit.sh
git diff --check
```

Configure separate Debug directories with `-DCMAKE_OSX_ARCHITECTURES=arm64`
and the same pinned JUCE source. Set each of `CMAKE_C_FLAGS`, `CMAKE_CXX_FLAGS`
and `CMAKE_EXE_LINKER_FLAGS` to the relevant sanitizer switch:
`-fsanitize=thread`, or `-fsanitize=address,undefined,float-cast-overflow`.
Add `-fno-omit-frame-pointer` to the C/C++ flags, then build with `--config Debug`.
The recorded builds used the identical pinned checkout at
`/private/tmp/zynforge-audit-20261005-juce`.

```sh
open -n -W \
  --env ASAN_OPTIONS=halt_on_error=1:log_path=/private/tmp/zynforge-review-asan \
  --env UBSAN_OPTIONS=halt_on_error=1:log_path=/private/tmp/zynforge-review-ubsan \
  "build-review-asan/ZynforgeRecording_artefacts/Debug/Zynforge Recording.app" \
  --args --run-tests --test-report=/private/tmp/zynforge-review-asan-full.log
open -n -W \
  --env TSAN_OPTIONS=halt_on_error=1:log_path=/private/tmp/zynforge-review-tsan \
  "build-review-tsan/ZynforgeRecording_artefacts/Debug/Zynforge Recording.app" \
  --args --run-tests --test-report=/private/tmp/zynforge-review-tsan-full.log
```

Run one suite at a time after builds finish. Inspect the final group/failure
counts and emitted sanitizer diagnostics; `open` returning zero is insufficient.
All audio and failure-injection fixtures use disposable media.

## Final verification

- Universal Release GUI and embedded capture helper built successfully. Both
  contain arm64 and x86_64; deep/strict signatures pass. Embedded/standalone
  helper UUIDs match for both architectures (the bundle helper is re-signed).
- **563 groups, zero failures in each full Release, ThreadSanitizer, and
  ASan/UBSan/float-cast-overflow run.** This is the original 519 groups plus 44
  new groups. No configured sanitizer diagnostic files were emitted.
- Six auto-stop fixture tests pass. Design and invariant gates pass, as does
  `git diff --check`. The revised import gate also failed both injected
  regressions in a disposable source copy: picker bypass and removed worker.
- No-device helper startup with 27 inputs on an ephemeral localhost port and
  clean exit under TERM and INT pass. These are not physical capture tests.
- A 30-second isolated normal startup reached the main component; the process
  sample shows painting and timer dispatch rather than the earlier TCC wait.
  Observed CPU was 0.2–9.2%, RSS roughly 96–172 MiB. Production settings hashes
  were unchanged and no new Zynforge crash report appeared. The UI automation
  bridge failed to start; AppleScript Quit returned “User canceled” (-128).
  Only this owned, nonrecording smoke PID was then terminated. Normal quit and
  interactive UI navigation are therefore **not** accepted by this check.
- Environment: macOS 26.6.2 / arm64, Xcode 27.0, Apple Clang 21.0.0,
  CMake 4.3.2. JUCE remains pinned at
  `51d11a2be6d5c97ccf12b4e5e827006e19f0555a`; the existing CMake-applied FLAC
  patch has identical content in the Release and sanitizer checkouts.

Final logs use `/private/tmp/zynforge-review-{release,asan,tsan}-full.log`.
A durable evidence copy, manifest, patch and SHA-256 sums are retained beside
this repository in `ZynforgeRecording-review-evidence-20261005/`.

## Operator report and remaining limits

All 21 confirmed findings in this register are repaired and have regression
coverage. The initial mixer-undo bookkeeping now causes zero redundant settings
reloads (previously 162 for the fixture); resetting a 27-track session uses five
settings transactions rather than 163. Overflow fixtures retain audio/gap order
through unequal stereo loss, descriptor saturation/recovery and continuation.
Float-AIFF output preserves IEEE float samples, and a final flush failure cannot
be reported as a successful capture, export or punch.

At completion of the source repair, this build was not installed. The user
subsequently requested installation on this Mac: the verified `443c769` GUI/helper
pair now replaces `/Applications/Zynforge Recording.app`, with the prior S6
bundle retained for rollback. See [INSTALL.md](../INSTALL.md) for hashes and
installation evidence. The Mac mini, DMGs, user sessions and recordings remain
unchanged.

Physical 27-track capture/STOP latency, the external volume, real console links,
USB/device hot-plug, long-take memory/I/O behavior and interactive undo/navigation
still need a disposable rig rehearsal. Native Intel execution was not performed.
No new leak-census claim is made: standalone LeakSanitizer is unavailable on this
macOS target and the earlier Apple-framework leak residual remains documented in
[the S6 audit](AUDIT-2026-10-05.md). A clean sanitizer workload is not proof that
all device lifecycle paths or every possible race are covered.

For the rehearsal, record STOP-to-responsive and STOP-to-finalized separately,
watch writer backlog/missed samples and all destination failure flags, and run
`tools/verify_take.sh` on the disposable take. Check equal track lengths, gaps at
the correct timeline position, backup/mirror media, final report status and
save/undo behavior before relying on the new build for a live session.
