#include "Markers.h"

namespace zynforge
{
    static juce::File markersFileFor (const juce::File& dir)
    {
        return dir.getChildFile ("markers.json");
    }

    void MarkersManager::setContext (const juce::File& dir, double sr)
    {
        sessionDir = dir;
        sampleRate = sr > 0 ? sr : sampleRate;
        markers.clear();
        load();
    }

    void MarkersManager::clearContext()
    {
        sessionDir = juce::File();
        markers.clear();
    }

    void MarkersManager::drop (juce::int64 sampleOffset, const juce::String& name)
    {
        Marker m;
        m.sampleOffset = sampleOffset;
        m.name = name.isNotEmpty() ? name
                                   : "Marker " + juce::String (markers.size() + 1);
        markers.push_back (std::move (m));
        save();
    }

    void MarkersManager::clear()
    {
        markers.clear();
        save();
    }

    Marker MarkersManager::getLast() const
    {
        if (markers.empty()) return {};
        return markers.back();
    }

    Marker MarkersManager::getMarker (int i) const
    {
        if (i < 0 || i >= (int) markers.size()) return {};
        return markers[(std::size_t) i];
    }

    void MarkersManager::removeMarker (int i)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers.erase (markers.begin() + i);
        save();
    }

    void MarkersManager::renameMarker (int i, const juce::String& newName)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers[(std::size_t) i].name = newName;
        save();
    }

    void MarkersManager::setMarkerSample (int i, juce::int64 sample)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers[(std::size_t) i].sampleOffset = sample;
        save();
    }

    bool MarkersManager::save() const
    {
        if (! sessionDir.isDirectory()) return false;

        juce::DynamicObject::Ptr root (new juce::DynamicObject());
        root->setProperty ("sampleRate", sampleRate);

        juce::Array<juce::var> arr;
        for (const auto& m : markers)
        {
            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("sample", (juce::int64) m.sampleOffset);
            o->setProperty ("name",   m.name);
            o->setProperty ("type",   m.type);
            arr.add (juce::var (o.get()));
        }
        root->setProperty ("markers", arr);

        const auto json = juce::JSON::toString (juce::var (root.get()), true);
        return markersFileFor (sessionDir).replaceWithText (json);
    }

    bool MarkersManager::load()
    {
        markers.clear();
        if (! sessionDir.isDirectory()) return false;

        const auto file = markersFileFor (sessionDir);
        if (! file.existsAsFile()) return false;

        juce::var parsed;
        if (juce::JSON::parse (file.loadFileAsString(), parsed).failed())
            return false;

        if (auto* obj = parsed.getDynamicObject())
        {
            if (auto sr = obj->getProperty ("sampleRate"); sr.isDouble() || sr.isInt())
                sampleRate = (double) sr;

            if (auto* arr = obj->getProperty ("markers").getArray())
            {
                for (const auto& v : *arr)
                {
                    if (auto* mo = v.getDynamicObject())
                    {
                        Marker m;
                        m.sampleOffset = (juce::int64) mo->getProperty ("sample");
                        m.name         = mo->getProperty ("name").toString();
                        m.type         = mo->getProperty ("type").toString();
                        if (m.type.isEmpty()) m.type = "user";
                        markers.push_back (std::move (m));
                    }
                }
            }
        }
        return true;
    }
}
