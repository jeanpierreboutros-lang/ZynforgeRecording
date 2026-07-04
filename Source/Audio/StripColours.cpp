#include "StripColours.h"

namespace zynforge
{
    namespace
    {
        // REPLACE in-memory state with the on-disk file (not merge): pick up
        // other writers' changes AND drop keys they deleted since we last
        // loaded. A plain reload() MERGES, so it resurrects a sibling module's
        // just-deleted key when this module later rewrites the shared
        // .settings file wholesale (the "cleared pan comes back" bug).
        inline void reloadReplace (juce::PropertiesFile& pf)
        {
            if (pf.getFile().existsAsFile()) { pf.clear(); pf.reload(); }
        }
    }

    StripColours::StripColours()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        props = std::make_unique<juce::PropertiesFile> (opts);
    }

    juce::String StripColours::keyFor (int channelIndex)
    {
        return "strip_color_" + juce::String (channelIndex);
    }

    bool StripColours::hasColour (int channelIndex) const
    {
        return props != nullptr && props->containsKey (keyFor (channelIndex));
    }

    juce::Colour StripColours::getColour (int channelIndex) const
    {
        if (props == nullptr) return {};
        const auto v = props->getIntValue (keyFor (channelIndex), 0);
        return juce::Colour::fromString ("ff" + juce::String::toHexString (v & 0xffffff).paddedLeft ('0', 6));
    }

    void StripColours::setColour (int channelIndex, juce::Colour c)
    {
        if (props == nullptr) return;
        // Reload first: appProps + the other Strip* modules share this one
        // .settings file, so pick up their latest keys before we mutate+save,
        // otherwise our whole-file save would clobber keys they wrote since we
        // last loaded.
        reloadReplace (*props);
        props->setValue (keyFor (channelIndex), (int) (c.getARGB() & 0xffffff));
        props->saveIfNeeded();
    }

    void StripColours::clearColour (int channelIndex)
    {
        if (props == nullptr) return;
        reloadReplace (*props);
        props->removeValue (keyFor (channelIndex));
        props->saveIfNeeded();
    }
}
