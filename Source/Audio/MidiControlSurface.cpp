#include "MidiControlSurface.h"
#include "AudioEngine.h"

namespace zynforge
{
    MidiControlSurface::MidiControlSurface (AudioEngine& e) : engine (e)
    {
        for (int i = 0; i < kStrips; ++i)
        { lastFaderDb[i] = -999.0f; lastMute[i] = lastSolo[i] = lastArm[i] = false;
          lastMeter[i] = -1; lastPan[i] = -2.0f; lastName[i] = {}; }
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
        const int bank = bankOffset.load (std::memory_order_relaxed);

        // Motor fader move -> channel gain (atomic store; safe off this thread).
        if (m.isPitchWheel())
        {
            const int ch = bank + (m.getChannel() - 1);   // MCU channel 1..8 -> strip + bank
            if (ch >= 0 && ch < rec.getNumTracks())
                rec.getTrack (ch).gainDb.store (mcu::faderToDb (m.getPitchWheelValue()),
                                                std::memory_order_relaxed);
            return;
        }

        // V-pot turn -> pan (relative encoder).
        if (m.isController() && mcu::isVpotCc (m.getControllerNumber()))
        {
            const int ch = bank + mcu::vpotStrip (m.getControllerNumber());
            if (ch >= 0 && ch < rec.getNumTracks())
            {
                auto& t = rec.getTrack (ch);
                const float pan = juce::jlimit (-1.0f, 1.0f,
                    t.pan.load (std::memory_order_relaxed)
                    + (float) mcu::decodeVpotDelta (m.getControllerValue()) * 0.05f);
                t.pan.store (pan, std::memory_order_relaxed);
            }
            return;
        }

        if (! (m.isNoteOn() && m.getVelocity() > 0)) return;
        const auto hit = mcu::decodeButton (m.getNoteNumber());
        const int ch = bank + hit.strip;

        switch (hit.action)
        {
            case mcu::Action::BankLeft:
            case mcu::Action::ChanLeft:
            {
                const int step = hit.action == mcu::Action::BankLeft ? kStrips : 1;
                bankOffset.store (juce::jmax (0, bank - step), std::memory_order_relaxed);
                forceRefresh.store (true, std::memory_order_relaxed);
                return;
            }
            case mcu::Action::BankRight:
            case mcu::Action::ChanRight:
            {
                const int step = hit.action == mcu::Action::BankRight ? kStrips : 1;
                const int maxBank = juce::jmax (0, rec.getNumTracks() - kStrips);
                bankOffset.store (juce::jmin (maxBank, bank + step), std::memory_order_relaxed);
                forceRefresh.store (true, std::memory_order_relaxed);
                return;
            }
            case mcu::Action::Mute:
                if (ch < rec.getNumTracks())
                { auto& t = rec.getTrack (ch); t.muted.store (! t.muted.load (std::memory_order_relaxed), std::memory_order_relaxed); }
                return;
            case mcu::Action::Solo:
                if (ch < rec.getNumTracks())
                { auto& t = rec.getTrack (ch); t.soloed.store (! t.soloed.load (std::memory_order_relaxed), std::memory_order_relaxed); }
                return;
            case mcu::Action::Arm:
                if (ch < rec.getNumTracks())
                { auto& t = rec.getTrack (ch); t.armed.store (! t.armed.load (std::memory_order_relaxed), std::memory_order_relaxed); }
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
        const int n    = rec.getNumTracks();
        const int bank = bankOffset.load (std::memory_order_relaxed);

        if (forceRefresh.exchange (false, std::memory_order_relaxed))
            for (int i = 0; i < kStrips; ++i)
            { lastFaderDb[i] = -999.0f; lastMute[i] = lastSolo[i] = lastArm[i] = false;
              lastMeter[i] = -1; lastPan[i] = -2.0f; lastName[i] = {}; }

        for (int i = 0; i < kStrips; ++i)
        {
            const int  ch     = bank + i;
            const bool present = ch >= 0 && ch < n;
            const float dB  = present ? rec.getTrack (ch).gainDb.load (std::memory_order_relaxed) : mcu::kFaderMinDb;
            const bool mute = present && rec.getTrack (ch).muted.load (std::memory_order_relaxed);
            const bool solo = present && rec.getTrack (ch).soloed.load (std::memory_order_relaxed);
            const bool arm  = present && rec.getTrack (ch).armed.load (std::memory_order_relaxed);
            const float pan = present ? rec.getTrack (ch).pan.load (std::memory_order_relaxed) : 0.0f;
            const int   met = present ? mcu::peakToMeter (rec.getTrack (ch).peak.load (std::memory_order_relaxed)) : 0;
            const auto  name = present ? rec.getTrack (ch).name : juce::String();

            if (std::abs (dB - lastFaderDb[i]) > 0.05f)
            { output->sendMessageNow (mcu::fader (i, dB)); lastFaderDb[i] = dB; }
            if (mute != lastMute[i]) { output->sendMessageNow (mcu::led (mcu::Action::Mute, i, mute)); lastMute[i] = mute; }
            if (solo != lastSolo[i]) { output->sendMessageNow (mcu::led (mcu::Action::Solo, i, solo)); lastSolo[i] = solo; }
            if (arm  != lastArm[i])  { output->sendMessageNow (mcu::led (mcu::Action::Arm,  i, arm));  lastArm[i]  = arm; }
            if (std::abs (pan - lastPan[i]) > 0.01f) { output->sendMessageNow (mcu::vpotRing (i, pan)); lastPan[i] = pan; }
            if (met != lastMeter[i]) { output->sendMessageNow (mcu::meter (i, met)); lastMeter[i] = met; }
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
