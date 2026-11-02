#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace zynforge::atomicfile
{
    // "Name.ext", then "Name (2).ext" and so on: the first name free in `directory`.
    inline juce::File firstFreeName (const juce::File& directory, const juce::String& name,
                                     const juce::String& extension = {})
    {
        auto file = directory.getChildFile (name + extension);
        for (int n = 2; file.exists(); ++n)
            file = directory.getChildFile (name + " (" + juce::String (n) + ")" + extension);
        return file;
    }

    // A writer's staging file: here only until its rename, and never part of
    // a session. It can vanish at any moment, so nothing should copy or compare it.
    inline bool isStagingFile (const juce::File& file)
    {
        const auto name = file.getFileName();
        return name.startsWithChar ('.') && name.endsWith (".tmp");
    }

    // Each caller gets a unique sibling, so overlapping report writers cannot
    // truncate one another's staging file. replaceFileIn maps to same-volume
    // rename on POSIX and leaves an existing target untouched on failure.
    inline bool writeWith (const juce::File& target, const std::function<bool (juce::OutputStream&)>& fill)
    {
        if (! target.getParentDirectory().isDirectory()) return false;
        const auto temporary = target.getSiblingFile (
            "." + target.getFileName() + "." + juce::Uuid().toString() + ".tmp");
        const juce::ScopeGuard cleanup { [&] { temporary.deleteFile(); } };

        // File::replaceWithText() asserts in Debug builds if it cannot create
        // its own internal temporary file.  Write the unique staging file
        // explicitly so an ordinary I/O failure is reported to the caller
        // instead of terminating the application.
        bool wrote = false;
        juce::int64 accepted = -1;
        {
            juce::FileOutputStream output (temporary);
            if (output.openedOk())
            {
                wrote = fill (output);
                accepted = output.getPosition();
                output.flush();
                wrote = wrote && output.getStatus().wasOk();
            }
        }

        // The stream buffers small files and flush() drops the result of that
        // last write, so a disk that fills up here leaves a short file and a
        // clean status. Only the file's real size can tell.
        return wrote && temporary.existsAsFile() && temporary.getSize() == accepted
            && temporary.replaceFileIn (target);
    }

    inline bool writeText (const juce::File& target, const juce::String& text)
    {
        return writeWith (target, [&text] (juce::OutputStream& output)
                          { return output.writeText (text, false, false, nullptr); });
    }

    inline bool writeBytes (const juce::File& target, const void* data, size_t size)
    {
        return writeWith (target, [data, size] (juce::OutputStream& output)
                          { return output.write (data, size); });
    }
}
