#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace zynforge
{
    // Read-only summary of the open session -- Session > Session Info...
    class SessionInfoDialog
    {
    public:
        struct Fields
        {
            juce::String name;
            juce::String sampleRate;
            juce::String bitDepth;
            juce::String captureFormat;
        };

        static void launch (Fields);

    private:
        friend class ReviewPersistenceRepairTests;
        // Isolated UI tests read what would be shown, without a native dialog.
        inline static std::function<void (Fields)> launchForTests;
    };
}
