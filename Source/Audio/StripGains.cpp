#include "StripGains.h"
#include "SettingsFile.h"

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

        props = makeSettingsFile (opts);
    }

    StripGains::~StripGains()
    {
        stopTimer();
        flush();
    }

    juce::String StripGains::gainKey (int ch) { return "strip_gain_" + juce::String (ch); }
    juce::String StripGains::panKey  (int ch) { return "strip_pan_"  + juce::String (ch); }

    const juce::var* StripGains::pendingValue (const juce::String& key) const
    {
        return pending.getVarPointer (juce::Identifier (key));
    }

    void StripGains::reloadKeepingPending()
    {
        // Reload first so our whole-file save doesn't clobber keys the other
        // writers (appProps + sibling Strip* modules) sharing this .settings
        // file have written since we last loaded.
        reloadSettingsReplacing (*props);
        for (const auto& nv : pending)
            props->setValue (nv.name.toString(), nv.value);
        pending.clear();
    }

    void StripGains::write (const juce::String& key, double value)
    {
        if (! props) return;
        const juce::ScopedLock sl (lock);
        pending.set (juce::Identifier (key), value);
        const auto now = juce::Time::getMillisecondCounter();
        if (! everWritten || now - lastWriteMs >= kMinWriteIntervalMs)
        {
            reloadKeepingPending();
            props->saveIfNeeded();
            lastWriteMs = now;
            everWritten = true;
        }
        else if (! isTimerRunning())
        {
            startTimer ((int) kMinWriteIntervalMs);
        }
    }

    void StripGains::flush()
    {
        if (! props) return;
        const juce::ScopedLock sl (lock);
        if (pending.isEmpty()) return;
        reloadKeepingPending();
        props->saveIfNeeded();
        lastWriteMs = juce::Time::getMillisecondCounter();
        everWritten = true;
    }

    void StripGains::timerCallback()
    {
        stopTimer();
        flush();
    }

    bool  StripGains::hasGain (int ch) const
    {
        const juce::ScopedLock sl (lock);
        return pendingValue (gainKey (ch)) != nullptr || (props && props->containsKey (gainKey (ch)));
    }
    float StripGains::getGainDb (int ch) const
    {
        const juce::ScopedLock sl (lock);
        if (auto* v = pendingValue (gainKey (ch))) return (float) (double) *v;
        return props ? (float) props->getDoubleValue (gainKey (ch), 0.0) : 0.0f;
    }
    void  StripGains::setGainDb (int ch, float dB) { write (gainKey (ch), (double) dB); }
    void  StripGains::clearGain (int ch)
    {
        if (! props) return;
        const juce::ScopedLock sl (lock);
        reloadKeepingPending();
        props->removeValue (gainKey (ch));
        props->saveIfNeeded();
    }

    bool  StripGains::hasPan (int ch) const
    {
        const juce::ScopedLock sl (lock);
        return pendingValue (panKey (ch)) != nullptr || (props && props->containsKey (panKey (ch)));
    }
    float StripGains::getPan (int ch) const
    {
        const juce::ScopedLock sl (lock);
        if (auto* v = pendingValue (panKey (ch))) return (float) (double) *v;
        return props ? (float) props->getDoubleValue (panKey (ch), 0.0) : 0.0f;
    }
    void  StripGains::setPan (int ch, float pan) { write (panKey (ch), (double) pan); }
    void  StripGains::clearPan (int ch)
    {
        if (! props) return;
        const juce::ScopedLock sl (lock);
        reloadKeepingPending();
        props->removeValue (panKey (ch));
        props->saveIfNeeded();
    }
    void StripGains::resetRange (int firstIndex, int lastIndexExclusive)
    {
        if (! props || firstIndex >= lastIndexExclusive) return;
        const juce::ScopedLock sl (lock);
        reloadKeepingPending();
        for (int i = firstIndex; i < lastIndexExclusive; ++i)
        {
            props->setValue (gainKey (i), 0.0);
            props->setValue (panKey (i), 0.0);
        }
        props->saveIfNeeded();
    }

    void StripGains::clearRange (int firstIndex, int lastIndexExclusive)
    {
        if (! props || firstIndex >= lastIndexExclusive) return;
        const juce::ScopedLock sl (lock);
        reloadKeepingPending();
        for (int i = firstIndex; i < lastIndexExclusive; ++i)
        {
            props->removeValue (gainKey (i));
            props->removeValue (panKey (i));
        }
        props->saveIfNeeded();
    }

}
