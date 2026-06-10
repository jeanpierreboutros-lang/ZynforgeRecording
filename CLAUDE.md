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

For ad-hoc architecture questions, the source of truth is the code itself — start from `Source/Audio/AudioEngine.{h,cpp}` and `Source/UI/MainComponent.{h,cpp}`. Clip/take/comp + offline-render logic lives in `Source/Audio/AudioEngineClips.cpp` (offline renders are **windowed**: `forEachArrangementWindow` / `forEachStereoMixWindow` render fixed 64k-sample windows so memory stays O(window) on multi-hour shows; `bounceTrackArrangementToWav` / `bounceStereoMixToWav` stream those windows straight to 24-bit WAV and are what File ▸ Bounce uses — `renderTrackArrangement` / `renderStereoMix` are the buffer-returning wrappers for tests/short material; `compRangeFromTake` does take comping; `pasteClip` carries a per-clip `audioFile` for cross-track paste; **`healSeparationAt/Range`**, **`stripSilence`** (peak-envelope scan → clips around gaps, with pad), **`consolidateRange`** (flatten a range to a new flat file), **`setClipName`/`setClipGainDb`/`setClipMuted`/`setClipLocked`** are the clip ops). BS.1770 loudness is `Source/Audio/LoudnessMeter.h`; the EDIT overview navigator is `Source/UI/TimelineMinimap.h`.

**Undo model.** Clip + automation edits push `ClipSnapshotAction`/`AutomationSnapshotAction` immediately. Mixer ops (fader/pan/mute/solo/rename/recolour/routing) have NO per-gesture wiring — `MainComponentEdit.cpp::pollMixerUndo()` (from the 10 Hz timer) snapshots the whole mixer and records ONE `MixerSnapshotAction` once a change settles (~300 ms); `editUndo` flushes any pending change first, and re-baselines after undo/redo + on session open. **Recording is deliberately not undoable.** Keyboard shortcuts in `MainComponentKeys.cpp` match on **key code**, not `getTextCharacter` (unreliable under Cmd on macOS); JUCE doesn't wire native accelerators for these non-command menu items, so the keyboard handler is the real path (the menu's "⌘Z" hint is display-only).

