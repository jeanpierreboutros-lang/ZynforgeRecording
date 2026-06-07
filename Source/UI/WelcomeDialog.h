#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/MultitrackRecorder.h"
#include <functional>

namespace zynforge
{
    // Pro Tools-style welcome screen shown on app boot. Two-pane
    // layout: a dark side rail with "New" / "Open" actions, and a
    // content pane on the right with the matching form.
    //
    // The dialog uses the standard ZynForge palette throughout --
    // bgDeep side rail, bgPanel content, brand-orange selection
    // stripe -- so it reads as part of the app, not a separate
    // OS-themed surface.
    //
    // On dismiss, exactly one of the callbacks fires (or neither if
    // the engineer hits Esc / closes the window).
    class WelcomeDialog
    {
    public:
        struct NewResult
        {
            juce::String  name;
            juce::File    location;        // parent folder for the new session
            CaptureFormat captureFormat;
            double        sampleRate;
            bool          interleaved;
            juce::String  ioSettings;
            juce::String  inputDeviceName;   // chosen record-from input device ("" = unchanged)
            juce::String  outputDeviceName;  // chosen monitor output device  ("" = unchanged)
        };

        using OnCreateNew  = std::function<void (const NewResult&)>;
        using OnOpenExisting = std::function<void (const juce::File& /*sessionDir*/)>;

        // Available audio devices to populate the New-session device pickers.
        struct DeviceChoices
        {
            juce::StringArray inputs;        // input device names
            juce::StringArray outputs;       // output device names
            juce::String      currentInput;  // pre-selected input
            juce::String      currentOutput; // pre-selected output
        };

        // Modal-async. The dialog shows the New panel by default.
        static void launch (const juce::File&    defaultLocation,
                            double                lastSampleRate,
                            CaptureFormat         lastCaptureFormat,
                            const DeviceChoices&  devices,
                            OnCreateNew           onCreate,
                            OnOpenExisting        onOpen);
    };
}
