#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>

namespace zynforge
{
    // MIDI clock master -- emits 0xF8 (clock pulse) at 24 PPQN from the
    // session's tempo, plus 0xFA (start) on play / 0xFC (stop) on stop /
    // 0xFB (continue) on resume. A juce::HighResolutionTimer fires on
    // its own thread so the audio thread is never involved. Jitter is
    // on the order of macOS scheduler granularity (~1-2 ms) -- fine for
    // sync slaves that smooth incoming clock (most synths, drum machines).
    //
    // The engineer picks a destination MIDI device by name. setEnabled
    // toggles output on/off; the device opens lazily on first enable.
    class MidiClockOut final : private juce::HighResolutionTimer
    {
    public:
        MidiClockOut() = default;
        ~MidiClockOut() override { stop(); }

        // List installed system MIDI output device names (for the UI picker).
        static juce::StringArray listOutputs()
        {
            juce::StringArray names;
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
                names.add (d.name);
            return names;
        }

        // Open / replace the MIDI output device. Empty name = closes.
        void setOutputDevice (const juce::String& deviceName)
        {
            const juce::ScopedLock sl (lock);
            output.reset();
            if (deviceName.isEmpty()) return;

            for (const auto& d : juce::MidiOutput::getAvailableDevices())
            {
                if (d.name == deviceName)
                {
                    output = juce::MidiOutput::openDevice (d.identifier);
                    break;
                }
            }
        }

        juce::String getOutputDeviceName() const
        {
            const juce::ScopedLock sl (lock);
            return output != nullptr ? output->getName() : juce::String();
        }

        void setTempoBpm (float bpm) noexcept
        {
            // 24 PPQN -- quarter note = bpm / 60. Each pulse interval =
            // 60_000 / (bpm * 24) milliseconds.
            tempoBpm.store (juce::jmax (20.0f, bpm), std::memory_order_relaxed);
        }

        void setEnabled (bool e)
        {
            enabled.store (e, std::memory_order_release);
            if (e)
            {
                // 1 ms tick -- finer than the smallest clock interval
                // (~2 ms at 999 BPM). The timer self-adjusts: each tick
                // checks whether enough samples have elapsed since the
                // last pulse to emit another.
                startTimer (1);
            }
            else
            {
                stopTimer();
                sendStop();
            }
        }

        bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

        // Engine calls these on play / pause / stop / resume so the
        // slave hardware locks to the session transport.
        void sendStart()
        {
            const juce::ScopedLock sl (lock);
            if (output != nullptr) output->sendMessageNow (juce::MidiMessage::midiStart());
            // Finding #6: publish the fresh timestamp BEFORE flipping running
            // true (release). The timer reads running with acquire, so once it
            // observes running it also sees this timestamp -- never a stale one
            // from a prior transport that would trigger a catch-up 0xF8 burst.
            msAtLastPulse.store ((double) juce::Time::getMillisecondCounterHiRes(),
                                 std::memory_order_relaxed);
            running.store (true, std::memory_order_release);
        }
        void sendContinue()
        {
            const juce::ScopedLock sl (lock);
            if (output != nullptr) output->sendMessageNow (juce::MidiMessage::midiContinue());
            msAtLastPulse.store ((double) juce::Time::getMillisecondCounterHiRes(),
                                 std::memory_order_relaxed);
            running.store (true, std::memory_order_release);
        }
        void sendStop()
        {
            const juce::ScopedLock sl (lock);
            if (output != nullptr) output->sendMessageNow (juce::MidiMessage::midiStop());
            running.store (false, std::memory_order_release);
        }

    private:
        void hiResTimerCallback() override
        {
            // Acquire on running pairs with the release store in sendStart/
            // sendContinue: if we observe running, we also observe the fresh
            // msAtLastPulse published just before it (finding #6).
            if (! enabled.load (std::memory_order_acquire)
                || ! running.load (std::memory_order_acquire)) return;

            const float bpm     = tempoBpm.load (std::memory_order_relaxed);
            const double pulseMs = 60000.0 / (double) (bpm * 24.0f);

            const double now = (double) juce::Time::getMillisecondCounterHiRes();
            // Only this timer thread mutates msAtLastPulse while running, so a
            // local read-modify-write is race-free vs the acquire above.
            double last = msAtLastPulse.load (std::memory_order_relaxed);
            while (now - last >= pulseMs)
            {
                {
                    const juce::ScopedLock sl (lock);
                    if (output != nullptr)
                        output->sendMessageNow (juce::MidiMessage::midiClock());
                }
                last += pulseMs;
            }
            msAtLastPulse.store (last, std::memory_order_relaxed);
        }

        void stop()
        {
            stopTimer();
            const juce::ScopedLock sl (lock);
            output.reset();
        }

        mutable juce::CriticalSection       lock;
        std::unique_ptr<juce::MidiOutput>   output;
        std::atomic<float>  tempoBpm { 120.0f };
        std::atomic<bool>   enabled  { false };
        std::atomic<bool>   running        { false };
        std::atomic<double> msAtLastPulse  { 0.0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiClockOut)
    };
}
