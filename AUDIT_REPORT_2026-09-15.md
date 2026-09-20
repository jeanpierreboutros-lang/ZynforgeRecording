# Whole-project audit — 2026-09-15

> Historical verification snapshot. For the current protocol-v3 build and the later 16-finding reliability pass, see [AUDIT_FIXES_2026-09-20.md](AUDIT_FIXES_2026-09-20.md).

## Outcome

The audit found **41 confirmed issues: 0 critical, 12 high, 26 medium and 3 low**. All 41 were fixed and each fix is covered by a focused unit/integration test, an invariant gate, or a build/package/CI verification gate. A fresh macOS-12 universal Release build passed **350 test groups with zero failures on arm64 and x86_64**, and the final ASan+UBSan Debug build passed 350/0. Xcode static analysis has no app-owned diagnostics. The 27-rule invariant audit and design audit are clean. GitHub's clean Debug/Release workflow and final native launch/render smoke test passed.

This is a software-validation result, not show certification. The exact DiGiCo SD5 + RME HDSPe AoX-D + 56-input + 48 kHz rig, a continuous three-hour rehearsal, device/clock loss, real backup drives, VoiceOver navigation and notarized distribution were not available. Application code through `df5ad36` is committed and pushed to `origin/main`; that exact bundle is installed at `/Applications/Zynforge Recording.app` with its matching helper and a recoverable prior-app backup.

## Expected behaviour established from the project

ZynForge Recording is a macOS live multitrack recorder and virtual-soundcheck player, not a general DAW. Its safety contract is: never overwrite an existing take or deliverable on a failed operation; keep channels sample-aligned; surface primary/backup/mirror failures; keep the capture daemon authoritative when selected; preserve sessions through interruption; keep network control authenticated and bounded; and keep expensive file work off the audio and message threads. See [README.md](README.md), [architecture.md](architecture.md), [testing.md](testing.md) and [SHOW-READINESS.md](SHOW-READINESS.md).

## High findings

### H1 — Stop & Quit could leave daemon recording

- **Affected:** quit/transport handling in `MainComponent`, capture supervisor/link.
- **Reproduce; expected/actual:** select daemon capture, start a take, then use Stop & Quit. Expected an acknowledged stop and finalized files; the GUI could exit while the daemon remained rolling.
- **Root cause:** the quit path stopped only the local recorder and trusted stale external status.
- **Fix:** route quit through the same acknowledged active-capture stop helper used by transport, and refuse to quit if stop/finalization fails.
- **Verification:** `MainTransportRegressionTests` exercises confirmed quit with a real daemon process.
- **Remaining:** physical device shutdown while CoreAudio is wedged still needs field testing.

### H2 — OSC and Companion recording bypassed daemon mode

- **Affected:** `AudioEngine::performRemoteTransport`, `OscRemote.cpp`, `CompanionServer.cpp`.
- **Reproduce; expected/actual:** enable daemon mode and start/stop from OSC or Companion. Expected commands to reach the daemon; they operated the idle GUI recorder instead.
- **Root cause:** remote adapters called local record/play methods directly.
- **Fix:** add one host interception boundary and route both adapters through it; daemon mode now fails closed after a disconnect.
- **Verification:** OSC, Companion and `MainTransportRegressionTests` cover remote stop, daemon disconnect and no-fallback behaviour.
- **Remaining:** real SD5 network traffic is not available; DiGiCo control remains read-only unless its handshake succeeds.

### H3 — Capture helper missing from distributed app

- **Affected:** `CMakeLists.txt`, `CaptureSupervisor`.
- **Reproduce; expected/actual:** copy the built `.app` to another location and enable daemon mode. Expected the matching helper in `Contents/MacOS`; it was emitted only as a separate build artefact.
- **Root cause:** no bundle dependency/copy step existed.
- **Fix:** make the GUI depend on `ZynforgeCapture` and embed the helper in every macOS app bundle.
- **Verification:** capture-supervisor bundle test and direct bundle inspection find the executable.
- **Remaining:** GUI and helper must always be deployed together because capture protocol version 2 is not backward compatible.

### H4 — Queued engine callbacks could use freed objects

