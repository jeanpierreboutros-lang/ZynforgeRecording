#include "StripRouting.h"
#include "SettingsFile.h"

namespace zynforge
{
    StripRouting::StripRouting()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        props = makeSettingsFile (opts);
    }

    juce::String StripRouting::inKey  (int i) { return "strip_in_"  + juce::String (i); }
    juce::String StripRouting::outKey (int i) { return "strip_out_" + juce::String (i); }

    bool StripRouting::hasInput  (int i) const { return props && props->containsKey (inKey  (i)); }
    bool StripRouting::hasOutput (int i) const { return props && props->containsKey (outKey (i)); }
    int  StripRouting::getInput  (int i) const { return props ? props->getIntValue (inKey  (i), -1) : -1; }
    int  StripRouting::getOutput (int i) const { return props ? props->getIntValue (outKey (i), -1) : -1; }

    void StripRouting::setInput  (int i, int dev)
    {
        if (! props) return;
        // Reload first so our whole-file save doesn't clobber keys the other
        // writers (appProps + sibling Strip* modules) sharing this .settings
        // file have written since we last loaded.
        reloadSettingsReplacing (*props);
        props->setValue (inKey  (i), dev);
        props->saveIfNeeded();
    }
    void StripRouting::setOutput (int i, int dev)
    {
        if (! props) return;
        reloadSettingsReplacing (*props);
        props->setValue (outKey (i), dev);
        props->saveIfNeeded();
    }
    void StripRouting::clearInput  (int i) { if (props) { reloadSettingsReplacing (*props); props->removeValue (inKey  (i)); props->saveIfNeeded(); } }
    void StripRouting::clearOutput (int i) { if (props) { reloadSettingsReplacing (*props); props->removeValue (outKey (i)); props->saveIfNeeded(); } }
    void StripRouting::clearRange (int firstIndex, int lastIndexExclusive)
    {
        if (! props || firstIndex >= lastIndexExclusive) return;
        reloadSettingsReplacing (*props);
        for (int i = firstIndex; i < lastIndexExclusive; ++i)
        {
            props->removeValue (inKey (i));
            props->removeValue (outKey (i));
        }
        props->saveIfNeeded();
    }

}
