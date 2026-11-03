# EditPage::TrackRow

`Source/UI/EditTrackRow.h` (`EditPage::TrackRow`, included only by `EditPage.cpp`) — the per-track horizontal row in the EDIT view.

## Description

Each row has a fixed-width header on the left (colour wash, name, R / MUTE / SOLO mirrors, size handle) and a scrollable content pane on the right (waveform, clip handles, fades, crossfades, edit cursor, playhead, marker ticks). Painted at 24 Hz when visible, gated off when neither visible nor recording.

The row and shared timeline mapping/constants were extracted from the former `EditPage.cpp` god file into `EditTrackRow.h`. It remains a nested class for access/layout compatibility, but its 4,000+ lines no longer obscure the owning page implementation.

## When to use

`EditPage::TrackList` creates one TrackRow per logical strip (stereo pairs collapse to one row). Lifetime tracks the session — rebuilds on track count change.

## Constructor

```cpp
TrackRow (int trackIndex, bool isStereoPair,
          AudioEngine& engine,
          juce::AudioFormatManager&, juce::AudioThumbnailCache&);
```

| Param | Type | Notes |
|---|---|---|
| `trackIndex` | `int` | 0-based physical track index |
| `isStereoPair` | `bool` | True if this row represents a stereo L+R pair |
| `engine` | `AudioEngine&` | All audio + clip queries route through it |
| `formatManager` / `thumbnailCache` | refs | Waveform thumbnail rendering |

## Lane modes

The right pane's content adapts to a per-row `LaneMode`:

| Mode | Shows | Editable via |
|---|---|---|
| `Waveform` | Audio thumbnail + clip handles | Clip drag (Move / TrimL / TrimR / FadeIn / FadeOut / Crossfade-mid), Smart / Trim / Grabber / Fade / Selector tools |
| `Markers` | Marker tick stripe | Read-only — markers live on the time ruler |

Switching mode: the row's right-click menu (`TrackRow::rowMenu`), on the channel name, the bare header or any lane with no clip under the pointer. It holds lane and size, and Delete Channel on the header only. Both stay with the strip and save as `ui.editRows`. A Markers lane draws MARKERS LANE and still shows a rolling take.

Clip-menu actions apply to both halves of a stereo row and to each edit-group peer's OWN clip under the clicked one (`peerClipTargets`). A channel picked in the Input or Output combo goes through `setStripLinkedRouting` and sets both sides, as MIXER and PATCH do. → Master goes through `sendStripToMaster` and moves the output alone. Waveforms are drawn through `drawThumbnailVisible`, which reads only the visible columns, so deep zoom stays responsive. Crossfades pair clips in timeline order. Hit-tests walk a clip list top first (`topClipAt`, `clipHitOrder`), the reverse of paint. While the timeline is read-only (recording, finalizing or a session job), drags already in flight stop editing.

## Height presets

7-step + Fit:

| Preset | Pixels |
|---|---|
| Micro | 28 |
| Mini | 50 |
| Small | 80 |
| Medium | 110 |
| Large | 160 |
| Jumbo | 220 |
| Extreme | 320 |
| FitToWindow | viewport / row count |
| Custom (drag-resize) | engineer-set |

## Tools (per-mouse-event)

Recording arm changes go through the engine and are refused during a take. Locked clips are protected from split/ripple/crop movement. Reordering delegates to the shared track transaction so stereo pairs and media/state move together; successful topology changes clear old index-based undo and clipboard state. Empty initialized arrangements stay silent after reload/export, and pasted clips retain source-file/channel identity even on previously unrecorded tracks. These behaviors are covered by the September engine regressions; mouse/hit-test workflows still require native UI checks.

Per-clip Consolidate launches the engine's asynchronous arrangement worker. The row disables itself while work is active, applies undo only after a successful current-session result, and uses a `SafePointer` for completion. Main-menu Consolidate follows the same contract. Local or daemon recording and an existing session-I/O job disable destructive editing.

