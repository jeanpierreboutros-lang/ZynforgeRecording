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
        parts.push_back (mainFile);
        const auto dir  = mainFile.getParentDirectory();
        const auto stem = mainFile.getFileNameWithoutExtension();   // "Track_07"
        const auto ext  = mainFile.getFileExtension();
        for (int p = 2; ; ++p)
        {
            auto f = dir.getChildFile (stem + "_part"
                        + juce::String::formatted ("%02d", p) + ext);
            if (f.existsAsFile()) parts.push_back (f); else break;
        }
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
            for (auto& f : files)
                if (std::unique_ptr<juce::AudioFormatReader> r { fm.createReaderFor (f) })
                    rs.push_back (std::move (r));
            if (rs.empty())      return nullptr;
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
