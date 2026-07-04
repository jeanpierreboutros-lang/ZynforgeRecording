#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <vector>

namespace zynforge
{
    // The files that make up one take, in playback order: the main
    // Track_NN.<ext> followed by its Track_NN_partXX.<ext> continuations
    // (from auto-split past the container cap, or from continue-recording).
    // Returns just the main file when there are no parts; empty if it's absent.
    inline std::vector<juce::File> findTakeParts (const juce::File& mainFile)
    {
        std::vector<juce::File> parts;
        if (! mainFile.existsAsFile()) return parts;
        const auto dir  = mainFile.getParentDirectory();
        const auto stem = mainFile.getFileNameWithoutExtension();   // "Track_07"
        const auto ext  = mainFile.getFileExtension();

        // Highest part index that exists. Scan the WHOLE set on disk rather
        // than stopping at the first numbering gap: an orphaned later part
        // (e.g. _part04 present while _part03 is missing) must still be
        // accounted for, or (a) a fresh continuation could be numbered into
        // the hole and shadow/overwrite the orphan, and (b) every part after
        // the hole would silently shift earlier in the take. Missing
        // intermediate parts are included below as their EXPECTED paths so
        // the reader (ConcatReader::create) sees the hole and refuses instead
        // of seamlessly concatenating the survivors.
        int highest = 1;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false,
                                                 stem + "_part*" + ext))
        {
            const int pn = f.getFileNameWithoutExtension()
                            .fromLastOccurrenceOf ("_part", false, false).getIntValue();
            if (pn > highest) highest = pn;
        }

        parts.push_back (mainFile);
        for (int p = 2; p <= highest; ++p)
            parts.push_back (dir.getChildFile (stem + "_part"
                                + juce::String::formatted ("%02d", p) + ext));
        return parts;
    }

    // ── Multi-part take reader ─────────────────────────────────────────────
    // Presents a take split across several part files as ONE seamless reader:
    // owns the per-part readers and maps a global sample position to the part
    // holding it, reading across boundaries (padding past-end with silence).
    // Read-only; all parts share format/channels/SR (same take), so the public
    // format fields come from the first part. Used by SessionPlayer (playback)
    // and the EDIT thumbnail (so the whole take draws, not just part 1).
    class ConcatReader final : public juce::AudioFormatReader
    {
    public:
        explicit ConcatReader (std::vector<std::unique_ptr<juce::AudioFormatReader>> ps)
            : juce::AudioFormatReader (nullptr, "ZF multi-part"), parts (std::move (ps))
        {
            jassert (! parts.empty());
            auto& first = *parts.front();
            sampleRate            = first.sampleRate;
            bitsPerSample         = first.bitsPerSample;
            usesFloatingPointData = first.usesFloatingPointData;
            numChannels           = first.numChannels;
            metadataValues        = first.metadataValues;

            juce::int64 acc = 0;
            for (auto& p : parts) { partStart.push_back (acc); acc += p->lengthInSamples; }
            lengthInSamples = acc;
        }

        // Build a ConcatReader for a take's files via `fm`. Returns nullptr if
        // none open. Caller owns the result.
        static std::unique_ptr<juce::AudioFormatReader> create (
            juce::AudioFormatManager& fm, const std::vector<juce::File>& files)
        {
            std::vector<std::unique_ptr<juce::AudioFormatReader>> rs;
            int  firstOpened = -1, lastOpened = -1;
            bool holeBetween = false;
            for (int i = 0; i < (int) files.size(); ++i)
            {
                if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (files[(size_t) i]) })
                {
                    // A part opened AFTER a hole -> that hole is a MISSING /
                    // unreadable MIDDLE part; concatenating past it would shift
                    // this part and every later one earlier in the take.
                    if (lastOpened >= 0 && i != lastOpened + 1) holeBetween = true;
                    if (firstOpened < 0) firstOpened = i;
                    lastOpened = i;
                    rs.push_back (std::move (r));
                }
            }
            if (rs.empty()) return nullptr;

            // Refuse loudly when a MIDDLE part (a hole between survivors) or a
            // LEADING part (files before the first that opened) is missing /
            // unreadable: its length is unknowable, so we cannot insert a
            // correctly-sized silent gap, and silently dropping it would
            // time-shift the rest of the take. A missing TRAILING part only
            // truncates the tail (no shift), so the contiguous survivors from
            // part 1 are used as-is -- preserving the all-parts-present path
            // exactly.
            if (holeBetween || firstOpened > 0)
            {
                jassertfalse;
                return nullptr;
            }

            if (rs.size() == 1)  return std::move (rs.front());
            return std::make_unique<ConcatReader> (std::move (rs));
        }

        bool readSamples (int* const* dest, int numDest, int destOffset,
                          juce::int64 startSampleInFile, int numSamples) override
        {
            int written = 0;
            juce::int64 pos = startSampleInFile;
            while (numSamples > 0)
            {
                int pi = -1;
                for (int i = (int) parts.size() - 1; i >= 0; --i)
                    if (pos >= partStart[(size_t) i]) { pi = i; break; }
                if (pi < 0 || pos >= lengthInSamples)
                {
                    for (int c = 0; c < numDest; ++c)
                        if (dest[c] != nullptr)
                            juce::FloatVectorOperations::clear (
                                reinterpret_cast<float*> (dest[c] + destOffset + written), numSamples);
                    break;
                }
                const juce::int64 local = pos - partStart[(size_t) pi];
                const int chunk = (int) juce::jmin ((juce::int64) numSamples,
                                                    parts[(size_t) pi]->lengthInSamples - local);
                if (chunk <= 0) break;
                parts[(size_t) pi]->readSamples (dest, numDest, destOffset + written, local, chunk);
                written    += chunk;
                pos        += chunk;
                numSamples -= chunk;
            }
            return true;
        }

    private:
        std::vector<std::unique_ptr<juce::AudioFormatReader>> parts;
        std::vector<juce::int64>                              partStart;
    };
}
