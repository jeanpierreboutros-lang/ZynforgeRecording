#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>
#include <atomic>

#include "ForgeTokens.h"

namespace zynforge::brand
{
    // ── Values come from the FAMILY token source of truth: ZynForgeBrand/
    // tokens.json -> generated ForgeTokens.h (vendored next to this file).
    // This header defines Recording's juce::Colour constants FROM forge::
    // values and keeps the helpers; edit tokens.json + regenerate to retune
    // the whole ZynForge family at once. See ZynForgeBrand/FORGE.md.

    // ── Brand mark ────────────────────────────────────────────────────────
    inline const auto brandOrange = juce::Colour (forge::brandOrange);  // forge accent
    inline const auto brandDeep = juce::Colour (forge::brandDeep);  // navy ground

    // ── Background levels (3-step) ────────────────────────────────────────
    inline const auto bgDeep = juce::Colour (forge::bgDeep);  // deepest plane
    inline const auto bgPanel = juce::Colour (forge::bgPanel);  // panel surface
    inline const auto bgStrip = juce::Colour (forge::bgStrip);  // main app surface
    inline const auto bgElevated = juce::Colour (forge::bgElevated);  // popups, dialogs

    // ── Control chrome ────────────────────────────────────────────────────
    inline const auto edge = juce::Colour (forge::edge);
    inline const auto borderSubtle = juce::Colour (forge::borderSubtle);
    inline const auto borderBright = juce::Colour (forge::borderBright);

    // Background for text editors, combo bodies, and any other input
    // chrome that wants to read as a recessed slot on the panel.
    // Deliberately deeper than bgDeep so it sinks below the surface.
    inline const auto inputBg = juce::Colour (forge::inputBg);

    // Loop-region / selection band -- Pro Tools-blue-adjacent.
    // Used as a low-alpha fill + medium-alpha edge on the EDIT view.
    inline const auto accentEdit = juce::Colour (forge::accentEdit);

    // ── Text (4-step, all WCAG-AA against bg-surface) ─────────────────────
    inline const auto textPrimary = juce::Colour (forge::textPrimary);
    inline const auto textSecondary = juce::Colour (forge::textSecondary);
    inline const auto textTertiary = juce::Colour (forge::textTertiary);
    inline const auto textMuted = juce::Colour (forge::textMuted);  // genuine disabled state

    // ── Semantic signal accents ───────────────────────────────────────────
    inline const auto accentRecord = juce::Colour (forge::accentRecord);  // record / danger
    inline const auto accentPlay = juce::Colour (forge::accentPlay);  // transport active
    inline const auto accentVS = juce::Colour (forge::accentVS);  // virtual soundcheck
    inline const auto accentStatus = juce::Colour (forge::accentStatus);  // session-clock green
    inline const auto accentSolo = juce::Colour (forge::accentSolo);  // solo yellow
    inline const auto alertAmber = juce::Colour (forge::alertAmber);
    inline const auto engagedAmber = juce::Colour (forge::engagedAmber);  // BYPASS / LIVE / LOCK
    inline const auto featureEngaged = juce::Colour (forge::featureEngaged);  // cool-teal toggle-on

    // ── Meter colours (deliberately duller than accents) ──────────────────
    inline const auto meterGreen = juce::Colour (forge::meterGreen);
    inline const auto meterAmber = juce::Colour (forge::meterAmber);
    inline const auto meterRed = juce::Colour (forge::meterRed);
    inline const auto meterIdle = juce::Colour (forge::meterIdle);   // visible against bgDeep

    // ── Forge-heat meter ramp (brand signature) ───────────────────────────
    // ZynForge's identity: cold steel that runs HOT where the signal lives.
    // The safe zone stays green (live-sound muscle memory), but the hot zone
    // climbs like heated metal -- ember → bright forge-orange → white-hot at
    // clip. Loud literally glows like steel in a forge.
    inline const auto meterEmber = juce::Colour (forge::meterEmber);  // ember (heating up)
    inline const auto meterHot = juce::Colour (forge::meterHot);  // bright forge orange
    inline const auto meterWhiteHot = juce::Colour (forge::meterWhiteHot);  // white-hot (clip-adjacent)

