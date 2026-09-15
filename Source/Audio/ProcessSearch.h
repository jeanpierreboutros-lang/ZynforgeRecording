#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::processsearch
{
    inline juce::File findExecutableInPath (const juce::String& executable,
                                             const juce::String& searchPath)
    {
        if (executable.isEmpty()) return {};
        auto directories = juce::StringArray::fromTokens (searchPath, ":", "");
        directories.removeEmptyStrings();
        for (const auto& directory : directories)
        {
            const auto candidate = juce::File (directory).getChildFile (executable);
            if (candidate.existsAsFile()) return candidate;
        }
        return {};
    }
}
