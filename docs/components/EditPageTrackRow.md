# EditPage::TrackRow

`Source/UI/EditTrackRow.h` (`EditPage::TrackRow`, included only by `EditPage.cpp`) — the per-track horizontal row in the EDIT view.

## Description

Each row has a fixed-width header on the left (colour wash, name, R / MUTE / SOLO mirrors, lane-mode picker, size handle) and a scrollable content pane on the right (waveform, automation lane overlay, clip handles, fades, crossfades, transient ticks, edit cursor, playhead, marker ticks). Painted at 24 Hz when visible, gated off when neither visible nor recording.

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
| `engine` | `AudioEngine&` | All audio + automation + clip queries route through it |
| `formatManager` / `thumbnailCache` | refs | Waveform thumbnail rendering |

## Lane modes

The right pane's content adapts to a per-row `LaneMode`:

| Mode | Shows | Editable via |
|---|---|---|
| `Waveform` | Audio thumbnail + clip handles | Clip drag (Move / TrimL / TrimR / FadeIn / FadeOut / Crossfade-mid), Smart / Trim / Grabber / Fade / Scrubber / Selector tools |
| `Volume` / `Pan` | Automation lane with points, curves, tension handles | Add / Delete tools, drag points, drag tension handles |
| `Mute` | Automation lane (snapped to 0/1, Hold curves) | Add / Delete |
| `Click` | Beat-overlay grid (when a click track exists) | Read-only |
| `Tempo` | Tempo map points | Add / Delete tempo changes |
| `Markers` | Marker tick stripe | Read-only — markers live on the time ruler |

Switching mode: per-row VIEW menu in the header. The toolbar's `Param` choice ALSO drives this — if the engineer picks "Pan" in the toolbar, every row's lane shows the pan automation regardless of individual VIEW mode.

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

Normalize and per-clip Consolidate launch the engine's asynchronous arrangement workers. The row disables itself while work is active, applies undo only after a successful current-session result, and uses a `SafePointer` for completion. Main-menu Strip Silence/Consolidate follow the same contract. Local or daemon recording and an existing session-I/O job disable destructive editing.

Reads `EditToolsBar::getTool()` and `AutomationToolbar::getTool()` to bias hit-testing:

| Tool | Behaviour |
|---|---|
| Smart | Default — clip-edge zones trim, body moves, fade handles fade |
| Selector | Drag a loop region; sets the player's loop |
| Trim | Edge drag = trim, body click = no-op |
| Grabber | Body drag = move, edges = no-op |
| Fade | Click a clip = fade dialog |
| Scrubber | Drag = playhead chases mouse |
| Add Point | Automation lane click drops a point at (samplePos, value) |
| Delete Point | Automation lane click removes nearest point |
| Select | Drag automation point or tension handle |

## Drag state (mutually exclusive)

Only one drag-in-progress at a time:

- `draggingClipIdx + draggingClipModeInt` — clip-edit drag (Move / TrimL/R / Fade)
- `draggingPointIdx` — automation point drag
- `draggingTensionSegIdx` — automation curve-bend drag
- `draggingXfadeAIdx` — crossfade midpoint drag (the outgoing clip's index; incoming = +1)
- `reorderArmed` + `reorderActive` — strip reorder via swatch column

## Tokens used

- **Colours**: `brand::stripColour(index)` for the row's personality wash; the same colour `lift`ed → `waveBg` (the light Pro Tools-style clip-fill) and `sink`ed → `waveDark` (the DARK waveform body drawn on top); `brand::meterHot` for the live-capture record envelope (dims to the channel colour on stop); `brand::accentStatus` for fade lines + edit cursor; `brand::accentSolo` for split markers + selection edges; `brand::engagedAmber` for transient ticks; `brand::brandOrange` for marker ticks; `brand::shadow::elev3` for the automation point border
- **Typography**: `brand::type::caption()` for lane labels; `brand::type::sectionTitle()` for the row's strip name; `brand::type::mono(10.5f, true)` for the cursor time readout
- **Alpha**: `brand::alpha::dimmed` for the waveform-base under non-Waveform lane modes; `brand::alpha::prominent` for playhead overlay; `brand::alpha::muted` for crossfade band fill

## Special paint paths

| What | Where | When |
|---|---|---|
| Pro Tools-style clip waveform | Wave pane | Waveform lane mode — light `waveBg = lift(stripColour)` clip block + DARK `waveDark = sink(waveBg)` thumbnail on top, honest levels (1.5× auto-gain cap); clip gain just scales the waveform (no gain line) |
| Live-capture envelope | Wave pane, `meterHot` | While the row is armed + recording (incl. continue / punch) — placed by timeline fraction; dims to the channel colour on stop |
| Detected transient ticks | Top of wave pane, 6 px engagedAmber strokes | When `engine.getTransientsForTrack(index+1)` returns non-empty |
| Crossfade visualisation | Overlap between adjacent clips, paints X-shape | Always when two clips overlap |
| Curve-aware automation path | Lane area | When points are present, walks segments + applies curve interpolation matching `automationValueAt` |
| Tension drag handle | Segment midpoints | Non-Hold segments, ≥18 px wide, non-flat (start != end) |
| Take chip "TAKE N / M" | Header, top-right | When the track has > 1 take |
| Loop-region overlay | Wave pane, translucent blue | When `player.hasLoopRegion()` |

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Use `currentLaneParam()` to resolve the active lane | Inline another `toolbar->getParam() switch` — they go out of sync |
| Route automation edits through `automationEditWrapper` | Mutate engine.add/remove directly — breaks undo |
| Pre-cache the click row index via `setClickTrackPresent` | Look up "is this the click row" per paint |

## Accessibility

- Automation points support keyboard previous/next, value nudge, and delete through the EDIT focus model
- Tooltips on the per-row VIEW menu
- Edit cursor doubles as visual focus indicator when no audio is loaded (engineer can drop markers on an empty timeline)

## Known limits

- The class remains large and nested, even after extraction to `EditTrackRow.h`; a later split still requires a clearer row-model/component boundary.
- Drag-edit gestures are mouse-only; tablets are second-class.
- Per-row visibility (Memory Location recall) shows/hides via `setVisible` but doesn't reflow the row above/below — they stay in place.
