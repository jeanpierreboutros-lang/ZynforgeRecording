#include "TrackExporter.h"

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

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (source));
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
        const int bits = isMp3 ? 24 : juce::jlimit (16, 32, opts.bitsPerSample);
        auto writer = makePcmWriter (isMp3 ? ExportFormat::Wav24 : opts.format,
                                     outStream.get(), destSR, (unsigned int) channels,
                                     bits);
        if (writer == nullptr) { outError = "Cannot create writer"; return false; }
        outStream.release(); // writer owns the stream now

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
                outError = "Write failed";
                return false;
            }
            written += thisBlock;
        }
        writer = nullptr;     // flush + close
        resampler.releaseResources();

        if (! isMp3) return true;

        // MP3 stage: invoke lame on the temp WAV.
        const auto lame = findLameBinary();
        if (lame == juce::File())
        {
            outError = "lame not found (install via `brew install lame`)";
            destPcmFile.deleteFile();
            return false;
        }

        const auto destMp3 = destWithoutExt.withFileExtension (".mp3");
        destMp3.deleteFile();

        juce::ChildProcess proc;
        const juce::StringArray cmd {
            lame.getFullPathName(),
            "-b", juce::String (opts.mp3Bitrate),
            "--quiet",
            destPcmFile.getFullPathName(),
            destMp3.getFullPathName()
        };

        const bool started = proc.start (cmd);
        if (! started) { outError = "Failed to launch lame"; destPcmFile.deleteFile(); return false; }

        proc.waitForProcessToFinish (120000);
        const auto code = proc.getExitCode();
        destPcmFile.deleteFile();

        if (code != 0)
        {
            outError = "lame exited with code " + juce::String (code);
            return false;
        }
        return true;
    }
}
