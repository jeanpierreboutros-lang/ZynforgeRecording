#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"

namespace zynforge
{
    // Dark-styled audio device picker with explicit Apply / Cancel.
    //
    // JUCE's AudioDeviceSelectorComponent applies changes to the audio
    // device manager immediately on every selection -- so we snapshot the
    // current device state when the dialog opens, then either keep the
    // edits (Apply) or restore the snapshot (Cancel / close-box / Esc).
    class AudioDeviceDialog
    {
    public:
        // Returns the live DialogWindow so the caller can keep a SafePointer
        // and close it on a second click of the launching button.
        // selfOwned == true launches a self-deleting async dialog (returns
        // nullptr) for callers that can't track + reap the window -- e.g. the
        // PatchPage empty-state button, which otherwise LEAKED the dialog (its
        // Cancel/dtor restore never fired and a second dialog could stack).
        static juce::DialogWindow* launch (AudioEngine& engine, bool selfOwned = false);
    };
}
