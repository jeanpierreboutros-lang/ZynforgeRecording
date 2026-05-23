# Decisions Log

This file records key technical and product decisions for ZynForge Recording as lightweight Architecture Decision Records (ADRs). Each entry captures the context, decision, rationale, and consequences so future-you (or another engineer joining the project) can understand *why* something is the way it is — not just *what* it is.

When making a non-trivial decision, add a new entry below using the template at the bottom. Keep entries short and concrete. Link related ADRs by title rather than by anchor — anchors rot when sections are reordered.

---

## No plugin hosting — 2026-04-12

**Status:** Accepted
**Context:** Early users asked whether ZynForge Recording should host AU / VST plugins for offline mixdown or VSC tone shaping. The decision had to be made before the routing graph was architected.
**Decision:** ZynForge Recording does not host plugins. The audio callback's routing graph is fixed: input → recorder, file → player → optional clip gain → mute/solo/VCA/aux → master.
**Rationale:** Hosting plugins forces the engine to support arbitrary parameter automation, plugin sidecar processes, plugin crash recovery, and a per-plugin UI surface. That is a different product (a DAW). The product positioning is *live recorder + virtual soundcheck*, and the engineer brings their own console for tone shaping. Keeping the engine plugin-free preserves real-time safety guarantees and keeps the live-show attack surface small.
**Consequences:** Cannot offer in-the-box EQ / compression. No automation lanes. No third-party effect ecosystem. Engineers expecting DAW-like behaviour will need to be told this upfront in `DESIGN.md`.
**Alternatives Considered:** (a) Host AU only, gated by an "expert mode" flag — rejected; the support burden is identical to full hosting. (b) Build a tiny first-party effect chain (HPF + gain trim) — rejected as scope creep.
**Related Documents:** `DESIGN.md`, `architecture.md` §7.

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