- **Affected:** asynchronous OSC/network-to-engine callbacks and shutdown.
- **Reproduce; expected/actual:** queue a remote action and immediately destroy the engine/window. Expected cancellation; a queued raw pointer could execute after destruction.
- **Root cause:** callbacks captured naked owner pointers with no invalidation token.
- **Fix:** callbacks capture a shared invalidatable engine handle, cleared before teardown; UI callbacks use `SafePointer`.
- **Verification:** OSC teardown regression plus invariant checks for queued engine/UI callbacks.
- **Remaining:** ThreadSanitizer was not available; ASan found no lifetime errors in the exercised paths.

### H5 — Track transactions could escape through symlinks

- **Affected:** `PathSafety.h`, `TrackFileTransaction.h`, reorder/delete operations.
- **Reproduce; expected/actual:** place a session or track path behind a symlink resolving outside the session and reorder tracks. Expected refusal; lexical containment allowed external files into the transaction.
- **Root cause:** paths were compared without resolving filesystem aliases.
- **Fix:** canonicalize both directions, reject escaping aliases, journal moves, and roll back failed installs.
- **Verification:** symlink-alias, failed-install and interrupted-transaction regressions.
- **Remaining:** forced power loss during an actual multi-drive rename still needs disposable-hardware validation.

### H6 — Save As could recurse or copy private external files

- **Affected:** `MainComponentSessionIO.cpp` session copy.
- **Reproduce; expected/actual:** choose a destination that resolves inside the source, or include a symlink in the source. Expected refusal; traversal could recurse indefinitely or copy data outside the session.
- **Root cause:** lexical child checks and recursive copying followed aliases.
- **Fix:** canonical containment checks, explicit symlink refusal, cancellable background copy and cleanup of partial destinations.
- **Verification:** path-safety regressions and the session-copy invariant.
- **Remaining:** network volumes with changing mount aliases need manual testing.

### H7 — Active-session operations could target an older player session

- **Affected:** `AudioEngine` active-session resolution and session load.
- **Reproduce; expected/actual:** change/create a session while the player still holds an older directory, then save/export. Expected the explicitly active folder; lookup could return the older player folder.
- **Root cause:** active identity was inferred from recorder/player state instead of being authoritative.
- **Fix:** pin and persist `activeSession`, prefer it in lookup, and set it during every load/creation boundary.
- **Verification:** session-integrity and active-directory regressions.
- **Remaining:** manual Finder double-click/open-state UI was not automatable in this environment.

### H8 — Failed track export could erase an existing deliverable

- **Affected:** `TrackExporter.cpp` WAV/AIFF/FLAC/MP3 output.
- **Reproduce; expected/actual:** export over an existing file and force writer/encoder failure. Expected the old file intact; it was deleted before success.
- **Root cause:** final paths were opened directly.
- **Fix:** render/encode to unique sibling files and atomically install only after success; remove partials on failure/cancel.
- **Verification:** invalid-format regression preserves a sentinel existing export and checks no partial remains.
- **Remaining:** genuine disk-full behaviour should also be exercised on a disposable volume.

### H9 — Failed bounce could erase an existing mix or stem

- **Affected:** `AudioEngineClips.cpp` bounce helpers.
- **Reproduce; expected/actual:** cancel or fail a bounce over an existing WAV. Expected the previous bounce; direct creation replaced it first.
- **Root cause:** render destination and published destination were the same file.
- **Fix:** bounce to unique partial WAVs and install atomically on completion.
- **Verification:** cancellation regression preserves the sentinel file; arrangement/mix/stereo bounce tests pass.
- **Remaining:** multi-hour bounce cancellation was simulated, not manually observed in the UI.

### H10 — Session metadata writes could truncate last-known-good state

- **Affected:** session/project/mix/cue/report/journal/settings text writers across UI, audio, console and recorder code.
- **Reproduce; expected/actual:** interrupt or fail a save after the target is opened. Expected the prior JSON/project; `replaceWithText` could truncate it first.
- **Root cause:** direct replacement and JUCE moves that delete the target before rename were treated as atomic.
- **Fix:** central unique-sibling writer using same-volume `replaceFileIn`; migrate all production persistent text writes.
- **Verification:** eight concurrent large writers never publish torn text or collide; invariant forbids direct production `replaceWithText`.
- **Remaining:** filesystem-level durability after sudden power loss depends on the volume; no explicit `fsync` contract is provided by JUCE.

