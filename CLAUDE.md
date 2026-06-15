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

**EDIT view file layout (2026-06-14):** `EditPage.cpp` was split — the ~3,800-line nested class `EditPage::TrackRow` (the per-track row: header + waveform/automation lane) plus the shared EDIT constants + `TimelineMapper` now live in **`Source/UI/EditTrackRow.h`** (included only by `EditPage.cpp`). `EditPage.cpp` (~870 lines) keeps `TrackList` + the `EditPage` methods. Edit the row's paint/hit-test/clip logic in `EditTrackRow.h`.

**Mixer layout + density (`MainComponentLayout.cpp`):** strips lay out either single-row (width-preset driven) or **GRID** (12-per-row × 2 visible rows = 24 faders, vertical scroll) when `mixerGridView` is on (toolbar `gridButton`, persisted). Width presets carry a per-preset floor AND **ceil** (M ≈70–120px stays compact so 12 fit; L 150–280px triggers the **2-row header** so the full channel name shows on its own line). The strip's fader cluster (`ChannelStrip::resized`) packs dB ruler + fader + meter tight and centres them. **`LedMeter` auto-drops its dB labels when narrower than ~30px** (compact strips / EDIT) — never assert on a negative bar rect, and it removes the redundant right-hand scale. **Faders are click-drag-only**: `FineFader` (channel/master/VCA/bus — the name is legacy; it no longer fine-trims) ignores the wheel and forwards it to the parent so the mixer scrolls.

