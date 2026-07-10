# Coding Standards — ZynForge Recording

The goal of this document is to keep the codebase consistent enough that any contributor (human or Claude) can read and edit it without first reverse-engineering the previous author's conventions. Where a convention is project-specific — and not just generic C++ / JUCE style — it is called out explicitly.

## General Principles

- **Clarity over cleverness.** Code is read many more times than it is written. Prefer the obvious form.
- **Real-time discipline is mechanical, not best-effort.** See `decisions.md`. The audio callback obeys hard rules; the rest of the codebase obeys taste.
- **Persistence is part of the feature.** A user-editable field that does not round-trip to disk is incomplete, not a follow-up.
- **Shared `PropertiesFile` reloads REPLACE, never merge.** `appProps` and the four `Strip*` modules point five separate `juce::PropertiesFile` instances at one `.settings` file. Before a mutate+save, reload to pick up other writers' changes — but with `clear()` + `reload()` (replace), not a bare `reload()` (merge). A merge keeps in-memory keys that are gone from disk, so the next whole-file save resurrects a key another module just deleted. Use `reloadReplace(*props)` / `reloadAppPropsBeforeWrite()`, guarded on the file existing.
- **The design system is the truth.** Colours, fonts, spacing, shadows come from `Source/Theme/`. Inline literals are a smell.

## Naming Conventions

- **Files:** `PascalCase.{h,cpp}` matching the primary class. `AudioEngine.h` declares class `AudioEngine`. Header-only utilities are fine for small helpers.
- **Namespaces:** `zynforge::` for project code; nested `zynforge::brand` for design tokens; `zynforge::dialog` for dialog helpers. Do not put project code at global scope.
- **Classes / structs:** `PascalCase`. Mark `final` if not meant to be inherited.
- **Free functions / methods:** `camelCase`.
- **Member variables:** `camelCase`. No Hungarian. No `m_` prefix.
- **Constants:** `kCamelCase` (Google style) — `kNumSends`, `kFftSize`, `kTickMs`.
- **Atomics:** name reads naturally — `armed`, `peak`, `recording` — not `armedAtomic`.
- **Enums:** `enum class Name : int { ... }`. Scoped, explicit underlying type.

## Code Formatting and Style

- **C++20.** Use `std::optional`, `std::span`, `concept`, structured bindings, designated initialisers, `if constexpr` freely.
- **Indentation:** 4 spaces. No tabs.
- **Braces:** Allman (open brace on its own line) for functions and types; K&R inline for short control flow when the body is one short statement.
- **Line length:** Aim for ~100 chars; hard wrap at 120.
- **Includes:** Project headers in `""`, system / JUCE headers in `<>`. Group order: same-file pair → other project → JUCE → std. One blank line between groups.
- **`auto`:** Use when the type is obvious from the right-hand side (iterators, `make_unique`, JUCE factory returns). Spell out the type when it informs the reader.
- **No `using namespace`** at file scope. Inside a function is acceptable for `std::chrono_literals`.

## Preferred Patterns and Anti-Patterns

### Always

