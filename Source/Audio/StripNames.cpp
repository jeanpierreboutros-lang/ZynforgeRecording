#include "StripNames.h"
#include "SettingsFile.h"

namespace zynforge
{
    StripNames::StripNames()
    {
        juce::PropertiesFile::Options opts;
        // Tests persist to a separate file so they never touch the real
        // per-channel names the engineer set.
        opts.applicationName     = testModeFlag() ? "Zynforge Recording (tests)"
                                                   : "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        props = makeSettingsFile (opts);
    }

    juce::String StripNames::keyFor (int channelIndex)
    {
        return "strip_name_" + juce::String (channelIndex);
    }

    bool StripNames::hasName (int channelIndex) const
    {
        return props != nullptr && props->containsKey (keyFor (channelIndex));
    }

    juce::String StripNames::getName (int channelIndex) const
    {
        if (props == nullptr) return {};
        return props->getValue (keyFor (channelIndex));
    }

    void StripNames::setName (int channelIndex, const juce::String& name)
    {
        if (props == nullptr) return;
        if (name.isEmpty()) { clearName (channelIndex); return; }
        // Reload first so our whole-file save doesn't clobber keys the other
        // writers (appProps + sibling Strip* modules) sharing this .settings
        // file have written since we last loaded.
        reloadSettingsReplacing (*props);
        props->setValue (keyFor (channelIndex), name);
        props->saveIfNeeded();
    }

    void StripNames::clearName (int channelIndex)
    {
        if (props == nullptr) return;
        reloadSettingsReplacing (*props);
        props->removeValue (keyFor (channelIndex));
        props->saveIfNeeded();
    }
    void StripNames::clearRange (int firstIndex, int lastIndexExclusive)
    {
        if (! props || firstIndex >= lastIndexExclusive) return;
        reloadSettingsReplacing (*props);
        for (int i = firstIndex; i < lastIndexExclusive; ++i)
        {
            props->removeValue (keyFor (i));
        }
        props->saveIfNeeded();
    }

}