EDIT-view specifics in `Source/UI/EditTrackRow.h` (was `EditPage.cpp`): the `TrackRow` header (swatch / name / R-I-S-M / VIEW / slim `LedMeter` / routing) is **pinned** — drawn last via `paintHeader(g, headerOriginX())` and laid out + hit-tested at the horizontal scroll offset so it never scrolls off (the `ScrollViewport` subclass fires `relayoutHeaders()` on scroll; mouse gates go through `inWavePane/inSwatch/inNameZone`). Each clip is a **region block** with its own waveform (mapped to the clip's file region, so move/slip-trim show the right audio); a 1-2-5 s grid sits behind, and Move/trim drags snap through `engine.snapSampleToGrid`. EDIT tool keys (`S/T/R/G/F/B`) live in `MainComponentKeys.cpp`; `switchView` grabs keyboard focus so they + Delete actually arrive. The strip colour picker is `Source/UI/StripColourPicker.{h,cpp}` — a hue×shade gradient palette (no free-form selector); the per-strip default is now **neutral grey** (`brand::stripColour` → `stripDefaultGrey`, no longer the per-index `personality`).

**macOS menus + session loading (read before touching either).** `MainComponent` is the `MenuBarModel`; macOS caches each item's greyed/enabled state until `menuItemsChanged()` is called, so `MainComponentMenu.cpp::refreshMenuStateIfChanged()` polls a signature of every menu-gating condition from the 10 Hz timer — **if you add a menu item whose enablement depends on state, add that condition to the signature or it freezes greyed.** **Menu-id DISPATCH RANGES are a live footgun:** `menuItemSelected` handles several id RANGES (`100–199` export, `200–249` templates, `261–289` template-default, `401–430` MIDI outs, `620–699` timecode). A static single-purpose item assigned an id inside one of those ranges becomes **dead code** — the range branch swallows it (this is why the companion server, at id 270 ∈ 261–289, never started; fixed by moving it to 950). New static items use **high ids (900+)**; `MenuDispatchTests` now fails if a marquee static item lands in a range, but pick safe ids anyway. Allocated so far: 950 companion server, 951 auto-save, 952 post-show QC, 953 detect songs → markers, 954–957 console link (connect / soundcheck-stage toggle / capture gains / restore gains), 958 capture-daemon toggle (flagged out-of-process recording; its enable/recording state is in the menu signature). The console items' enablement (`consoleLink.isConnected()` + patch state + **profile capability** `canRepatch`/`canCaptureGains`) is part of the menu-state signature. **Console control is profile-based** (`Source/Network/ConsoleProfile.h`): `ConsoleLink` is a transport + state machine; each console family is a `ConsoleProfile` declaring `canRepatch`/`canCaptureGains`/`hasNativeVsc` + its OSC address model. X32/M32 is the full reference; DiGiCo/Yamaha/SSL/A&H are native-VSC profiles (guarded out of OSC repatch). Don't fake repatch for a desk we can't talk to — see the 2026-06-10 ADR in `decisions.md`. The menu bar is `File · Edit · Track · Session · Help` (Edit = timeline/audio; Track = channel management). Every session open — File ▸ Open, the Welcome dialog, and the launch **auto-reopen of the last session** — funnels through `MainComponentSessionIO.cpp::openSessionFolder()`, which pins the active dir, restores setlist + UI + full mixer state, loads audio, and **sizes the mixer from the loaded audio when a session has no `session_mix.json`** (recorded-but-never-Saved recovery). Export reads recordings from `Audio Files/` (legacy root fallback), not the session root.

## Critical universal rules

### Real-time discipline (audio thread)

- The audio callback never allocates, locks, opens files, calls `Logger`, or touches the message thread.
- State transitions happen via `std::atomic` flags. Writers + readers are constructed / destroyed only on the message or background threads.
- Scratch buffers (`outputAccum`, `playerScratch`, companion ring) pre-allocate in `audioDeviceAboutToStart`.
- **Live recording waveform** is built on the audio thread: `TrackState::liveWavePush` bins captured input into min/max pairs and pushes them through a pre-allocated lock-free `AbstractFifo` (`liveFifo`); the EDIT timer drains it (`liveWaveDrain`) and draws. Keep it allocation-free + lock-free on the push side — never grow the ring, log, or block there. Reset via `liveWaveReset()` on record start (message thread). This is why the live waveform is detailed + final-on-stop (no post-stop re-scan), like Reaper/Pro Tools.
  - **The live envelope OWNS the lane while capturing — even over clip blocks.** In `EditTrackRow::paint`, `showLive` (`liveRecording || liveHold`) takes precedence over `haveClipBlocks`: the continuous live path draws when `! haveClipBlocks || showLive`, and the static clip-block renderer is skipped while `showLive`. This is essential for a **continue** — every stop seeds a default clip over the take, so a continued take has `haveClipBlocks == true`; without the override the growing capture would be hidden behind the static clips until the post-stop re-scan. The existing take draws dim (`drawContinueExisting`, `[0, base)`), the new audio grows hot on top (`drawRecEnvelope`), then the clips take back over on stop. Don't gate the live envelope behind `! haveClipBlocks` alone.
  - **The live envelope is placed by TIMELINE FRACTION, not stretched to the lane.** `recPeakL` covers `[0, recPos]` (`recPos = recPeakL.size() * kLiveBinSamples`); the lane covers `[0, total] = max(playerTotal, recPos)`. `EditTrackRow::paint` confines `drawRecEnvelope` to `liveFrac = recPos/total` of the lane and draws the dim existing take across `existingFrac = playerTotal/total`. For a **continue** these collapse to the old behaviour (`liveFrac == 1`, take is the `[0, takeEnd)` prefix). For a **punch** `total` is the full take so `liveFrac < 1` and the capture sits AT the punch point (with the whole take dim under it) — **don't** draw the envelope across the full lane width or a punch looks like it records at the END. The silent prefill columns in `recPeakL` encode the offset within that confined slice.
  - **`EditTrackRow::paint` ALWAYS paints the pinned header last, in every lane VIEW mode.** The lane-content drawing runs inside a `drawLane` lambda so the non-waveform modes (Click / Tempo / Markers / Volume·Pan·Mute automation / comp lanes) can early-`return` from the *lambda* without skipping `paintHeader(...)` below. They used to `return` straight out of `paint()`, skipping the header — so when the timeline was scrolled the automation line/curve drew over the pinned name + I/O combos (the header is normally redrawn on top to cover lane content under the pinned-left region). Don't add a lane mode that returns from `paint()` directly; return from `drawLane` (or fall through) so the header still paints.
  - **Live-draw arming is SELF-DRIVEN per row, every tick — never a one-shot.** `TrackList::pushRecLevels` (EDIT timer) calls `row->setLiveRecording(true, prefill)` for each **armed** row every tick before draining, deriving the live state from the current recording+armed state (`setLiveRecording` only resets on the off→on edge, so it's idempotent). Do **not** re-introduce a one-shot "record just started" arm: if that single edge is missed or clobbered by a refresh between ticks, `appendLivePeak` early-returns on `! liveRecording` and silently discards every drained peak for the rest of the take (the live waveform never builds — this was the long-standing "continue doesn't show the waveform" bug). Arm **only armed rows** — a blanket arm of every row put non-armed lanes into an empty live state and blanked their existing waveforms. `prefill = recordBaseSamples / kLiveBinSamples` anchors the continue's new audio after the existing take. `LiveWaveContinueTests` locks the recorder-side data path (a continue emits live-overview columns like a fresh take).
- **A take is a SEQUENCE OF FILES.** `Track_NN.<ext>` + `Track_NN_partXX` continuations (auto-split past the container cap, or **continue-recording** via `armContinue` → `nextContinuationPart`) are stitched into one seamless take by `ConcatReader` (`Source/Audio/MultiPartReader.h`) on playback (`SessionPlayer`) and in the EDIT thumbnail (`thumbnail.setReader`). **Continue/append never modifies an existing file** — it only adds a new finalised part, so it can't corrupt what's captured. Prefer this (a new part) over rewriting a file. The splice below is now only for a mid-take punch.
- **STOP parks the transport at the take's END, not 0 (live-recorder convention).** `AudioEngine::stopRecording` captures `recorder.getRecordTimelineSamples()` **before** `recorder.stopRecording()` (which zeroes the recorder's base offset), then after `player.loadSession` — which rewinds to 0 — restores the playhead there with `player.setPositionSamples(...)`. This is deliberate and the opposite of a DAW (Reaper's Stop snaps the edit cursor back to the pass start): leaving the playhead at the end is what makes a subsequent RECORD take the **append-a-new-part** branch in `MainComponent::onRecordClicked` (`pos == total` ⇒ `continueAppend`), so continue resumes seamlessly. Don't "helpfully" rewind to 0 on stop — that reintroduces the "continue starts from the beginning" bug.
- **Punch-in is OFFLINE, never a real-time write into a take.** A punch records a clean fresh `Track_NN` from 0 like any take; the existing copies — **and all their continuation parts** — are moved to `.punchbase` sidecars in `startRecording` (before the writer truncates) and spliced back in `stopRecording` via `Source/Audio/PunchSplice.h` (`base[0,punchIn)+fresh+base[after]`, temp + atomic swap, all copies). The splice sits **after `closeWriters()` but before the async SHA thread** so the report's SHA/length describe the spliced files. Keep it there — never push punch writes onto the audio thread, and never overwrite a take in place (a failure must revert to the untouched base). **Multi-part bases ARE supported** (a take built up by continue-recording): `splicePunchParts` reads the whole take (all stashed part sidecars) via `ConcatReader` and **flattens** it into one spliced `Track_NN` (the parts are deleted on success, restored on failure). So you can punch anywhere on any take, single-file or split. `armPunchIn` sets `recordBaseSamples = punchInPos` so the clock / playhead / live-capture waveform anchor AT the punch point (the new audio builds there, existing take dim underneath) — same machinery as a continue. The punch point is the EDIT cursor (set by a wave-pane click, `EditTrackRow::mouseDown`) or the playhead. See the punch ADR + *Read-only offline DSP is allowed…* in `decisions.md`.

### Brand + design system

- **Structural-vs-signal orange (2026-06-14):** bright/hot orange and `meterHot` mean **STATE** (record-armed, peaking) — never permanent chrome. `brand::structuralForge()` is the structural-identity orange (= brandOrange) for the strip spine / fader-cap groove / header seams / forge-marks, and it is drawn **ember-subdued at rest** (low alpha), escalating to full only when armed. Don't reuse `meterHot` for always-on chrome (it makes a live meter indistinguishable from idle UI). Identity marks (dialog badge, splash) keep full brand orange.
  - **The EDIT waveform fill is the CHANNEL COLOUR, not forge-heat (2026-06-15).** `EditTrackRow`'s `setHeatWaveFill` was a permanent ember→orange→white-hot vertical gradient — always-on orange chrome that violated the rule above and read as a harsh barcode on dense audio. It's now a near-solid fill in the channel's own colour (loud peaks lift slightly brighter + a whisper of warmth). The live-CAPTURE envelope stays `meterHot` (recording = state); the post-stop held envelope dims to the channel colour. Keep dynamic orange on the meters / armed state, not the static waveform.
- **Prompts:** `MainComponentInit` sets the ZynForge LAF as the **app-wide default** (`juce::LookAndFeel::setDefaultLookAndFeel(&laf)`, reset to nullptr in the dtor), so EVERY `juce::AlertWindow::showAsync(...)` reads as a first-party prompt (grey chrome + forge-mark badge via `ZynForgeLookAndFeel::drawAlertBox`, which mirrors JUCE's 80px icon column and draws the badge for non-`NoIcon` alerts). This also fixes app-wide **font resolution** — JUCE resolves typefaces through the default LAF, so without it custom-paint `Inter`/`JetBrains Mono` fell back to system fonts.
- Colours come from `Source/Theme/BrandColors.h`. Never use raw `juce::Colour::fromRGB` or `juce::Colours::black/white` for chrome. **The VALUES behind BrandColors/BrandTokens come from the family token source of truth** (`Desktop/zynforge/ZynForgeBrand/tokens.json` → generated `Source/Theme/ForgeTokens.h`, vendored): to change a brand value, edit tokens.json, run `generate.py`, re-vendor the header — never edit ForgeTokens.h or hard-code a value here. See `ZynForgeBrand/FORGE.md`.
- Fonts come from `Source/Theme/BrandTokens.h` via `brand::type::*` (scale: `micro`/`label`/`caption`/`uiBody`/`channelName`/`sectionTitle`/`headline`/`subhead`/`display`/`hero`). Never construct a raw `juce::Font`, and never re-size a role with `.withHeight(n)` — pick the right rung (add one to tokens.json if none fits).
- Dialog modals use `Source/Theme/DialogChrome.h::dialog::paintChrome(...)`. Custom dialog `paint()` is a smell.
- Shadows: `brand::shadow::elev1/elev2/elev3`. Never inline `Colours::black.withAlpha(...)`.
- Specular gloss / light scrim (the one sanctioned white): `brand::gloss(alpha)`. Never inline `Colours::white.withAlpha(...)`.
- Alpha: `brand::alpha::{faint,chrome,edgeSoft,subtle,wash,dimmed,ghost,scrim,muted,prominent,bold}`. Never inline `withAlpha(0.xx)`; if no step fits, catalog it in `BrandColors.h`.
- Tints (lighten/darken): `brand::lift(c, amt)` / `brand::sink(c, amt)`, with named amounts in `brand::tint::{faint,hover,edge,deep}`. **Never call raw `juce::Colour::brighter()/darker()` outside `Theme/`** — the gate bans it.
- Corner radius: `brand::radius::{sm,md,lg,xl}`. Never pass a raw float (sub-2 px meter/icon micro-radii excepted).
- Text on a saturated accent: `brand::onSignal(bg)`. Never hardcode black or white.
- `tools/design_audit.sh` is the CI gate enforcing all of the above (6 rules) — must stay CLEAN.

### Stereo + view linkage

- A stereo pair is two adjacent physical tracks with `isStereo=true` on the L track. Every view (MIXER, EDIT, PATCH) must collapse the pair into one logical entry.
- Mute / solo / arm / gain / pan / colour / name / VCA / edit-group changes mirror across the pair automatically — never half-update.
- VCA gain + mute on a stereo pair: the R partner reads the L track's lane (in the audio callback) so both halves follow the curve. Pan stays per-channel so the stereo image isn't collapsed.
- **A stereo pair is captured as ONE interleaved 2-channel file** named for the L slot (`Track_NN.wav` with 2 channels); there is **no** `Track_(N+1).wav` for the R half. The pair still spans **two logical strips** (L = index N with `isStereo`, R = index N+1) on every surface; only the on-disk file is single. This is the native-stereo model adopted 2026-06-13 (see the ADR in `decisions.md`) — it replaced the old "two mono files + `isStereo` flag" storage. How each layer handles it:
  - **Recorder** (`MultitrackRecorder`): the L `WriterChannel` opens one 2-ch writer (`openWriterAtPath(..., numChannels=2)`, `WriterChannel::numChannels=2`); the R slot gets an EMPTY `WriterChannel`. The drain step for the L writer reads BOTH FIFOs (`fifos[i]` + `fifos[i+1]`), stages them into the shard's `stageL`/`stageR`, and does one `writeFromFloatArrays(..., 2, n)`. Byte/roll/RF64 accounting multiplies by `numChannels`. The audio-thread push path + per-channel FIFOs are unchanged (meters, pre-roll untouched). Pre-roll dumps both channels into the 2-ch writer. **The R FIFO stays single-consumer even across a shard boundary**: the R slot's `WriterChannel` is empty, so whichever shard owns the R index skips it (`if (w.writer == nullptr) continue;`) and never drains its FIFO — only the L's drain (one thread) reads both FIFOs. So an odd-indexed pair split across two shards is still race-free; pairs do **not** need to be shard-aligned.
  - **Player** (`SessionPlayer`): `loadSession` sniffs `reader->numChannels`; a 2-ch file expands into TWO `Track`s sharing ONE `BufferingAudioReader` — the L track reads file channel 0, the R channel 1 (`useLeft`/`useRight`). One output stream per physical channel, so index→output is identical to the legacy layout.
  - **Offline render / bounce** (`AudioEngineClips.cpp::ArrangementSource`): `open()` resolves the channel — a 2-ch own-file → read ch 0 (L); no own-file but the prior slot's `Track_<track>` is 2-ch → read ch 1 (R). `bounceStereoPairToWav` thus pulls both halves from the one file.
  - **EDIT waveform** (`EditPage.cpp` `TrackRow`): when the pair is one file (`stereoOneFile`), the L lane draws file channel 0 and the R lane channel 1 from the same thumbnail (`drawLaneL`/`drawLaneR` → `drawChannel`), instead of two separate mono thumbnails.
  - **Import** (`MainComponentSessionIO.cpp`): a stereo source is written as ONE interleaved 2-ch `Track_NN.wav` (`writeStereo`), matching native capture; it spreads pan hard-L/R and **persists `session_mix.json` immediately**.
  - **Legacy compatibility**: sessions recorded the OLD way (two mono `Track_NN.wav` + `Track_(N+1).wav`) keep working everywhere — the player loads two mono tracks, `ArrangementSource` reads each own-file at ch 0, EDIT draws two mono thumbnails. The channel-sniffing makes both layouts transparent.
  - Other collapse points unchanged: **Meterbridge** skips the R half + gives the L meter a `setStereoPartner`; the **Export individual tracks** dialog shows one "(stereo)" row per logical strip and maps it back via `bounceStereoPairToWav`; `TrackExporter::exportStereoPair` still handles raw per-track stereo export with SR conversion.

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
