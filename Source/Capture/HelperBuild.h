#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <map>
#include <mutex>
#include <utility>

// Which build of the capture helper is which. A helper outlives the window
// that started it, so an app updated while one ran used to attach to the old
// helper and drive it with nothing said. The helper names its build in the
// hello; the window compares that with the helper it ships and replaces an
// idle one that differs (CaptureSupervisor::connectOrLaunch).
//
// A build is named by its executable's contents, so a window and the helper
// beside it in one bundle (or one build tree) always agree, and any other
// binary does not. The helper takes its name once, at start: an update that
// replaces the file afterwards must not change what a running helper says.
namespace zynforge::capture::helperbuild
{
    inline constexpr const char* kExecutableName = "ZynforgeCapture";

    // The first 32 hex digits of the file's SHA-256. Empty when unreadable.
    // Cached by path, size and date: a window asks at every connect.
    inline juce::String idOf (const juce::File& executable)
    {
        if (! executable.existsAsFile()) return {};
        const auto stamp = juce::String (executable.getSize()) + ":"
                         + juce::String (executable.getLastModificationTime().toMilliseconds());
        static std::mutex lock;
        static std::map<juce::String, std::pair<juce::String, juce::String>> cache;
        const std::lock_guard<std::mutex> guard (lock);
        auto& entry = cache[executable.getFullPathName()];
        if (entry.first != stamp || entry.second.isEmpty())
        {
            juce::FileInputStream input (executable);
            if (! input.openedOk()) return {};
            entry = { stamp, juce::SHA256 (input).toHexString().substring (0, 32) };
        }
        return entry.second;
    }

    // The helper this process is, or the one it ships beside: an app bundle
    // (and the test runner) carries it in Contents/MacOS next to its own
    // executable.
    inline juce::File ownHelper()
    {
        const auto executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        return executable.getFileName() == kExecutableName ? executable
                                                           : executable.getSiblingFile (kExecutableName);
    }
}
