#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

// How numbers and sentences read on screen, in one place, so that two screens
// can't show the same fact two ways.
namespace zynforge
{
    // "1 track", "9 tracks".
    inline juce::String counted (juce::int64 n, const juce::String& noun)
    {
        return juce::String (n) + " " + noun + (n == 1 ? "" : "s");
    }

    // Messages are written with an ASCII " -- " because juce::String reads a
    // char literal as ASCII. Everything that draws a message for the engineer
    // sets it through here. The source text stays as written, so a test or a
    // log can still match on it.
    inline juce::String typeset (const juce::String& text)
    {
        return text.contains (" -- ") ? text.replace (" -- ", juce::String::fromUTF8 (" \xe2\x80\x94 "))
                                      : text;
    }

    // Drive sizes and speeds are decimal, as Finder and drive makers count
    // them, so the free space here matches what the engineer sees elsewhere.
    inline double bytesToGB (juce::int64 bytes) noexcept { return (double) bytes / 1.0e9; }
    inline double bytesToMB (double bytes) noexcept      { return bytes / 1.0e6; }

    // Minus infinity, in the one form every strip readout shows it: a silent
    // strip's peak, a fader at its OFF floor, the master's level. JetBrains
    // Mono and the UI sans both draw the glyph cleanly at 10 and 11 pt.
    inline juce::String minusInfinity() { return juce::String::fromUTF8 ("-\xe2\x88\x9e"); }

    // A level readout: "-12.3 dB", or minusInfinity() + " dB" at or below the
    // floor. Without the unit (a readout too narrow for it) "-12.3".
    inline juce::String levelText (float dB, float floorDb, bool withUnit = true)
    {
        return (dB <= floorDb ? minusInfinity() : juce::String (dB, 1)) + (withUnit ? " dB" : "");
    }

    // Record time left, in the one form every screen shows it.
    inline juce::String recordTimeText (double seconds)
    {
        if (seconds <= 0.0 || std::isinf (seconds)) return "--";
        const auto hours   = (int) (seconds / 3600.0);
        const auto minutes = ((int) (seconds / 60.0)) % 60;
        if (hours > 99) return ">99h";
        return juce::String::formatted ("%dh%02dm", hours, minutes);
    }
}
