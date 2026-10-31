#pragma once

#include "SessionInterchange.h"

namespace zynforge::reaperproject
{
    // Plain-text .RPP writer. The field choices below were measured against
    // REAPER 7.48, by rendering projects and reading them back through its
    // scripting API; tools/verify_reaper_export.py repeats that proof.

    // Equal-power fades: shape 7 follows sin(t * pi/2) to within 0.003.
    // Shape 0 is exactly the linear ramp.
    constexpr int kLinearFadeShape = 0, kEqualPowerFadeShape = 7;

    // With this per-track pan law, a mono track in balance mode and a stereo
    // track in dual-pan mode both match mix::constantPowerPan to five decimals,
    // so an unedited export balances the way the session mixes.
    constexpr const char* kConstantPowerLaw = "0.70710678";
    constexpr int kPanModeBalance = 3, kPanModeDualPan = 6;

    // RPP has no escape character: a field is wrapped in whichever of " ' `
    // it does not contain. With all three present, REAPER itself turns the
    // backticks into apostrophes, so the name survives with that one change.
    inline juce::String quoted (const juce::String& text)
    {
        const bool dq = text.containsChar ('"'), sq = text.containsChar ('\''), bq = text.containsChar ('`');
        if (dq && sq && bq) return "`" + text.replaceCharacter ('`', '\'') + "`";
        const juce::String mark (dq ? (sq ? "`" : "'") : "\"");
        return mark + text + mark;
    }

    // A name can lose a backtick; a path cannot, or the clip opens offline. A
    // line break would end the statement it sits in.
    inline bool isQuotable (const juce::String& path)
    {
        return ! path.containsAnyOf ("\r\n")
            && ! (path.containsChar ('"') && path.containsChar ('\'') && path.containsChar ('`'));
    }

    inline juce::String number (double value, int places = 14) { return juce::String (value, places); }

    inline juce::String relativePath (const juce::File& target, const juce::File& projectFile)
    {
        return target.getRelativePathFrom (projectFile.getParentDirectory()).replaceCharacter ('\\', '/');
    }

    // REAPER stores R in the low byte on every platform, with bit 24 meaning
    // "colour set"; ColorToNative differs per OS, the project file does not.
    inline juce::int64 peakColour (juce::uint32 argb)
    {
        const juce::Colour colour (argb);
        return 0x1000000
             | (juce::int64) colour.getRed()
             | ((juce::int64) colour.getGreen() << 8)
             | ((juce::int64) colour.getBlue()  << 16);
    }

    inline juce::String sourceType (const juce::File& file)
    {
        return file.hasFileExtension ("flac") ? "FLAC" : "WAVE";
    }

    inline void writeItem (juce::String& out, const interchange::Item& item, double sampleRate,
                           const juce::File& projectFile)
    {
        const int shape = item.fadeCurve == 1 ? kEqualPowerFadeShape : kLinearFadeShape;
        const auto fade = [&] (juce::int64 samples)
        {
            return juce::String (shape) + " " + number ((double) samples / sampleRate)
                 + " 0 " + juce::String (shape) + " 0 0 0";
        };
        const double gain = juce::Decibels::decibelsToGain ((double) item.gainDb, -60.0);

        out << "    <ITEM\n"
            << "      POSITION " << number ((double) item.position / sampleRate) << "\n"
            << "      LENGTH " << number ((double) item.length / sampleRate) << "\n"
            << "      LOOP 0\n"
            << "      FADEIN " << fade (item.fadeIn) << "\n"
            << "      FADEOUT " << fade (item.fadeOut) << "\n"
            << "      MUTE " << (item.muted ? 1 : 0) << " 0\n"
            << "      NAME " << quoted (item.name) << "\n"
            << "      VOLPAN " << number (gain, 10) << " 0 1 -1\n"
            << "      SOFFS " << number ((double) item.sourceOffset / item.fileRate) << "\n"
            << "      PLAYRATE 1 1 0 -1 0 0.0025\n";
        // A stereo file on a mono strip: 3 plays its left side, 4 its right.
        if (item.channel >= 0 && item.fileChannels >= 2)
            out << "      CHANMODE " << (item.channel == 0 ? 3 : 4) << "\n";
        out << "      <SOURCE " << sourceType (item.file) << "\n"
            << "        FILE " << quoted (relativePath (item.file, projectFile)) << "\n"
            << "      >\n"
            << "    >\n";
    }

    // `savedAt` is REAPER's project timestamp (seconds since 1970).
    inline juce::String build (const interchange::Project& project, const juce::File& projectFile,
                               const juce::StringArray& notes, juce::int64 savedAt)
    {
        juce::String out;
        out.preallocateBytes (4096 + project.tracks.size() * 1024);
        out << "<REAPER_PROJECT 0.1 \"6.0/ZynforgeRecording\" " << savedAt << "\n";
        if (! notes.isEmpty())
        {
            out << "  <NOTES 0 0\n";
            for (const auto& line : notes)
                out << "    |" << interchange::cleanText (line) << "\n";
            out << "  >\n";
        }
        // Clips that overlap sum in the session. REAPER's other modes let an
        // enclosed item replace the one around it.
        out << "  ITEMMIX 1\n"
            // A live recording has no grid: a tempo change in the DAW must not
            // move or stretch the audio.
            << "  TIMELOCKMODE 0\n"
            << "  TEMPOENVLOCKMODE 0\n"
            << "  SAMPLERATE " << juce::roundToInt (project.sampleRate) << " 1 0\n"
            << "  TEMPO " << number (project.tempoBpm, 6) << " " << project.timeSigNum
            << " " << project.timeSigDen << "\n"
            << "  MASTERMUTESOLO " << (project.masterMuted ? 1 : 0) << "\n"
            << "  MASTER_NCH 2 2\n"
            << "  MASTER_VOLUME " << number (project.masterGain, 10) << " 0 -1 -1 1\n";

        int markerId = 0;
        for (const auto& marker : project.markers)
            out << "  MARKER " << ++markerId << " " << number ((double) marker.sample / project.sampleRate)
                << " " << quoted (marker.name) << " 0\n";

        for (const auto& track : project.tracks)
        {
            const bool stereo = track.channels == 2;
            out << "  <TRACK\n"
                << "    NAME " << quoted (track.name) << "\n";
            if (track.colourArgb != 0)
                out << "    PEAKCOL " << peakColour (track.colourArgb) << "\n";
            out << "    PANMODE " << (stereo ? kPanModeDualPan : kPanModeBalance) << "\n"
                << "    VOLPAN " << number (track.gain, 10) << " " << number (stereo ? 0.0 : track.pan, 6)
                << " " << kConstantPowerLaw << " "
                << number (stereo ? track.panLeft : -1.0, 6) << " "
                << number (stereo ? track.panRight : 1.0, 6) << "\n"
                << "    MUTESOLO " << (track.muted ? 1 : 0) << " 0 0\n"
                << "    NCHAN 2\n";
            for (const auto& item : track.items)
                writeItem (out, item, project.sampleRate, projectFile);
            out << "  >\n";
        }
        out << ">\n";
        return out;
    }
}
