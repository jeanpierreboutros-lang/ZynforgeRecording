#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>

namespace zynforge
{
    // Once a process enters test mode it can never write production preferences.
    inline std::atomic<bool> isolatedSettings { false };
    inline void isolateSettings() noexcept { isolatedSettings.store (true); }
    inline std::unique_ptr<juce::PropertiesFile> makeSettingsFile (const juce::PropertiesFile::Options& options)
    {
        if (isolatedSettings.load())
        {
            static const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getNonexistentChildFile ("zynforge-tests-" + juce::Uuid().toString(), ".settings");
            return std::make_unique<juce::PropertiesFile> (file, options);
        }
        return std::make_unique<juce::PropertiesFile> (options);
    }
}
