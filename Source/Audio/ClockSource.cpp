#include "ClockSource.h"

#include <chrono>
#include <vector>

#if JUCE_MAC
 #include <CoreAudio/CoreAudio.h>
#endif

namespace zynforge::clocksource
{
   #if JUCE_MAC
    namespace
    {
        template <typename Value>
        bool getProperty (AudioObjectID object, AudioObjectPropertySelector selector,
                          AudioObjectPropertyScope scope, Value& value)
        {
            const AudioObjectPropertyAddress address { selector, scope, kAudioObjectPropertyElementMain };
            UInt32 size = sizeof (Value);
            return AudioObjectGetPropertyData (object, &address, 0, nullptr, &size, &value) == noErr
                && size == sizeof (Value);
        }

        std::vector<AudioObjectID> allDevices()
        {
            const AudioObjectPropertyAddress address { kAudioHardwarePropertyDevices,
                                                       kAudioObjectPropertyScopeGlobal,
                                                       kAudioObjectPropertyElementMain };
            UInt32 size = 0;
            if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
                return {};
            std::vector<AudioObjectID> devices (size / sizeof (AudioObjectID));
            if (devices.empty()
                || AudioObjectGetPropertyData (kAudioObjectSystemObject, &address, 0, nullptr,
                                               &size, devices.data()) != noErr)
                return {};
            devices.resize (size / sizeof (AudioObjectID));
            return devices;
        }

        juce::String deviceNameOf (AudioObjectID device)
        {
            CFStringRef name = nullptr;
            if (! getProperty (device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, name)
                || name == nullptr)
                return {};
            const auto text = juce::String::fromCFString (name);
            CFRelease (name);
            return text;
        }

        juce::String clockSourceName (AudioObjectID device, AudioObjectPropertyScope scope, UInt32 source)
        {
            CFStringRef name = nullptr;
            AudioValueTranslation translation { &source, sizeof (source), &name, sizeof (name) };
            if (! getProperty (device, kAudioDevicePropertyClockSourceNameForIDCFString, scope, translation)
                || name == nullptr)
                return {};
            const auto text = juce::String::fromCFString (name);
            CFRelease (name);
            return text;
        }
    }

    Reading readDevice (const juce::String& deviceName)
    {
        if (deviceName.isEmpty()) return {};

        // JUCE numbers duplicate names ("Name (2)") and we cannot map those
        // back to one device, so two matches or none reports "not available"
        // rather than the clock of the wrong interface.
        AudioObjectID device = kAudioObjectUnknown;
        int matches = 0;
        for (const auto candidate : allDevices())
            if (deviceNameOf (candidate) == deviceName) { device = candidate; ++matches; }
        if (matches != 1) return {};

        for (const auto scope : { kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyScopeOutput })
        {
            const AudioObjectPropertyAddress address { kAudioDevicePropertyClockSource, scope,
                                                       kAudioObjectPropertyElementMain };
            if (! AudioObjectHasProperty (device, &address)) continue;
            UInt32 source = 0;
            if (! getProperty (device, kAudioDevicePropertyClockSource, scope, source)) continue;
            const auto name = clockSourceName (device, scope, source);
            return { classify (name), name };
        }
        return { Kind::unreported, {} };
    }
   #else
    Reading readDevice (const juce::String&) { return {}; }
   #endif

    Probe::Probe (Reader reader) : readerFn (std::move (reader)) {}

    void Probe::request (const juce::String& deviceName)
    {
        if (deviceName.isEmpty()) return;
        {
            const std::lock_guard<std::mutex> guard (shared->lock);
            if (shared->inFlight) return;
            shared->inFlight = true;
            // A stalled read must leave "unknown", not the last device's clock.
            shared->reading = {};
        }
        const auto launched = juce::Thread::launch (
            [state = shared, read = readerFn, deviceName]
            {
                const auto reading = read (deviceName);
                const std::lock_guard<std::mutex> guard (state->lock);
                state->reading  = reading;
                state->inFlight = false;
                state->finished.notify_all();
            });
        if (! launched)
        {
            const std::lock_guard<std::mutex> guard (shared->lock);
            shared->inFlight = false;
        }
    }

    Reading Probe::wait (const juce::String& deviceName, int timeoutMs)
    {
        if (deviceName.isEmpty()) return {};
        request (deviceName);
        std::unique_lock<std::mutex> guard (shared->lock);
        if (! shared->finished.wait_for (guard, std::chrono::milliseconds (timeoutMs),
                                         [this] { return ! shared->inFlight; }))
            return {};   // the driver did not answer in time
        return shared->reading;
    }

    Reading Probe::latest() const
    {
        const std::lock_guard<std::mutex> guard (shared->lock);
        return shared->reading;
    }
}
