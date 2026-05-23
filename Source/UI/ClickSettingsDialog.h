#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Click-track settings -- two voices summed into the metronome track.
    // CLICK 1 is the downbeat (accent), CLICK 2 fires on the remaining
    // beats in the bar. Each voice has its own volume, waveform, and
    // subdivision so the engineer can build patterns like
    //   1 : cowbell, quarter note
    //   2 : woodblock, 8th note triplets
    // ...without leaving the dialog.
    struct ClickSettings
    {
        enum class Voice : int { Sine = 0, Beep, Cowbell, WoodBlock, Click };
        // Note-value buttons in the reference Click II layout (Pro Tools):
        //   whole / half / quarter / eighth / sixteenth / triplet / silent.
        enum class Subdivision : int { Whole = 0, Half, Quarter, Eighth, Sixteenth, Triplet, Silent };

        struct Voice1Then2
        {
            float       volumeDb { 0.0f };
            Voice       voice    { Voice::Click };
            Subdivision sub      { Subdivision::Quarter };
        };

        bool        on       { true };
        bool        followMeter { false };  // reserved -- for meter changes
        Voice1Then2 click1;                  // accent / downbeat
        Voice1Then2 click2;                  // remaining beats
    };

    class ClickSettingsDialog
    {
    public:
        using SaveCallback     = std::function<void (const ClickSettings&)>;
        using GenerateCallback = std::function<void()>;

        // Opens modal-async. onSave fires whenever any control changes
        // (live edit), so the engineer hears the changes when they
        // press Generate. onGenerate is invoked when the engineer wants
        // to (re)render the click track WAV with the latest settings.
        static void launch (ClickSettings  initial,
                            float          bpm,
                            SaveCallback   onSave,
                            GenerateCallback onGenerate);
    };
}
