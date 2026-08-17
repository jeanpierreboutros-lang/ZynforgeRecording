#include "TrackExporter.h"
#include "MultiPartReader.h"

namespace zynforge
{
    TrackExporter::TrackExporter()
    {
        formatManager.registerBasicFormats();   // WAV + AIFF
        formatManager.registerFormat (new juce::FlacAudioFormat(), false);
    }

    juce::String TrackExporter::extensionFor (ExportFormat f)
    {
        switch (f)
        {
            case ExportFormat::Wav24:  return ".wav";
            case ExportFormat::Aiff24: return ".aif";
            case ExportFormat::Flac24: return ".flac";
            case ExportFormat::Mp3:    return ".mp3";
        }
        return ".wav";
    }

    juce::File TrackExporter::findLameBinary()
    {
        const juce::StringArray candidates {
            "/opt/homebrew/bin/lame",
            "/usr/local/bin/lame",
            "/usr/bin/lame"
        };
        for (auto& path : candidates)
        {
            juce::File f (path);
            if (f.existsAsFile()) return f;
        }

        juce::ChildProcess which;
        if (which.start (juce::StringArray ({ "/usr/bin/which", "lame" })))
        {
            which.waitForProcessToFinish (2000);
            const auto out = which.readAllProcessOutput().trim();
            if (out.isNotEmpty())
            {
                juce::File f (out);
                if (f.existsAsFile()) return f;
            }
        }
        return {};
    }

    // Bit depth the chosen container can actually write. FLAC tops out at
    // 24-bit, so a blanket jlimit(16, 32) handed it 32 and createWriterFor
    // returned nullptr -- the export died with an opaque "Cannot create
    // writer" instead of just using the best depth FLAC supports.
    static int bitsForFormat (ExportFormat fmt, int requested)
    {
        if (fmt == ExportFormat::Flac24) return juce::jlimit (16, 24, requested);
        return juce::jlimit (16, 32, requested);
    }

    static std::unique_ptr<juce::AudioFormatWriter> makePcmWriter (
        ExportFormat fmt,
        juce::OutputStream* out,
        double sampleRate,
        unsigned int numChannels,
        int bitsPerSample)
    {
        switch (fmt)
        {
            case ExportFormat::Wav24:
            case ExportFormat::Mp3:   // temp WAV in MP3 path
            {
                juce::WavAudioFormat f;
                return std::unique_ptr<juce::AudioFormatWriter> (
                    f.createWriterFor (out, sampleRate, numChannels, bitsPerSample, {}, 0));
            }
            case ExportFormat::Aiff24:
            {
                juce::AiffAudioFormat f;
                return std::unique_ptr<juce::AudioFormatWriter> (
                    f.createWriterFor (out, sampleRate, numChannels, bitsPerSample, {}, 0));
            }
            case ExportFormat::Flac24:
            {
                juce::FlacAudioFormat f;
                return std::unique_ptr<juce::AudioFormatWriter> (
                    f.createWriterFor (out, sampleRate, numChannels, bitsPerSample, {}, 5));
            }
        }
        return {};
    }

