#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"

#include <functional>

namespace zynforge
{
    // Session ▸ Session Settings dialog. `onRelocate` runs after the dialog
    // closes when the engineer picks Change... for the recording path;
    // `onApplySampleRate` receives the chosen session rate on Apply.
    struct SessionSettingsDialog
    {
        static void launch (AudioEngine& engine,
                            std::function<void()> onRelocate,
                            std::function<void (double)> onApplySampleRate);
    };
}
