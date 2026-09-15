#pragma once

#include <juce_core/juce_core.h>

namespace zynforge::cloud
{
// Launch an engineer-configured uploader without capturing its output.  The
// ChildProcess wrapper is intentionally short-lived; using JUCE's default
// stdout/stderr pipes would close their read end here and can SIGPIPE a verbose
// uploader before it finishes.
inline bool launchUpload (const juce::String& commandTemplate,
                          const juce::File& sessionDirectory)
{
    if (commandTemplate.trim().isEmpty() || ! sessionDirectory.isDirectory())
        return false;
    auto arguments = juce::StringArray::fromTokens (commandTemplate, true);
    arguments.removeEmptyStrings();
    // ChildProcess::start(String) preserves quote characters in every argv
    // token on POSIX (only argv[0] is unquoted internally). Passing the quoted
    // session path through that overload therefore handed uploaders a path
    // literally containing `"` characters. Parse the template first, unquote
    // every argument, and only then substitute the path as inert argv data.
    // This also handles valid macOS filenames containing quote characters.
    for (int i = 0; i < arguments.size(); ++i)
        arguments.set (i, arguments[i].unquoted().replace (
                            "{SESSION}", sessionDirectory.getFullPathName(), false));
    if (arguments.isEmpty()) return false;
    juce::ChildProcess child;
    return child.start (arguments, 0);
}
}
