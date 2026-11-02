#include "TrackExporter.h"
#include "CheckedFileStream.h"
#include "FloatAiffWriter.h"
#include "CheckedReaderSource.h"
#include "MultiPartReader.h"
#include "SincResampler.h"

#include <cmath>

namespace zynforge
{
    namespace
    {
        juce::File temporarySibling (const juce::File& finalFile,
                                     const juce::String& extension)
        {
            return finalFile.getSiblingFile ("." + finalFile.getFileNameWithoutExtension()
                                             + "-" + juce::Uuid().toString()
                                             + ".partial" + extension);
        }

        bool installCompletedExport (const juce::File& temporary,
                                     const juce::File& destination,
                                     juce::String& error)
        {
            if (temporary.replaceFileIn (destination)) return true;
            temporary.deleteFile();
            error = "Could not install completed export; previous file was preserved";
            return false;
        }

        constexpr const char* kFinalWriteFailed = "Final write failed (destination full?)";
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

    juce::String TrackExporter::sizeRefusal (const ExportOptions& opts, int numChannels,
                                             juce::int64 frames)
    {
        if (opts.format != ExportFormat::Aiff24) return {};
        const bool floating = bitsForFormat (opts.format, opts.bitsPerSample) == 32;
        const auto limit = floating ? FloatAiffWriter::kMaxDataBytes : (juce::uint64) 0xfff00000u;
        const auto bytes = (juce::uint64) juce::jmax<juce::int64> (0, frames) * (juce::uint64) numChannels
                         * (juce::uint64) (bitsForFormat (opts.format, opts.bitsPerSample) / 8);
        if (bytes <= limit) return {};
        return "An AIFF file of this format stops at " + juce::String ((double) limit / 1.0e9, 1)
             + " GB and this one needs " + juce::String ((double) bytes / 1.0e9, 1)
             + " GB. Export WAV or FLAC instead.";
    }

    TrackExporter::TrackExporter()
    {
        // JUCE's basic set already includes FLAC when JUCE_USE_FLAC is
        // enabled (as it is for this target). Registering it again trips a
        // Debug assertion and leaves a duplicate handler in Release builds.
        formatManager.registerBasicFormats();
    }

