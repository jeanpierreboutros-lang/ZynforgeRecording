#pragma once

#include "AtomicFile.h"
#include "SessionRemovedData.h"

// The one way session_mix.json is written. The recorder's strips are not
// always a session's mixer: the active session can be pinned from settings, or
// from a daemon's status, with nothing loaded yet. Writing them then replaces a
// show's whole mixer with an empty one, so the writer asks whose mixer this is.
namespace zynforge::sessionmix
{
    inline const char* const kFileName = "session_mix.json";

    // Strips in a parsed mix, or -1 when it is not a usable mix.
    inline int stripCount (const juce::var& mix)
    {
        auto* root = mix.getDynamicObject();
        if (root == nullptr || ! root->hasProperty ("trackCount")) return -1;
        return juce::jmax (-1, (int) root->getProperty ("trackCount"));
    }

    // Strips in the mix saved in a session folder; -1 when missing or damaged.
    inline int stripCountIn (const juce::File& sessionDir)
    {
        return stripCount (juce::JSON::parse (sessionDir.getChildFile (kFileName)));
    }

    inline juce::String notThisSessionsMixer (const juce::File& sessionDir)
    {
        return "the mixer in memory is not " + sessionDir.getFileName()
             + "'s mixer, so the session's saved files were left untouched";
    }

    // Why `json` may not become this session's mix, or empty when it may.
    // `mixerOwner` is the session the in-memory mixer belongs to.
    inline juce::String refusal (const juce::File& sessionDir, const juce::File& mixerOwner,
                                 const juce::String& json)
    {
        if (sessionDir == juce::File() || mixerOwner != sessionDir)
            return notThisSessionsMixer (sessionDir);
        const int incoming = stripCount (juce::JSON::parse (json));
        if (incoming < 0)
            return "the mixer could not be written out, so the saved mix was left untouched";
        if (incoming == 0 && stripCountIn (sessionDir) > 0)
            return "the mixer is empty but " + sessionDir.getFileName()
                 + " has saved strips, so the saved mix was left untouched";
        return {};
    }

    inline bool write (const juce::File& sessionDir, const juce::File& mixerOwner,
                       const juce::String& json, juce::String* error = nullptr)
    {
        auto why = refusal (sessionDir, mixerOwner, json);
        if (why.isEmpty() && removeddata::keepOriginals (sessionDir) == removeddata::Kept::failed)
            why = removeddata::refusal (sessionDir);
        if (why.isEmpty() && ! atomicfile::writeText (sessionDir.getChildFile (kFileName), json))
            why = "Could not save " + juce::String (kFileName);
        if (error != nullptr) *error = why;
        return why.isEmpty();
    }
}
