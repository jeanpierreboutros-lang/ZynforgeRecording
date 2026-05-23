#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/MultitrackRecorder.h"
#include <functional>

namespace zynforge
{
    // Pro Tools-style "New Session" dialog. Configures the session name,
    // local-storage location, file type, sample rate, bit depth, and
    // interleaved flag -- without the Templates section.
    //
    // The callback fires only on Create; Cancel returns silently.
    class NewSessionDialog
    {
    public:
        struct Result
        {
            juce::String  name;
            juce::File    location;        // parent folder for the new session
            CaptureFormat captureFormat;   // derived from file type + bit depth
            double        sampleRate;
            bool          interleaved;
            juce::String  ioSettings;      // "Last Used", "Stereo Mix", "5.1 Film", ...
        };

        using ResultCallback = std::function<void (const Result&)>;

        // Opens modal-async. defaultLocation is the parent folder shown
        // in the picker (typically ~/Music/Zynforge Sessions). lastSR is
        // pre-selected in the Sample Rate combo.
        static void launch (const juce::File& defaultLocation,
                            double            lastSampleRate,
                            CaptureFormat     lastCaptureFormat,
                            ResultCallback    onCreate);
    };
}
