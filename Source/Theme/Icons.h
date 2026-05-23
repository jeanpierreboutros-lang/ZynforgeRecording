#pragma once

#include <juce_graphics/juce_graphics.h>

namespace zynforge::icons
{
    // Minimal in-code icon set so the header chrome reads as icons +
    // labels without bundling external SVG / font files. Every icon is
    // drawn as a juce::Path inside a unit square (0..1); call draw()
    // with the bounds you want it painted into.

    enum class Glyph
    {
        Record, Play, Stop, Lock, Unlock, Plus, Device, Patch, Meters,
        Osc, Vsc, File, Disk, Mix, Edit, Setlist, Tempo, Click, Trash,
        Selection, Cut, Copy, Paste, Undo, Redo
    };

    inline juce::Path pathFor (Glyph g)
    {
        juce::Path p;
        switch (g)
        {
            case Glyph::Record:
                p.addEllipse (0.20f, 0.20f, 0.60f, 0.60f);
                break;

            case Glyph::Play:
                p.startNewSubPath (0.25f, 0.18f);
                p.lineTo          (0.80f, 0.50f);
                p.lineTo          (0.25f, 0.82f);
                p.closeSubPath();
                break;

            case Glyph::Stop:
                p.addRectangle (0.22f, 0.22f, 0.56f, 0.56f);
                break;

            case Glyph::Lock:
            {
                // Clean padlock: rounded body + U-shackle on top.
                p.addRoundedRectangle (0.26f, 0.48f, 0.48f, 0.40f, 0.07f);
                const float halfPi = juce::MathConstants<float>::halfPi;
                p.startNewSubPath (0.34f, 0.48f);
                p.lineTo          (0.34f, 0.36f);
                p.addCentredArc   (0.50f, 0.36f, 0.16f, 0.16f, 0.0f,
                                   -halfPi, halfPi, false);
                p.lineTo          (0.66f, 0.48f);
                // Keyhole -- small dot inside the body
                p.addEllipse (0.46f, 0.64f, 0.08f, 0.08f);
                break;
            }

            case Glyph::Unlock:
            {
                // Same body as Lock, but shackle is rotated open on the right.
                p.addRoundedRectangle (0.26f, 0.48f, 0.48f, 0.40f, 0.07f);
                const float halfPi = juce::MathConstants<float>::halfPi;
                p.startNewSubPath (0.34f, 0.48f);
                p.lineTo          (0.34f, 0.30f);
                p.addCentredArc   (0.50f, 0.30f, 0.16f, 0.16f, 0.0f,
                                   -halfPi, halfPi * 0.6f, false);
                break;
            }

            case Glyph::Plus:
                p.addRectangle (0.45f, 0.18f, 0.10f, 0.64f);
                p.addRectangle (0.18f, 0.45f, 0.64f, 0.10f);
                break;

            case Glyph::Device:
                // Audio interface front panel: rounded body, two large
                // knobs on the left, two TRS jacks on the right.
                p.addRoundedRectangle (0.10f, 0.32f, 0.80f, 0.36f, 0.06f);
                p.addEllipse (0.18f, 0.40f, 0.16f, 0.20f);   // knob 1
                p.addEllipse (0.38f, 0.40f, 0.16f, 0.20f);   // knob 2
                p.addEllipse (0.60f, 0.42f, 0.10f, 0.16f);   // jack 1
                p.addEllipse (0.74f, 0.42f, 0.10f, 0.16f);   // jack 2
                break;

            case Glyph::Patch:
                p.addEllipse (0.18f, 0.30f, 0.16f, 0.16f);
                p.addEllipse (0.66f, 0.30f, 0.16f, 0.16f);
                p.addEllipse (0.18f, 0.56f, 0.16f, 0.16f);
                p.addEllipse (0.66f, 0.56f, 0.16f, 0.16f);
                p.startNewSubPath (0.34f, 0.38f);
                p.lineTo          (0.66f, 0.64f);
                p.startNewSubPath (0.34f, 0.64f);
                p.lineTo          (0.66f, 0.38f);
                break;

            case Glyph::Meters:
                for (int i = 0; i < 4; ++i)
                    p.addRectangle (0.18f + i * 0.18f,
                                    0.78f - i * 0.14f,
                                    0.12f,
                                    0.04f + i * 0.16f);
                break;

            case Glyph::Osc:
                p.startNewSubPath (0.12f, 0.50f);
                for (int i = 0; i <= 30; ++i)
                {
                    const float x = 0.12f + (float) i / 30.0f * 0.76f;
                    const float y = 0.50f
                                  + 0.22f * std::sin ((float) i * 0.7f);
                    p.lineTo (x, y);
                }
                break;

            case Glyph::Vsc:
                // 'VSC' = an arrow loop = playback-routed-as-input idea
                p.startNewSubPath (0.20f, 0.30f);
                p.lineTo          (0.80f, 0.30f);
                p.lineTo          (0.66f, 0.18f);
                p.startNewSubPath (0.80f, 0.30f);
                p.lineTo          (0.66f, 0.42f);
                p.startNewSubPath (0.80f, 0.70f);
                p.lineTo          (0.20f, 0.70f);
                p.lineTo          (0.34f, 0.58f);
                p.startNewSubPath (0.20f, 0.70f);
                p.lineTo          (0.34f, 0.82f);
                break;

            case Glyph::File:
                p.startNewSubPath (0.22f, 0.14f);
                p.lineTo          (0.62f, 0.14f);
                p.lineTo          (0.78f, 0.30f);
                p.lineTo          (0.78f, 0.86f);
                p.lineTo          (0.22f, 0.86f);
                p.closeSubPath();
                p.startNewSubPath (0.62f, 0.14f);
                p.lineTo          (0.62f, 0.30f);
                p.lineTo          (0.78f, 0.30f);
                break;

            case Glyph::Disk:
                p.addEllipse (0.16f, 0.16f, 0.68f, 0.68f);
                p.addEllipse (0.42f, 0.42f, 0.16f, 0.16f);
                break;

            case Glyph::Mix:
                // Three fader strips
                for (int i = 0; i < 3; ++i)
                {
                    const float x = 0.22f + i * 0.18f;
                    p.startNewSubPath (x, 0.18f);
                    p.lineTo          (x, 0.82f);
                    p.addRectangle    (x - 0.05f, 0.30f + i * 0.10f, 0.10f, 0.06f);
                }
                break;

            case Glyph::Edit:
                // Pencil
                p.startNewSubPath (0.16f, 0.82f);
                p.lineTo          (0.30f, 0.82f);
                p.lineTo          (0.78f, 0.34f);
                p.lineTo          (0.66f, 0.22f);
                p.lineTo          (0.16f, 0.70f);
                p.closeSubPath();
                break;

            case Glyph::Setlist:
                // Bulleted list
                for (int i = 0; i < 4; ++i)
                {
                    const float y = 0.20f + i * 0.20f;
                    p.addEllipse (0.20f, y, 0.08f, 0.08f);
                    p.addRectangle (0.34f, y + 0.02f, 0.46f, 0.04f);
                }
                break;

            case Glyph::Tempo:
                // Metronome triangle + arm
                p.startNewSubPath (0.50f, 0.18f);
                p.lineTo          (0.78f, 0.82f);
                p.lineTo          (0.22f, 0.82f);
                p.closeSubPath();
                p.startNewSubPath (0.50f, 0.50f);
                p.lineTo          (0.68f, 0.30f);
                break;

            case Glyph::Click:
                p.addEllipse (0.42f, 0.18f, 0.16f, 0.16f);
                p.addEllipse (0.42f, 0.66f, 0.16f, 0.16f);
                p.addRectangle (0.48f, 0.34f, 0.04f, 0.32f);
                break;

            case Glyph::Trash:
                p.addRoundedRectangle (0.24f, 0.30f, 0.52f, 0.54f, 0.04f);
                p.addRectangle (0.18f, 0.22f, 0.64f, 0.06f);
                p.startNewSubPath (0.40f, 0.20f);
                p.lineTo          (0.60f, 0.20f);
                break;

            case Glyph::Selection:
                p.addRectangle (0.18f, 0.18f, 0.32f, 0.32f);
                p.addRectangle (0.50f, 0.50f, 0.32f, 0.32f);
                break;

            case Glyph::Cut:
                p.addEllipse (0.18f, 0.58f, 0.20f, 0.20f);
                p.addEllipse (0.62f, 0.58f, 0.20f, 0.20f);
                p.startNewSubPath (0.30f, 0.62f);
                p.lineTo          (0.62f, 0.30f);
                p.startNewSubPath (0.70f, 0.62f);
                p.lineTo          (0.38f, 0.30f);
                break;

            case Glyph::Copy:
                p.addRoundedRectangle (0.18f, 0.18f, 0.50f, 0.50f, 0.04f);
                p.addRoundedRectangle (0.32f, 0.32f, 0.50f, 0.50f, 0.04f);
                break;

            case Glyph::Paste:
                p.addRoundedRectangle (0.22f, 0.22f, 0.56f, 0.64f, 0.04f);
                p.addRectangle (0.34f, 0.12f, 0.32f, 0.16f);
                break;

            case Glyph::Undo:
                p.startNewSubPath (0.20f, 0.50f);
                p.cubicTo (0.20f, 0.20f, 0.70f, 0.20f, 0.78f, 0.50f);
                p.startNewSubPath (0.20f, 0.50f);
                p.lineTo          (0.10f, 0.40f);
                p.startNewSubPath (0.20f, 0.50f);
                p.lineTo          (0.30f, 0.40f);
                break;

            case Glyph::Redo:
                p.startNewSubPath (0.80f, 0.50f);
                p.cubicTo (0.80f, 0.20f, 0.30f, 0.20f, 0.22f, 0.50f);
                p.startNewSubPath (0.80f, 0.50f);
                p.lineTo          (0.70f, 0.40f);
                p.startNewSubPath (0.80f, 0.50f);
                p.lineTo          (0.90f, 0.40f);
                break;
        }
        return p;
    }

    inline void draw (juce::Graphics& g,
                      Glyph glyph,
                      juce::Rectangle<float> bounds,
                      juce::Colour colour,
                      float strokeThickness = 1.2f)
    {
        auto p = pathFor (glyph);
        p.applyTransform (juce::AffineTransform::scale (bounds.getWidth(),
                                                        bounds.getHeight())
                              .translated (bounds.getX(), bounds.getY()));
        g.setColour (colour);
        // 'Filled' shapes -- Record, Stop, Play, Plus, Lock body, Trash
        // body -- get filled. Stroked-only outlines for the rest.
        const bool filled = glyph == Glyph::Record
                         || glyph == Glyph::Stop
                         || glyph == Glyph::Play
                         || glyph == Glyph::Plus
                         || glyph == Glyph::Selection;
        // Lock / Device read clearer at small sizes when the stroke is
        // a touch heavier than the default 1.2.
        const float thick = (glyph == Glyph::Lock
                          || glyph == Glyph::Unlock
                          || glyph == Glyph::Device) ? 1.6f : strokeThickness;
        if (filled) g.fillPath (p);
        else        g.strokePath (p, juce::PathStrokeType (thick));
    }
}
