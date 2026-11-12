#include "AudioImport.h"
#include "CheckedAudioReader.h"
#include "CheckedFileStream.h"
#include "CheckedReaderSource.h"
#include "SincResampler.h"

namespace zynforge::audioimport
{
    namespace detail
    {
        bool cancelled (const std::atomic<bool>* flag) noexcept
        {
            return flag != nullptr && flag->load (std::memory_order_relaxed);
        }

        // One take file made from the source: one channel, or a stereo pair.
        struct Output
        {
            juce::File destination;
            int firstSourceChannel { 0 };
            int channels { 1 };
        };

        // Every output in ONE pass over the source: one read, one resampler.
        // A 64-channel show file used to be read and resampled 64 times, which
        // took hours and held the session the whole time. All outputs land, or
        // none: a failure or cancel removes every partial and installed file.
        bool writeConvertedOutputs (juce::AudioFormatReader& reader,
                                    const std::vector<Output>& outputs,
                                    double targetRate,
                                    const std::atomic<bool>* cancel,
                                    bool* clipped)
        {
            if (cancelled (cancel) || outputs.empty()) return false;
            // Import appends tracks. A stale/unexpected destination is user
            // media, not permission to overwrite it.
            for (const auto& output : outputs)
                if (output.destination.exists()) return false;

            struct Open
            {
                juce::File temporary;
                CheckedFileStream::Failed closeFailed;
                std::unique_ptr<juce::AudioFormatWriter> writer;
            };
            std::vector<Open> open (outputs.size());
            std::vector<juce::File> installed;
            bool complete = false;
            const juce::ScopeGuard removePartial { [&]
            {
                if (complete) return;
                for (auto& o : open) { o.writer.reset(); o.temporary.deleteFile(); }
                for (const auto& f : installed) f.deleteFile();
            } };

            const int sourceChannels = juce::jmax (1, (int) reader.numChannels);
            const double sourceRate = reader.sampleRate > 0.0 ? reader.sampleRate : targetRate;

            juce::WavAudioFormat wav;
            for (size_t i = 0; i < outputs.size(); ++i)
            {
                const auto& destination = outputs[i].destination;
                open[i].temporary = destination.getSiblingFile (
                    "." + destination.getFileNameWithoutExtension() + "-"
                    + juce::Uuid().toString() + ".partial.wav");
                open[i].closeFailed = CheckedFileStream::makeFailedFlag();
                auto stream = CheckedFileStream::open (open[i].temporary, open[i].closeFailed);
                if (stream == nullptr) return false;
                open[i].writer.reset (wav.createWriterFor (stream.get(), targetRate,
                                                           (unsigned int) outputs[i].channels, 24, {}, 0));
                if (open[i].writer == nullptr) return false;
                stream.release();
            }

            constexpr int chunk = 16384;
            juce::AudioBuffer<float> buffer (sourceChannels, chunk);
            auto writeBlock = [&] (int count)
            {
                if (clipped != nullptr && buffer.getMagnitude (0, count) > 1.0f) *clipped = true;
                for (size_t i = 0; i < outputs.size(); ++i)
                {
                    const int first = juce::jlimit (0, sourceChannels - 1, outputs[i].firstSourceChannel);
                    const float* channels[2] = { buffer.getReadPointer (first),
                                                 buffer.getReadPointer (juce::jmin (first + 1, sourceChannels - 1)) };
                    if (! open[i].writer->writeFromFloatArrays (channels, outputs[i].channels, count)) return false;
                }
                return true;
            };

            if (juce::approximatelyEqual (sourceRate, targetRate))
            {
                juce::int64 position = 0;
                while (position < reader.lengthInSamples)
                {
                    if (cancelled (cancel)) return false;
                    const int count = (int) juce::jmin ((juce::int64) chunk,
                                                        reader.lengthInSamples - position);
                    if (! reader.read (&buffer, 0, count, position, true, true)
                        || ! writeBlock (count))
                        return false;
                    sourceFramesReadForTests += count;
                    position += count;
                }
            }
            else
            {
                CheckedReaderSource source (reader);
                SincResampler resampler (&source, sourceChannels);
                resampler.setResamplingRatio (sourceRate / targetRate);
                resampler.prepareToPlay (chunk, targetRate);

                const auto destinationLength = (juce::int64)
                    ((double) reader.lengthInSamples * targetRate / sourceRate);
                juce::int64 written = 0;
                while (written < destinationLength)
                {
                    if (cancelled (cancel)) return false;
                    const int count = (int) juce::jmin ((juce::int64) chunk,
                                                        destinationLength - written);
                    buffer.clear();
                    juce::AudioSourceChannelInfo info (&buffer, 0, count);
                    resampler.getNextAudioBlock (info);
                    if (source.hasFailed() || ! writeBlock (count)) return false;
                    sourceFramesReadForTests += count;
                    written += count;
                }
                resampler.releaseResources();
            }

            for (auto& o : open)
            {
                o.writer.reset(); // close/flush before preserving the destination
                if (o.closeFailed->load()) return false;
            }
            for (size_t i = 0; i < outputs.size(); ++i)
            {
                if (outputs[i].destination.exists() || ! open[i].temporary.moveFileTo (outputs[i].destination))
                    return false;
                installed.push_back (outputs[i].destination);
            }
            complete = true;
            return true;
        }

