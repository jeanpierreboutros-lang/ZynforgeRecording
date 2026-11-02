#pragma once

#include "ReaperProject.h"
#include "SessionInterchange.h"

namespace zynforge::dawproject
{
    // DAWproject 1.0 (github.com/bitwig/dawproject): a ZIP of project.xml and
    // metadata.xml. Audio stays beside the session and is referenced, never
    // embedded: a show is many gigabytes and juce::ZipFile::Builder is not
    // ZIP64-safe. The layout follows the format's own RAW_SECONDS and
    // FILE_WITH_RELATIVE_PATH reference scenarios: the arrangement is in
    // seconds, so nothing here depends on the tempo.

    constexpr const char* kVersion = "1.0";

    class Ids
    {
    public:
        juce::String next() { return "id" + juce::String (counter++); }
    private:
        int counter { 0 };
    };

    inline juce::String seconds (double value) { return juce::String (value, 12); }

    inline juce::String colourAttribute (juce::uint32 argb)
    {
        const juce::Colour colour (argb);
        return "#" + juce::String::toHexString (colour.getRed()).paddedLeft ('0', 2)
                   + juce::String::toHexString (colour.getGreen()).paddedLeft ('0', 2)
                   + juce::String::toHexString (colour.getBlue()).paddedLeft ('0', 2);
    }

    inline void addParameter (juce::XmlElement& parent, const char* tag, const juce::String& unit,
                              double minimum, double maximum, double value, Ids& ids, const char* name)
    {
        auto* parameter = parent.createNewChildElement (tag);
        parameter->setAttribute ("id", ids.next());
        parameter->setAttribute ("name", name);
        parameter->setAttribute ("unit", unit);
        parameter->setAttribute ("min", minimum);
        parameter->setAttribute ("max", maximum);
        parameter->setAttribute ("value", value);
    }

    inline void addMixerControls (juce::XmlElement& channel, bool muted, double pan, double gain, Ids& ids)
    {
        auto* mute = channel.createNewChildElement ("Mute");
        mute->setAttribute ("id", ids.next());
        mute->setAttribute ("name", "Mute");
        mute->setAttribute ("value", muted ? "true" : "false");
        addParameter (channel, "Pan", "normalized", 0.0, 1.0, (pan + 1.0) * 0.5, ids, "Pan");
        addParameter (channel, "Volume", "linear", 0.0, juce::jmax (2.0, gain), gain, ids, "Volume");
    }

    inline std::unique_ptr<juce::XmlElement> buildChannel (const juce::String& id, int channels,
                                                           const juce::String& role,
                                                           const juce::String& destination)
    {
        auto channel = std::make_unique<juce::XmlElement> ("Channel");
        channel->setAttribute ("id", id);
        channel->setAttribute ("audioChannels", channels);
        channel->setAttribute ("role", role);
        channel->setAttribute ("solo", "false");
        if (destination.isNotEmpty()) channel->setAttribute ("destination", destination);
        return channel;
    }

    inline void addClip (juce::XmlElement& clips, const interchange::Item& item, double sampleRate,
                         const juce::File& projectFile, Ids& ids)
    {
        const auto fileSeconds = (double) item.fileLength / item.fileRate;
        const auto start       = (double) item.sourceOffset / item.fileRate;
        const auto duration    = (double) item.length / sampleRate;

        auto* clip = clips.createNewChildElement ("Clip");
        clip->setAttribute ("time", seconds ((double) item.position / sampleRate));
        clip->setAttribute ("duration", seconds (duration));
        clip->setAttribute ("contentTimeUnit", "seconds");
        clip->setAttribute ("playStart", seconds (start));
        clip->setAttribute ("playStop", seconds (start + duration));
        clip->setAttribute ("fadeTimeUnit", "seconds");
        clip->setAttribute ("fadeInTime", seconds ((double) item.fadeIn / sampleRate));
        clip->setAttribute ("fadeOutTime", seconds ((double) item.fadeOut / sampleRate));
        clip->setAttribute ("enable", item.muted ? "false" : "true");
        clip->setAttribute ("name", item.name);

        auto* audio = clip->createNewChildElement ("Audio");
        audio->setAttribute ("id", ids.next());
        audio->setAttribute ("timeUnit", "seconds");
        audio->setAttribute ("channels", item.fileChannels);
        audio->setAttribute ("sampleRate", juce::roundToInt (item.fileRate));
        audio->setAttribute ("duration", seconds (fileSeconds));

        auto* file = audio->createNewChildElement ("File");
        file->setAttribute ("path", reaperproject::relativePath (item.file, projectFile));
        file->setAttribute ("external", "true");
    }

