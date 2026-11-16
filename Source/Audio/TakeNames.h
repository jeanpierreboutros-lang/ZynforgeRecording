#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

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

    // A label as a file name holds it: what labelFor writes, at any length.
    // No character labelFor turns into a space or drops, single spaces only,
    // none at either end, and not only digits. The 64-byte cap is not checked:
    // a name read back decomposed (NFD) from HFS+ or a backup drive spells an
    // accented letter as the letter and a combining accent, a byte longer each
    // time, and it is still the take labelFor named.
    inline bool isLabel (const juce::String& label)
    {
        static const juce::String toSpace ("/\\:*?<>|[];,.#_'\"`");
        if (label.isEmpty() || label.startsWithChar (' ') || label.endsWithChar (' ') || label.contains ("  "))
            return false;
        for (auto p = label.getCharPointer(); ! p.isEmpty(); ++p)
        {
            const auto c = *p;
            if (c < 0x20 || c == 0x7f || toSpace.containsChar (c)) return false;
            if (c != ' ' && juce::CharacterFunctions::isWhitespace (c)) return false;
        }
        return ! label.removeCharacters (" ").containsOnly ("0123456789");
    }

    struct Parsed
    {
        int          number { 0 };   // 1-based track number; 0 = not named for a track
        juce::String stem;           // "Track_07" or "07 KiCK"
        juce::String rest;           // what follows: ".wav", "_part02.wav", ".punchbase.wav", ".json" ...
    };

    // Reads a file NAME (no folder). "Track_" takes 1 to 3 digits, as before. A
    // named stem's number is written exactly as freshStem writes it ("07", "123")
    // and its label as labelFor makes one (isLabel), so a file someone drops in
    // the folder ("1 intro.wav", "07 Vox, lead.wav") is not taken for a track's.
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
            if (! isLabel (fileName.substring (labelStart, end))) return {};
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

    // A punch's copy of a take file, moved aside until the splice:
    // "<stem>.punchbase.<ext>" or "<stem>_partKK.punchbase.<ext>".
    inline bool isPunchSidecar (const Parsed& p)
    {
        const auto at = p.rest.lastIndexOf (".punchbase.");
        if (p.number <= 0 || at < 0 || ! isAudioExtension (p.rest.substring (at + 10))) return false;
        return isTakeAudio (parse (p.stem + p.rest.substring (0, at) + p.rest.substring (at + 10)));
    }

    // A Consolidate render: "<stem>_consolidated_K.<ext>".
    inline bool isRender (const Parsed& p)
    {
        if (p.number <= 0 || ! p.rest.startsWith ("_consolidated_")) return false;
        const auto dot = p.rest.indexOfChar ('.');
        if (dot < 0 || ! isAudioExtension (p.rest.substring (dot))) return false;
        const auto digits = p.rest.substring (14, dot);
        return digits.isNotEmpty() && digits.containsOnly ("0123456789");
    }

    // The 1-based track a main take file belongs to, or 0.
    inline int mainNumber (const juce::File& f)
    {
        const auto p = parse (f.getFileName());
        return isMain (p) ? p.number : 0;
    }

    // Whole-folder listings (every file stat'ed) made on this thread: what
    // the tests count to see that one operation lists a folder once, not once
    // per strip.
    inline int& folderListingsOnThisThread() noexcept
    {
        static thread_local int listings = 0;
        return listings;
    }

    inline juce::Array<juce::File> listFolder (const juce::File& dir)
    {
        if (! dir.isDirectory()) return {};
        ++folderListingsOnThisThread();
        return dir.findChildFiles (juce::File::findFiles, false, "*");
    }

    // Every file in `dir` (not below it) named for a track, sorted by name:
    // takes, parts, punch sidecars and markers, renders, and anything else that
    // starts with a track's stem. Callers filter by kind. This is what a
    // "Track_*" glob was before names: hidden "._" files never match.
    inline juce::Array<juce::File> trackFiles (const juce::File& dir)
    {
        juce::Array<juce::File> out;
        for (const auto& f : listFolder (dir))
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
        for (const auto& f : listFolder (dir))
            if (isTakeAudio (parse (f.getFileName()))) return true;
        return false;
    }

    // "two take files, 05 KiCK copy.wav and 05 KiCK.wav: move one of them out
    // of Audio Files": why a strip has no take anything here may pick.
    inline juce::String describeClash (const juce::StringArray& files, const juce::File& folder)
    {
        static const char* const counts[] = { "no", "one", "two", "three", "four", "five" };
        const auto count = files.size() < 6 ? juce::String (counts[files.size()]) : juce::String (files.size());
        juce::String list;
        for (int i = 0; i < files.size(); ++i)
            list << (i == 0 ? "" : i == files.size() - 1 ? " and " : ", ") << files[i];
        return count + " take files, " + list + ": move one of them out of " + folder.getFileName();
    }

    // Every track's take in one folder, from one listing. The number is the
    // identity, and a folder holds one stem per number. A main file's stem
    // names the track; without one, a part's (a take whose lead-in is still
    // owed); without either, a punch sidecar's (a take whose files a punch has
    // moved aside), so nothing new is ever named beside it. Nothing else claims
    // a number: a Consolidate render, a DAW's peak file ("03 SNARE.wav.reapeaks")
    // or any other file that starts like a take is not one.
    //
    // A number whose mains and parts carry more than one stem CLASHES: a Finder
    // duplicate ("05 KiCK copy.wav"), a "Keep Both" or sync copy, a DAW glue, a
    // main renamed in Finder away from its parts, or a "Track_05.wav" an older
    // build wrote beside "05 KiCK.wav". Nothing here picks one: mainFile is
    // empty for it, and every caller that plays, records, moves, checks or
    // exports a strip says so and names the files (clash).
    //
    // Names::legacyOnly reads only "Track_NN" names: a session folder's root,
    // where takes lived before Audio Files existed. A song kept there
    // ("01 Intro.wav") is not a take.
    class StemIndex
    {
    public:
        enum class Names { any, legacyOnly };

        StemIndex() = default;

        explicit StemIndex (const juce::File& dir, Names names = Names::any)
            : folder (dir)
        {
            juce::Array<juce::File> files;
            for (const auto& f : listFolder (dir))
            {
                const auto p = parse (f.getFileName());
                if (p.number > 0 && (names == Names::any || p.stem.startsWith ("Track_"))) files.add (f);
            }
            files.sort();
            for (const auto& f : files)
            {
                const auto p = parse (f.getFileName());
                auto& n = numbers[p.number];
                if (isMain (p) || partNumber (p) > 0)
                {
                    (isMain (p) ? n.mains : n.parts).add (f);
                    if (! n.stems.contains (p.stem))
                    {
                        n.stems.add (p.stem);
                        n.clashFiles.add (f.getFileName());
                    }
                }
                else if (isPunchSidecar (p)) { if (n.sidecarStem.isEmpty()) n.sidecarStem = p.stem; }
                else if (isRender (p)) n.renders.add (f);
            }
        }

        const juce::File& directory() const noexcept { return folder; }

        // The stem on disk for strip `track` (0-based); empty when it has none.
        // For a number that clashes, the first main's: never write by it
        // without checking clashes first.
        juce::String stem (int track) const
        {
            const auto* n = find (track);
            if (n == nullptr) return {};
            if (! n->mains.isEmpty()) return parse (n->mains.getFirst().getFileName()).stem;
            if (! n->parts.isEmpty()) return parse (n->parts.getFirst().getFileName()).stem;
            return n->sidecarStem;
        }

        // The stem on disk, or else the legacy name (what a reader of an
        // old-layout session expects for a track with no file).
        juce::String stemOrLegacy (int track) const
        {
            const auto s = stem (track);
            return s.isNotEmpty() ? s : legacyStem (track);
        }

        // Does a take, a part or a punch sidecar claim strip `track`?
        bool has (int track) const { return stem (track).isNotEmpty(); }

        // Does strip `track` hold a main file or a part?
        bool holdsTakeAudio (int track) const
        {
            const auto* n = find (track);
            return n != nullptr && ! (n->mains.isEmpty() && n->parts.isEmpty());
        }

        bool clashes (int track) const
        {
            const auto* n = find (track);
            return n != nullptr && n->stems.size() > 1;
        }

        // One file of each stem that claims a clashing strip, by name; empty
        // when it does not clash.
        juce::StringArray clashingFiles (int track) const
        {
            const auto* n = find (track);
            return n != nullptr && n->stems.size() > 1 ? n->clashFiles : juce::StringArray();
        }

        // "two take files, A and B: move one of them out of Audio Files ...",
        // or empty.
        juce::String clash (int track) const
        {
            const auto files = clashingFiles (track);
            return files.isEmpty() ? juce::String() : describeClash (files, folder);
        }

        // Strips (0-based) that clash, in order.
        std::vector<int> clashingTracks() const
        {
            std::vector<int> out;
            for (const auto& [number, n] : numbers)
                if (n.stems.size() > 1) out.push_back (number - 1);
            return out;
        }

        // Strips (0-based) holding a main file or a part, in order.
        std::vector<int> tracksWithTakeAudio() const
        {
            std::vector<int> out;
            for (const auto& [number, n] : numbers)
                if (! (n.mains.isEmpty() && n.parts.isEmpty())) out.push_back (number - 1);
            return out;
        }

        // The take's main file as the readers pick it: .wav, then .flac, .aif,
        // .aiff. Empty when the strip has none, or clashes.
        juce::File mainFile (int track) const
        {
            const auto* n = find (track);
            if (n == nullptr || n->stems.size() > 1 || n->mains.isEmpty()) return {};
            for (auto* ext : { ".wav", ".flac", ".aif", ".aiff" })
                for (const auto& f : n->mains)
                    if (f.getFileExtension().equalsIgnoreCase (ext)) return f;
            return {};
        }

        // The take's mains and parts, by name; empty for none.
        juce::Array<juce::File> takeAudio (int track) const
        {
            const auto* n = find (track);
            if (n == nullptr) return {};
            auto files = n->mains;
            files.addArray (n->parts);
            files.sort();
            return files;
        }

        // The strip's Consolidate renders, whatever stem they carry.
        juce::Array<juce::File> renders (int track) const
        {
            const auto* n = find (track);
            return n != nullptr ? n->renders : juce::Array<juce::File>();
        }

    private:
        struct Number
        {
            juce::Array<juce::File> mains, parts, renders;   // by name
            juce::StringArray stems, clashFiles;             // each main/part stem once, and its first file
            juce::String sidecarStem;
        };

        const Number* find (int track) const
        {
            const auto it = numbers.find (track + 1);
            return it == numbers.end() ? nullptr : &it->second;
        }

        juce::File folder;
        std::map<int, Number> numbers;   // 1-based number
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
