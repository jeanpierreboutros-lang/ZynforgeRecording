#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>

namespace zynforge::brand
{
    // ── Tokens taken byte-for-byte from the shared ZynForge design
    // system (DESIGN.md authored 2026-05-21). Live and Recording must
    // stay aligned here so the family reads as one product.

    // ── Brand mark ────────────────────────────────────────────────────────
    inline const auto brandOrange = juce::Colour::fromRGB (0xff, 0x77, 0x33);  // forge accent
    inline const auto brandDeep   = juce::Colour::fromRGB (0x0a, 0x0a, 0x0f);  // navy ground

    // ── Background levels (3-step) ────────────────────────────────────────
    inline const auto bgDeep      = juce::Colour::fromRGB (0x0d, 0x0d, 0x12);  // deepest plane
    inline const auto bgPanel     = juce::Colour::fromRGB (0x12, 0x13, 0x16);  // panel surface
    inline const auto bgStrip     = juce::Colour::fromRGB (0x1e, 0x1e, 0x1e);  // main app surface
    inline const auto bgElevated  = juce::Colour::fromRGB (0x30, 0x30, 0x30);  // popups, dialogs

    // ── Control chrome ────────────────────────────────────────────────────
    inline const auto edge          = juce::Colour::fromRGB (0x26, 0x28, 0x2e);
    inline const auto borderSubtle  = juce::Colour::fromRGB (0x3a, 0x3a, 0x3a);
    inline const auto borderBright  = juce::Colour::fromRGB (0x5a, 0x5a, 0x5a);

    // Background for text editors, combo bodies, and any other input
    // chrome that wants to read as a recessed slot on the panel.
    // Deliberately deeper than bgDeep so it sinks below the surface.
    inline const auto inputBg       = juce::Colour::fromRGB (0x00, 0x00, 0x00);

    // Loop-region / selection band -- Pro Tools-blue-adjacent.
    // Used as a low-alpha fill + medium-alpha edge on the EDIT view.
    inline const auto accentEdit    = juce::Colour::fromRGB (0x3a, 0x90, 0xe0);

    // ── Text (4-step, all WCAG-AA against bg-surface) ─────────────────────
    inline const auto textPrimary   = juce::Colour::fromRGB (0xff, 0xff, 0xff);
    inline const auto textSecondary = juce::Colour::fromRGB (0xb8, 0xc2, 0xcc);
    inline const auto textTertiary  = juce::Colour::fromRGB (0x7a, 0x8a, 0x9a);
    inline const auto textMuted     = juce::Colour::fromRGB (0x55, 0x60, 0x6c);  // genuine disabled state

    // ── Semantic signal accents ───────────────────────────────────────────
    inline const auto accentRecord    = juce::Colour::fromRGB (0xff, 0x3b, 0x3b);  // record / danger
    inline const auto accentPlay      = juce::Colour::fromRGB (0x4a, 0xd8, 0x78);  // transport active
    inline const auto accentVS        = juce::Colour::fromRGB (0xff, 0xb4, 0x2a);  // virtual soundcheck
    inline const auto accentStatus    = juce::Colour::fromRGB (0x5d, 0xd8, 0x7a);  // session-clock green
    inline const auto accentSolo      = juce::Colour::fromRGB (0xff, 0xd6, 0x4d);  // solo yellow
    inline const auto alertAmber      = juce::Colour::fromRGB (0xe8, 0x98, 0x27);
    inline const auto engagedAmber    = juce::Colour::fromRGB (0xff, 0x9e, 0x2c);  // BYPASS / LIVE / LOCK
    inline const auto featureEngaged  = juce::Colour::fromRGB (0x33, 0xc4, 0xd4);  // cool-teal toggle-on

    // ── Meter colours (deliberately duller than accents) ──────────────────
    inline const auto meterGreen   = juce::Colour::fromRGB (0x3c, 0xb8, 0x78);
    inline const auto meterAmber   = juce::Colour::fromRGB (0xf0, 0xc0, 0x60);
    inline const auto meterRed     = juce::Colour::fromRGB (0xdc, 0x38, 0x38);
    inline const auto meterIdle    = juce::Colour::fromRGB (0x2c, 0x30, 0x3a);   // visible against bgDeep

    // ── Transport / control-button chrome (3 hover states) ────────────────
    inline const auto controlBg          = juce::Colour::fromRGB (0x2a, 0x2c, 0x30);
    inline const auto controlBgHover     = juce::Colour::fromRGB (0x36, 0x38, 0x3e);
    inline const auto controlBgDown      = juce::Colour::fromRGB (0x3e, 0x40, 0x46);
    inline const auto controlBorder      = juce::Colour::fromRGB (0x14, 0x15, 0x18);