### H11 — Built app bundle had an invalid resource signature

- **Affected:** macOS packaging in `CMakeLists.txt`.
- **Reproduce; expected/actual:** build with Unix Makefiles, then run `codesign --verify --deep --strict`. Expected a valid local bundle; resources were unsealed and the copied helper invalidated the linker signature.
- **Root cause:** no final bundle sealing step after helper embedding.
- **Fix:** ad-hoc sign the helper and then the complete bundle after copying; Xcode/release signing may replace it later.
- **Verification:** strict/deep verification now reports “valid on disk” and validates the helper; CI retains this gate.
- **Remaining:** ad-hoc signing is not Developer ID signing or notarization, so Gatekeeper distribution assessment still fails.

### H12 — Clean Xcode builds targeted the host OS instead of the documented minimum

- **Affected:** `CMakeLists.txt`, platform-support documentation and clean Xcode builds.
- **Reproduce; expected/actual:** configure from an empty build directory, build, then inspect `MACOSX_DEPLOYMENT_TARGET`. Expected the documented minimum; toolchain settings assigned after `project()` were too late, so the universal binary linked for the host OS (26.6). An older cached 11.0 project also failed immediately under Xcode 27, whose supported floor is 12.0.
- **Root cause:** the deployment target and architectures were initialized after CMake enabled its compilers, and the declared 11.0 floor was obsolete for Xcode 27.
- **Fix:** set macOS toolchain variables before `project()`, raise the supported floor to 12.0 and update compatibility documentation.
- **Verification:** the invariant gate enforces both value and ordering; a brand-new Xcode project is inspected for `MACOSX_DEPLOYMENT_TARGET = 12.0`, then built/tested as arm64+x86_64.
- **Remaining:** macOS 11 is no longer supported. Retaining it would require a separately maintained older Xcode/SDK release pipeline.

## Medium findings

### M1 — Engine-reference member lifetime ordering was unsafe

- **Affected:** `MainComponent.h` members including `SessionMirror`, `ConsoleLink` and `CaptureSupervisor`.
- **Reproduce; expected/actual:** construct/destroy the main view. Expected reference-owning services to live strictly inside `AudioEngine`; declaration order could bind before or outlive it.
- **Root cause:** C++ constructs/destroys members by declaration order, not initializer-list order.
- **Fix:** reorder members so every retained engine reference is nested in the engine lifetime.
- **Verification:** construction/destruction tests plus a declaration-order invariant.
- **Remaining:** none observed under ASan.

### M2 — Timeline CSV allowed spreadsheet formula injection

- **Affected:** `TimelineExport.h`.
- **Reproduce; expected/actual:** export a track/cue name beginning with `=`, `+`, `-`, `@`, tab or newline and open in a spreadsheet. Expected text; it could execute as a formula/directive.
- **Root cause:** CSV quoting does not neutralize formula prefixes.
- **Fix:** prefix dangerous fields with an apostrophe before RFC-style quoting.
- **Verification:** dedicated values including `WEBSERVICE` and command-style payloads.
- **Remaining:** consumers that deliberately strip the apostrophe can re-enable formulas.

### M3 — Preflight reported misleading storage/backup state

- **Affected:** preflight probes and dialog.
- **Reproduce; expected/actual:** run preflight with no active session or a configured-but-idle backup. Expected configured storage and truthful state; labels implied active writes or probed the wrong root.
- **Root cause:** active-take state and configured destination were conflated.
- **Fix:** resolve configured storage while idle and distinguish configured from actively writing.
- **Verification:** preflight storage-root and backup-readiness tests.
- **Remaining:** actual drive speed/SMART status varies and must be measured on show drives.

### M4 — Companion shutdown could block for five seconds

