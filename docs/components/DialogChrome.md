# DialogChrome

`Source/Theme/DialogChrome.h` — the shared chrome + control-styling helpers every in-app dialog paints through. Not a component; a header of free functions so dialogs can't drift apart.

## Description

A set of `inline` paint + style helpers in `namespace zynforge::dialog`. Any custom dialog calls `dialog::paintChrome(g, *this, "TITLE")` from its `paint()` and styles its controls with `dialog::style*`. This gives every dialog the same title bar (muted ember stripe + **forge-mark badge** + upper-cased section title), neutral-grey body, footer divider, and identically-styled combos / editors / labels / buttons / check rows. The `ZynForgeLookAndFeel` stamps the matching forge badge on stock `AlertWindow` prompts, so hand-rolled dialogs and alert prompts read as one family.

## When to use

Every modal that isn't a bare `AlertWindow`. Reach for these instead of re-implementing a title bar or re-colouring a combo. If you find yourself writing a `styleCombo` in a dialog, delete it and use `dialog::styleCombo` (this is exactly the duplication the 2026-06-14 audit removed from `NewSessionDialog`).

## API

| Function | Use |
|---|---|
| `paintChrome(g, host, title)` | One call: background + title bar (badge + text) + footer divider |
| `paintBackground / paintTitle / paintFooterDivider` | The three pieces, if a dialog needs them separately |
| `drawForgeBadge(g, rect)` | The forge-mark glyph alone (title bar uses it; mirrors the prompt badge) |
| `bodyBounds(host) / footerBounds(host)` | Working area between title bar and footer; footer button row |
| `styleFieldLabel(label)` | Secondary-text caption above an input |
| `styleTextEditor(ed) / styleCombo(cb)` | Recessed dark field with edge outline |
| `stylePrimary(btn) / styleSecondary(btn)` | Accent action / neutral cancel button |
| `styleCheckRow(toggle)` | Marks a `ToggleButton` with `checkRowKey` so the LookAndFeel paints a yes/no choice as a tick box + label, not a pill |
| `primeNameEditor(aw, id, okResultCode = 1)` | Select-all + focus + Return-fires-OK on an AlertWindow text field; Return exits with `okResultCode` |

## Chrome dimensions (single source of truth)

| Const | Value | Meaning |
|---|---|---|
| `titleH` | 44 | Title-bar height — **body must start below this** |
| `footerH` | 60 | Footer button band |
| `stripeW` | 3 | Muted ember (`selectFill`) left accent |
| `btnPrimary` | 110 | Footer primary button width |
| `btnSecond` | 90 | Footer secondary button width |
| `btnH` | 28 | Footer button height |

The title bar is inset `space::md` each side: stripe, `space::md` gap, a 26 px badge, `space::sm` gap, then the title. `footerBounds(host)` is the bottom `footerH` band reduced by `space::md`.

A dialog that lays its body out from `y = 0` (instead of below `titleH`) collides with the title — the 2026-06-14 `NewSessionDialog` "Name overlaps NEW SESSION" bug. Use `bodyBounds(host)` or trim `titleH` off the top in `resized()`.

## States

| State | Visual |
|---|---|
| Default control | `bgDeep` field, `edge` outline |
| Combo arrow | `textTertiary` |
| Focused editor | `selectFill` outline; text highlight `selectFill` at `alpha::half` |
| Primary button | `selectFill` fill, `onSignal(selectFill)` foreground |
| Secondary button | `controlOff` fill, `textPrimary` |

## Tokens used

- **Colours**: `brand::bgElevated` (flat solid body, via `verticalGradient`, which now returns a solid fill), `brand::selectFill` (stripe, focus, primary), `brand::controlBg` (badge plate) with a `gloss(0.10)` outline, `brand::structuralForge()` (badge hex at 0.12 fill / 0.55 stroke, chevrons solid), `brand::bgDeep` / `edge` (fields, footer divider), `brand::controlOff` (secondary), `brand::textPrimary` / `textSecondary` / `textTertiary`
- **Typography**: `brand::type::sectionTitle()` (title), `brand::type::uiBody()` (field labels)
- **Spacing**: `brand::space::md` / `sm`
- **Radius**: `brand::radius::md` (badge)

## Do's and Don'ts

| ✅ Do | ❌ Don't |
|---|---|
| `paintChrome` for every custom dialog | Hand-roll a title bar — you'll lose the badge + dimensions |
| Style controls with `dialog::style*` | Define a private `styleCombo` in the dialog — that's how dialogs drift |
| Start the body below `titleH` (`bodyBounds`) | Lay out from `y=0` — the first field collides with the title |
| Keep prompts neutral grey (`bgElevated`) | Use blue-tinted `bgPanel`/`bgDeep` for the body |
