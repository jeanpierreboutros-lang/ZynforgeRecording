#include "StripColours.h"

namespace zynforge
{
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
        props->setValue (keyFor (channelIndex), (int) (c.getARGB() & 0xffffff));
        props->saveIfNeeded();
    }

    void StripColours::clearColour (int channelIndex)
    {
        if (props == nullptr) return;
        props->removeValue (keyFor (channelIndex));
        props->saveIfNeeded();
    }
}