- **Affected:** `CompanionServer` pending transport commands.
- **Reproduce; expected/actual:** issue a command queued to the UI and stop the server before it runs. Expected prompt shutdown; worker waited its full command timeout.
- **Root cause:** pending commands had no cancellation completion.
- **Fix:** register pending results and complete them as cancelled during stop before joining workers.
- **Verification:** stop-with-queued-command integration test requires under 1.5 seconds.
- **Remaining:** OS-level socket teardown timing on sleeping Wi-Fi clients remains platform dependent.

### M5 — Component-owned asynchronous callbacks could outlive UI

- **Affected:** menus, choosers, alerts and delayed actions in UI files.
- **Reproduce; expected/actual:** open an asynchronous dialog/action and close the window immediately. Expected the completion to no-op; raw `this` captures could dereference freed components.
- **Root cause:** callback lifetime was independent of its visual owner.
- **Fix:** use `juce::Component::SafePointer` at every independent UI callback boundary.
- **Verification:** invariant scans asynchronous component callbacks; ASan suite passes.
- **Remaining:** native UI automation service was unavailable, so every modal race was not clicked manually.

### M6 — Recovery could claim removal when marker deletion failed

- **Affected:** `SessionRecoveryDialog.h`.
- **Reproduce; expected/actual:** make a recovery marker undeletable and dismiss it. Expected an error; the UI removed the row regardless, causing it to reappear later.
- **Root cause:** delete return values were ignored.
- **Fix:** remove UI state only after successful filesystem deletion and surface failure.
- **Verification:** recovery/file-failure tests and source-path review.
- **Remaining:** immutable/ACL-protected volumes need manual UI verification.

### M7 — Cloud uploader path/pipe handling could fail silently

- **Affected:** `CloudUpload.h`.
- **Reproduce; expected/actual:** use a session path containing spaces/quotes or an uploader that writes substantial output. Expected exact argv and detached execution; literal quotes or closed inherited pipes could break/SIGPIPE the uploader.
- **Root cause:** string-command parsing preserved quote characters and default child pipes outlived their reader.
- **Fix:** tokenize/unquote first, substitute the session path as inert argv data, and launch without captured pipes.
- **Verification:** process-wrapper test receives the exact path including quotes and remains alive.
- **Remaining:** uploader success is still external; launch success does not prove remote completion.

### M8 — Daemon stop could report success after metadata-finalization failure

- **Affected:** recorder stop result, capture protocol and main transport.
- **Reproduce; expected/actual:** obstruct `session.report.json`, then stop daemon capture. Expected an explicit incomplete-finalization error; stop could be acknowledged as success.
- **Root cause:** audio closure and metadata persistence were collapsed into a void/success path.
- **Fix:** propagate finalization status through daemon reply and refuse false-success UI state.
- **Verification:** daemon finalization-failure regression.
- **Remaining:** multitrack audio may be usable even when metadata fails; the operator must inspect/report the incomplete stop.

### M9 — `loadSession` did not pin the loaded directory

- **Affected:** `AudioEngine::loadSession`.
- **Reproduce; expected/actual:** call load directly then save/query active session. Expected the loaded directory; active state could remain empty/stale.
- **Root cause:** callers were expected to set active state separately.
- **Fix:** make load establish the active directory itself.
- **Verification:** session lookup/load regression.
- **Remaining:** none observed.

### M10 — Duplicate FLAC registration crashed Debug builds

- **Affected:** `TrackExporter` constructor.
- **Reproduce; expected/actual:** construct an exporter in Debug. Expected one FLAC handler; `registerBasicFormats` plus explicit FLAC registration asserted and duplicated it in Release.
- **Root cause:** JUCE basic formats already include FLAC when enabled.
- **Fix:** remove the duplicate registration.
- **Verification:** exporter-construction and FLAC export regression under assertion-enabled ASan Debug.
- **Remaining:** none.

### M11 — Damaged multipart input triggered Debug assertions

- **Affected:** multipart discovery/reader.
- **Reproduce; expected/actual:** open a take missing a middle continuation part. Expected refusal; inconsistent input reached a JUCE assertion.
- **Root cause:** discovery assumed a contiguous sequence after sorting.
- **Fix:** validate numbering/completeness before constructing the concatenated reader.
- **Verification:** missing-middle-part regression returns failure without crashing.
- **Remaining:** damaged audio payloads beyond container-reader validation depend on JUCE codecs.

