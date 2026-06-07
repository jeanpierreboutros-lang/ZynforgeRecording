#include "MidiControlSurface.h"
#include "AudioEngine.h"

namespace zynforge
{
    MidiControlSurface::MidiControlSurface (AudioEngine& e) : engine (e)
    {
        for (int i = 0; i < kStrips; ++i)
        { lastFaderDb[i] = -999.0f; lastMute[i] = lastSolo[i] = lastArm[i] = false; lastName[i] = {}; }
    }

    MidiControlSurface::~MidiControlSurface() { stop(); }

    bool MidiControlSurface::start (const juce::String& inputName, const juce::String& outputName)
    {
        stop();
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == inputName) { input = juce::MidiInput::openDevice (d.identifier, this); break; }
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == outputName) { output = juce::MidiOutput::openDevice (d.identifier); break; }

        if (input != nullptr) input->start();
        // Force a full state push on the next tick.
        for (int i = 0; i < kStrips; ++i) { lastFaderDb[i] = -999.0f; lastName[i] = {}; }
        startTimerHz (15);
        return input != nullptr;   // a surface with only output still echoes, but input is the point
    }

    void MidiControlSurface::stop()
    {
        stopTimer();
        if (input  != nullptr) { input->stop(); input.reset(); }
        output.reset();
    }

    void MidiControlSurface::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
    {
        auto& rec = engine.getRecorder();

        // Motor fader move -> channel gain (atomic store; safe off this thread).
        if (m.isPitchWheel())
        {
            const int strip = m.getChannel() - 1;   // MCU channel 1..8 -> strip 0..7
            if (strip >= 0 && strip < kStrips && strip < rec.getNumTracks())
                rec.getTrack (strip).gainDb.store (mcu::faderToDb (m.getPitchWheelValue()),
                                                   std::memory_order_relaxed);
            return;
        }

        if (! (m.isNoteOn() && m.getVelocity() > 0)) return;
        const auto hit = mcu::decodeButton (m.getNoteNumber());

        switch (hit.action)
        {
            case mcu::Action::Mute:
                if (hit.strip < rec.getNumTracks())
                { auto& t = rec.getTrack (hit.strip); t.muted.store (! t.muted.load (std::memory_order_relaxed), std::memory_order_relaxed); }
                return;
            case mcu::Action::Solo:
                if (hit.strip < rec.getNumTracks())
                { auto& t = rec.getTrack (hit.strip); t.soloed.store (! t.soloed.load (std::memory_order_relaxed), std::memory_order_relaxed); }
                return;
            case mcu::Action::Arm:
                if (hit.strip < rec.getNumTracks())
                { auto& t = rec.getTrack (hit.strip); t.armed.store (! t.armed.load (std::memory_order_relaxed), std::memory_order_relaxed); }
                return;
            case mcu::Action::Play:
            case mcu::Action::Stop:
            case mcu::Action::Record:
            {
                // Transport touches the player -> marshal to the message thread.
                auto* eng = &engine;
                const auto a = hit.action;
                juce::MessageManager::callAsync ([eng, a]
                {
                    if (a == mcu::Action::Play)  { if (eng->isPlaying()) eng->stopPlayback(); else eng->startPlayback(); }
                    if (a == mcu::Action::Stop)  eng->stopPlayback();
                    // Record over MCU intentionally not wired in v1 (avoids an
                    // accidental take from a stray surface press).
                });
                return;
            }
            default: return;
        }
    }

    void MidiControlSurface::timerCallback()
    {
        if (output == nullptr) return;
        auto& rec = engine.getRecorder();
        const int n = rec.getNumTracks();

        for (int i = 0; i < kStrips; ++i)
        {
            const bool present = i < n;
            const float dB  = present ? rec.getTrack (i).gainDb.load (std::memory_order_relaxed) : mcu::kFaderMinDb;
            const bool mute = present && rec.getTrack (i).muted.load (std::memory_order_relaxed);
            const bool solo = present && rec.getTrack (i).soloed.load (std::memory_order_relaxed);
            const bool arm  = present && rec.getTrack (i).armed.load (std::memory_order_relaxed);
            const auto name = present ? rec.getTrack (i).name : juce::String();

            if (std::abs (dB - lastFaderDb[i]) > 0.05f)
            { output->sendMessageNow (mcu::fader (i, dB)); lastFaderDb[i] = dB; }
            if (mute != lastMute[i]) { output->sendMessageNow (mcu::led (mcu::Action::Mute, i, mute)); lastMute[i] = mute; }
            if (solo != lastSolo[i]) { output->sendMessageNow (mcu::led (mcu::Action::Solo, i, solo)); lastSolo[i] = solo; }
            if (arm  != lastArm[i])  { output->sendMessageNow (mcu::led (mcu::Action::Arm,  i, arm));  lastArm[i]  = arm; }
            if (name != lastName[i]) { output->sendMessageNow (mcu::scribble (i, name)); lastName[i] = name; }
        }

        const bool playing = engine.isPlaying();
        if (playing != lastPlaying)
        {
            output->sendMessageNow (mcu::led (mcu::Action::Play, 0, playing));
            output->sendMessageNow (mcu::led (mcu::Action::Stop, 0, ! playing));
            lastPlaying = playing;
        }
    }
}
