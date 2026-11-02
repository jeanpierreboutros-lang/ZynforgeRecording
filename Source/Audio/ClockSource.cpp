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

        juce::String deviceUidOf (AudioObjectID device)
        {
            CFStringRef uid = nullptr;
            if (! getProperty (device, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, uid)
                || uid == nullptr)
                return {};
            const auto text = juce::String::fromCFString (uid);
            CFRelease (uid);
            return text;
        }

        int channelsOf (AudioObjectID device, bool inputs)
        {
            const AudioObjectPropertyAddress address { kAudioDevicePropertyStreamConfiguration,
                                                       inputs ? kAudioObjectPropertyScopeInput
                                                              : kAudioObjectPropertyScopeOutput,
                                                       kAudioObjectPropertyElementMain };
            UInt32 size = 0;
            if (AudioObjectGetPropertyDataSize (device, &address, 0, nullptr, &size) != noErr || size == 0)
                return 0;
            juce::HeapBlock<char> storage (size);
            auto* list = reinterpret_cast<AudioBufferList*> (storage.get());
            if (AudioObjectGetPropertyData (device, &address, 0, nullptr, &size, list) != noErr)
                return 0;
            int total = 0;
            for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
                total += (int) list->mBuffers[i].mNumberChannels;
            return total;
        }

        // The same walk and the same numbering as JUCE's device scan, so a
        // listed name here is the name JUCE opens.
        std::vector<DeviceId> listDeviceIds (bool inputs)
        {
            juce::StringArray names, uids;
            for (const auto device : allDevices())
            {
                if (channelsOf (device, inputs) <= 0) continue;
                names.add (deviceNameOf (device));
                uids.add (deviceUidOf (device));
            }
            names.appendNumbersToDuplicates (false, true);
            std::vector<DeviceId> ids;
            for (int i = 0; i < names.size(); ++i) ids.push_back ({ names[i], uids[i] });
            return ids;
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
    namespace { std::vector<DeviceId> listDeviceIds (bool) { return {}; } }
   #endif

    std::vector<DeviceId> readDeviceIds (bool inputs, int timeoutMs)
    {
        struct Shared
        {
            std::mutex              lock;
            std::condition_variable finished;
            bool                    done { false };
            std::vector<DeviceId>   ids;
        };
        // The thread owns its half, so it can outlive a caller that stopped waiting.
        auto state = std::make_shared<Shared>();
        if (! juce::Thread::launch ([state, inputs]
            {
                auto ids = listDeviceIds (inputs);
                const std::lock_guard<std::mutex> guard (state->lock);
                state->ids  = std::move (ids);
                state->done = true;
                state->finished.notify_all();
            }))
            return {};
        std::unique_lock<std::mutex> guard (state->lock);
        if (! state->finished.wait_for (guard, std::chrono::milliseconds (timeoutMs),
                                        [&state] { return state->done; }))
            return {};
        return state->ids;
    }

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
