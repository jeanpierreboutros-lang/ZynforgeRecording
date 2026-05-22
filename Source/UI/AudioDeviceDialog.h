#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Dark-styled audio device picker with explicit Apply / Cancel.
    //
    // JUCE's AudioDeviceSelectorComponent applies changes to the audio
    // device manager immediately on every selection — so we snapshot the
    // current device state when the dialog opens, then either keep the
    // edits (Apply) or restore the snapshot (Cancel / close-box / Esc).
    class AudioDeviceDialog
    {
    public:
        static void launch (AudioEngine& engine);
    };
}
