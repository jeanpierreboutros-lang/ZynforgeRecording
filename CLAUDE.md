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
- **`design.md`** — Product requirements, user experience, brand identity, visual design system rationale.
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
- Specular gloss / light scrim (the one sanctioned white): `brand::gloss(alpha)`. Never inline `Colours::white.withAlpha(...)`.
- Alpha: `brand::alpha::{subtle,dimmed,ghost,scrim,muted,prominent,bold}`. Never inline `withAlpha(0.xx)`; if no step fits, catalog it in `BrandColors.h`.
- Corner radius: `brand::radius::{sm,md,lg,xl}`. Never pass a raw float (sub-2 px meter/icon micro-radii excepted).
- Text on a saturated accent: `brand::onSignal(bg)`. Never hardcode black or white.

### Stereo + view linkage

- A stereo pair is two adjacent physical tracks with `isStereo=true` on the L track. Every view (MIXER, EDIT, PATCH) must collapse the pair into one logical entry.
- Mute / solo / arm / gain / pan / colour / name / VCA / edit-group changes mirror across the pair automatically — never half-update.
- VCA gain + mute on a stereo pair: the R partner reads the L track's lane (in the audio callback) so both halves follow the curve. Pan stays per-channel so the stereo image isn't collapsed.

### Persistence

- Anything the user can edit (track names, colours, routing, gains, stereo pairs, takes, cues, time signature, automation, VCA + edit-group assignments, markers, tempo) must round-trip through one of the per-session files. RAM-only state is a data-loss bug.
- Per-session files live under the session folder:
  - **`session_mix.json`** — the engine writes the full per-strip mix state (name, colour, gain, pan, mute, solo, monitor, arm, routing, stereo, VCA + edit group) plus session-level tempo (`tempoBpm`, `timeSigNum`, `timeSigDenom`, `tempoMap`). Loaded on Open; size the recorder if `trackCount` > current.
  - **`markers.json`** — markers auto-save on add / drop / rename / move; loaded via `markers.setContext(dir, sr)`.
  - **`<Name>.zfproj`** — `setlist` (cues, incl. each cue's `automation` snapshot), `playlists` (comp Takes), `automation` (global lanes), `ui` (view / strip width / zoom).
  - **`session_groups.json`** is the older name for VCA + edit groups; superseded by `session_mix.json`. Don't reintroduce it.
- `Save` (and `Save As`) call `saveSessionStateTo`, which writes all of the above. Cues' own automation is captured by `addCueAtTransport` / `updateCueAtTransport`; recalling a cue **clears** every lane first, then loads the cue's snapshot (so a cue without a track-N entry doesn't leave another cue's curve on track N).
- `.zfproj` carries a `formatVersion` field. Treat its absence as v1 and fall back gracefully.
- Per-strip state used to live in **global** `appProps` keyed by index, which leaked between sessions. New sessions / freshly-grown strips now wipe the per-index `appProps` overrides (see `clearStripOverridesRange`); the session files are authoritative.

### Stable identity

- Cue snapshots, automation references, anything that must survive a strip reorder uses `TrackState::stripId` (a `juce::Uuid`), never an array index.

### Dialogs + prompts

- Every prompt that has a text or number field calls `dialog::primeNameEditor(aw, "fieldId")` after the buttons are added: it grabs focus, selects all text, and wires Enter in the field to `exitModalState(1)`. Every AlertWindow prompt in this app uses result code **1** for OK / Apply / Save / Create — don't break that convention.
- For custom `juce::Component` dialogs (`AddTracksDialog`, `NewSessionDialog`), do the equivalent by hand: `setSelectAllWhenFocused(true)` + `grabKeyboardFocus()` (deferred via `MessageManager::callAsync`) + `onReturnKey = [this]{ commit(); }`.

### Session folder layout

```
~/Music/Zynforge Sessions/<Name>/
├── Audio Files/                  Track_01.wav … (recordings)
├── Export Files/                 stereo bounce + per-track exports (was 'Bounced Files/')
├── Session File Backups/         timestamped .zfproj snapshots (auto-pruned, keep 10 newest)
├── Clip Groups/                  reserved
├── <Name>.zfproj                 cues, playlists, automation, UI layout
├── session_mix.json              per-strip mix + session tempo
├── markers.json                  per-session markers
├── session.report.json           sha256 + counts (written on clean stop)
└── WaveCache.wfm                 thumbnail cache (versioned header, auto-rebuilds on mismatch)
```

The `Video Files/` placeholder folder is **not** created. `.zfproj` is registered as the app's document type (`CFBundleDocumentTypes` → `CFBundleTypeIconFile=Icon`) so Finder shows the ZynForge icon.

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
- **Product / UX direction changes** → `design.md`.
- **Sprint priorities, completed work, new TODOs** → `tasks.md` at the end of every productive session.
- **Decisions whose *why* future-you will want** → add an ADR to `decisions.md`.
- **Conventions adopted that aren't yet documented** → `coding-standards.md`.
- **Test gaps spotted or new validation steps** → `testing.md`.
- **Anything user-visible** → entry under `## [Unreleased]` in `CHANGELOG.md`.

When a doc and code change disagree, the code is authoritative — but the disagreement is itself a doc bug. Fix it.
