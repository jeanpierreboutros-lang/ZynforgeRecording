#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>

namespace zynforge
{
    // Once a process enters test mode it can never write production preferences.
    inline std::atomic<bool> isolatedSettings { false };
    inline void isolateSettings() noexcept { isolatedSettings.store (true); }

    // Observable only in isolated tests. Count actual shared-file reload
    // attempts without callbacks or stack-owned observer lifetimes.
    inline std::atomic<juce::uint64> settingsReloadCountForTests { 0 };
    inline void reloadSettingsReplacing (juce::PropertiesFile& properties)
    {
        // JUCE reload merges; clear first to honour sibling writers' deletions.
        // Preserve unsaved first-run defaults when no file exists yet.
        if (properties.getFile().existsAsFile())
        {
            properties.clear();
            if (isolatedSettings.load (std::memory_order_relaxed))
                settingsReloadCountForTests.fetch_add (1, std::memory_order_relaxed);
            properties.reload();
        }
    }

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