    // ── Fader thumb body greys ────────────────────────────────────────────
    inline const auto faderThumbHi    = juce::Colour::fromRGB (0x33, 0x35, 0x3c);
    inline const auto faderThumbLo    = juce::Colour::fromRGB (0x1c, 0x1e, 0x23);
    inline const auto faderThumbEdge  = juce::Colour::fromRGB (0x55, 0x57, 0x60);
    inline const auto faderThumbGrip  = juce::Colour::fromRGB (0xa0, 0xa3, 0xad);

    // ── Strip-colour picker neutral presets ───────────────────────────────
    inline const auto swatchSlate    = juce::Colour::fromRGB (0x40, 0x45, 0x50);
    inline const auto swatchGraphite = juce::Colour::fromRGB (0x55, 0x55, 0x5a);

    // ── Signal semantics -- KEEP THESE MAPPINGS STABLE ACROSS THE APP ──
    //
    //   record arm     → red        (accentRecord)
    //   input monitor  → green      (accentPlay)
    //   mute           → orange     (brandOrange)
    //   solo           → yellow     (accentSolo)
    //   virtual sndchk → warm-amber (accentVS) -- lights the VSC button
    //   stream / send  → cool-teal  (featureEngaged)
    //   record danger  → red glow   (accentRecord)
    //
    // Use the named accessors below, not the raw colour tokens, so a
    // future re-skin only touches this header.
    inline juce::Colour signalRecord()      { return accentRecord; }
    inline juce::Colour signalMonitor()     { return accentPlay;   }
    inline juce::Colour signalMute()        { return brandOrange;  }
    inline juce::Colour signalSolo()        { return accentSolo;   }
    inline juce::Colour signalVsc()         { return accentVS;     }
    inline juce::Colour signalStream()      { return featureEngaged; }
    inline juce::Colour signalPlayhead()    { return accentPlay;   }
    // Armed-but-not-rolling: BigClock border, future "ready" states.
    // Maps to engagedAmber (the BYPASS / LIVE / LOCK token) -- it's
    // semantically a 'primed but not active' colour. Keeps brandOrange
    // free for its two documented uses (signalMute + brand assertion).
    inline juce::Colour signalArmedReady()  { return engagedAmber; }
    // EDIT tools active selection -- cool-teal so it doesn't collide
    // with any of the signal colours above.
    inline juce::Colour toolActive()        { return featureEngaged; }

    // ── Alpha tokens for layered overlays ────────────────────────────
    // Anything painted on top of the waveform / lane should pick one
    // of these. No more inline magic 0.18 / 0.40 / 0.85 numbers.
    namespace alpha
    {
        // Six-step opacity scale. Two new mid values (dimmed / muted)
        // and a near-opaque (bold) cover the most common ad-hoc
        // alpha literals in the codebase (counted by audit: 0.35,
        // 0.55-0.60, 0.95). The 3-step (subtle/ghost/prominent)
        // semantic intent is preserved -- the new values just give
        // engineers a named slot instead of inventing 0.35 inline.
        inline constexpr float subtle    = 0.18f;  // grid lines, off-beat ticks
        inline constexpr float dimmed    = 0.35f;  // background washes, mute scrim edges
        inline constexpr float ghost     = 0.40f;  // downbeats, secondary cues
        inline constexpr float scrim     = 0.45f;  // zebra-row tints, surface-dimming washes
        inline constexpr float muted     = 0.55f;  // mid-contrast overlays
        inline constexpr float prominent = 0.85f;  // playhead, focus highlight
        inline constexpr float bold      = 0.95f;  // near-opaque text on translucent bg

        // ── Ad-hoc values that DON'T fit the scale ──────────────
        // Audit catalog of every withAlpha(...) literal that doesn't
        // map to a named token above, with the reason it was picked.
        // Document new ad-hoc alphas HERE instead of leaving them
        // inline as magic numbers; if a new use case repeats one of
        // these often, promote it to a named token in the scale.
        //
        //   0.06  -- BigClock gloss highlight top, intentionally near-
        //            invisible to read as light source, not bg colour
        //   0.10  -- crossfade band fill (EditPage), the lightest
        //            possible band that's still visible against the
        //            waveform pane's mid-grey
        //   0.14  -- BigClock Mode::Playing background tint, a touch
        //            lighter than gloss-bg so the panel reads "active
        //            but not recording"
        //   0.22  -- mute scrim wash (EditPage TrackRow), strong
        //            enough to drop the muted clip below the rest
        //            without obscuring waveform shape
        //   0.25  -- timeline-strip player-position fade trail,
        //            picked to leave a visible streak without
        //            stealing attention from the playhead
        //   0.32  -- engaged-control surround (PUNCH toggle when on),
        //            mid-strength bloom around the toggle to read
        //            "this is what's controlling writes right now"
        //   0.45  -- PROMOTED: the surface-scrim use (zebra-row tints on
        //            the help + patch tables) is now alpha::scrim. The
        //            remaining 0.45 literals are component-internal
        //            accents, NOT surface scrims, so they stay ad-hoc:
        //              · Toast inner glow (base.brighter.withAlpha) --
        //                lift on the toast's own border, not a wash
        //              · LedMeter peak-lit segment -- a dimmed copy of
        //                the segment's own meter colour, tied to meter
        //                geometry rather than a layered overlay
        //            A single named token across all three would lie
        //            about intent, which is why only the scrim promoted.
        //   0.75  -- EditPage selection-band edge contrast (selector
        //            tool drag-region). Strong stroke without being
        //            opaque -- still lets the waveform show through
        //   0.78  -- EditPage automation value-label backing pill. One
        //            notch below prominent (0.85): opaque enough to keep
        //            the dB readout legible over a busy automation curve
        //            / waveform, but still hints at the lane underneath
    }