EDIT-view specifics in `Source/UI/EditPage.cpp`: the `TrackRow` header (swatch / name / R-I-S-M / VIEW / slim `LedMeter` / routing) is **pinned** — drawn last via `paintHeader(g, headerOriginX())` and laid out + hit-tested at the horizontal scroll offset so it never scrolls off (the `ScrollViewport` subclass fires `relayoutHeaders()` on scroll; mouse gates go through `inWavePane/inSwatch/inNameZone`). Each clip is a **region block** with its own waveform (mapped to the clip's file region, so move/slip-trim show the right audio); a 1-2-5 s grid sits behind, and Move/trim drags snap through `engine.snapSampleToGrid`. EDIT tool keys (`S/T/R/G/F/B`) live in `MainComponentKeys.cpp`; `switchView` grabs keyboard focus so they + Delete actually arrive. The strip colour picker is `Source/UI/StripColourPicker.{h,cpp}` — a hue×shade gradient palette (no free-form selector); the per-strip default is now **neutral grey** (`brand::stripColour` → `stripDefaultGrey`, no longer the per-index `personality`).

**macOS menus + session loading (read before touching either).** `MainComponent` is the `MenuBarModel`; macOS caches each item's greyed/enabled state until `menuItemsChanged()` is called, so `MainComponentMenu.cpp::refreshMenuStateIfChanged()` polls a signature of every menu-gating condition from the 10 Hz timer — **if you add a menu item whose enablement depends on state, add that condition to the signature or it freezes greyed.** **Menu-id DISPATCH RANGES are a live footgun:** `menuItemSelected` handles several id RANGES (`100–199` export, `200–249` templates, `261–289` template-default, `401–430` MIDI outs, `620–699` timecode). A static single-purpose item assigned an id inside one of those ranges becomes **dead code** — the range branch swallows it (this is why the companion server, at id 270 ∈ 261–289, never started; fixed by moving it to 950). New static items use **high ids (900+)**; `MenuDispatchTests` now fails if a marquee static item lands in a range, but pick safe ids anyway. Allocated so far: 950 companion server, 951 auto-save, 952 post-show QC, 953 detect songs → markers, 954–957 console link (connect / soundcheck-stage toggle / capture gains / restore gains). The console items' enablement (`consoleLink.isConnected()` + patch state + **profile capability** `canRepatch`/`canCaptureGains`) is part of the menu-state signature. **Console control is profile-based** (`Source/Network/ConsoleProfile.h`): `ConsoleLink` is a transport + state machine; each console family is a `ConsoleProfile` declaring `canRepatch`/`canCaptureGains`/`hasNativeVsc` + its OSC address model. X32/M32 is the full reference; DiGiCo/Yamaha/SSL/A&H are native-VSC profiles (guarded out of OSC repatch). Don't fake repatch for a desk we can't talk to — see the 2026-06-10 ADR in `decisions.md`. The menu bar is `File · Edit · Track · Session · Help` (Edit = timeline/audio; Track = channel management). Every session open — File ▸ Open, the Welcome dialog, and the launch **auto-reopen of the last session** — funnels through `MainComponentSessionIO.cpp::openSessionFolder()`, which pins the active dir, restores setlist + UI + full mixer state, loads audio, and **sizes the mixer from the loaded audio when a session has no `session_mix.json`** (recorded-but-never-Saved recovery). Export reads recordings from `Audio Files/` (legacy root fallback), not the session root.

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
- **Audio storage stays mono-per-channel** (`Track_NN.wav` per track); "stereo" is the pair + `isStereo` flag, NOT a single interleaved file. But it must *behave* as one stereo track on EVERY surface — when you add a surface that iterates physical tracks, collapse the pair there too. Done so far: **import** spreads pan hard-L/R and **persists `session_mix.json` immediately** (don't leave the flags RAM-only — the recovery path would re-split into two mono strips); **Meterbridge** (`Meterbridge.cpp`) skips the R half and gives the L meter a `setStereoPartner`; the **Export individual tracks** dialog (`onExportIndividualTracks`) shows one "(stereo)" row per logical strip and maps it back to both physical channels; **export/bounce** writes ONE interleaved stereo file — `TrackExporter::exportStereoPair` (raw per-track, with SR conversion) and `AudioEngine::bounceStereoPairToWav` (edited-arrangement stem, streamed). Both export callers in `MainComponentSessionIO.cpp` skip the R half when its L is handled.

### Persistence

- Anything the user can edit (track names, colours, routing, gains, stereo pairs, takes, cues, time signature, automation, VCA + edit-group assignments, markers, tempo) must round-trip through one of the per-session files. RAM-only state is a data-loss bug.
- Per-session files live under the session folder:
  - **`session_mix.json`** — the engine writes the full per-strip mix state (name, colour, gain, pan, mute, solo, monitor, arm, routing, stereo, VCA + edit group, **aux sends** — 4 slots of `{bus, dB, post}`) plus session-level tempo (`tempoBpm`, `timeSigNum`, `timeSigDenom`, `tempoMap`). Loaded on Open; size the recorder if `trackCount` > current. **Aux sends are authoritative here, NOT global appProps** (the old `strip_send_*` appProps keys leaked routing across sessions; `setTrackSend` no longer writes them and `applyPersistedStripState` no longer reads them).
  - **`markers.json`** — markers auto-save on add / drop / rename / move; loaded via `markers.setContext(dir, sr)`.
  - **`<Name>.zfproj`** — `setlist` (cues, incl. each cue's `automation` snapshot), `playlists` (comp Takes; each clip carries `fadeCurve` and, for cross-track paste, an `audioFile` filename resolved against `Audio Files/` on load), `automation` (global lanes), `ui` (view / strip width / zoom).
  - **`session_groups.json`** is the older name for VCA + edit groups; superseded by `session_mix.json`. Don't reintroduce it.
- `Save` (and `Save As`) call `saveSessionStateTo`, which writes all of the above. Cues' own automation is captured by `addCueAtTransport` / `updateCueAtTransport`; recalling a cue **clears** every lane first, then loads the cue's snapshot (so a cue without a track-N entry doesn't leave another cue's curve on track N).
- **Auto-save** (`MainComponent::serviceAutosave`, driven from the 10 Hz timer) calls the same `saveSessionStateTo` on a user-set interval (appProps `autosaveMinutes`, 0 = off, default 5; picker is Session ▸ Auto-Save & Backup…). It's gated on the undo manager's stored-command count so it only writes when the session actually changed since the last save — which also keeps the backup sessions from churning. The backup itself is `MainComponent::writeSessionBackupSnapshot` → `zynforge::sessionbackup::writeSnapshot` (`Source/Audio/SessionBackup.h`, headless-tested): a **full backup session** — a timestamped `Session File Backups/<Name>_<stamp>/` folder copying the session-defining files (`.zfproj`, `session_mix.json`, `session_settings.json`, `markers.json`), **never the audio**, pruned to 10. It's called from `saveSetlistToActiveSession`, so every Save, cue edit, and auto-save drops one. Recording is unaffected: audio is streamed live and the take is crash-safe regardless.
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
├── Session File Backups/         timestamped backup-session folders <Name>_<stamp>/ (session-defining files, no audio; keep 10 newest)
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