    // Meter colour for a height fraction (0 = floor, 1 = top). Centralises the
    // ramp so the gradient + segmented paths agree.
    inline juce::Colour meterHeatAt (float frac) noexcept
    {
        if (frac > forge::heat_whiteHotAbove) return meterWhiteHot;   // white-hot tip
        if (frac > forge::heat_hotAbove)      return meterHot;        // bright forge orange
        if (frac > forge::heat_emberAbove)    return meterEmber;      // ember
        return meterGreen;                         // safe zone
    }

    // ── Transport / control-button chrome (3 hover states) ────────────────
    inline const auto controlBg = juce::Colour (forge::controlBg);
    inline const auto controlBgHover = juce::Colour (forge::controlBgHover);
    inline const auto controlBgDown = juce::Colour (forge::controlBgDown);
    inline const auto controlBorder = juce::Colour (forge::controlBorder);

    // ── Fader thumb body greys ────────────────────────────────────────────
    inline const auto faderThumbHi = juce::Colour (forge::faderThumbHi);
    inline const auto faderThumbLo = juce::Colour (forge::faderThumbLo);
    inline const auto faderThumbEdge = juce::Colour (forge::faderThumbEdge);
    inline const auto faderThumbGrip = juce::Colour (forge::faderThumbGrip);

    // ── Strip-colour picker neutral presets ───────────────────────────────
    inline const auto swatchSlate = juce::Colour (forge::swatchSlate);
    inline const auto swatchGraphite = juce::Colour (forge::swatchGraphite);

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
        // Low-end chrome steps (promoted 2026-06-14 from the ad-hoc catalog --
        // these were the most-repeated sub-0.18 literals: forge-glyph fills,
        // hammered scrims, light value-pill washes).
        inline constexpr float faint    = forge::alpha_faint;  // lightest visible fill (crossfade band, gloss)
        inline constexpr float chrome    = forge::alpha_chrome;  // forge-mark glyph fill, faint structural tint
        inline constexpr float edgeSoft    = forge::alpha_edgeSoft;  // hammered-steel scrim, soft edge wash
        inline constexpr float subtle    = forge::alpha_subtle;  // grid lines, off-beat ticks
        inline constexpr float wash    = forge::alpha_wash;  // mid value-pill backing, structural seam wash
        inline constexpr float dimmed    = forge::alpha_dimmed;  // background washes, mute scrim edges
        inline constexpr float ghost    = forge::alpha_ghost;  // downbeats, secondary cues
        inline constexpr float scrim    = forge::alpha_scrim;  // zebra-row tints, surface-dimming washes
        inline constexpr float muted    = forge::alpha_muted;  // mid-contrast overlays
        inline constexpr float prominent    = forge::alpha_prominent;  // playhead, focus highlight
        inline constexpr float bold    = forge::alpha_bold;  // near-opaque text on translucent bg

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
        inline juce::Colour elev1() { return juce::Colours::black.withAlpha (forge::shadow_elev1); }
        inline juce::Colour elev2() { return juce::Colours::black.withAlpha (forge::shadow_elev2); }
        inline juce::Colour elev3() { return juce::Colours::black.withAlpha (forge::shadow_elev3); }
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

    // ── Tint helpers (the sanctioned brighten / darken seam) ──────────
    // Every lightening / darkening of a colour routes through these instead
    // of inlining juce::Colour::brighter()/darker() at the call site. Before
    // 2026-06-14 there were 60+ raw `.brighter(0.xx)` / `.darker(0.xx)` calls
    // -- a hover lift, an armed-edge brighten, a deboss darken -- each with an
    // ad-hoc amount the design gate couldn't see. Centralising them here makes
    // tints (a) gate-visible (raw brighter/darker is now banned outside Theme/)
    // and (b) re-themable from one seam. `amt` is the same 0..1 JUCE takes, so
    // the migration is visually identical; the win is the single chokepoint.
    inline juce::Colour lift (juce::Colour c, float amt) noexcept { return c.brighter (amt); }
    inline juce::Colour sink (juce::Colour c, float amt) noexcept { return c.darker  (amt); }

