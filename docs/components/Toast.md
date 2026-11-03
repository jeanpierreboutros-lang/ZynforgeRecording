# Toast

`Source/UI/Toast.h` — non-modal feedback pill in the bottom-right corner of the main window.

## Description

A queued, time-limited pill that fades in, holds, fades out. Used to surface every non-empty `showStatus(...)` call so engineers see calm acknowledgements without modal interruption. Sits above all UI (`setAlwaysOnTop`) but never blocks input — it takes no mouse clicks, so clicking through is fine.

`showStatus` picks the kind from the wording: a leading `!` or failure words ("error", "disk full", "could not", "blocked", "failed", "not saved", …) give `Error`; "can't", "fail", "mismatch", "timed out", "partial" or "stop record" give `Warning`; anything else is `Info`. `Success` is only used by direct `show()` calls.

## When to use

One instance per `MainComponent`, laid out at the bottom-right with `(width 320, height 56)` and an `18 px` margin from the window edges. Host calls `toast.show(message, kind)` on the message thread; the toast handles queue ordering and timing on its own.

## API

```cpp
enum class Kind { Info, Success, Warning, Error };
void show    (const juce::String& message, Kind kind = Kind::Info);
void showNow (const juce::String& message, Kind kind = Kind::Info);
const juce::String& getCurrentMessage() const noexcept;
```

`show` appends to the queue. `showNow` is for a prompt that would be out of date by the time the queue reached it: it replaces what is showing at once and goes ahead of everything waiting (the "Tap STOP again to end the recording" warning uses it). `getCurrentMessage` returns the message most recently taken off the queue; it still holds the last one after the pill hides.

| Param | Type | Notes |
|---|---|---|
| `message` | `juce::String` | Single-line display text; long messages end in an ellipsis at the available width. ` -- ` is drawn as an em dash |
| `kind` | `Kind` | `Info` (grey body, teal stripe), `Success` (green), `Warning` (amber), `Error` (red); selects body and edge colours |

## Lifecycle

Each pushed message has four phases:

| Phase | Duration | Behaviour |
|---|---|---|
| **In** | `brand::motion::quickFadeMs` (200 ms) | Alpha 0 → 1 |
| **Hold** | `brand::motion::toastHoldMs` (2800 ms) | Fully opaque, no movement |
| **Out** | `brand::motion::fadeOutMs` (320 ms) | Alpha 1 → 0 |
| **Idle** | — | Component invisible, timer stopped |

While a toast is in any non-Idle phase, the timer runs at 60 Hz (kTickMs = 16 ms) to animate alpha smoothly. When the queue empties, the timer stops — zero background CPU cost.

## Queue semantics

Multiple `show()` calls during a single display cycle queue up. The next message appears after the current one's `Out` phase completes. Queue is FIFO, except that `showNow` jumps to the front and cuts the current message short. No deduplication — pushing the same message twice shows it twice.

## States

| State | Visual | Notes |
|---|---|---|
| Hidden | Component invisible | Timer stopped |
| Fading in | Pill alpha rising | Triggered by `show()` if no current toast, or by `showNow()` at any time |
| Holding | Pill at full alpha | Time-bound; next push queues |
| Fading out | Pill alpha falling | Followed by next queue entry or Hidden |

## Visual

- Rounded pill (`brand::radius::lg`), inset 4 px inside the toast bounds, over a soft `shadow::elev3()` drop shadow 2 px below
- Flat solid body: `bgElevated` for Info, kind accent sunk 30 % (`sink(accent, 0.30)`) for Success/Warning/Error
- 4 px left edge in the `Kind` colour (`featureEngaged`, `accentPlay`, `alertAmber`, or `accentRecord`), `radius::sm`
- 1 px outline: the body lifted 45 % at `alpha::scrim`
- `brand::type::uiLabel()` text in `textPrimary`, 14 px from the left, centred vertically

## Tokens used

- **Colours**: `brand::featureEngaged` / `accentPlay` / `alertAmber` / `accentRecord` (kind accents), `brand::bgElevated` (Info body), `brand::textPrimary` (text), `brand::shadow::elev3()` (drop shadow)
- **Alpha**: `brand::alpha::scrim` (outline)
- **Typography**: `brand::type::uiLabel()`
- **Spacing**: hardcoded 18 px margin from window edges (audit candidate — could become `brand::space::xl + 2`)
- **Radius**: `brand::radius::lg` (pill), `brand::radius::sm` (stripe)
- **Motion**: `brand::motion::quickFadeMs` / `fadeOutMs` / `toastHoldMs`

Hard capture failures use Error feedback alongside the latched health/report state; an expiring toast is not the sole record of a failed take.

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| Use Toast for calm acknowledgements (`"Saved"`, `"Loaded"`, `"5 cues stored"`) | Use Toast for blocking decisions — use an AlertWindow / DialogChrome dialog instead |
| Use `Warning` kind sparingly — engineer should pause when they see amber | Spam `Warning` for routine non-issues |
| Queue messages with `show()` on the message thread | Call the component directly from worker/audio threads; marshal to the UI first |
| Keep messages to one line, ~60 characters | Push paragraphs — Toast clips at the pill width |