- Use `brand::*` tokens for every colour, font, spacing, radius, shadow.
- Use `dialog::paintChrome(...)` for modal `paint()`. Never hand-roll a dialog background.
- Make `juce::Component`s `final` unless inheritance is intended.
- Use `juce::Component::SafePointer` when a lambda captured by an async callback (timer, modal, message) refers back to a parent that might outlive it.
- Before anything frees the recorder's `TrackState` vector (`setStripCount` shrink, `removeStripAt`, a console-session rebuild — New Session, Close, opening a smaller session), call `condemnAllStrips()` so the live `ChannelStrip`s' strip/meter/spectrum timers stop before the rebuild. The strips are rebuilt on the 10 Hz timer AFTER the free, so a strip that still holds a freed `TrackState&` is a UAF. `ChannelStrip::paint` also guards on `stripValid` for the external-repaint case.
- A dialog launched **modal** (`LaunchOptions::launchAsync()` / `enterModalState`) must close via `dw->exitModalState(...)`, never `dw->setVisible(false)` — hiding a modal window leaves it blocking all input (invisible app freeze). `setVisible(false)` + the `syncTab` reaper is ONLY for the non-modal tracked `launchFloating` path. `AudioDeviceDialog` carries a `selfOwned` flag and branches its close on it.
- Use `std::atomic` for any value crossed between the audio thread and any other thread.
- Pass `juce::String` by const reference. Pass small POD by value.
- Wrap external commands (`lame`, `rclone`) in `juce::ChildProcess` and check the exit code.
- Set the grey ZynForge LookAndFeel on **every** `juce::AlertWindow` at construction — `aw->setLookAndFeel (&laf)` in a `MainComponent` method, or `&getLookAndFeel()` / `&self->getLookAndFeel()` from a `Component` / captured SafePointer. Prompts must read grey + light grey app-wide.
- Map EDIT-view timeline samples ↔ lane pixels through `TimelineMapper` (`EditPage.cpp`), not a re-inlined lambda: `toX` rounds, `toXFloor` truncates, `toSample` inverts. Build one per paint/event from the lane's inner rect + session length.
- When you add a menu item whose enabled/greyed state depends on app state, add that condition to `MainComponent::refreshMenuStateIfChanged()`'s signature (`MainComponentMenu.cpp`). macOS caches the native menu's enabled states until `menuItemsChanged()` fires; that polled signature is the only thing that triggers the refresh. Miss it and your item freezes in whatever state it had at launch.
- Compute a strip's effective gain (own gain + VCA-bus gain) **once** per audio block and share it; don't re-derive it per consumer inside the callback.
- Resolve stereo logical ↔ physical strip mapping through `AudioEngine::physicalFromLogical` / `logicalFromPhysical` — it's model topology, not UI state. Don't re-implement the stereo-collapse walk in a view.
- **Put record-safety guards at the ENGINE, not (only) the UI.** `startPlayback`/`stopRecording`/`setTrackStereo` refuse or fail-closed based on `recorder.isRecording()` themselves. OSC, MCU, the companion server, and timecode chase all reach the engine directly and bypass any UI-only guard. A new transport or track-layout entry point that only guards in the button is reachable from the network unguarded.
- **A same-session reload must PRESERVE clip/comp edits.** Reloading the player in place (click-track regen, strip reorder, stop-recording) goes through `loadSession(dir, /*preserveEdits*/ true)` / `seedDefaultClips(true)`, never the wiping session-OPEN path. Only a genuine open (whose `.zfproj` restore repopulates) passes `false`. And any code that reads a WHOLE take stitches its `Track_NN_partXX` parts via `ConcatReader` (playback, EDIT thumbnail, export, analysis) and globs every audio extension — matching `Track_NN.wav` alone drops continuations and FLAC/AIFF takes.
- When a consumer needs only a slice of the engine, depend on a segregated interface (`ITransport`, with more facets to follow) rather than the whole `AudioEngine&`. New facets keep the same shape: pure-virtual contract, `AudioEngine` implements it.
- Use `PlaceholderView` for any loading / empty / error surface — don't hand-roll an empty `juce::Label`. Overlay it on the content area in `resized()`, drive it with `showLoading` / `showEmpty` / `showError` / `clear`, and transition **only on a state change** (compare `getState()` or a small `lastKind`) so VoiceOver isn't re-announced each tick. An empty state with a remedy should pass a CTA label + callback (e.g. MIXER's "Add tracks", PATCH's "Audio settings…").
- Give every interactive or informational component an accessible identity: `setTitle` / `setDescription` (and `setHelpText` where useful), make actionable controls real focusable buttons, and `postAnnouncement` on a meaningful state change. The app shipped with **zero** accessibility; `PlaceholderView` is the reference implementation. New UI should not regress this — building "for millions" includes VoiceOver users.

### Never

- Allocate, lock, log, or call `Component::repaint` from the audio callback.
- Construct a raw `juce::Font` outside `Source/Theme/`. Use `brand::type::*` or `brand::fonts::*`.
- Use `juce::Colour::fromRGB(...)` or `juce::Colours::black/white` outside `Source/Theme/`. Use `brand::*` tokens or `brand::onSignal(bg)`. The **one** sanctioned white is a specular gloss / light scrim — route it through `brand::gloss(alpha)`, never an inline `Colours::white.withAlpha(...)`.
- Call `juce::LookAndFeel::setDefaultLookAndFeel(...)`. The app's global default is JUCE's, by design: a global ZynForge default crashes JUCE text shaping (`SimpleShapedText::shape`). Set the LAF **per window** instead (see the AlertWindow rule above).
- Inline a `withAlpha(0.xx)` literal. Use a named step from `brand::alpha::` (`subtle`/`dimmed`/`ghost`/`scrim`/`muted`/`prominent`/`bold`). If none fits, add a line to the ad-hoc catalog in `BrandColors.h` rather than leaving the magic number undocumented.
- Pass a raw corner-radius float to `fill/drawRoundedRectangle`. Use `brand::radius::{sm,md,lg,xl}`. Sub-2 px micro-radii on meter segments / icon glyphs are the only exception (geometry-forced, radius < half the element height).
- Reference a strip by its array index in any persisted form. Use `TrackState::stripId`.
- Add a plugin hosting hook. See `decisions.md` *No plugin hosting*.
- Credit Harrison LiveTrax / Waves Tracks Live anywhere.
- `--force` push or amend a pushed commit.