### M12 — CI hid assertion-only failures and supply/path mistakes

- **Affected:** `.github/workflows/ci.yml`, JUCE dependency declaration.
- **Reproduce; expected/actual:** introduce a JUCE assertion or case-sensitive path error. Expected CI failure; Release-only coverage could pass and path casing differed by filesystem.
- **Root cause:** only optimized tests ran; dependency/action references were mutable or weakly checked.
- **Fix:** run Debug and Release, use exact paths, pin JUCE and GitHub actions to immutable commits, verify bundled helper/signature.
- **Verification:** workflow review and local equivalent Debug/Release runs.
- **Remaining:** CI currently covers the hosted macOS-14/Xcode 15.4 environment; local Xcode 27 validation supplements it, but every intermediate Xcode release is not separately tested.

### M13 — Background exports could overlap session transitions

- **Affected:** `MainComponentSessionIO.cpp` export/bounce gates.
- **Reproduce; expected/actual:** start export, then Save As/switch session/start another export. Expected mutual exclusion; operations could race files or block the UI while joining a long encode.
- **Root cause:** export did not hold the shared session-I/O busy state for its whole lifetime.
- **Fix:** acquire/clear one exclusion gate and join owned cancellable workers.
- **Verification:** source invariant and export cancellation/completion regressions.
- **Remaining:** native progress/cancel interaction was not automatable.

### M14 — Audio import froze the UI and left partial media

- **Affected:** new `AudioImport` worker and import UI path.
- **Reproduce; expected/actual:** import long/mixed-rate files or cancel mid-import. Expected responsive UI and all-or-nothing per destination; decoding/resampling ran inline and wrote final names directly.
- **Root cause:** no worker boundary or transactional destination.
- **Fix:** owned background worker, cancellation, sample-rate conversion, unique partial files, collision refusal and atomic install.
- **Verification:** mono/stereo/mixed-rate, unreadable, collision and pre-cancel regressions.
- **Remaining:** `.m4a` availability depends on codecs JUCE can open on the target macOS.

### M15 — Bundled FLAC encoder invoked undefined behaviour

- **Affected:** pinned JUCE 8.0.4 libFLAC source via `CMakeLists.txt`.
- **Reproduce; expected/actual:** write FLAC under UBSan. Expected no UB; the bundled encoder performed pointer arithmetic on null when escape coding was disabled.
- **Root cause:** upstream third-party defect in the pinned revision.
- **Fix:** deterministic configure-time application of the current upstream null guard; fail configuration if source no longer matches expected old/fixed forms.
- **Verification:** FLAC tests pass with UBSan `halt_on_error=1`.
- **Remaining:** remove the patch only after a deliberate JUCE/FLAC upgrade and rerun sanitizers.

### M16 — Live WAV placeholder overflowed 32-bit RIFF fields

- **Affected:** `CompanionStreamFormat.h`, streaming header.
- **Reproduce; expected/actual:** request stream at high sample rates. Expected valid bounded RIFF sizes; signed 32-bit arithmetic could overflow into invalid fields.
- **Root cause:** duration/rate multiplication used an insufficient signed type and did not reserve RIFF header bytes.
- **Fix:** checked 64-bit calculation clamped to the maximum aligned data size.
- **Verification:** 48 kHz, 384 kHz and tiny-duration boundary tests.
- **Remaining:** stream is deliberately finite-size RIFF metadata over a live connection, not an RF64 endpoint.

### M17 — Click-track generation blocked UI and could destroy the previous click

- **Affected:** new `ClickTrackRenderer`, `MainComponentTools.cpp`.
- **Reproduce; expected/actual:** generate a show-length click, cancel/fail, or place a tempo change off the render block boundary. Expected responsive UI, exact tempo boundary and old click preservation; rendering blocked, removed the prior file early and changed tempo only per 32,768-sample block.
- **Root cause:** synchronous rendering to the final path with coarse tempo polling and unchecked writes.
- **Fix:** cancellable owned worker, exact change segmentation, writer-result checks, transactional install, three-hour empty-session fallback and session-aware completion.
- **Verification:** success, cancellation and invalid-request click-render tests; exact-boundary logic exercised in renderer tests.
- **Remaining:** very long/high-rate click files should be DAW-opened during rehearsal.

