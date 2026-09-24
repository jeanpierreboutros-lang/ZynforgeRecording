#include "AudioImport.h"

namespace zynforge::audioimport
{
    namespace
    {
        bool cancelled (const std::atomic<bool>* flag) noexcept
        {
            return flag != nullptr && flag->load (std::memory_order_relaxed);
        }

        bool writeConverted (juce::AudioFormatReader& reader,
                             const juce::File& destination,
                             double targetRate,
                             int outputChannels,
                             int monoSourceChannel,
                             const std::atomic<bool>* cancel)
        {
            if (cancelled (cancel)) return false;
            // Import appends tracks. A stale/unexpected destination is user
            // media, not permission to overwrite it.
            if (destination.exists()) return false;

            const auto temporary = destination.getSiblingFile (
                "." + destination.getFileNameWithoutExtension() + "-"
                + juce::Uuid().toString() + ".partial.wav");

            bool complete = false;
            const juce::ScopeGuard removePartial { [&]
            {
                if (! complete) temporary.deleteFile();
            } };

            const int sourceChannels = juce::jmax (1, (int) reader.numChannels);
            const double sourceRate = reader.sampleRate > 0.0 ? reader.sampleRate : targetRate;

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::FileOutputStream> output (temporary.createOutputStream());
            if (output == nullptr) return false;
            juce::StringPairArray metadata;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (output.get(), targetRate,
                                     (unsigned int) outputChannels, 24, metadata, 0));
            if (writer == nullptr) return false;
            output.release();

            constexpr int chunk = 16384;
            juce::AudioBuffer<float> buffer (sourceChannels, chunk);
            auto writeBlock = [&] (int count)
            {
                if (outputChannels == 2)
                {
                    const int sourceRight = juce::jmin (1, sourceChannels - 1);
                    const float* channels[2] = { buffer.getReadPointer (0),
                                                 buffer.getReadPointer (sourceRight) };
                    return writer->writeFromFloatArrays (channels, 2, count);
                }
                const float* mono[1] = {
                    buffer.getReadPointer (juce::jlimit (0, sourceChannels - 1,
                                                         monoSourceChannel)) };
                return writer->writeFromFloatArrays (mono, 1, count);
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
                    position += count;
                }
            }
            else
            {
                juce::AudioFormatReaderSource source (&reader, false);
                juce::ResamplingAudioSource resampler (&source, false, sourceChannels);
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
                    if (! writeBlock (count)) return false;
                    written += count;
                }
                resampler.releaseResources();
            }

            writer.reset(); // close/flush before preserving the destination
            if (destination.exists() || ! temporary.moveFileTo (destination)) return false;
            complete = true;
            return true;
        }
    }

    Result importFiles (const juce::Array<juce::File>& sources,
                        const juce::File& audioFilesDir,
                        int firstTrack,
                        double targetSampleRate,
                        const std::atomic<bool>* cancel)
    {
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

            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (source));
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
            // rest of a console multitrack. Stage the whole source as a unit;
            // on a failed channel, remove only the files created for this source.
            std::vector<juce::File> created;
            std::vector<ImportedTrack> staged;
            const int outputTracks = sourceChannels > 2 ? sourceChannels : 1;
            bool failedSource = false;
            for (int channel = 0; channel < outputTracks; ++channel)
            {
                const int index = nextTrack + channel;
                const auto destination = audioFilesDir.getChildFile (
                    "Track_" + juce::String (index + 1).paddedLeft ('0', 2) + ".wav");
                if (! writeConverted (*reader, destination, targetSampleRate,
                                      stereo ? 2 : 1, channel, cancel))
                {
                    failedSource = true;
                    break;
                }
                created.push_back (destination);
                staged.push_back ({ index, stereo,
                    sourceChannels > 2
                        ? source.getFileNameWithoutExtension() + " Ch " + juce::String (channel + 1)
                        : source.getFileNameWithoutExtension() });
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
            nextTrack += sourceChannels > 2 ? sourceChannels : (stereo ? 2 : 1);
        }
        return result;
    }
}
