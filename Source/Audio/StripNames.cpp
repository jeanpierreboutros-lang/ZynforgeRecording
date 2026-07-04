#include "StripNames.h"

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

        props = std::make_unique<juce::PropertiesFile> (opts);
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
        reloadReplace (*props);
        props->setValue (keyFor (channelIndex), name);
        props->saveIfNeeded();
    }

    void StripNames::clearName (int channelIndex)
    {
        if (props == nullptr) return;
        reloadReplace (*props);
        props->removeValue (keyFor (channelIndex));
        props->saveIfNeeded();
    }
}