### M18 — Session scaffold could omit or choose the wrong `.zfproj`

- **Affected:** `SessionProjPath.h`, `ensureSessionScaffold`, session properties/cues.
- **Reproduce; expected/actual:** create a new destination with no project, or several legacy projects. Expected canonical `<folder>.zfproj`; helper returned a plausible path even when absent, causing scaffold to return early, and fallback order was nondeterministic.
- **Root cause:** path value was mistaken for file existence and unsorted directory results were used.
- **Fix:** test `existsAsFile`, prefer the canonical project, sort legacy fallback candidates, and share the resolver.
- **Verification:** missing/canonical/legacy project regression.
- **Remaining:** conflicting legacy projects still require the deterministic first fallback; they are not merged.

### M19 — Optional live stereo mix was unreachable and write failures were silent

- **Affected:** recording/settings dialog, `AudioEngine` stereo-mix writer.
- **Reproduce; expected/actual:** try to enable the documented option, or obstruct `Export Files`. Expected a control and visible failure; no UI setter existed and FIFO/write failures were ignored.
- **Root cause:** an internal flag was never connected and return values were discarded.
- **Fix:** add a recording-dialog toggle, expose setter, track open/FIFO failures, warn live and make stop report incomplete mix while preserving multitracks.
- **Verification:** optional-mix failure regression plus invariant for control/failure visibility.
- **Remaining:** stereo mix is convenience redundancy, not a substitute for multitracks or an independent recorder.

### M20 — Atomic writer asserted when staging creation failed

- **Affected:** `AtomicFile.h`.
- **Reproduce; expected/actual:** write to a path whose staging filename cannot be created. Expected `false`; JUCE `replaceWithText` asserted because its hidden temporary did not exist.
- **Root cause:** the outer atomic helper used another opaque temporary-writing API internally.
- **Fix:** open/write/flush/check the unique staging stream explicitly, then install only when it exists.
- **Verification:** overlong-component regression returns false with no target or debris; full Debug suite no longer traps.
- **Remaining:** callers must continue surfacing the returned failure.

### M21 — Companion streaming test treated binary WAV as UTF-8

- **Affected:** `CompanionServerTests.cpp`.
- **Reproduce; expected/actual:** run assertion-enabled tests when RIFF size bytes are invalid UTF-8. Expected the test to inspect headers/bytes; it asserted in `String::fromUTF8`, hiding later tests.
- **Root cause:** raw binary HTTP response was converted wholesale to text.
- **Fix:** project response bytes safely to ASCII for textual assertions while preserving tags.
- **Verification:** the previously crashing token/stream integration test passes in Debug and Release.
- **Remaining:** this was a test defect; browser playback still needs a real-device/manual check.

### M22 — MP3 encoder discovery could hang indefinitely

- **Affected:** `TrackExporter::findLameBinary`, new `ProcessSearch.h`.
- **Reproduce; expected/actual:** remove standard `lame` paths and make path lookup stall. Expected bounded “lame not found”; code ignored `which` timeout then called blocking `readAllProcessOutput`.
- **Root cause:** timeout result was discarded.
- **Fix:** eliminate the subprocess and search PATH entries directly.
- **Verification:** deterministic multi-directory/path-with-spaces lookup regression.
- **Remaining:** the found file is checked for existence; actual executability/failure is reported by the encoder launch.

### M23 — Companion parser decoded hostile bytes before authentication

- **Affected:** `CompanionServer.cpp` request reader.
- **Reproduce; expected/actual:** send an unauthenticated POST containing invalid UTF-8 or a split multibyte body. Expected early rejection and continued service; per-chunk conversion could assert in Debug, and oversized lengths were truncated rather than rejected.
- **Root cause:** text decoding occurred before complete bounded byte framing/validation.
- **Fix:** enforce ASCII headers, reject duplicate/malformed/oversized `Content-Length`, buffer exactly the bounded body, validate UTF-8 once, then decode.
- **Verification:** raw invalid-byte request followed by an authenticated state request proves the server survives; complete Companion suite passes.
- **Remaining:** this is a deliberately small HTTP/1.1 subset; chunked request bodies are unsupported.

