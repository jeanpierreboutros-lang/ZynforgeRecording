#include "StripGains.h"

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

    StripGains::StripGains()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = testModeFlag() ? "Zynforge Recording (tests)"
                                                   : "Zynforge Recording";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "Zynforge Recording";
        opts.osxLibrarySubFolder = "Application Support";
        opts.storageFormat       = juce::PropertiesFile::storeAsXML;

        props = std::make_unique<juce::PropertiesFile> (opts);
    }

    juce::String StripGains::gainKey (int ch) { return "strip_gain_" + juce::String (ch); }
    juce::String StripGains::panKey  (int ch) { return "strip_pan_"  + juce::String (ch); }

    bool  StripGains::hasGain (int ch) const   { return props && props->containsKey (gainKey (ch)); }
    float StripGains::getGainDb (int ch) const { return props ? (float) props->getDoubleValue (gainKey (ch), 0.0) : 0.0f; }
    void  StripGains::setGainDb (int ch, float dB)
    {
        if (! props) return;
        // Reload first so our whole-file save doesn't clobber keys the other
        // writers (appProps + sibling Strip* modules) sharing this .settings
        // file have written since we last loaded.
        reloadReplace (*props);
        props->setValue (gainKey (ch), (double) dB);
        props->saveIfNeeded();
    }
    void  StripGains::clearGain (int ch)
    {
        if (! props) return;
        reloadReplace (*props);
        props->removeValue (gainKey (ch));
        props->saveIfNeeded();
    }

    bool  StripGains::hasPan (int ch) const   { return props && props->containsKey (panKey (ch)); }
    float StripGains::getPan (int ch) const   { return props ? (float) props->getDoubleValue (panKey (ch), 0.0) : 0.0f; }
    void  StripGains::setPan (int ch, float pan)
    {
        if (! props) return;
        reloadReplace (*props);
        props->setValue (panKey (ch), (double) pan);
        props->saveIfNeeded();
    }
    void  StripGains::clearPan (int ch)
    {
        if (! props) return;
        reloadReplace (*props);
        props->removeValue (panKey (ch));
        props->saveIfNeeded();
    }
}