Reads `EditToolsBar::getTool()` to bias hit-testing:

| Tool | Behaviour |
|---|---|
| Smart | Default — clip-edge zones trim, body moves, fade handles fade |
| Selector | Drag a loop region; sets the player's loop |
| Trim | Edge drag = trim, body click = no-op |
| Grabber | Body drag = move, edges = no-op |
| Fade | Click a clip = fade dialog |

## Source and group bounds

Right trims stop at the referenced source's length, including cross-track clips. Group drags calculate one legal movement before changing any peer so a timeline-zero or source boundary cannot alter relative spacing. Snapped drags retain pointer movement until it crosses a snap step. Resolve peer indices before mutating source clips.

Grouped clipboard operations preserve track/time offsets, source channel, fades, gain and mute; all paste destinations are checked before insertion. Healing requires compatible source/gain/mute/fade properties and an unfaded internal join.

Read failures propagate through rendering and consolidation. A missing explicit source must not fall back to the destination track or silently produce a successful export. See [the October regressions](../history/AUDIT_FIXES_2026-10-03.md).

## Drag state (mutually exclusive)

Only one drag-in-progress at a time:

- `draggingClipIdx + draggingClipModeInt` — clip-edit drag (Move / TrimL/R / Fade)
- `draggingXfadeAIdx` — crossfade midpoint drag (the outgoing clip's index; incoming = +1)
- `reorderArmed` + `reorderActive` — strip reorder via swatch column

## Tokens used

- **Colours**: `brand::stripColour(index)` for the row's personality wash; the same colour `lift`ed → `waveBg` (the light Pro Tools-style clip-fill) and `sink`ed → `waveDark` (the DARK waveform body drawn on top); `brand::meterHot` for the live-capture record envelope (dims to the channel colour on stop); `brand::accentStatus` for fade lines + edit cursor; `brand::accentSolo` for split markers + selection edges; `brand::brandOrange` for marker ticks
- **Typography**: `brand::type::caption()` for lane labels; `brand::type::sectionTitle()` for the row's strip name; `brand::type::mono(10.5f, true)` for the cursor time readout
- **Alpha**: `brand::alpha::dimmed` for the waveform-base under non-Waveform lane modes; `brand::alpha::prominent` for playhead overlay; `brand::alpha::muted` for crossfade band fill

## Special paint paths

At long time scales, the overview combines a short-window median body with thin true-peak maxima. Below the overview threshold, the native thumbnail supplies detailed waveform drawing. A cache older than the finalized media is discarded. Live capture uses bounded peak history, including original-resolution bins near the recording head; it does not repeatedly rescan growing files.

| What | Where | When |
|---|---|---|
| Pro Tools-style clip waveform | Wave pane | Waveform lane mode — light `waveBg = lift(stripColour)` clip block + DARK `waveDark = sink(waveBg)` thumbnail on top, source-level waveform with bounded display gain; clip gain scales the waveform |
| Live-capture envelope | Wave pane, `meterHot` | While the row is armed + recording (incl. continue / punch) — placed by timeline fraction; dims to the channel colour on stop |
| Crossfade visualisation | Overlap between adjacent clips (in timeline order), paints X-shape | Always when two clips overlap |
| Partial fade ramp | Fade diagonal | A piece of a split fade draws only its part of the original ramp |
| Loop-region overlay | Wave pane, translucent blue | When `player.hasLoopRegion()` |

## Accessibility

- Edit cursor doubles as visual focus indicator when no audio is loaded (engineer can drop markers on an empty timeline)

## Known limits

- The class remains large and nested, even after extraction to `EditTrackRow.h`; a later split still requires a clearer row-model/component boundary.
- Drag-edit gestures are mouse-only; tablets are second-class.
- Per-row visibility (Memory Location recall) shows/hides via `setVisible` but doesn't reflow the row above/below — they stay in place.
