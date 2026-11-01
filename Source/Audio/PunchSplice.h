#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <limits>
#include <vector>

#include "MultiPartReader.h"   // ConcatReader -- multi-part base for the splice
#include "FloatAiffWriter.h"

namespace zynforge
{
    // The bext fields of a WAV take. A spliced file begins with the base's
    // first sample, so it keeps the base's stamp and text.
    inline juce::StringPairArray bextFieldsOf (const juce::AudioFormatReader& reader)
    {
        juce::StringPairArray kept;
        for (auto* key : { juce::WavAudioFormat::bwavDescription, juce::WavAudioFormat::bwavOriginator,
                           juce::WavAudioFormat::bwavOriginatorRef, juce::WavAudioFormat::bwavOriginationDate,
                           juce::WavAudioFormat::bwavOriginationTime, juce::WavAudioFormat::bwavTimeReference,
                           juce::WavAudioFormat::bwavCodingHistory })
            if (reader.metadataValues.containsKey (key))
                kept.set (key, reader.metadataValues[key]);
        return kept;
    }

    inline bool writeSilentFrames (juce::AudioFormatWriter& writer, int channels, juce::int64 frames)
    {
        constexpr int kBlock = 16384;
        const std::vector<float> zeros ((std::size_t) kBlock, 0.0f);
        const std::vector<const float*> planes ((std::size_t) channels, zeros.data());
        for (auto remaining = frames; remaining > 0;)
        {
            const int count = (int) juce::jmin ((juce::int64) kBlock, remaining);
            if (! writer.writeFromFloatArrays (planes.data(), channels, count)) return false;
            remaining -= count;
        }
        return true;
    }

    // Splice core, working from an already-open base READER (single file or a
    // ConcatReader spanning a multi-part take). Writes a brand-new outFile in
    // `baseExt`'s container; never mutates inputs. See splicePunchFile for the
    // semantics. Returns false (deleting any partial outFile) on any failure.
    //
    // A result too big for one file in its container (AIFF, FLAC) continues in
    // numbered siblings, the same way a long take rolls. `written` then lists
    // every file in order; a failure deletes them all.
    inline bool splicePunchFromReader (juce::AudioFormatManager&, juce::AudioFormatReader& baseR,
                                       juce::AudioFormatReader& insR, const juce::String& baseExt,
                                       juce::int64 punchInSample, const juce::File& outFile,
                                       juce::int64 maxBytesPerFile, std::vector<juce::File>& written,
                                       const std::function<void()>& onProgress = {})
    {

        const int         chans   = (int) baseR.numChannels;
        const juce::int64 baseLen = baseR.lengthInSamples;
        const juce::int64 insLen  = insR.lengthInSamples;
        if (insLen <= 0) return false;

        if ((int) insR.numChannels != chans)                      return false;
        if (std::abs (insR.sampleRate - baseR.sampleRate) > 1.0)  return false;

        // The punch stays where it was played. A track that ended before it
        // gets silence up to that point, never an early landing.
        const juce::int64 punchIn    = juce::jmax ((juce::int64) 0, punchInSample);
        const juce::int64 baseHead   = juce::jmin (punchIn, baseLen);
        const juce::int64 gap        = punchIn - baseHead;
        const juce::int64 afterStart = juce::jmin (baseLen, punchIn + insLen);
        const juce::int64 afterLen   = baseLen - afterStart;

        const auto ext = baseExt.toLowerCase();
        std::unique_ptr<juce::AudioFormat> fmt;
        int  bits    = (int) baseR.bitsPerSample;
        const bool isFloat = baseR.usesFloatingPointData;
        juce::StringPairArray metadata;
        if (ext == ".flac")                       { fmt.reset (new juce::FlacAudioFormat());  bits = juce::jmin (bits, 24); }
        else if (ext == ".aif" || ext == ".aiff")   fmt.reset (new juce::AiffAudioFormat());
        else                                      { fmt.reset (new juce::WavAudioFormat());   metadata = bextFieldsOf (baseR); }

        const juce::int64 frameBytes = (juce::int64) chans * ((isFloat ? 32 : bits) / 8);
        const juce::int64 framesPerFile = juce::jmax ((juce::int64) 1, (maxBytesPerFile - 1) / frameBytes);

        std::unique_ptr<juce::AudioFormatWriter> w;
        juce::int64 room = 0;
        const auto closeFile = [&w]
        {
            const bool closed = w == nullptr || flushFloatAiffBeforeClose (w.get());
            w.reset();
            return closed;
        };
        const auto nextFile = [&]
        {
            if (! closeFile()) return false;
            const auto file = written.empty() ? outFile
                : outFile.getSiblingFile (outFile.getFileNameWithoutExtension()
                                          + juce::String::formatted ("%02d", (int) written.size() + 1)
                                          + outFile.getFileExtension());
            file.deleteFile();
            std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
            if (os == nullptr) return false;
            w.reset (isFloat && (ext == ".aif" || ext == ".aiff")
                         ? FloatAiffWriter::create (os.get(), baseR.sampleRate, (unsigned int) chans)
                         : fmt->createWriterFor (os.get(), baseR.sampleRate, (unsigned int) chans,
                                                 isFloat ? 32 : bits, metadata, 0));
            if (w == nullptr) return false;
            os.release();
            written.push_back (file);
            room = framesPerFile;
            return true;
        };
        // Runs `write` over `frames`, in as many goes as the file ceiling needs.
        const auto span = [&] (juce::int64 frames, const std::function<bool (juce::int64, int)>& write)
        {
            for (juce::int64 done = 0; done < frames;)
            {
                if (room == 0 && ! nextFile()) return false;
                const int count = (int) juce::jmin (frames - done, room, (juce::int64) (1 << 20));
                if (! write (done, count)) return false;
                done += count;
                room -= count;
                if (onProgress) onProgress();
            }
            return true;
        };

        const bool ok = (room > 0 || nextFile())
            && span (baseHead, [&] (juce::int64 at, int count) { return w->writeFromAudioReader (baseR, at, count); })
            && span (gap,      [&] (juce::int64, int count)    { return writeSilentFrames (*w, chans, count); })
            && span (insLen,   [&] (juce::int64 at, int count) { return w->writeFromAudioReader (insR, at, count); })
            && span (afterLen, [&] (juce::int64 at, int count) { return w->writeFromAudioReader (baseR, afterStart + at, count); })
            && closeFile();
        w.reset();
        if (! ok)
            for (const auto& file : written) file.deleteFile();
        return ok;
    }

