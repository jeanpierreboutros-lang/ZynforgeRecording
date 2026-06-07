#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace zynforge::mcu
{
    // Mackie Control Universal protocol bits, as pure helpers so the encode /
    // decode is unit-testable away from any MIDI hardware.
    //
    //  - Faders: 14-bit pitch-bend, one MIDI channel per strip (0..7) + 8=master.
    //  - Buttons: note-on (vel 127 = press, 0 = release / LED off; we use note-on
    //    to set LEDs on output).
    //  - Scribble strips: a SysEx LCD write.

    // Fader law: map the 14-bit fader value <-> dB over a sensible live range.
    // Reversible so the surface echo round-trips. Unity (0 dB) lands ~84 % up.
    constexpr float kFaderMinDb = -65.0f;
    constexpr float kFaderMaxDb = 12.0f;

    inline float faderToDb (int value14) noexcept
    {
        const float n = juce::jlimit (0, 16383, value14) / 16383.0f;
        return juce::jmap (n, 0.0f, 1.0f, kFaderMinDb, kFaderMaxDb);
    }
    inline int dbToFader (float dB) noexcept
    {
        const float n = juce::jlimit (0.0f, 1.0f, juce::jmap (dB, kFaderMinDb, kFaderMaxDb, 0.0f, 1.0f));
        return juce::jlimit (0, 16383, (int) std::lround (n * 16383.0f));
    }

    // Button note numbers (subset we act on). Per-strip buttons are base + strip.
    enum Note
    {
        RecBase    = 0x00,   // 0..7   arm
        SoloBase   = 0x08,   // 8..15  solo
        MuteBase   = 0x10,   // 16..23 mute
        SelectBase = 0x18,   // 24..31 select
        Rewind     = 0x5B,
        Forward    = 0x5C,
        Stop       = 0x5D,
        Play       = 0x5E,
        Record     = 0x5F,
    };

    enum class Action { None, Arm, Solo, Mute, Select, Rewind, Forward, Stop, Play, Record };

    struct ButtonHit { Action action { Action::None }; int strip { -1 }; };

    inline ButtonHit decodeButton (int note) noexcept
    {
        if (note >= RecBase    && note < RecBase + 8)    return { Action::Arm,    note - RecBase };
        if (note >= SoloBase   && note < SoloBase + 8)   return { Action::Solo,   note - SoloBase };
        if (note >= MuteBase   && note < MuteBase + 8)   return { Action::Mute,   note - MuteBase };
        if (note >= SelectBase && note < SelectBase + 8) return { Action::Select, note - SelectBase };
        switch (note)
        {
            case Rewind:  return { Action::Rewind,  -1 };
            case Forward: return { Action::Forward, -1 };
            case Stop:    return { Action::Stop,    -1 };
            case Play:    return { Action::Play,    -1 };
            case Record:  return { Action::Record,  -1 };
        }
        return {};
    }

    inline int buttonNote (Action a, int strip) noexcept
    {
        switch (a)
        {
            case Action::Arm:    return RecBase + strip;
            case Action::Solo:   return SoloBase + strip;
            case Action::Mute:   return MuteBase + strip;
            case Action::Select: return SelectBase + strip;
            case Action::Rewind: return Rewind;  case Action::Forward: return Forward;
            case Action::Stop:   return Stop;    case Action::Play:    return Play;
            case Action::Record: return Record;  default: return -1;
        }
    }

    // SysEx scribble-strip write: F0 00 00 66 14 12 <offset> <chars> F7.
    // The MCU LCD is 2 lines x 56 chars; offset 0 = top-left. We write a
    // 7-char cell per strip (top line) at offset = strip * 7.
    inline juce::MidiMessage scribble (int strip, const juce::String& text)
    {
        juce::Array<juce::uint8> b { 0xF0, 0x00, 0x00, 0x66, 0x14, 0x12,
                                     (juce::uint8) juce::jlimit (0, 111, strip * 7) };
        const auto t = (text + "       ").substring (0, 7);
        for (int i = 0; i < 7; ++i) b.add ((juce::uint8) (t[i] >= 32 && t[i] < 127 ? t[i] : ' '));
        b.add (0xF7);
        return juce::MidiMessage (b.getRawDataPointer(), b.size());
    }

    inline juce::MidiMessage fader (int strip, float dB)
    {
        return juce::MidiMessage::pitchWheel (juce::jlimit (1, 16, strip + 1), dbToFader (dB));
    }
    inline juce::MidiMessage led (Action a, int strip, bool on)
    {
        return juce::MidiMessage::noteOn (1, buttonNote (a, strip), (juce::uint8) (on ? 127 : 0));
    }
}
