#include "StripGains.h"

namespace zynforge
{
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
        props->setValue (gainKey (ch), (double) dB);
        props->saveIfNeeded();
    }
    void  StripGains::clearGain (int ch)
    {
        if (! props) return;
        props->removeValue (gainKey (ch));
        props->saveIfNeeded();
    }

    bool  StripGains::hasPan (int ch) const   { return props && props->containsKey (panKey (ch)); }
    float StripGains::getPan (int ch) const   { return props ? (float) props->getDoubleValue (panKey (ch), 0.0) : 0.0f; }
    void  StripGains::setPan (int ch, float pan)
    {
        if (! props) return;
        props->setValue (panKey (ch), (double) pan);
        props->saveIfNeeded();
    }
    void  StripGains::clearPan (int ch)
    {
        if (! props) return;
        props->removeValue (panKey (ch));
        props->saveIfNeeded();
    }
}
