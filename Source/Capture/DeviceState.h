#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

namespace zynforge::capture
{
    // The device this process is running now, in the form
    // AudioDeviceManager::initialise reads. createStateXml cannot stand in: it is
    // null until a setup was applied as a chosen device, and after a device
    // drops it goes on naming the last choice while the manager runs whatever it
    // fell back to.
    inline std::unique_ptr<juce::XmlElement> liveDeviceState (const juce::AudioDeviceManager& manager,
                                                              juce::AudioIODevice& device)
    {
        const auto setup = manager.getAudioDeviceSetup();
        auto state = std::make_unique<juce::XmlElement> ("DEVICESETUP");
        state->setAttribute ("deviceType", manager.getCurrentAudioDeviceType());
        state->setAttribute ("audioOutputDeviceName", setup.outputDeviceName);
        state->setAttribute ("audioInputDeviceName", setup.inputDeviceName);
        state->setAttribute ("audioDeviceRate", device.getCurrentSampleRate());
        state->setAttribute ("audioDeviceBufferSize", device.getCurrentBufferSizeSamples());
        if (! setup.useDefaultInputChannels)
            state->setAttribute ("audioDeviceInChans", setup.inputChannels.toString (2));
        if (! setup.useDefaultOutputChannels)
            state->setAttribute ("audioDeviceOutChans", setup.outputChannels.toString (2));
        return state;
    }
}