        bool writeConverted (juce::AudioFormatReader& reader,
                             const juce::File& destination,
                             double targetRate,
                             int outputChannels,
                             int monoSourceChannel,
                             const std::atomic<bool>* cancel,
                             bool* clipped)
        {
            return writeConvertedOutputs (reader, { { destination, outputChannels == 2 ? 0 : monoSourceChannel,
                                                      outputChannels == 2 ? 2 : 1 } },
                                          targetRate, cancel, clipped);
        }
    }

    Result importFiles (const juce::Array<juce::File>& sources,
                        const juce::File& audioFilesDir,
                        int firstTrack,
                        double targetSampleRate,
                        const std::atomic<bool>* cancel)
    {
        using namespace detail;
        Result result;
        if (! audioFilesDir.isDirectory() || targetSampleRate <= 0.0)
        {
            result.failed = sources.size();
            return result;
        }

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        int nextTrack = juce::jmax (0, firstTrack);

        for (const auto& source : sources)
        {
            if (cancelled (cancel))
            {
                result.cancelled = true;
                break;
            }

            const auto reader = openCheckedReader (formats, source);
            if (reader == nullptr)
            {
                ++result.failed;
                continue;
            }

            const int sourceChannels = (int) reader->numChannels;
            if (sourceChannels < 1 || nextTrack + sourceChannels > 256)
            {
                ++result.failed;
                continue;
            }
            const bool stereo = sourceChannels == 2;
            const bool converted = reader->sampleRate > 0.0
                && ! juce::approximatelyEqual (reader->sampleRate, targetSampleRate);

            // Files with more than two channels are split into one mono take
            // per source channel. Importing only L/R silently discarded the
            // rest of a console multitrack. Each pass writes up to
            // kChannelsPerPass of them from one read of the source: open files
            // stay well inside the process limit, and a 64-channel show file is
            // read once. A failure removes every file made for this source.
            constexpr int kChannelsPerPass = 64;
            std::vector<juce::File> created;
            std::vector<ImportedTrack> staged;
            const int outputTracks = sourceChannels > 2 ? sourceChannels : 1;
            bool failedSource = false, clipped = false;
            for (int first = 0; first < outputTracks && ! failedSource; first += kChannelsPerPass)
            {
                std::vector<Output> pass;
                for (int channel = first; channel < juce::jmin (outputTracks, first + kChannelsPerPass); ++channel)
                    pass.push_back ({ audioFilesDir.getChildFile (
                                          "Track_" + juce::String (nextTrack + channel + 1).paddedLeft ('0', 2) + ".wav"),
                                      channel, stereo ? 2 : 1 });
                if (! writeConvertedOutputs (*reader, pass, targetSampleRate, cancel, &clipped))
                {
                    failedSource = true;
                    break;
                }
                for (size_t i = 0; i < pass.size(); ++i)
                {
                    const int channel = first + (int) i;
                    created.push_back (pass[i].destination);
                    staged.push_back ({ nextTrack + channel, stereo,
                        sourceChannels > 2
                            ? source.getFileNameWithoutExtension() + " Ch " + juce::String (channel + 1)
                            : source.getFileNameWithoutExtension() });
                }
            }
            if (failedSource)
            {
                for (const auto& f : created) f.deleteFile();
                if (cancelled (cancel))
                {
                    result.cancelled = true;
                    break;
                }
                ++result.failed;
                continue;
            }
            result.tracks.insert (result.tracks.end(), staged.begin(), staged.end());
            ++result.importedFiles;
            if (converted) ++result.converted;
            if (clipped) ++result.clipped;
            nextTrack += sourceChannels > 2 ? sourceChannels : (stereo ? 2 : 1);
        }
        return result;
    }
}
