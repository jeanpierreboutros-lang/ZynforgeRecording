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
# 4. Inline withAlpha literals not in the documented BrandColors catalog
CATALOG="0.06f|0.10f|0.14f|0.22f|0.25f|0.32f|0.45f|0.75f|0.78f"
check "withAlpha literals are tokens or catalogued ad-hoc values" \
  "$(scope 'withAlpha (0\.' | grep -vE "alpha::|brand::|shadow::|gloss" | grep -vE "withAlpha \(($CATALOG)\)")"
# 5. RoundedRectangle radii must be tokens (micro-radii <= 2.0f exempt per CLAUDE.md)
check "rounded-rect radii are brand::radius tokens (<=2px micro exempt)" \
  "$(scope 'RoundedRectangle' | grep -E ', ([3-9][0-9]*\.?[0-9]*|2\.[1-9])f?\);' | grep -vE 'radius::|brand::')"
[ $FAIL -eq 0 ] && echo "design audit: CLEAN" || echo "design audit: FAILED"
exit $FAIL