    juce::String TrackExporter::extensionFor (ExportFormat f)
    {
        switch (f)
        {
            case ExportFormat::Wav24:  return ".wav";
            case ExportFormat::Aiff24: return ".aif";
            case ExportFormat::Flac24: return ".flac";
        }
        return ".wav";
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
            {
                juce::WavAudioFormat f;
                return std::unique_ptr<juce::AudioFormatWriter> (
                    f.createWriterFor (out, sampleRate, numChannels, bitsPerSample, {}, 0));
            }
            case ExportFormat::Aiff24:
            {
                if (bitsPerSample == 32)
                    return std::unique_ptr<juce::AudioFormatWriter> (
                        FloatAiffWriter::create (out, sampleRate, numChannels));
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
        // drop everything after the first stop.
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
        if (reader == nullptr) { outError = "Cannot read source"; return false; }

        const auto channels  = (int) reader->numChannels;
        const auto srcSR     = reader->sampleRate;
        const auto destSR    = opts.sampleRate;
        const auto srcLen    = reader->lengthInSamples;
        const auto destLen   = (juce::int64) ((double) srcLen * destSR / srcSR);
        outError = sizeRefusal (opts, channels, destLen);
        if (outError.isNotEmpty()) return false;

        const auto finalPcmFile = destWithoutExt.withFileExtension (extensionFor (opts.format));
        const auto destPcmFile = temporarySibling (finalPcmFile, ".wav");
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto outStream = CheckedFileStream::open (destPcmFile, closeFailed);
        if (outStream == nullptr) { outError = "Cannot write to destination"; return false; }

        auto writer = makePcmWriter (opts.format, outStream.get(), destSR, (unsigned int) channels,
                                     bitsForFormat (opts.format, opts.bitsPerSample));
        if (writer == nullptr)
        {
            outStream.reset();
            destPcmFile.deleteFile();
            outError = "Cannot create writer";
            return false;
        }
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
            if (! flushFloatAiffBeforeClose (writer.get()))
            {
                outError = "Final audio header flush failed";
                writer = nullptr; destPcmFile.deleteFile();
                return false;
            }
            writer = nullptr;   // flush + close
            if (closeFailed->load())
            {
                outError = kFinalWriteFailed;
                destPcmFile.deleteFile();
                return false;
            }
            return installCompletedExport (destPcmFile, finalPcmFile, outError);
        }

        // Keep the reader alive and propagate decoder/I/O failures through resampling.
        CheckedReaderSource readerSrc (*reader);
        SincResampler resampler (&readerSrc, channels);
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
            if (readerSrc.hasFailed())
            {
                outError = "Read failed";
                writer = nullptr;
                resampler.releaseResources();
                destPcmFile.deleteFile();
                return false;
            }

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
        if (! flushFloatAiffBeforeClose (writer.get()))
        {
            outError = "Final audio header flush failed";
            writer = nullptr; resampler.releaseResources(); destPcmFile.deleteFile();
            return false;
        }
        writer = nullptr;     // flush + close
        resampler.releaseResources();
        if (closeFailed->load())
        {
            outError = kFinalWriteFailed;
            destPcmFile.deleteFile();
            return false;
        }

        return installCompletedExport (destPcmFile, finalPcmFile, outError);
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
        if (readerL->sampleRate != readerR->sampleRate)
        {
            outError = "Stereo source sample rates must match";
            return false;
        }

        const auto srcSR   = readerL->sampleRate;
        const auto destSR  = opts.sampleRate;
        const auto srcLen  = juce::jmax (readerL->lengthInSamples, readerR->lengthInSamples);
        const auto destLen = (juce::int64) ((double) srcLen * destSR / srcSR);
        outError = sizeRefusal (opts, 2, destLen);
        if (outError.isNotEmpty()) return false;

        const auto finalPcmFile = destWithoutExt.withFileExtension (extensionFor (opts.format));
        const auto destPcmFile = temporarySibling (finalPcmFile, ".wav");
        const auto closeFailed = CheckedFileStream::makeFailedFlag();
        auto outStream = CheckedFileStream::open (destPcmFile, closeFailed);
        if (outStream == nullptr) { outError = "Cannot write to destination"; return false; }

        auto writer = makePcmWriter (opts.format, outStream.get(), destSR, 2,   // 2 channels
                                     bitsForFormat (opts.format, opts.bitsPerSample));
        if (writer == nullptr)
        {
            outStream.reset();
            destPcmFile.deleteFile();
            outError = "Cannot create writer";
            return false;
        }
        outStream.release();

        // Each mono source resamples independently; we interleave the two
        // resampled mono blocks into one stereo block per write.
        CheckedReaderSource readerSrcL (*readerL);
        CheckedReaderSource readerSrcR (*readerR);
        SincResampler resL (&readerSrcL, 1);
        SincResampler resR (&readerSrcR, 1);
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
            if (readerSrcL.hasFailed() || readerSrcR.hasFailed())
            {
                outError = "Read failed";
                writer = nullptr;
                resL.releaseResources();
                resR.releaseResources();
                destPcmFile.deleteFile();
                return false;
            }
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
        const bool finalized = flushFloatAiffBeforeClose (writer.get());
        writer = nullptr;
        resL.releaseResources();
        resR.releaseResources();

        if (! finalized || closeFailed->load())
        {
            outError = finalized ? kFinalWriteFailed : "Final audio header flush failed";
            destPcmFile.deleteFile();
            return false;
        }

        return installCompletedExport (destPcmFile, finalPcmFile, outError);
    }

    ExportFileStream::ExportFileStream (TrackExporter& e, const juce::File& dest,
                                        const ExportOptions& opts, double renderRate, int numChannels)
        : exporter (e), destWithoutExt (dest), options (opts)
    {
        convertOnCommit = ! juce::approximatelyEqual (renderRate, opts.sampleRate);
        const auto finalFile = dest.withFileExtension (TrackExporter::extensionFor (opts.format));
        temporary = temporarySibling (finalFile, ".wav");

        closeFailed = CheckedFileStream::makeFailedFlag();
        auto stream = CheckedFileStream::open (temporary, closeFailed);
        if (stream == nullptr) return;
        writer = convertOnCommit
            ? makePcmWriter (ExportFormat::Wav24, stream.get(), renderRate, (unsigned int) numChannels, 32)
            : makePcmWriter (opts.format, stream.get(), renderRate, (unsigned int) numChannels,
                             bitsForFormat (opts.format, opts.bitsPerSample));
        if (writer != nullptr) stream.release();
    }

    ExportFileStream::~ExportFileStream()
    {
        writer = nullptr;
        temporary.deleteFile();
    }

    bool ExportFileStream::write (const juce::AudioBuffer<float>& block, int numSamples)
    {
        return writer != nullptr && writer->writeFromAudioSampleBuffer (block, 0, numSamples);
    }

    bool ExportFileStream::commit (juce::String& error)
    {
        if (writer == nullptr) { error = "Cannot write to destination"; return false; }
        const bool finalized = flushFloatAiffBeforeClose (writer.get());
        writer = nullptr;
        if (! finalized) { error = "Final audio header flush failed"; return false; }
        if (closeFailed->load()) { error = kFinalWriteFailed; return false; }

        if (convertOnCommit)
            return exporter.exportTrack (temporary, destWithoutExt, options, error);
        const auto finalFile = destWithoutExt.withFileExtension (TrackExporter::extensionFor (options.format));
        return installCompletedExport (temporary, finalFile, error);
    }
}
