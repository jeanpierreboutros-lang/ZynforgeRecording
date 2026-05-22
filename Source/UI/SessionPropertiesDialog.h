#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Session metadata editor — accessed via Session ▸ Properties…
    // Reads from + writes to the <SessionName>.zfproj JSON document
    // sitting at the root of the active session folder. Engineer fills
    // in name / artist / venue / FOH / date / notes; sample rate and
    // bit depth are read-only (set when the session was created).
    class SessionPropertiesDialog
    {
    public:
        struct Fields
        {
            juce::String name;
            juce::String artist;
            juce::String venue;
            juce::String fohEngineer;
            juce::String date;        // ISO yyyy-mm-dd
            juce::String notes;
            // Read-only display values:
            juce::String sampleRate;
            juce::String bitDepth;
            juce::String captureFormat;
        };

        using SaveCallback = std::function<void (const Fields&)>;

        // Opens modal-async. Calls onSave with the edited fields on
        // Save; does nothing on Cancel.
        static void launch (Fields initial, SaveCallback onSave);
    };
}