### M24 — Regression tests could crash after a failed precondition

- **Affected:** `AutomationTests.cpp`, `EngineStateTests.cpp`.
- **Reproduce; expected/actual:** make JSON parsing or test-property construction fail. Expected a precise failed expectation and continued report; JUCE expectations are non-fatal, so the next line dereferenced the null pointer and crashed the test executable.
- **Root cause:** tests treated `expect` as an aborting assertion.
- **Fix:** validate every pointer/shape and execute dependent checks only when the precondition holds.
- **Verification:** Xcode static analysis no longer reports the two null-call paths; complete Debug and Release suites pass.
- **Remaining:** test helpers must continue to distinguish JUCE's non-fatal expectations from fatal assertions.

### M25 — Scope-guard syntax failed on the supported CI compiler

- **Affected:** `AtomicFile.h`, `AudioImport.cpp`, `ClickTrackRenderer.cpp`; macOS-14/Xcode 15.4 CI.
- **Reproduce; expected/actual:** push the locally clean Xcode 27 build to GitHub. Expected the supported CI compiler to build it; Xcode 15.4 stopped at all three `juce::ScopeGuard(...)` sites with “no matching constructor”.
- **Root cause:** parenthesized aggregate initialization accepted by the newer local compiler was not accepted by the older supported runner compiler.
- **Fix:** use standard brace aggregate initialization at every scope-guard site.
- **Verification:** the universal Release rebuild and both architecture test runs remained 350/0; [GitHub run 34986170067](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/34986170067) passed clean Debug and Release builds/tests on Xcode 15.4.
- **Remaining:** compiler coverage is Xcode 15.4 and 27, not every intermediate Xcode release.

### M26 — CI action runtimes were deprecated

- **Affected:** `.github/workflows/ci.yml`.
- **Reproduce; expected/actual:** inspect the otherwise-green GitHub workflow annotations. Expected a warning-free supported runner configuration; checkout, cache and artifact-upload v4 pins targeted deprecated Node 20 and were being force-run on Node 24.
- **Root cause:** immutable action pins were secure against tag movement but had not advanced with the actions' supported runtime lines.
- **Fix:** resolve the current official releases and pin checkout v7.0.1, cache v6.1.0 and upload-artifact v7.0.1 to their exact commits.
- **Verification:** the final GitHub workflow rerun passes its gates, clean Debug/Release builds and tests, helper/signature verification and artifact upload without the Node 20 annotation.
- **Remaining:** pinned action releases still require deliberate periodic review; Dependabot is not configured for Actions updates.

## Low findings

### L1 — Zero persisted through an ambiguous null-like overload

- **Affected:** one `PropertiesFile::setValue("stripCount", 0)` call.
- **Reproduce; expected/actual:** compile with strict warnings or inspect overload resolution. Expected integer zero; the literal could select/appear as a null pointer overload.
- **Root cause:** untyped zero literal.
- **Fix:** wrap it as `juce::var(0)`.
- **Verification:** rebuilt Debug and Release; warning removed at that call.
- **Remaining:** other non-actionable conversion/shadow/deprecation warnings remain technical debt.

### L2 — Oversized AAF properties crashed Debug before controlled failure

- **Affected:** `AafProperties.h` experimental AAF primitives.
- **Reproduce; expected/actual:** serialize more than 65,535 properties or one value larger than 65,535 bytes. Expected `std::length_error`; Debug hit `jassertfalse` first.
- **Root cause:** assertions were used for externally representable size-limit errors.
- **Fix:** return the same controlled exception in Debug and Release.
- **Verification:** both 16-bit limits have regression tests.
- **Remaining:** AAF export is unfinished and internally round-trip-tested only; compatibility with Pro Tools/pyaaf2 is explicitly unverified.

### L3 — Dead code obscured automation and recorder checks

