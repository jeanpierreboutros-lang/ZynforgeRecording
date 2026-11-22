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
    // Reversible so the surface echo round-trips. Unity (0 dB) lands ~83 % up.
    // The bottom is the engine's own floor (setTrackGainDb holds -60..+12): a
    // lower one let the bottom of the travel read -65, the engine keep -60,
    // and the echo push the motor back up to where -60 sat.
    constexpr float kFaderMinDb = -60.0f;
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
        BankLeft   = 0x2E,   // fader bank -8
        BankRight  = 0x2F,   // fader bank +8
        ChanLeft   = 0x30,   // nudge bank -1
        ChanRight  = 0x31,   // nudge bank +1
        Rewind     = 0x5B,
        Forward    = 0x5C,
        Stop       = 0x5D,
        Play       = 0x5E,
        Record     = 0x5F,
    };

    enum class Action { None, Arm, Solo, Mute, Select, Rewind, Forward, Stop, Play, Record,
                        BankLeft, BankRight, ChanLeft, ChanRight };

    // Fader touch: note-on 127 when a hand is on a touch-sensitive fader,
    // velocity 0 (or note-off) when it lets go. 0x68..0x6F are the strips,
    // 0x70 the master. Returns the strip (8 = master), or -1.
    constexpr int kTouchBase   = 0x68;
    constexpr int kMasterTouch = 0x70;
    inline int touchedFader (int note) noexcept
    {
        return note >= kTouchBase && note <= kMasterTouch ? note - kTouchBase : -1;
    }

    struct ButtonHit { Action action { Action::None }; int strip { -1 }; };

    inline ButtonHit decodeButton (int note) noexcept
    {
        if (note >= RecBase    && note < RecBase + 8)    return { Action::Arm,    note - RecBase };
        if (note >= SoloBase   && note < SoloBase + 8)   return { Action::Solo,   note - SoloBase };
        if (note >= MuteBase   && note < MuteBase + 8)   return { Action::Mute,   note - MuteBase };
        if (note >= SelectBase && note < SelectBase + 8) return { Action::Select, note - SelectBase };
        switch (note)
        {
            case BankLeft:  return { Action::BankLeft,  -1 };
            case BankRight: return { Action::BankRight, -1 };
            case ChanLeft:  return { Action::ChanLeft,  -1 };
            case ChanRight: return { Action::ChanRight, -1 };
            case Rewind:    return { Action::Rewind,  -1 };
            case Forward:   return { Action::Forward, -1 };
            case Stop:      return { Action::Stop,    -1 };
            case Play:      return { Action::Play,    -1 };
            case Record:    return { Action::Record,  -1 };
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

    // ── Meters ──────────────────────────────────────────────────────────
    // MCU meters are channel-pressure: data = (strip << 4) | level, level
    // 0..12, 0xE = peak-hold, 0xF = clip/overload.
    inline int peakToMeter (float peakLinear) noexcept
    {
        if (peakLinear >= 0.999f) return 0xF;                 // clip
        const float dbFS = juce::Decibels::gainToDecibels (juce::jmax (1.0e-5f, peakLinear));
        return juce::jlimit (0, 12, (int) std::lround (juce::jmap (dbFS, -54.0f, 0.0f, 0.0f, 12.0f)));
    }
    inline juce::MidiMessage meter (int strip, int level)
    {
        return juce::MidiMessage::channelPressureChange (1, ((strip & 0x7) << 4) | (level & 0xF));
    }

    // ── V-pots (pan) ────────────────────────────────────────────────────
    // Input: a relative encoder CC (0x10..0x17). Value bit 6 = direction,
    // bits 0..5 = tick count. Returns a signed delta (+cw / -ccw).
    inline int decodeVpotDelta (int ccValue) noexcept
    {
        const int ticks = ccValue & 0x3F;
        return (ccValue & 0x40) ? -ticks : ticks;
    }
    inline bool isVpotCc (int cc) noexcept { return cc >= 0x10 && cc <= 0x17; }
    inline int  vpotStrip (int cc) noexcept { return cc - 0x10; }

    // Output: the V-pot LED ring. CC 0x30..0x37, value = (mode<<4)|position.
    // Single-dot mode (0), position 1..11 with 6 = centre. pan in [-1, 1].
    inline juce::MidiMessage vpotRing (int strip, float pan)
    {
        const int pos = juce::jlimit (1, 11, (int) std::lround (juce::jmap (pan, -1.0f, 1.0f, 1.0f, 11.0f)));
        return juce::MidiMessage::controllerEvent (1, 0x30 + (strip & 0x7), pos & 0x0F);
    }

    // ── Master fader ────────────────────────────────────────────────────
    // The 9th fader is the master / monitor level: pitch-bend on MIDI
    // channel 9. dbToFader/faderToDb law is shared with the strips.
    constexpr int kMasterChannel = 9;            // 1-based MIDI channel
    inline bool isMasterFader (int midiChannel1Based) noexcept { return midiChannel1Based == kMasterChannel; }
    inline juce::MidiMessage masterFader (float dB)
    {
        return juce::MidiMessage::pitchWheel (kMasterChannel, dbToFader (dB));
    }

    // ── Jog wheel ───────────────────────────────────────────────────────
    // The big jog wheel is a relative encoder on CC 0x3C, same sign law as
    // a V-pot (bit 6 = direction, bits 0..5 = ticks). Returns signed ticks.
    constexpr int kJogCc = 0x3C;
    inline bool isJogCc (int cc) noexcept { return cc == kJogCc; }
    inline int  decodeJogDelta (int ccValue) noexcept { return decodeVpotDelta (ccValue); }

    // ── Timecode / time display ─────────────────────────────────────────
    // The 10-digit LED time display takes one CC per digit: controller
    // 0x40 = rightmost digit, 0x49 = leftmost. The value's low 6 bits are
    // the 7-seg character (ASCII '0'..'9' and ' ' map straight through);
    // OR 0x40 lights that digit's decimal point. Mackie Control groups the
    // digits hours 3, minutes 2, seconds 2, frames 3 (the BARS / BEATS /
    // SUB DIV / TICKS legend over the same digits), so HH:MM:SS:FF does not
    // fill the right-hand 8: see timeDisplayMessages.
    inline juce::MidiMessage timeDigit (int position0Right, char c, bool dot) noexcept
    {
        const int chr = ((int) (juce::uint8) c) & 0x3F;
        return juce::MidiMessage::controllerEvent (1, 0x40 + juce::jlimit (0, 9, position0Right),
                                                   (chr | (dot ? 0x40 : 0)) & 0x7F);
    }

    // Format a transport position (seconds) as HHMMSSFF for the display:
    // exactly 8 characters. A readout names the frame the position is IN, so
    // the frame count truncates (the last moment of frame 29 reads 29, never
    // the next second). The tiny allowance keeps a position that is exactly
    // on a frame boundary, but came out a hair under it in floating point, in
    // the frame it starts.
    inline juce::String timeString (double seconds, int fps) noexcept
    {
        const bool neg = seconds < 0.0;
        double s = std::abs (seconds);
        const int totalFrames = (int) std::floor (s * juce::jmax (1, fps) + 1.0e-6);
        const int ff = totalFrames % juce::jmax (1, fps);
        const int totSec = totalFrames / juce::jmax (1, fps);
        const int hh = (totSec / 3600) % 100;
        const int mm = (totSec / 60) % 60;
        const int ss = totSec % 60;
        juce::ignoreUnused (neg);
        return juce::String::formatted ("%02d%02d%02d%02d", hh, mm, ss, ff);
    }

    // Emit the 10 per-digit messages for an HH:MM:SS:FF readout into `out`,
    // left to right, in the Mackie Control grouping: hours 0x49-0x47,
    // minutes 0x46-0x45, seconds 0x44-0x43, frames 0x42-0x40. Two-digit
    // hours and frames sit right-aligned in their 3-digit groups, so 0x49
    // and 0x42 stay blank. The dot on the last digit of the hours, minutes
    // and seconds groups separates them; the frames group ends the readout
    // and has none.
    inline void timeDisplayMessages (double seconds, int fps, std::vector<juce::MidiMessage>& out)
    {
        const auto tc = timeString (seconds, fps);       // 8 chars "HHMMSSFF"
        const auto digit = [&out, &tc] (int position0Right, int charIndex, bool dot)
        {
            out.push_back (timeDigit (position0Right, (char) tc[charIndex], dot));
        };
        out.push_back (timeDigit (9, ' ', false));
        digit (8, 0, false);  digit (7, 1, true);        // hours
        digit (6, 2, false);  digit (5, 3, true);        // minutes
        digit (4, 4, false);  digit (3, 5, true);        // seconds
        out.push_back (timeDigit (2, ' ', false));
        digit (1, 6, false);  digit (0, 7, false);       // frames
    }
}
