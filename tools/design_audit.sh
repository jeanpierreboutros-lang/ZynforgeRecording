#!/bin/bash
# ZynForge Recording design-system audit (ported from ZynForge Live's hook).
# Enforces the FORGE/brand token rules from CLAUDE.md. Exits non-zero on any
# violation; run locally or in CI. Scope: app code, not Theme/ (the token
# layer itself) and not Tests/.
cd "$(dirname "$0")/.." || exit 1
FAIL=0
scope() { grep -rn "$1" Source/UI Source/Audio Source/Network Source/Capture \
          --include='*.cpp' --include='*.h' 2>/dev/null \
          | grep -v 'Source/.*Tests/' | grep -v 'Theme/'; }
check() { # $1 label, $2 matches
  if [ -n "$2" ]; then echo "✗ $1"; echo "$2" | head -5; FAIL=1; else echo "✓ $1"; fi
}
# 1. Raw colour construction outside Theme/ (word-boundary so 'setColour (' doesn't hit)
check "no raw juce::Colour(0x..)/fromRGB outside Theme/" \
  "$(scope 'juce::Colour (0xff\|Colour::fromRGB' )"
# 2. Bare white/black (gloss()/onSignal() are the sanctioned paths)
check "no bare Colours::white/black outside Theme/" \
  "$(scope 'Colours::white\b\|Colours::black\b' | grep -v transparentBlack)"
# 3. Raw font construction (brand::type:: is the path)
check "no raw juce::Font/FontOptions outside Theme/" \
  "$(scope 'juce::Font (juce::FontOptions\|juce::FontOptions ()')"
# 4. Inline withAlpha FLOAT literals -- every opacity must be a brand::alpha::
# token (or a shadow::/gloss helper). 2026-06-16: the ad-hoc catalog is GONE --
# all literals were migrated to named alpha steps (added soft/half/strong), so a
# raw withAlpha(0.NN) now FAILS. Computed/animated alphas use withAlpha((expr))
# and aren't matched by this literal pattern.
check "withAlpha opacities are brand::alpha:: tokens (no raw floats)" \
  "$(scope 'withAlpha (0\.' | grep -vE "alpha::|shadow::|gloss")"
# 5. RoundedRectangle radii must be tokens (micro-radii <= 2.0f exempt per CLAUDE.md)
check "rounded-rect radii are brand::radius tokens (<=2px micro exempt)" \
  "$(scope 'RoundedRectangle' | grep -E ', ([3-9][0-9]*\.?[0-9]*|2\.[1-9])f?\);' | grep -vE 'radius::|brand::')"
# 6. Raw brighten/darken: every tint must route through brand::lift()/sink()
# (the sanctioned seam) so tints are gate-visible + re-themable. Theme/ holds
# the implementation and is out of scope.
check "no raw .brighter()/.darker() outside Theme/ (use brand::lift/sink)" \
  "$(scope '\.brighter (\|\.darker (')"
# 7. Spacing ratchet -- raw-integer reduced() should migrate to brand::space::
# tokens. This doesn't auto-fix existing magic numbers, but it FAILS if the count
# GROWS, so NEW spacing uses tokens. Lower SPACE_CEIL as you migrate the tail.
SPACE_CEIL=169
SPACING=$(scope 'reduced (' | grep -E 'reduced \([0-9]' | grep -v 'brand::space' | wc -l | tr -d ' ')
if [ "$SPACING" -gt "$SPACE_CEIL" ]; then
  echo "✗ raw-int reduced() grew to $SPACING (> $SPACE_CEIL) -- use brand::space:: for new spacing"; FAIL=1
else echo "✓ spacing ratchet: $SPACING raw reduced() <= $SPACE_CEIL (migrate tail to brand::space::)"; fi
[ $FAIL -eq 0 ] && echo "design audit: CLEAN" || echo "design audit: FAILED"
exit $FAIL