    inline juce::String buildProjectXml (const interchange::Project& project,
                                         const juce::File& projectFile,
                                         const juce::String& appVersion)
    {
        Ids ids;
        juce::XmlElement root ("Project");
        root.setAttribute ("version", kVersion);

        auto* application = root.createNewChildElement ("Application");
        application->setAttribute ("name", "Zynforge Recording");
        application->setAttribute ("version", appVersion);

        auto* transport = root.createNewChildElement ("Transport");
        addParameter (*transport, "Tempo", "bpm", 20.0, 999.0, 120.0, ids, "Tempo");
        auto* signature = transport->createNewChildElement ("TimeSignature");
        signature->setAttribute ("id", ids.next());
        signature->setAttribute ("numerator", 4);
        signature->setAttribute ("denominator", 4);

        auto* structure = root.createNewChildElement ("Structure");
        const auto masterChannelId = ids.next();
        auto* master = structure->createNewChildElement ("Track");
        master->setAttribute ("id", ids.next());
        master->setAttribute ("name", "Master");
        master->setAttribute ("contentType", "audio");
        master->setAttribute ("loaded", "true");
        auto masterChannel = buildChannel (masterChannelId, 2, "master", {});
        addMixerControls (*masterChannel, project.masterMuted, 0.0, project.masterGain, ids);
        master->addChildElement (masterChannel.release());

        std::vector<juce::String> trackIds;
        for (const auto& track : project.tracks)
        {
            const auto trackId = ids.next();
            trackIds.push_back (trackId);
            auto* element = structure->createNewChildElement ("Track");
            element->setAttribute ("id", trackId);
            element->setAttribute ("name", track.name);
            element->setAttribute ("contentType", "audio");
            element->setAttribute ("loaded", "true");
            if (track.colourArgb != 0)
                element->setAttribute ("color", colourAttribute (track.colourArgb));
            auto channel = buildChannel (ids.next(), track.channels, "regular", masterChannelId);
            const double pan = track.channels == 2 ? (track.panLeft + track.panRight) * 0.5 : track.pan;
            addMixerControls (*channel, track.muted, pan, track.gain, ids);
            element->addChildElement (channel.release());
        }

        auto* arrangement = root.createNewChildElement ("Arrangement");
        arrangement->setAttribute ("id", ids.next());
        auto* lanes = arrangement->createNewChildElement ("Lanes");
        lanes->setAttribute ("id", ids.next());
        lanes->setAttribute ("timeUnit", "seconds");
        for (size_t i = 0; i < project.tracks.size(); ++i)
        {
            auto* clips = lanes->createNewChildElement ("Clips");
            clips->setAttribute ("id", ids.next());
            clips->setAttribute ("track", trackIds[i]);
            for (const auto& item : project.tracks[i].items)
                addClip (*clips, item, project.sampleRate, projectFile, ids);
        }

        // The schema requires at least one Marker inside Markers.
        if (! project.markers.empty())
        {
            auto* markers = arrangement->createNewChildElement ("Markers");
            markers->setAttribute ("id", ids.next());
            markers->setAttribute ("timeUnit", "seconds");
            for (const auto& marker : project.markers)
            {
                auto* element = markers->createNewChildElement ("Marker");
                element->setAttribute ("time", seconds ((double) marker.sample / project.sampleRate));
                element->setAttribute ("name", marker.name);
            }
        }
        return root.toString();
    }

    inline juce::String buildMetadataXml (const juce::String& title, const juce::StringArray& notes)
    {
        juce::XmlElement root ("MetaData");
        root.createNewChildElement ("Title")->addTextElement (title);
        if (! notes.isEmpty())
            root.createNewChildElement ("Comment")->addTextElement (
                interchange::cleanText (notes.joinIntoString (" ")));
        return root.toString();
    }

    inline juce::MemoryBlock buildArchive (const juce::String& projectXml, const juce::String& metadataXml)
    {
        juce::MemoryOutputStream zipped;
        juce::ZipFile::Builder builder;
        const auto now = juce::Time::getCurrentTime();
        for (const auto& [name, text] : { std::pair<juce::String, juce::String> ("project.xml", projectXml),
                                          std::pair<juce::String, juce::String> ("metadata.xml", metadataXml) })
        {
            builder.addEntry (new juce::MemoryInputStream (text.toRawUTF8(), text.getNumBytesAsUTF8(), true),
                              9, name, now);
        }
        builder.writeToStream (zipped, nullptr);
        return zipped.getMemoryBlock();
    }
}