### Brand-token examples

```cpp
// ✗  bgPanel hard-coded, font constructed inline, hex shadow.
g.setColour (juce::Colour (0xff121316));
g.fillRect (r);
g.setFont (juce::Font (juce::FontOptions().withHeight (13.5f).withStyle ("Bold")));
g.setColour (juce::Colours::black.withAlpha (0.35f));

// ✓  same surface via the design system (FLAT — solid fill, no gradient).
g.setColour (brand::bgPanel);
g.fillRect (r);
g.setFont (brand::type::ui (13.5f, true));
g.setColour (brand::shadow::elev2());
```

## Error Handling and Logging

- **User-facing errors** become a `Toast` (`Source/UI/Toast.h`) or an `AlertWindow` for blocking conditions. Never silently fail. Silent refusal at the engine boundary (e.g. reorder during playback) is a bug.
- **Internal invariants** — `jassert` for "this should never happen" in debug. Don't `assert` in release-critical RT code.
- **Logging** — `juce::Logger::writeToLog` from the message thread only. The audio callback never logs.
- **File I/O errors** — check `juce::Result` / `bool` returns from JUCE writers. On failure, surface to the engineer and continue (a single failed FLAC encode shouldn't kill the recording session).
- **External-process errors** — `juce::ChildProcess::getExitCode()` must be checked. Non-zero exit → toast a warning with the command's stderr tail.

## Testing Expectations

See `testing.md` for the full strategy. High level:

- Every change is **build-tested** (`cmake --build build --config Release`).
- Every change runs the **headless test suite** (`Source/Tests/`, `juce::UnitTest`): `ZYNFORGE_RUN_TESTS=1 "…/Zynforge Recording.app/Contents/MacOS/Zynforge Recording"` (or `--run-tests`). Report lands at `~/Library/Logs/Zynforge/test-report.log`. **Quit any running GUI instance first** — otherwise LaunchServices re-focuses it and the test process exits without running (the log isn't rewritten; check its mtime before trusting the pass count).
- New `Source/Audio/` behaviour gets a test. Clip edits + recording integrity are covered (`RecorderPlayerTests`, `RecordingIntegrityTests`, `AudioCallbackTests`); add to them rather than starting a parallel harness.
- Every change is **smoke-tested** (launch, confirm no new crash report, RSS / CPU healthy).
- UI-only behaviour (paint, hit-test, modal flow) isn't covered by the headless suite — say so explicitly and eyeball it; don't claim UI correctness from a green build.

## Documentation and Comments

- **Comment the *why*, never the *what*.** Well-named functions document themselves. Comments document a hidden constraint, a surprising decision, a workaround for a JUCE bug, a non-obvious invariant.
- **No multi-paragraph docstrings.** A short paragraph at the top of a non-trivial class is fine.
- **No "added for issue #123" / "used by X" comments.** Those rot. Belong in the commit message or PR description.
- **No emoji in code, comments, or docs** unless the user explicitly asks for it.
- **Update `CLAUDE.md` + `README.md`** after every change that alters how the app works or how a contributor would build / run it.

## Git / Commit Guidelines

- **Subject line** in the imperative ("Add VCA stereo shortcut", not "Added" or "Adds").
- **Body** explains the *why* and any context future-you needs. The diff already shows the *what*.
- **Co-author trailer** — `Co-Authored-By: Claude Opus <noreply@anthropic.com>` when Claude wrote code in the commit.
- **Push** to `origin/main` after every change (workflow rule 4). No PR review process today.
- **Never** `--force` push to `main`. Never amend a pushed commit.
- **Never** `--no-verify` or skip hooks unless the user explicitly asks.

## Project-specific TODOs

- [TODO] Confirm whether `std::format` is the target for new string formatting, or whether `juce::String::formatted` remains the convention.
- [TODO] Document `final` policy for header-only components inside `Source/UI/*.h`.