    // The whole punch: base parts plus every file the punch wrote, read as
    // seamless takes. Never mutates its inputs.
    inline bool splicePunchTake (juce::AudioFormatManager& fm,
                                 const std::vector<juce::File>& baseParts,
                                 const std::vector<juce::File>& insertParts,
                                 juce::int64                    punchInSample,
                                 const juce::File&              outFile,
                                 juce::int64                    maxBytesPerFile,
                                 std::vector<juce::File>&       written,
                                 const std::function<void()>&   onProgress = {})
    {
        if (baseParts.empty() || insertParts.empty()) return false;
        auto baseR = ConcatReader::create (fm, baseParts);
        auto insR  = ConcatReader::create (fm, insertParts);
        // Never report an unchanged base as a successful punch.
        if (baseR == nullptr || insR == nullptr) return false;
        return splicePunchFromReader (fm, *baseR, *insR, baseParts.front().getFileExtension(),
                                      punchInSample, outFile, maxBytesPerFile, written, onProgress);
    }

    // ── Offline punch-in splice ────────────────────────────────────────────
    // Capture-safe by construction: reads `base` + `insert`, writes a brand-new
    // `outFile`, and NEVER mutates either input. The caller atomically swaps
    // outFile over the original only after this returns true, so a failure
    // anywhere leaves the original take untouched.
    //
    // The result is the sample-accurate concatenation:
    //
    //     base[0, punchIn)  ++  insert[0, insertLen)  ++  base[punchIn+insertLen, baseLen)
    //
    // i.e. `insert` REPLACES base's region [punchIn, punchIn+insertLen); base's
    // audio BEFORE the punch-in and AFTER the punch-out is preserved. This is
    // the destructive punch-in an engineer expects: drop in on a section, keep
    // the rest of the take. A punch past base's end extends it: silence fills
    // [baseLen, punchIn), so `insert` still starts at punchIn.
    //
    // Output format (sample rate / channels / bit depth / container) matches
    // `base`. Returns false (deleting any partial outFile) if either reader
    // fails to open or the two files disagree on channel count / sample rate
    // -- splicing mismatched audio would corrupt the take, so we refuse.
    inline bool splicePunchFile (juce::AudioFormatManager& fm,
                                 const juce::File& base,
                                 const juce::File& insert,
                                 juce::int64        punchInSample,
                                 const juce::File&  outFile)
    {
        std::vector<juce::File> written;
        return splicePunchTake (fm, { base }, { insert }, punchInSample, outFile,
                                std::numeric_limits<juce::int64>::max(), written);
    }

    // ── Multi-part punch splice (flatten) ──────────────────────────────────
    // Same as splicePunchFile, but the base take is given as its ordered part
    // files (Track_NN + its Track_NN_partXX continuations). They're read as ONE
    // seamless base via ConcatReader, so a punch into a SPLIT take is spliced
    // correctly and FLATTENED into a single outFile (the caller then deletes the
    // now-redundant parts). One part -> the simple single-file path; empty list
    // -> false. This is what lets you punch in anywhere on a take that was built
    // up by continue-recording (which appends parts).
    inline bool splicePunchParts (juce::AudioFormatManager& fm,
                                  const std::vector<juce::File>& baseParts,
                                  const juce::File&              insert,
                                  juce::int64                    punchInSample,
                                  const juce::File&              outFile)
    {
        std::vector<juce::File> written;
        return splicePunchTake (fm, baseParts, { insert }, punchInSample, outFile,
                                std::numeric_limits<juce::int64>::max(), written);
    }
}
