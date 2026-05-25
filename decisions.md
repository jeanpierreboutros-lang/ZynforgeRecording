# Decisions Log

This file records key technical and product decisions for ZynForge Recording as lightweight Architecture Decision Records (ADRs). Each entry captures the context, decision, rationale, and consequences so future-you (or another engineer joining the project) can understand *why* something is the way it is — not just *what* it is.

When making a non-trivial decision, add a new entry below using the template at the bottom. Keep entries short and concrete. Link related ADRs by title rather than by anchor — anchors rot when sections are reordered.

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
**Alternatives Considered:** Per-session password (engineer-set) -- rejected for friction. WebSocket auth handshake -- rejected as over-engineering for an HTTP polling client. TLS / HTTPS -- still pending; the token closes the casual-attack gap and TLS would close the on-path-attacker gap.
**Related Documents:** `Source/Network/CompanionServer.{h,cpp}`, `AudioEngine::startCompanionServer{,OnLan}`.

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

**Status:** Accepted
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
