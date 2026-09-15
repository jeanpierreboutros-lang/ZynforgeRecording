#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::atomicfile
{
    // Each caller gets a unique sibling, so overlapping report writers cannot
    // truncate one another's staging file. replaceFileIn maps to same-volume
    // rename on POSIX and leaves an existing target untouched on failure.
    inline bool writeText (const juce::File& target, const juce::String& text)
    {
        if (! target.getParentDirectory().isDirectory()) return false;
        const auto temporary = target.getSiblingFile (
            "." + target.getFileName() + "." + juce::Uuid().toString() + ".tmp");
        const juce::ScopeGuard cleanup ([&] { temporary.deleteFile(); });

        // File::replaceWithText() asserts in Debug builds if it cannot create
        // its own internal temporary file.  Write the unique staging file
        // explicitly so an ordinary I/O failure is reported to the caller
        // instead of terminating the application.
        bool wrote = false;
        {
            juce::FileOutputStream output (temporary);
            if (output.openedOk())
            {
                wrote = output.writeText (text, false, false, nullptr);
                output.flush();
                wrote = wrote && output.getStatus().wasOk();
            }
        }

        return wrote && temporary.existsAsFile() && temporary.replaceFileIn (target);
    }
}