- **Affected:** `AudioEngineAutomation.cpp`, `AudioCallbackTests.cpp`.
- **Reproduce; expected/actual:** run Xcode analysis and strict compilation. Expected each computed value/capture to feed behaviour; the curve variable was initialized and always overwritten/returned, while tests stored an unchecked frame count and retained an unused lambda capture.
- **Root cause:** residue from earlier implementations.
- **Fix:** remove the unused initialization, test variable and capture while preserving explicit assignment on every live curve branch.
- **Verification:** curve/pre-roll regressions pass and the project-owned dead-store findings disappear from static analysis.
- **Remaining:** ordinary third-party JUCE/libFLAC/HarfBuzz/SheenBidi analyzer warnings remain upstream noise.

## Verification performed

- Clean arm64 Release configure/build using Unix Makefiles and the pinned JUCE 8.0.4 commit.
- Brand-new Xcode 27 project configured without command-line architecture/deployment overrides; generated settings and Mach-O load commands both report macOS 12.0, and the app plus capture helper are universal arm64+x86_64.
- Universal Release app: **350 groups, 0 failures, exit 0 on arm64** and **350/0, exit 0 on x86_64 under Rosetta**.
- ASan+UBSan Debug (`detect_leaks=0`, unsupported on this macOS): **350 groups, 0 failures, exit 0**; includes 48 channels × 8 seconds and 64-channel zero-missed-sample capture tests.
- Xcode arm64 Debug static analysis: succeeds with no app-owned diagnostics; 15 unique diagnostics remain in vendored JUCE/libFLAC/HarfBuzz/SheenBidi code.
- `tools/invariants_audit.sh`: **27 checks, clean**.
- `tools/design_audit.sh`: clean; spacing ratchet remains 169.
- `codesign --verify --deep --strict`: passes after final bundle sealing; embedded `ZynforgeCapture` validates.
- Gatekeeper `spctl` assessment rejects the ad-hoc build, as expected without Developer ID signing/notarization; this is not presented as a distributable release.
- Native smoke: the final universal app launched, stayed alive, and New Session rendered correctly with 48 kHz selected; no fresh crash report appeared. The computer-control accessibility service failed to start, so deeper automated native clicks were unavailable.
- Source/repository scans: no production direct persistent `replaceWithText`, no remaining `jassertfalse`, no TODO/FIXME/HACK markers, no tracked common-format private keys/tokens, and risky process/network/thread/file paths were reviewed.
- CI's three action pins were resolved against their exact upstream GitHub commits; `git diff --check` is clean.
- [GitHub run 34986170067](https://github.com/jeanpierreboutros-lang/ZynforgeRecording/actions/runs/34986170067) passed clean Debug and Release builds/tests plus bundled-helper verification on macOS-14/Xcode 15.4.
- The installed bundle is byte-for-byte identical to the verified local build; its main/helper hashes match, both binaries are universal with a macOS 12.0 minimum, deep/strict signature verification passes, the installed executable passes 350/0, and its native New Session UI launched without a new crash report.

## Checks not completed

- Static/security scanners `clang-tidy`, `cppcheck`, `semgrep`, `scan-build` and `osv-scanner`: not installed.
- LeakSanitizer: unsupported by the platform runtime; ASan and UBSan were used.
- Developer ID signing, notarization and Gatekeeper distribution acceptance: no certificate/profile was supplied. The bundle is validly ad-hoc signed only and `spctl` rejects it.
- Exact-rig acceptance: no SD5, AoX-D, selected MADI/Dante path, show Mac/chassis, primary/backup drives or three-hour window was available.

## Show decision

The installed build is suitable for the **exact-rig rehearsal**, not yet suitable as the only recorder at a show. For 56 mono channels at 48 kHz/24-bit, raw audio is approximately **58.06 GB for two hours per destination**; plan at least 70 GB usable headroom per primary/backup destination for the show, and the project’s safer recommendation remains 150 GB free per drive for a three-hour acceptance rehearsal plus soundcheck/overrun. Follow [SHOW-READINESS.md](SHOW-READINESS.md) and retain a genuinely independent recorder.