    // Common, named tint amounts -- reach for these so repeated lifts share a
    // meaning instead of each picking a number. Off-amount one-offs may still
    // pass a literal to lift()/sink().
    namespace tint
    {
        inline constexpr float faint = 0.06f;  // barely-there surface separation (zebra plates, panel lift)
        inline constexpr float hover = 0.20f;  // pointer-over lift on a control
        inline constexpr float edge  = 0.30f;  // chip / swatch edge highlight, colour-key brighten
        inline constexpr float deep  = 0.40f;  // strong rim / hot-edge lift, deboss sink
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
        juce::Colour (forge::personality[0]),  // INS 1 -- dusty blue
        juce::Colour (forge::personality[1]),  // INS 2 -- moss      (bumped)
        juce::Colour (forge::personality[2]),  // INS 3 -- olive     (bumped)
        juce::Colour (forge::personality[3]),  // INS 4 -- violet    (bumped)
        juce::Colour (forge::personality[4]),  // INS 5 -- wine
        juce::Colour (forge::personality[5]),  // INS 6 -- teal      (bumped)
        juce::Colour (forge::personality[6]),  // INS 7 -- amber
        juce::Colour (forge::personality[7]),  // INS 8 -- mustard
    };

    // Default strip colour. Channels open with a neutral default; the engineer
    // colours the ones that matter from the swatch picker (that sets an explicit
    // colourARGB which getResolvedColour() prefers over this default). The
    // per-index `personality` palette above stays available for manual picks.
    inline const auto stripDefaultGrey = juce::Colour (forge::stripDefaultGrey);

    inline juce::Colour stripColour (int /*index*/) noexcept
    {
        return stripDefaultGrey;
    }

    // ── Heated Steel tokens ───────────────────────────────────────────────
    // Promoted from the trial drop-ins so every forged surface routes through
    // a named token instead of an inline hex. (Canonical home is tokens.json →
    // regenerate ForgeTokens.h; mirrored here until that regen runs so Live +
    // the family apps can pick them up.)
    inline const auto steelHeaderHi  = juce::Colour (0xff0b0b0e);  // near-black header plate top
    inline const auto steelHeaderLo  = juce::Colour (0xff000000);  // header plate bottom
    inline const auto steelMasterHi  = juce::Colour (0xff16171c);  // master-strip plate top (deeper than channel)
    inline const auto steelMasterLo  = juce::Colour (0xff070809);  // master-strip plate bottom
    inline const auto debossInk      = juce::Colour (0xff000000);  // stamped-number engraved shadow
    inline const auto debossFace     = juce::Colour (0xffb4b7bf);  // stamped-number raised steel face
    // Structural identity orange. Separate accessor from signalMute() so the
    // spine / stamp / seam can be tuned (or HC-widened) apart from the mute
    // state button, even though both resolve to brandOrange today.
    inline juce::Colour structuralForge() noexcept { return brandOrange; }

    // ── Reduced-motion flag ───────────────────────────────────────────────
    // Global accessibility switch. When true, decorative animations (the
    // record-arm spine pulse + the big-clock breathe/ignite) hold a static
    // end-state instead of animating. Defaults OFF. Wire the setter to the
    // host's reduced-motion source once per launch, e.g. on macOS:
    //   brand::setReduceMotion ([[NSWorkspace sharedWorkspace]
    //       accessibilityDisplayShouldReduceMotion]);
    // (do it in a .mm, and re-query on the relevant workspace notification),
    // or from an in-app Preferences toggle. Nothing reads this until set.
    inline std::atomic<bool>& reduceMotionFlag() noexcept
    {
        static std::atomic<bool> f { false };
        return f;
    }
    inline bool reduceMotion() noexcept { return reduceMotionFlag().load (std::memory_order_relaxed); }
    inline void setReduceMotion (bool on) noexcept { reduceMotionFlag().store (on, std::memory_order_relaxed); }

    // ── Surface fill helper ────────────────────────────────────────────────
    // FLAT design: returns a degenerate "gradient" whose two stops are the SAME
    // base colour, so every `setGradientFill (verticalGradient (...))` call site
    // renders a SOLID fill -- no glossy top-to-bottom ramp. lift / shadow are
    // accepted (so callers don't change) but intentionally ignored.
    inline juce::ColourGradient verticalGradient (juce::Colour base,
                                                  juce::Rectangle<float> r,
                                                  float lift   = 0.10f,
                                                  float shadow = 0.20f) noexcept
    {
        juce::ignoreUnused (lift, shadow);
        return juce::ColourGradient (base, r.getX(), r.getY(),
                                     base, r.getX(), r.getBottom(), false);
    }
}
