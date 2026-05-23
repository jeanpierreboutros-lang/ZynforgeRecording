# ZynForge Recording — Claude operating notes

## Project

JUCE 8 / C++20 / CMake, macOS-first (Universal) multitrack recording + playback application with virtual soundcheck. Sibling to the ZynForge Live plugin-insert host.

**Stack:** JUCE 8.0.4 (via CMake FetchContent), C++20, CMake (Xcode generator), Apple Silicon + Intel universal build, macOS 11.0 deployment target. Embedded HTTP companion server (`StreamingSocket`). Bundled fonts: Inter + JetBrains Mono. NDI via runtime `dlopen`.

## Workflow rules — non-negotiable

1. **After every code change**, update `CLAUDE.md` + `README.md` to reflect what's actually in the codebase.
2. **Build** with `cmake --build build --config Release` after every change. Don't claim success without a successful build.
3. **Smoke-test** by launching the app from `build/ZynforgeRecording_artefacts/Release/`. Watch RSS, CPU, and `~/Library/Logs/DiagnosticReports/Zynforge*.ips`.
4. **Commit and push to `origin/main`** after every change. Auto-push is the policy — no asking.
5. **Do not credit** Harrison LiveTrax / Waves Tracks Live as inspiration anywhere — not in code, comments, docs, or commit messages.
6. **This is a recorder + virtual soundcheck**, not a mixer. No plugins. No talkback. No in-the-box effects.
7. **MIXER / EDIT / PATCH views are linked.** A change in one must reflect in the other two. Stereo pairs collapse to one logical strip in all three views.

## Documentation & References

Read the relevant file(s) before starting implementation work when the topic matches:

- **`architecture.md`** — Technical architecture, component map, real-time threading model, data flow, build / boot sequence.
- **`DESIGN.md`** — Product requirements, user experience, brand identity, visual design system rationale.
- **`tasks.md`** — Current priorities and active work. **Check first** for context on ongoing initiatives before starting anything substantial.
- **`decisions.md`** — Architecture Decision Records. Read before making structural or product-positioning choices.
- **`coding-standards.md`** — Naming, formatting, real-time safety patterns, JUCE conventions, brand-token rules.
- **`testing.md`** — Build / smoke-test / field validation procedure. What to verify per change type.
- **`CHANGELOG.md`** — User-visible history. Add an entry on any shipped change.

For ad-hoc architecture questions, the source of truth is the code itself — start from `Source/Audio/AudioEngine.{h,cpp}` and `Source/UI/MainComponent.{h,cpp}`.

## Critical universal rules

### Real-time discipline (audio thread)

- The audio callback never allocates, locks, opens files, calls `Logger`, or touches the message thread.
- State transitions happen via `std::atomic` flags. Writers + readers are constructed / destroyed only on the message or background threads.
- Scratch buffers (`outputAccum`, `playerScratch`, companion ring) pre-allocate in `audioDeviceAboutToStart`.

### Brand + design system

- Colours come from `Source/Theme/BrandColors.h`. Never use raw `juce::Colour::fromRGB` or `juce::Colours::black/white` for chrome.
- Fonts come from `Source/Theme/BrandTokens.h` via `brand::type::*`. Never construct a raw `juce::Font`.
- Dialog modals use `Source/Theme/DialogChrome.h::dialog::paintChrome(...)`. Custom dialog `paint()` is a smell.
- Shadows: `brand::shadow::elev1/elev2/elev3`. Never inline `Colours::black.withAlpha(...)`.
- Text on a saturated accent: `brand::onSignal(bg)`. Never hardcode black or white.

### Stereo + view linkage

- A stereo pair is two adjacent physical tracks with `isStereo=true` on the L track. Every view (MIXER, EDIT, PATCH) must collapse the pair into one logical entry.
- Mute / solo / arm / gain / pan / colour / name changes mirror across the pair automatically — never half-update.

### Persistence

- Anything the user can edit (track names, colours, routing, gains, stereo pairs, takes, cues, time signature) must round-trip through `.zfproj` or `appProps`. RAM-only state is a data-loss bug.
- `.zfproj` carries a `formatVersion` field. Treat its absence as v1 and fall back gracefully.

### Stable identity

- Cue snapshots, automation references, anything that must survive a strip reorder uses `TrackState::stripId` (a `juce::Uuid`), never an array index.

## Build + smoke test recipe

```bash
cmake -B build -G Xcode                                                  # first time only
cmake --build build --config Release                                     # every change
open "build/ZynforgeRecording_artefacts/Release/Zynforge Recording.app"  # smoke-test
ps -p $(pgrep -f "Zynforge Recording") -o pid,rss,etime,%cpu             # ~49 MB / <1% at idle
ls -lt ~/Library/Logs/DiagnosticReports/Zynforge*.ips | head -1          # check no new crash
git add -A && git commit -m "..." && git push origin main                # ship
```

## Sibling project

ZynForge Live (plugin-insert host) lives elsewhere. They share the visual identity (`Source/Theme/`) but not code. Cross-pollinate brand changes; do not cross-pollinate features.

## Proposing documentation updates

When implementation surfaces information that should outlive this conversation, update the right doc and include the update in the same commit as the code change:

- **Architecture changes** (new component, threading model shift, removed module) → `architecture.md`.
- **Product / UX direction changes** → `DESIGN.md`.
- **Sprint priorities, completed work, new TODOs** → `tasks.md` at the end of every productive session.
- **Decisions whose *why* future-you will want** → add an ADR to `decisions.md`.
- **Conventions adopted that aren't yet documented** → `coding-standards.md`.
- **Test gaps spotted or new validation steps** → `testing.md`.
- **Anything user-visible** → entry under `## [Unreleased]` in `CHANGELOG.md`.

When a doc and code change disagree, the code is authoritative — but the disagreement is itself a doc bug. Fix it.
