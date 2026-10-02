#pragma once

#include <juce_core/juce_core.h>

namespace zynforge
{
    // AudioThumbnailCache keys entries by source path. A take can replace the
    // media at that path after a cache was saved, so a matching cache version
    // alone does not prove its peaks still describe the current audio.
    inline bool waveCachePredatesAudio (const juce::File& cacheFile,
                                       const juce::File& sessionDir)
    {
        if (! cacheFile.existsAsFile()) return false;

        const auto cacheTime = cacheFile.getLastModificationTime();
        const auto audioFiles = sessionDir.getChildFile ("Audio Files");
        const auto sourceDir = audioFiles.isDirectory() ? audioFiles : sessionDir;
        juce::Array<juce::File> files;
        sourceDir.findChildFiles (files, juce::File::findFiles, false);

        for (const auto& file : files)
        {
            if (! file.getFileName().startsWithIgnoreCase ("Track_")) continue;
            const auto ext = file.getFileExtension().toLowerCase();
            if (ext != ".wav" && ext != ".flac" && ext != ".aif" && ext != ".aiff")
                continue;
            if (file.getLastModificationTime() > cacheTime
                || file.getCreationTime() > cacheTime)
                return true;
        }
        return false;
    }
}
