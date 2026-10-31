#pragma once

#include <juce_core/juce_core.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

// The device's clock source. A wrong clock is a classic way to ruin a MADI or
// Dante recording, and it is a setting on the interface, so all we can do is
// show it. CoreAudio has no standard property for whether the device is
// actually LOCKED to that source, so this never claims that it is.
namespace zynforge::clocksource
{
    enum class Kind
    {
        unavailable,   // no reader on this platform, or the device could not be asked
        unreported,    // the device has no clock-source property (common)
        internal,
        external
    };

    struct Reading
    {
        Kind         kind { Kind::unavailable };
        juce::String name;
    };

    // Vendors name their sources freely; "Internal" is the only spelling with
    // a meaning we rely on, so anything else that is named counts as external.
    inline Kind classify (const juce::String& sourceName)
    {
        auto words = juce::StringArray::fromTokens (
            sourceName.toLowerCase().replaceCharacters ("-_/()[].,:", "          "), false);
        words.removeEmptyStrings();
        if (words.isEmpty()) return Kind::unreported;
        return words.contains ("internal") || words.contains ("int") ? Kind::internal
                                                                     : Kind::external;
    }

    inline juce::String kindName (Kind kind)
    {
        switch (kind)
        {
            case Kind::internal:   return "internal";
            case Kind::external:   return "external";
            case Kind::unreported: return "unreported";
            case Kind::unavailable: break;
        }
        return "unavailable";
    }

    // Pre-flight line. Neither missing case is a failure: most interfaces
    // simply do not publish this.
    inline juce::String preflightRow (const Reading& reading)
    {
        switch (reading.kind)
        {
            case Kind::external:
                return "[OK]  Clock source: " + reading.name;
            case Kind::internal:
                return "[!]  Clock source: " + reading.name
                     + " -- right only if this device is the clock master";
            case Kind::unreported:
                return "[i]  Clock source: this device does not report one";
            case Kind::unavailable: break;
        }
        return "[i]  Clock source: not available";
    }

    // Asks the HAL about the device JUCE calls `deviceName`. It talks to
    // coreaudiod and a driver, so it can stall: only ever call it through Probe.
    Reading readDevice (const juce::String& deviceName);

    // Runs the read on its own thread and never makes a caller wait longer
    // than it asked to. One read at a time; a stalled driver holds that one
    // read and nothing else. The thread owns only `Shared`, so it can outlive
    // the Probe.
    class Probe
    {
    public:
        using Reader = std::function<Reading (const juce::String&)>;

        explicit Probe (Reader reader = readDevice);

        void    request (const juce::String& deviceName);   // never waits
        Reading wait (const juce::String& deviceName, int timeoutMs);
        Reading latest() const;

    private:
        struct Shared
        {
            std::mutex              lock;
            std::condition_variable finished;
            bool                    inFlight { false };
            Reading                 reading;
        };
        Reader readerFn;
        std::shared_ptr<Shared> shared { std::make_shared<Shared>() };
    };
}
