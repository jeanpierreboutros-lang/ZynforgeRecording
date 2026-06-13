# Decisions Log

This file records key technical and product decisions for ZynForge Recording as lightweight Architecture Decision Records (ADRs). Each entry captures the context, decision, rationale, and consequences so future-you (or another engineer joining the project) can understand *why* something is the way it is — not just *what* it is.

When making a non-trivial decision, add a new entry below using the template at the bottom. Keep entries short and concrete. Link related ADRs by title rather than by anchor — anchors rot when sections are reordered.

---

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