    // ── Elevation / shadow scale ──────────────────────────────────────
    // Replaces hand-rolled juce::Colours::black.withAlpha (0.xxf) calls.
    // Three levels, picked by perceived lift: strips/chips < fader caps
    // /dialogs < modal popovers.
    namespace shadow
    {
        inline juce::Colour elev1() { return juce::Colours::black.withAlpha (0.25f); }
        inline juce::Colour elev2() { return juce::Colours::black.withAlpha (0.40f); }
        inline juce::Colour elev3() { return juce::Colours::black.withAlpha (0.55f); }
    }

    // ── Specular / scrim white-overlay helper ─────────────────────────
    // The one sanctioned use of pure white in chrome: a top-edge gloss
    // highlight or a light scrim that reads as a *light source*, not a
    // brand colour. Centralises the `Colours::white.withAlpha(a)` idiom
    // (fader caps, BigClock glass, channel-strip chip edge) so a re-skin
    // has a single seam. Use this instead of inlining Colours::white.
    inline juce::Colour gloss (float a) noexcept
    {
        return juce::Colours::white.withAlpha (a);
    }

    // ── Text-on-accent helper ─────────────────────────────────────────
    // Picks a legible foreground for a saturated button / chip background.
    // Bright accents (yellow Solo, green Play, orange) want black; deep
    // accents (red Record, teal Stream) want white. One call, one rule.
    inline juce::Colour onSignal (juce::Colour bg) noexcept
    {
        return bg.getPerceivedBrightness() > 0.55f
                   ? juce::Colours::black
                   : textPrimary;
    }

    // ── Personality wash colours -- INS 1-8 ────────────────────────────────
    //
    // Pass 2 (2026-05-23): the moss / olive / violet / teal swatches
    // were still getting absorbed into bgDeep at XS strip widths.
    // Saturation + luminance bumped ~12-15% on each so every personality
    // reads cleanly even in a 256-strip layout. The hue is preserved.
    inline const std::array<juce::Colour, 8> personality {
        juce::Colour::fromRGB (0x4c, 0x6c, 0x88),  // INS 1 -- dusty blue
        juce::Colour::fromRGB (0x52, 0x8a, 0x6a),  // INS 2 -- moss      (bumped)
        juce::Colour::fromRGB (0x80, 0x76, 0x46),  // INS 3 -- olive     (bumped)
        juce::Colour::fromRGB (0x76, 0x5a, 0x8c),  // INS 4 -- violet    (bumped)
        juce::Colour::fromRGB (0x78, 0x3c, 0x4e),  // INS 5 -- wine
        juce::Colour::fromRGB (0x3c, 0x82, 0x86),  // INS 6 -- teal      (bumped)
        juce::Colour::fromRGB (0x80, 0x5c, 0x32),  // INS 7 -- amber
        juce::Colour::fromRGB (0x7a, 0x6a, 0x38),  // INS 8 -- mustard
    };

    // Default strip colour. Channels are a neutral grey when first added --
    // the engineer colours the ones that matter from the swatch picker. (The
    // per-index `personality` palette above is kept for reference / future
    // use; the default no longer auto-assigns it.)
    inline const auto stripDefaultGrey = juce::Colour::fromRGB (0x53, 0x55, 0x5b);

    inline juce::Colour stripColour (int /*index*/) noexcept
    {
        return stripDefaultGrey;
    }

    // ── Gradient helpers -- every painted surface should use these ─────────
    // Top-to-bottom vertical gradient for buttons / pills / strip backs.
    inline juce::ColourGradient verticalGradient (juce::Colour base,
                                                  juce::Rectangle<float> r,
                                                  float lift   = 0.10f,
                                                  float shadow = 0.20f) noexcept
    {
        return juce::ColourGradient (base.brighter (lift),   r.getX(), r.getY(),
                                     base.darker  (shadow),  r.getX(), r.getBottom(),
                                     false);
    }
}
