# Toast

`Source/UI/Toast.h` — non-modal feedback pill in the bottom-right corner of the main window.

## Description

A queued, time-limited pill that fades in, holds, fades out. Used to surface every non-empty `showStatus(...)` call so engineers see calm acknowledgements without modal interruption. Sits above all UI (`setAlwaysOnTop`) but never blocks input — it takes no mouse clicks, so clicking through is fine.

`showStatus` picks the kind from the wording: a leading `!` or failure words ("error", "disk full", "could not", "blocked", "failed", "not saved", …) give `Error`; "can't", "fail", "mismatch", "timed out", "partial" or "stop record" give `Warning`; anything else is `Info`. `Success` is only used by direct `show()` calls.

## When to use

One instance per `MainComponent`, given a slot at the bottom-right of `(width 320, height 56)` with an `18 px` margin from the window edges. The slot's width and bottom-right corner hold; a message that needs more lines grows the toast upward, up to 200 px. Host calls `toast.show(message, kind)` on the message thread; the toast handles queue ordering and timing on its own.

## API

```cpp
enum class Kind { Info, Success, Warning, Error };
static constexpr int kMaxWaiting = 3;
void show    (const juce::String& message, Kind kind = Kind::Info);
void showNow (const juce::String& message, Kind kind = Kind::Info);
const juce::String& getCurrentMessage() const noexcept;
int  waitingCount() const noexcept;                  // messages behind the one showing
bool isWaiting (const juce::String& message) const;
bool showsWholeMessage() const;                       // nothing of the showing message cut off
```

`show` appends to the queue (an empty message is ignored). `waitingCount`, `isWaiting` and `showsWholeMessage` are for tests. `showNow` is for a prompt that would be out of date by the time the queue reached it: it replaces what is showing at once and goes ahead of everything waiting (the "Tap STOP again to end the recording" warning uses it). `getCurrentMessage` returns the message most recently taken off the queue; it still holds the last one after the pill hides.

| Param | Type | Notes |
|---|---|---|
| `message` | `juce::String` | Display text, wrapped by word. ` -- ` is drawn as an em dash |
| `kind` | `Kind` | `Info` (grey body, teal stripe), `Success` (green), `Warning` (amber), `Error` (red); selects body and edge colours |

## Lifecycle

Each pushed message has four phases:

| Phase | Duration | Behaviour |
|---|---|---|
| **In** | `brand::motion::quickFadeMs` (200 ms) | Alpha 0 → 1 |
| **Hold** | `brand::motion::toastHoldMs` (2800 ms), plus 1200 ms for each line past the first | Fully opaque, no movement |
| **Out** | `brand::motion::fadeOutMs` (320 ms) | Alpha 1 → 0 |
| **Idle** | — | Component invisible, timer stopped |

While a toast is in any non-Idle phase, the timer runs at 60 Hz (kTickMs = 16 ms) to animate alpha smoothly. When the queue empties, the timer stops — zero background CPU cost.

## Queue semantics

Multiple `show()` calls during a single display cycle queue up. The next message appears after the current one's `Out` phase completes. Queue is FIFO, except that `showNow` jumps to the front and cuts the current message short.

Two notices of the same kind are one when they differ only in their numbers (and the plural those bring): "3 strips selected", then "4 strips selected". The newer takes the older's place, whether that one is showing (its hold starts again) or waiting. At most `kMaxWaiting` (3) wait; past that the oldest of the least urgent goes (`Info`/`Success` before `Warning` before `Error`), so neutral news never pushes out an error.

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
- `brand::type::uiLabel()` text in `textPrimary`, 14 px from the left and 10 px from the right, wrapped by word and centred vertically in its area

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
| Keep messages short; a long one wraps and grows the toast | Push paragraphs — the toast stops growing at 200 px |
