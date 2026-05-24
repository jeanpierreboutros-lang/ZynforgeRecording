# Toast

`Source/UI/Toast.h` — non-modal feedback pill in the bottom-right corner of the main window.

## Description

A queued, time-limited pill that fades in, holds, fades out. Used to surface every `showStatus(...)` call so engineers see calm acknowledgements without modal interruption. Sits above all UI but never blocks input — clicking through is fine.

## When to use

One instance per `MainComponent`, laid out at the bottom-right with `(width 320, height 56)` and an `18 px` margin from the window edges. Host calls `toast.push(message, kind)` from anywhere; the toast handles queue ordering and timing on its own.

## API

```cpp
enum class Kind { Info, Warning };
void push (juce::String message, Kind kind = Kind::Info);
```

| Param | Type | Notes |
|---|---|---|
| `message` | `juce::String` | One-line text. Multi-line is allowed but rare |
| `kind` | `Kind` | `Info` (`accentStatus`) or `Warning` (`alertAmber`); colours the left edge |

## Lifecycle

Each pushed message has four phases:

| Phase | Duration | Behaviour |
|---|---|---|
| **In** | `brand::motion::quickFadeMs` (200 ms) | Alpha 0 → 1, slight slide-up from below |
| **Hold** | `brand::motion::toastHoldMs` (2800 ms) | Fully opaque, no movement |
| **Out** | `brand::motion::fadeOutMs` (320 ms) | Alpha 1 → 0 |
| **Idle** | — | Component invisible, timer stopped |

While a toast is in any non-Idle phase, the timer runs at 60 Hz (kTickMs = 16 ms) to animate alpha smoothly. When the queue empties, the timer stops — zero background CPU cost.

## Queue semantics

Multiple `push()` calls during a single display cycle queue up. The next message appears after the current one's `Out` phase completes. Queue is FIFO. No deduplication — pushing the same message twice shows it twice.

## States

| State | Visual | Notes |
|---|---|---|
| Hidden | Component invisible | Timer stopped |
| Fading in | Pill scales up + alpha rising | Triggered by `push()` if no current toast |
| Holding | Pill at full alpha | Time-bound; next push queues |
| Fading out | Pill alpha falling | Followed by next queue entry or Hidden |

## Visual

- Rounded pill (`brand::radius::xl`)
- `brand::bgElevated` background with subtle gradient
- 4 px left edge in the `Kind` colour (`accentStatus` for Info, `alertAmber` for Warning)
- `brand::type::uiBody()` text, centred vertically

## Tokens used

- **Colours**: `brand::accentStatus` (Info edge), `brand::alertAmber` (Warning edge), `brand::bgElevated` (pill body), `brand::textPrimary` (text)
- **Typography**: `brand::type::uiBody()`
- **Spacing**: hardcoded 18 px margin from window edges (audit candidate — could become `brand::space::xl + 2`)
- **Radius**: `brand::radius::xl`
- **Motion**: `brand::motion::quickFadeMs` / `fadeOutMs` / `toastHoldMs`

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Use Toast for calm acknowledgements (`"Saved"`, `"Loaded"`, `"5 cues stored"`) | Use Toast for blocking decisions — use an AlertWindow / DialogChrome dialog instead |
| Use `Warning` kind sparingly — engineer should pause when they see amber | Spam `Warning` for routine non-issues |
| Trust the queue — push freely, ordering is preserved | Implement your own queueing on top — Toast already handles concurrent pushes |
| Keep messages to one line, ~60 characters | Push paragraphs — Toast clips at the pill width |
