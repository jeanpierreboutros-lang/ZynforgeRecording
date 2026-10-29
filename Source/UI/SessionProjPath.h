#pragma once

#include <juce_core/juce_core.h>

namespace zynforge
{
    // Returns the active `<SessionName>.zfproj` file inside `dir`,
    // or (when none exists yet) the canonical path it would be
    // created at. Used by every site that round-trips session data
    // through .zfproj -- save/load, cue persistence, UI layout.
    //
    // Header-only / `inline` so each translation unit that needs it
    // includes this file directly; no separate .cpp needed.
    inline juce::File findSessionProj (const juce::File& dir)
    {
        if (! dir.isDirectory()) return {};
        const auto canonical = dir.getChildFile (dir.getFileName() + ".zfproj");
        if (canonical.existsAsFile()) return canonical;
        // Skip hidden files: on exFAT/FAT/SMB macOS writes AppleDouble
        // "._X.zfproj" companions, which sort first and are not JSON.
        auto candidates = dir.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                              false, "*.zfproj");
        candidates.sort();
        return candidates.isEmpty() ? canonical : candidates.getFirst();
    }
}
