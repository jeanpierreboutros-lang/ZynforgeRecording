#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace zynforge
{
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

        // `insert` may legitimately be absent / empty (e.g. a punch that
        // captured nothing): treat as a zero-length insert so the result is
        // just `base` unchanged -- still a safe, valid output.
        std::unique_ptr<juce::AudioFormatReader> insR (fm.createReaderFor (insert));

        const int        chans   = (int) baseR->numChannels;
        const juce::int64 baseLen = baseR->lengthInSamples;
        const juce::int64 insLen  = insR != nullptr ? insR->lengthInSamples : 0;

        if (insR != nullptr)
        {
            // Refuse to splice audio that doesn't line up -- a channel/SR
            // mismatch would silently corrupt the take.
            if ((int) insR->numChannels != chans)                 return false;
            if (std::abs (insR->sampleRate - baseR->sampleRate) > 1.0) return false;
        }

        const juce::int64 punchIn    = juce::jlimit ((juce::int64) 0, baseLen, punchInSample);
        const juce::int64 afterStart = juce::jmin (baseLen, punchIn + insLen);
        const juce::int64 afterLen   = baseLen - afterStart;

        // Writer matching base's container + bit depth.
        const auto ext = base.getFileExtension().toLowerCase();
        std::unique_ptr<juce::AudioFormat> fmt;
        int  bits    = (int) baseR->bitsPerSample;
        const bool isFloat = baseR->usesFloatingPointData;
        if (ext == ".flac")                         { fmt.reset (new juce::FlacAudioFormat());  bits = juce::jmin (bits, 24); }
        else if (ext == ".aif" || ext == ".aiff")     fmt.reset (new juce::AiffAudioFormat());
        else                                          fmt.reset (new juce::WavAudioFormat());

        outFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> os (outFile.createOutputStream());
        if (os == nullptr) return false;

        std::unique_ptr<juce::AudioFormatWriter> w (
            fmt->createWriterFor (os.get(), baseR->sampleRate, (unsigned int) chans,
                                  isFloat ? 32 : bits, {}, 0));
        if (w == nullptr) return false;
        os.release();   // the writer owns the stream now

        // writeFromAudioReader reads any bit depth / format and converts to the
        // writer's format internally, in chunks -- no manual buffering needed.
        bool ok = true;
        if (punchIn > 0)                      ok = w->writeFromAudioReader (*baseR, 0,          punchIn);
        if (ok && insR != nullptr && insLen)  ok = w->writeFromAudioReader (*insR,  0,          insLen);
        if (ok && afterLen > 0)               ok = w->writeFromAudioReader (*baseR, afterStart, afterLen);

        w.reset();   // flush + finalise the header before we hand the file back
        if (! ok) outFile.deleteFile();
        return ok;
    }
}
