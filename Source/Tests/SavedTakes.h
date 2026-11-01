#pragma once

#include "../Audio/AudioEngine.h"

#include <vector>

// Sessions saved by earlier builds can hold several takes on a strip, and the
// app no longer makes them. These put takes in and read them back through the
// JSON a session file carries, the path a real open and save take.
namespace zynforge::savedtakes
{
    inline juce::DynamicObject* entryFor (const juce::var& document, int track)
    {
        if (const auto* entries = document.getArray())
            for (const auto& entry : *entries)
                if (auto* object = entry.getDynamicObject())
                    if ((int) object->getProperty ("track") == track)
                        return object;
        return nullptr;
    }

    inline juce::var savedDocument (const AudioEngine& engine)
    {
        return juce::JSON::parse (juce::JSON::toString (engine.playlistsToJson()));
    }

    inline int count (const AudioEngine& engine, int track)
    {
        const auto document = savedDocument (engine);
        const auto* entry = entryFor (document, track);
        const auto* takes = entry != nullptr ? entry->getProperty ("takes").getArray() : nullptr;
        return takes != nullptr ? takes->size() : 0;
    }

    // Timeline position and length of each clip in one take, as saved.
    inline std::vector<Clip> clips (const AudioEngine& engine, int track, int take)
    {
        std::vector<Clip> result;
        const auto document = savedDocument (engine);
        const auto* entry = entryFor (document, track);
        const auto* takes = entry != nullptr ? entry->getProperty ("takes").getArray() : nullptr;
        if (takes == nullptr || take < 0 || take >= takes->size()) return result;
        if (const auto* list = (*takes)[take]["clips"].getArray())
            for (const auto& saved : *list)
            {
                Clip clip;
                clip.timelineStartSamples = (juce::int64) saved["tlStart"];
                clip.fileLengthSamples    = (juce::int64) saved["fileLen"];
                result.push_back (clip);
            }
        return result;
    }

    // The session as if it had been saved with this take active.
    inline void makeActive (AudioEngine& engine, int track, int take)
    {
        const auto document = savedDocument (engine);
        if (auto* entry = entryFor (document, track))
        {
            entry->setProperty ("activeTake", take);
            engine.loadPlaylistsFromJson (document);
        }
    }

    // The session as if it had been saved with a copy of the active take as a
    // further take, active. Returns its index, or -1 with no such strip.
    inline int addTake (AudioEngine& engine, int track, const juce::String& name)
    {
        const auto document = savedDocument (engine);
        auto* entry = entryFor (document, track);
        auto* takes = entry != nullptr ? entry->getProperty ("takes").getArray() : nullptr;
        if (takes == nullptr || takes->isEmpty()) return -1;
        auto copy = (*takes)[(int) entry->getProperty ("activeTake")].clone();
        copy.getDynamicObject()->setProperty ("name", name);
        takes->add (copy);
        const int index = takes->size() - 1;
        entry->setProperty ("activeTake", index);
        engine.loadPlaylistsFromJson (document);
        return index;
    }
}
