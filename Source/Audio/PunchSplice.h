#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

#include "MultiPartReader.h"   // ConcatReader -- multi-part base for the splice

namespace zynforge
{
    // Splice core, working from an already-open base READER (single file or a
    // ConcatReader spanning a multi-part take). Writes a brand-new outFile in
    // `baseExt`'s container; never mutates inputs. See splicePunchFile for the
    // semantics. Returns false (deleting any partial outFile) on any failure.
    inline bool splicePunchFromReader (juce::AudioFormatManager& fm,
                                       juce::AudioFormatReader&   baseR,
                                       const juce::String&        baseExt,
                                       const juce::File&          insert,
                                       juce::int64                punchInSample,
                                       const juce::File&          outFile)
    {
        std::unique_ptr<juce::AudioFormatReader> insR (fm.createReaderFor (insert));

        const int         chans   = (int) baseR.numChannels;
        const juce::int64 baseLen = baseR.lengthInSamples;
        const juce::int64 insLen  = insR != nullptr ? insR->lengthInSamples : 0;

        if (insR != nullptr)
        {
            if ((int) insR->numChannels != chans)                      return false;
            if (std::abs (insR->sampleRate - baseR.sampleRate) > 1.0)  return false;
        }

        const juce::int64 punchIn    = juce::jlimit ((juce::int64) 0, baseLen, punchInSample);
        const juce::int64 afterStart = juce::jmin (baseLen, punchIn + insLen);
        const juce::int64 afterLen   = baseLen - afterStart;

        const auto ext = baseExt.toLowerCase();
        std::unique_ptr<juce::AudioFormat> fmt;
        int  bits    = (int) baseR.bitsPerSample;
        const bool isFloat = baseR.usesFloatingPointData;
        if (ext == ".flac")                       { fmt.reset (new juce::FlacAudioFormat());  bits = juce::jmin (bits, 24); }
        else if (ext == ".aif" || ext == ".aiff")   fmt.reset (new juce::AiffAudioFormat());
        else                                        fmt.reset (new juce::WavAudioFormat());

        outFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (outFile.createOutputStream());
        if (os == nullptr) return false;

        std::unique_ptr<juce::AudioFormatWriter> w (
            fmt->createWriterFor (os.get(), baseR.sampleRate, (unsigned int) chans,
                                  isFloat ? 32 : bits, {}, 0));
        if (w == nullptr) return false;
        os.release();

        bool ok = true;
        if (punchIn > 0)                      ok = w->writeFromAudioReader (baseR, 0,          punchIn);
        if (ok && insR != nullptr && insLen)  ok = w->writeFromAudioReader (*insR, 0,          insLen);
        if (ok && afterLen > 0)               ok = w->writeFromAudioReader (baseR, afterStart, afterLen);

        w.reset();
        if (! ok) outFile.deleteFile();
        return ok;
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
    // the rest of the take.
    //
    // Output format (sample rate / channels / bit depth / container) matches
    // `base`. Returns false (deleting any partial outFile) if either reader
    // fails to open or the two files disagree on channel count / sample rate
    // -- splicing mismatched audio would corrupt the take, so we refuse.
    //
    // punchInSample is clamped to [0, baseLen]; punching at/after the end simply
    // appends `insert` (the "extend the take" case falls out for free).
    inline bool splicePunchFile (juce::AudioFormatManager& fm,
                                 const juce::File& base,
                                 const juce::File& insert,
                                 juce::int64        punchInSample,
                                 const juce::File&  outFile)
    {
        std::unique_ptr<juce::AudioFormatReader> baseR (fm.createReaderFor (base));
        if (baseR == nullptr) return false;
        return splicePunchFromReader (fm, *baseR, base.getFileExtension(),
                                      insert, punchInSample, outFile);
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
        if (baseParts.empty()) return false;
        if (baseParts.size() == 1)
            return splicePunchFile (fm, baseParts.front(), insert, punchInSample, outFile);

        auto baseR = ConcatReader::create (fm, baseParts);
        if (baseR == nullptr) return false;
        return splicePunchFromReader (fm, *baseR, baseParts.front().getFileExtension(),
                                      insert, punchInSample, outFile);
    }
}