    bool TrackExporter::exportTrack (const juce::File& source,
                                     const juce::File& destWithoutExt,
                                     const ExportOptions& opts,
                                     juce::String& outError)
    {
        outError.clear();

        if (! source.existsAsFile()) { outError = "Source not found"; return false; }

        // A take can be split across continuation parts (Track_NN_partXX from
        // continue-recording or auto-split). Stitch them so the export contains
        // the WHOLE take, not just part 1 -- the raw export used to silently
        // drop everything after the first stop. Falls back to the single file
        // if the parts have a hole (create refuses) so we still export part 1.
        const auto parts = findTakeParts (source);
        std::unique_ptr<juce::AudioFormatReader> reader (
            ConcatReader::create (formatManager, parts));
        if (reader == nullptr && parts.size() > 1)
        {
            // create() refused a multi-part take because a MIDDLE part is
            // missing/unreadable -- do NOT silently fall back to exporting part
            // 1 (which drops the later parts and reports success on a "never
            // lose audio" tool). Fail loudly so the engineer knows.
            outError = "Take has a missing/unreadable part -- export would be truncated";
            return false;
        }
        if (reader == nullptr) reader.reset (formatManager.createReaderFor (source));
        if (reader == nullptr) { outError = "Cannot read source"; return false; }

        const auto channels  = (int) reader->numChannels;
        const auto srcSR     = reader->sampleRate;
        const auto destSR    = opts.sampleRate;
        const auto srcLen    = reader->lengthInSamples;
        const auto destLen   = (juce::int64) ((double) srcLen * destSR / srcSR);

        // For MP3 we first render to a temp WAV at the chosen sample rate,
        // then call out to lame.
        const bool isMp3   = (opts.format == ExportFormat::Mp3);
        auto destPcmFile   = isMp3
                              ? destWithoutExt.withFileExtension (".tmp.wav")
                              : destWithoutExt.withFileExtension (extensionFor (opts.format));

        destPcmFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> outStream (destPcmFile.createOutputStream());
        if (outStream == nullptr) { outError = "Cannot write to destination"; return false; }

        // For MP3 we always render the intermediate WAV at 24-bit.
        const int bits = isMp3 ? 24 : bitsForFormat (opts.format, opts.bitsPerSample);
        auto writer = makePcmWriter (isMp3 ? ExportFormat::Wav24 : opts.format,
                                     outStream.get(), destSR, (unsigned int) channels,
                                     bits);
        if (writer == nullptr) { outError = "Cannot create writer"; return false; }
        outStream.release(); // writer owns the stream now

        // Same-rate export: copy straight through. Running a rate-1.0 export
        // through ResamplingAudioSource put a Lagrange interpolator in the path
        // for no reason -- the output was neither bit-exact nor sample-aligned
        // with the source, which matters when the stems are going back into
        // another DAW alongside the originals.
        if (juce::approximatelyEqual (srcSR, destSR))
        {
            juce::AudioBuffer<float> copyBuf (channels, 4096);
            juce::int64 pos = 0;
            while (pos < srcLen)
            {
                const int n = (int) juce::jmin ((juce::int64) 4096, srcLen - pos);
                copyBuf.clear();
                if (! reader->read (&copyBuf, 0, n, pos, true, true))
                {
                    outError = "Read failed";
                    writer = nullptr; destPcmFile.deleteFile();
                    return false;
                }
                const auto** arr = (const float**) copyBuf.getArrayOfReadPointers();
                if (! writer->writeFromFloatArrays (arr, channels, n))
                {
                    outError = "Write failed (destination full?)";
                    writer = nullptr; destPcmFile.deleteFile();
                    return false;
                }
                pos += n;
            }
            writer = nullptr;   // flush + close
            if (! isMp3) return true;
            return encodeMp3 (destPcmFile, destWithoutExt, opts, outError);
        }

        // Resampling pipeline: keep `reader` alive -- pass `deleteWhenRemoved=false`.
        juce::AudioFormatReaderSource readerSrc (reader.get(), false);
        juce::ResamplingAudioSource    resampler (&readerSrc, false, channels);
        resampler.setResamplingRatio (srcSR / destSR);
        const int block = 4096;
        resampler.prepareToPlay (block, destSR);

        juce::AudioBuffer<float> buf (channels, block);
        juce::int64 written = 0;
        while (written < destLen)
        {
            const int thisBlock = (int) juce::jmin ((juce::int64) block, destLen - written);
            buf.clear();
            juce::AudioSourceChannelInfo info (&buf, 0, thisBlock);
            resampler.getNextAudioBlock (info);

            const auto** arr = (const float**) buf.getArrayOfReadPointers();
            if (! writer->writeFromFloatArrays (arr, channels, thisBlock))
            {
                // Close + DELETE the partial file. Returning with it in place
                // left a truncated export on disk that looks like a finished
                // one -- the worst outcome for a "never lose audio" tool.
                outError = "Write failed (destination full?)";
                writer = nullptr;
                resampler.releaseResources();
                destPcmFile.deleteFile();
                return false;
            }
            written += thisBlock;
        }
        writer = nullptr;     // flush + close
        resampler.releaseResources();

        if (! isMp3) return true;
        return encodeMp3 (destPcmFile, destWithoutExt, opts, outError);
    }

