#pragma once

#include <juce_core/juce_core.h>

#include <filesystem>

namespace zynforge::pathsafety
{
inline bool isSameOrDescendant (const juce::File& root, const juce::File& candidate)
{
    if (root == juce::File() || candidate == juce::File()) return false;

    std::error_code rootError, candidateError;
    const auto canonicalRoot = std::filesystem::weakly_canonical (
        std::filesystem::path (root.getFullPathName().toStdString()), rootError);
    const auto canonicalCandidate = std::filesystem::weakly_canonical (
        std::filesystem::path (candidate.getFullPathName().toStdString()), candidateError);
    if (rootError || candidateError || canonicalRoot.empty() || canonicalCandidate.empty())
        return false;

    auto rootIt = canonicalRoot.begin();
    auto candidateIt = canonicalCandidate.begin();
    for (; rootIt != canonicalRoot.end() && candidateIt != canonicalCandidate.end();
         ++rootIt, ++candidateIt)
        if (*rootIt != *candidateIt) return false;
    return rootIt == canonicalRoot.end();
}

inline bool isStrictDescendant (const juce::File& root, const juce::File& candidate)
{
    return root != candidate && isSameOrDescendant (root, candidate);
}

// A typed name made fit to be one visible file or folder in a folder we chose.
// juce::File::getChildFile reads a name starting with "~" as a home folder
// ("~Show" is user Show's, "~" the engineer's own), "." and ".." as this folder
// and its parent, and any other leading "." makes a hidden name. Those, and
// spaces, are taken off the front. Run it after the caller's own cleaning
// (createLegalFileName) so a name that only becomes "." there is caught too.
// Empty when nothing is left: the caller picks a name or refuses.
inline juce::String visibleName (const juce::String& cleaned)
{
    auto name = cleaned.trim();
    int start = 0;
    while (start < name.length()
           && (name[start] == '.' || name[start] == '~' || juce::CharacterFunctions::isWhitespace (name[start])))
        ++start;
    return name.substring (start).trim();
}
}
