#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace zynforge
{
    // Persists per-channel display-name overrides via a juce::PropertiesFile
    // in the user's application support folder. Keys are
    // "strip_name_<channelIndex>". Absent key means "use the default
    // 'In N' label".
    class StripNames
    {
    public:
        StripNames();

        // Test isolation: when enabled (before any StripNames is built),
        // names persist to a SEPARATE "(tests)" settings file so the unit
        // tests can't pollute the real per-channel names the engineer set.
        // The test runner flips this on in Main.cpp's --run-tests path.
        static void setTestMode (bool on) noexcept { testModeFlag() = on; }

        bool         hasName (int channelIndex) const;
        juce::String getName (int channelIndex) const;
        void         setName (int channelIndex, const juce::String&);
        void         clearName (int channelIndex);

    private:
        static juce::String keyFor (int channelIndex);
        static bool& testModeFlag() noexcept { static bool t = false; return t; }
        std::unique_ptr<juce::PropertiesFile> props;
    };
}
