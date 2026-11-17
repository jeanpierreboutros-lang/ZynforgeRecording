#pragma once

#include <juce_graphics/juce_graphics.h>

#include "../Audio/ChannelAlarm.h"
#include "../Theme/BrandColors.h"

#include <cstdint>
#include <vector>

// What the dead-channel alarm means for each strip on screen. The banner names
// a channel, but with 56 channels and a dozen strips in view the faulted strip
// has to say so itself, in MIXER, EDIT and the meterbridge alike. These are the
// pure rules; the views only draw a Level.
namespace zynforge::stripalarm
{
    // Ordered: a strip shows the worst that applies to it.
    enum class Level : std::uint8_t { None = 0, NoSignal = 1, Lost = 2 };

    // The banner lines the channel alarms become (channelalarm::bannerText).
    inline constexpr const char* kStreamHead   = "!! INPUT STREAM LOST";
    inline constexpr const char* kChannelHead  = "! CHANNEL FAULT";
    inline constexpr const char* kNoSignalHead = "No signal yet this take";

    // One Level per track, for the live alarms. A strip is named by its first
    // track; both halves of a stereo pair carry its level, so a view can ask by
    // either. `pairedLeft (t)` is true when track t is the L of a stereo pair.
    // `out` is reused, so a steady tick allocates nothing.
    template <typename PairedLeft>
    void mapTracks (const std::vector<channelalarm::Alarm>& live, int numTracks,
                    const PairedLeft& pairedLeft, std::vector<Level>& out)
    {
        out.assign ((std::size_t) juce::jmax (0, numTracks), Level::None);
        const auto raise = [&] (int track, Level level)
        {
            if (track < 0 || track >= numTracks) return;
            const auto put = [&out, level] (int t)
            {
                auto& slot = out[(std::size_t) t];
                if ((int) level > (int) slot) slot = level;
            };
            put (track);
            if (track + 1 < numTracks && pairedLeft (track)) put (track + 1);
        };
        for (const auto& alarm : live)
        {
            if (alarm.endSample >= 0) continue;   // recovered, or ended with the take
            if (alarm.kind == channelalarm::kStream)
                for (int strip : alarm.strips) raise (strip, Level::Lost);
            else if (alarm.kind == channelalarm::kChannel)  raise (alarm.strip, Level::Lost);
            else if (alarm.kind == channelalarm::kNoSignal) raise (alarm.strip, Level::NoSignal);
        }
    }

    // The strip a click on each kind of line brings into view: the first one
    // it names, by first track. -1 when there is none.
    struct RevealTargets
    {
        int stream { -1 }, channel { -1 }, noSignal { -1 };
        bool operator== (const RevealTargets&) const = default;
    };

    inline RevealTargets revealTargets (const std::vector<channelalarm::Alarm>& live)
    {
        RevealTargets t;
        const auto first = [] (int& slot, int strip)
        {
            if (strip >= 0 && (slot < 0 || strip < slot)) slot = strip;
        };
        for (const auto& alarm : live)
        {
            if (alarm.endSample >= 0) continue;
            if (alarm.kind == channelalarm::kStream)
                for (int strip : alarm.strips) first (t.stream, strip);
            else if (alarm.kind == channelalarm::kChannel)  first (t.channel, alarm.strip);
            else if (alarm.kind == channelalarm::kNoSignal) first (t.noSignal, alarm.strip);
        }
        return t;
    }

    // The strip a banner line leads to, or -1 for a line about something else.
    inline int revealTrackFor (const juce::String& line, const RevealTargets& t)
    {
        if (line.startsWith (kStreamHead))   return t.stream;
        if (line.startsWith (kChannelHead))  return t.channel;
        if (line.startsWith (kNoSignalHead)) return t.noSignal;
        return -1;
    }

    // A lost input's header band and tag: the alarm red, sunk only as far as
    // white text on it needs to read at 4.5:1. The outline stays accentRecord.
    inline juce::Colour lostFill() { return brand::stripHeaderFill (brand::accentRecord); }
    // The tag's fill for a level (NoSignal is amber, a notice).
    inline juce::Colour tagFill (Level level) { return level == Level::Lost ? lostFill() : brand::alertAmber; }

    // The words a marked strip shows. Long when the strip has room.
    inline juce::String tagText (Level level, bool shortForm)
    {
        switch (level)
        {
            case Level::Lost:     return shortForm ? "LOST" : "INPUT LOST";
            case Level::NoSignal: return shortForm ? "NO SIG" : "NO SIGNAL";
            case Level::None:     break;
        }
        return {};
    }
}
