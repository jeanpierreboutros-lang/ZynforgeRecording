#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#if JUCE_MAC || JUCE_LINUX
 #include <sys/stat.h>
#endif

// Which build of the capture helper is which. A helper outlives the window
// that started it, so an app updated while one ran used to attach to the old
// helper and drive it with nothing said. The helper names its build in the
// hello; the window compares that with the helper it ships and replaces an
// idle one that differs (CaptureSupervisor::connectOrLaunch).
//
// A build is named by its executable FILE: device, inode, size and
// modification time, read with one stat. A window and the helper beside it in
// one bundle (or one build tree) read the same file and agree; an update
// replaces the file, and anything else is another file. Hashing the contents
// was exact but cost seconds in a sanitizer build, before the helper could
// open its port. A copy of the same helper elsewhere reads as another build,
// so an idle one is replaced: harmless. The helper takes its name once, at
// start: an update that replaces the file afterwards must not change what a
// running helper says.
namespace zynforge::capture::helperbuild
{
    inline constexpr const char* kExecutableName = "ZynforgeCapture";

    // 32 hex digits naming the file as it is now. Empty when it is not there.
    inline juce::String idOf (const juce::File& executable)
    {
       #if JUCE_MAC || JUCE_LINUX
        struct stat status {};
        if (::stat (executable.getFullPathName().toRawUTF8(), &status) != 0 || ! S_ISREG (status.st_mode))
            return {};
        #if JUCE_MAC
        const auto modifiedNs = (juce::int64) status.st_mtimespec.tv_nsec;
        #else
        const auto modifiedNs = (juce::int64) status.st_mtim.tv_nsec;
        #endif
        const auto stamp = juce::String ((juce::int64) status.st_dev) + ":" + juce::String ((juce::int64) status.st_ino)
                         + ":" + juce::String ((juce::int64) status.st_size)
                         + ":" + juce::String ((juce::int64) status.st_mtime) + "." + juce::String (modifiedNs);
        return juce::SHA256 (stamp.toUTF8()).toHexString().substring (0, 32);
       #else
        if (! executable.existsAsFile()) return {};
        const auto stamp = executable.getFullPathName() + ":" + juce::String (executable.getSize()) + ":"
                         + juce::String (executable.getLastModificationTime().toMilliseconds());
        return juce::SHA256 (stamp.toUTF8()).toHexString().substring (0, 32);
       #endif
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