    bool TrackExporter::exportStereoPair (const juce::File& sourceL,
                                          const juce::File& sourceR,
                                          const juce::File& destWithoutExt,
                                          const ExportOptions& opts,
                                          juce::String& outError)
    {
        outError.clear();
        if (! sourceL.existsAsFile() || ! sourceR.existsAsFile())
        { outError = "Source not found"; return false; }

        // Stitch each side's continuation parts. A missing/corrupt middle part
        // is a hard error: falling back to part 1 silently truncated exports.
        std::unique_ptr<juce::AudioFormatReader> readerL (
            ConcatReader::create (formatManager, findTakeParts (sourceL)));
        std::unique_ptr<juce::AudioFormatReader> readerR (
            ConcatReader::create (formatManager, findTakeParts (sourceR)));
        if (readerL == nullptr || readerR == nullptr) { outError = "Cannot read source"; return false; }

        const auto srcSR   = readerL->sampleRate;
        const auto destSR  = opts.sampleRate;
        const auto srcLen  = juce::jmax (readerL->lengthInSamples, readerR->lengthInSamples);
        const auto destLen = (juce::int64) ((double) srcLen * destSR / srcSR);

        const bool isMp3 = (opts.format == ExportFormat::Mp3);
        auto destPcmFile = isMp3
                            ? destWithoutExt.withFileExtension (".tmp.wav")
                            : destWithoutExt.withFileExtension (extensionFor (opts.format));

        destPcmFile.deleteFile();
        std::unique_ptr<juce::FileOutputStream> outStream (destPcmFile.createOutputStream());
        if (outStream == nullptr) { outError = "Cannot write to destination"; return false; }

        const int bits = isMp3 ? 24 : bitsForFormat (opts.format, opts.bitsPerSample);
        auto writer = makePcmWriter (isMp3 ? ExportFormat::Wav24 : opts.format,
                                     outStream.get(), destSR, 2, bits);   // 2 channels
        if (writer == nullptr) { outError = "Cannot create writer"; return false; }
        outStream.release();

        // Each mono source resamples independently; we interleave the two
        // resampled mono blocks into one stereo block per write.
        juce::AudioFormatReaderSource readerSrcL (readerL.get(), false);
        juce::AudioFormatReaderSource readerSrcR (readerR.get(), false);
        juce::ResamplingAudioSource   resL (&readerSrcL, false, 1);
        juce::ResamplingAudioSource   resR (&readerSrcR, false, 1);
        resL.setResamplingRatio (srcSR / destSR);
        resR.setResamplingRatio (srcSR / destSR);
        const int block = 4096;
        resL.prepareToPlay (block, destSR);
        resR.prepareToPlay (block, destSR);

        juce::AudioBuffer<float> stereo (2, block), monoL (1, block), monoR (1, block);
        juce::int64 written = 0;
        while (written < destLen)
        {
            const int thisBlock = (int) juce::jmin ((juce::int64) block, destLen - written);
            monoL.clear(); monoR.clear();
            { juce::AudioSourceChannelInfo i (&monoL, 0, thisBlock); resL.getNextAudioBlock (i); }
            { juce::AudioSourceChannelInfo i (&monoR, 0, thisBlock); resR.getNextAudioBlock (i); }
            stereo.copyFrom (0, 0, monoL, 0, 0, thisBlock);
            stereo.copyFrom (1, 0, monoR, 0, 0, thisBlock);
            const auto** arr = (const float**) stereo.getArrayOfReadPointers();
            if (! writer->writeFromFloatArrays (arr, 2, thisBlock))
            {
                outError = "Write failed (destination full?)";
                writer = nullptr;
                resL.releaseResources();
                resR.releaseResources();
                destPcmFile.deleteFile();   // don't leave a truncated export
                return false;
            }
            written += thisBlock;
        }
        writer = nullptr;
        resL.releaseResources();
        resR.releaseResources();

        if (! isMp3) return true;
        return encodeMp3 (destPcmFile, destWithoutExt, opts, outError);
    }

    bool TrackExporter::encodeMp3 (const juce::File& tempWav,
                                   const juce::File& destWithoutExt,
                                   const ExportOptions& opts,
                                   juce::String& outError)
    {
        const auto lame = findLameBinary();
        if (lame == juce::File())
        {
            outError = "lame not found (install via `brew install lame`)";
            tempWav.deleteFile();
            return false;
        }

        const auto destMp3 = destWithoutExt.withFileExtension (".mp3");
        destMp3.deleteFile();

        juce::ChildProcess proc;
        const juce::StringArray cmd {
            lame.getFullPathName(),
            "-b", juce::String (opts.mp3Bitrate),
            "--quiet",
            tempWav.getFullPathName(),
            destMp3.getFullPathName()
        };

        const bool started = proc.start (cmd);
        if (! started) { outError = "Failed to launch lame"; tempWav.deleteFile(); return false; }

        const bool finished = proc.waitForProcessToFinish (120000);
        if (! finished)
        {
            proc.kill();
            proc.waitForProcessToFinish (5000);
            tempWav.deleteFile();
            destMp3.deleteFile();
            outError = "lame timed out";
            return false;
        }
        const auto code = proc.getExitCode();
        tempWav.deleteFile();

        if (code != 0)
        {
            destMp3.deleteFile();
            outError = "lame exited with code " + juce::String (code);
            return false;
        }
        return true;
    }
}
