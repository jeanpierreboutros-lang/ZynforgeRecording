#pragma once

#include <juce_core/juce_core.h>

#include <map>

// How a take file is named (2026-11-16).
//
// A take file carries its strip's position and the strip's name at the moment
// the take started: "07 KiCK.wav". A strip with no name of its own (empty, or
// only digits, like the default "7") and every take recorded before this keep
// "Track_07.wav". Both forms are read everywhere.
//
// The NUMBER is the identity. Every lookup finds a track's files by it, never
// by the name, and one folder holds one base stem per number. Renaming a strip
// later does not rename its files; a strip move renumbers them and keeps the
// label. Parts, punch sidecars, renders and lead-in markers are the base stem
// plus the suffixes they always had: "07 KiCK_part02.wav",
// "07 KiCK.punchbase.wav", "07 KiCK_consolidated_1.wav",
// "Pending Lead-ins/07 KiCK.json".
//
// A label never holds '_' or '.', so no suffix can be read as part of a name
// (or a name as a suffix). It never holds a character JUCE's wildcard matching
// treats specially either: fnmatch's * ? [ ] \, the pattern separators , and
// ;, and quotes. So a glob built from a stem, like stem + "_part*", matches
// exactly what it says.
namespace zynforge::takenames
{
    inline constexpr int kMaxLabelBytes = 64;

    // ".wav", ".flac", ".aif", ".aiff", any case. Exactly one extension.
    inline bool isAudioExtension (const juce::String& ext)
    {
        const auto e = ext.toLowerCase();
        return e == ".wav" || e == ".flac" || e == ".aif" || e == ".aiff";
    }

    // The name every take had before 2026-11-16 (`track` is 0-based).
    inline juce::String legacyStem (int track)
    {
        return juce::String::formatted ("Track_%02d", track + 1);
    }

    // A strip name as a file label: characters a file system, a suffix or a
    // wildcard would misread become spaces, quotes are dropped, runs of spaces
    // become one, and it is cut to kMaxLabelBytes of UTF-8 on a character
    // boundary. Empty when nothing but digits is left: the number already says
    // that, and the file keeps its old name.
    inline juce::String labelFor (const juce::String& stripName)
    {
        static const juce::String toSpace ("/\\:*?<>|[];,.#_");
        juce::String label;
        int bytes = 0;
        bool space = false;
        for (auto p = stripName.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const auto c = *p;
            if (c == '\'' || c == '"' || c == '`') continue;
            if (c < 0x20 || c == 0x7f || toSpace.containsChar (c) || juce::CharacterFunctions::isWhitespace (c))
            {
                space = label.isNotEmpty();
                continue;
            }
            const int need = (int) juce::CharPointer_UTF8::getBytesRequiredFor (c) + (space ? 1 : 0);
            if (bytes + need > kMaxLabelBytes) break;
            if (space) label << ' ';
            label << juce::String::charToString (c);
            bytes += need;
            space = false;
        }
        if (label.removeCharacters (" ").containsOnly ("0123456789")) return {};
        return label;
    }

    // The base stem a NEW take of strip `track` (0-based) gets.
    inline juce::String freshStem (int track, const juce::String& stripName)
    {
        const auto label = labelFor (stripName);
        return label.isEmpty() ? legacyStem (track)
                               : juce::String::formatted ("%02d ", track + 1) + label;
    }

    struct Parsed
    {
        int          number { 0 };   // 1-based track number; 0 = not named for a track
        juce::String stem;           // "Track_07" or "07 KiCK"
        juce::String rest;           // what follows: ".wav", "_part02.wav", ".punchbase.wav", ".json" ...
    };

    // Reads a file NAME (no folder). "Track_" takes 1 to 3 digits, as before. A
    // named stem's number is written exactly as freshStem writes it ("07", "123")
    // and its label exactly as labelFor makes one, so a file someone drops in the
    // folder ("1 intro.wav", "07 Vox, lead.wav") is not taken for a track's.
    inline Parsed parse (const juce::String& fileName)
    {
        Parsed p;
        const int length = fileName.length();
        int end = 0;
        if (fileName.startsWith ("Track_"))
        {
            end = 6;
            while (end < length && end < 9 && juce::CharacterFunctions::isDigit (fileName[end])) ++end;
            if (end == 6 || (end < length && fileName[end] != '_' && fileName[end] != '.')) return {};
            p.number = fileName.substring (6, end).getIntValue();
        }
        else
        {
            while (end < length && end < 3 && juce::CharacterFunctions::isDigit (fileName[end])) ++end;
            if (end < 2 || end >= length || fileName[end] != ' ') return {};
            const auto digits = fileName.substring (0, end);
            p.number = digits.getIntValue();
            if (juce::String::formatted ("%02d", p.number) != digits) return {};
            const int labelStart = end + 1;
            end = labelStart;
            while (end < length && fileName[end] != '_' && fileName[end] != '.') ++end;
            const auto label = fileName.substring (labelStart, end);
            if (label.isEmpty() || labelFor (label) != label) return {};
        }
        if (p.number < 1) return {};
        p.stem = fileName.substring (0, end);
        p.rest = fileName.substring (end);
        return p;
    }

