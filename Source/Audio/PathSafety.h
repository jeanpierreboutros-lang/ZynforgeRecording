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
}