    // A base stem on its own: "Track_07" or "07 KiCK", nothing after it.
    inline bool isStem (const juce::String& stem)
    {
        const auto p = parse (stem);
        return p.number > 0 && p.rest.isEmpty();
    }

    // A take's main file: the stem and one audio extension.
    inline bool isMain (const Parsed& p)
    {
        return p.number > 0 && p.rest.lastIndexOfChar ('.') == 0 && isAudioExtension (p.rest);
    }

    // 2 and up for "<stem>_partKK.<audio ext>"; 0 for anything else.
    inline int partNumber (const Parsed& p)
    {
        if (p.number <= 0 || ! p.rest.startsWith ("_part")) return 0;
        const auto dot = p.rest.indexOfChar ('.');
        if (dot < 0 || ! isAudioExtension (p.rest.substring (dot))) return 0;
        const auto digits = p.rest.substring (5, dot);
        if (digits.isEmpty() || digits.length() > 4 || ! digits.containsOnly ("0123456789")) return 0;
        const int part = digits.getIntValue();
        return part >= 2 ? part : 0;
    }

    // The main file or a continuation part of a take: what plays.
    inline bool isTakeAudio (const Parsed& p) { return isMain (p) || partNumber (p) > 0; }

    // The 1-based track a main take file belongs to, or 0.
    inline int mainNumber (const juce::File& f)
    {
        const auto p = parse (f.getFileName());
        return isMain (p) ? p.number : 0;
    }

    // Every file in `dir` (not below it) named for a track, sorted by name:
    // takes, parts, punch sidecars and markers, renders, and anything else that
    // starts with a track's stem. Callers filter by kind. This is what a
    // "Track_*" glob was before names: hidden "._" files never match.
    inline juce::Array<juce::File> trackFiles (const juce::File& dir)
    {
        juce::Array<juce::File> out;
        if (! dir.isDirectory()) return out;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
            if (parse (f.getFileName()).number > 0) out.add (f);
        out.sort();
        return out;
    }

    // Every file in `dir` named for a track whose last extension is one of
    // `extensions` ("wav;flac;aif;aiff"): takes, parts, punch sidecars and
    // renders alike, as "Track_*.wav;Track_*.flac;..." matched before names.
    inline juce::Array<juce::File> trackFilesWithExtension (const juce::File& dir, const juce::String& extensions)
    {
        auto files = trackFiles (dir);
        files.removeIf ([&extensions] (const juce::File& f) { return ! f.hasFileExtension (extensions); });
        return files;
    }

    // Main files and parts only, any container.
    inline juce::Array<juce::File> takeAudioFiles (const juce::File& dir)
    {
        auto files = trackFiles (dir);
        files.removeIf ([] (const juce::File& f) { return ! isTakeAudio (parse (f.getFileName())); });
        return files;
    }

    // Does `dir` hold any track's take audio?
    inline bool holdsTakeAudio (const juce::File& dir)
    {
        if (! dir.isDirectory()) return false;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*"))
            if (isTakeAudio (parse (f.getFileName()))) return true;
        return false;
    }

    // Every track's base stem in one folder, from one listing. A main file's
    // stem wins; without one, the stem of any other file named for the track
    // (a part, a punch sidecar, a render) stands for it (a take whose main file a punch has moved aside, or an
    // orphaned part), so nothing new is ever named beside it. With two main
    // stems for one number, which nothing here writes, the first by name wins.
    class StemIndex
    {
    public:
        StemIndex() = default;

        explicit StemIndex (const juce::File& dir)
        {
            std::map<int, juce::String> other;
            for (const auto& f : trackFiles (dir))
            {
                const auto p = parse (f.getFileName());
                if (isMain (p)) stems.emplace (p.number, p.stem);
                else            other.emplace (p.number, p.stem);
            }
            for (const auto& [number, stem] : other) stems.emplace (number, stem);
        }

        // The stem on disk for strip `track` (0-based); empty when it has none.
        juce::String stem (int track) const
        {
            const auto it = stems.find (track + 1);
            return it == stems.end() ? juce::String() : it->second;
        }

        // The stem on disk, or else the legacy name (what a reader of an
        // old-layout session expects for a track with no file).
        juce::String stemOrLegacy (int track) const
        {
            const auto s = stem (track);
            return s.isNotEmpty() ? s : legacyStem (track);
        }

        bool has (int track) const { return stems.count (track + 1) > 0; }

    private:
        std::map<int, juce::String> stems;   // 1-based number -> stem
    };

    inline juce::String existingStem (const juce::File& dir, int track)
    {
        return StemIndex (dir).stem (track);
    }

    // A stem moved to another strip position keeps its label.
    inline juce::String renumbered (const juce::String& stem, int newTrack)
    {
        const auto p = parse (stem);
        if (p.number <= 0 || p.rest.isNotEmpty()) return {};
        return stem.startsWith ("Track_")
            ? legacyStem (newTrack)
            : juce::String::formatted ("%02d", newTrack + 1) + stem.fromFirstOccurrenceOf (" ", true, false);
    }
}
