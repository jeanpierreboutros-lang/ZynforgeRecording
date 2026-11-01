#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace zynforge
{
    enum class ExportFormat
    {
        Wav24,
        Aiff24,
        Flac24
    };

    struct ExportOptions
    {
        ExportFormat format        { ExportFormat::Wav24 };
        double       sampleRate    { 48000.0 };
        int          bitsPerSample { 24 };    // 16 / 24 / 32 (32 = IEEE float)
    };

    // Reads a source audio file, optionally resamples it, and writes it
    // to `dest` in the requested format. Returns true on success.
    class TrackExporter
    {
    public:
        TrackExporter();

        bool exportTrack (const juce::File& source,
                          const juce::File& destWithoutExtension,
                          const ExportOptions&,
                          juce::String& outError);

        // Interleave two mono sources (L + R) into ONE stereo file in the
        // requested format / sample rate. Used for stereo-pair tracks so a
        // paired / imported stereo strip exports as a single stereo file
        // rather than two mono stems.
        bool exportStereoPair (const juce::File& sourceL,
                               const juce::File& sourceR,
                               const juce::File& destWithoutExtension,
                               const ExportOptions&,
                               juce::String& outError);

        // Maps a format to the file extension that exportTrack will produce.
        static juce::String extensionFor (ExportFormat);

        // Why a file of this size cannot be written in this format, or empty.
        // Checked first, so a format limit never surfaces hours in as a write
        // failure on a disk with space.
        static juce::String sizeRefusal (const ExportOptions&, int numChannels, juce::int64 frames);

    private:
        friend class StereoExportTests; // deterministic decoder-failure fixture
        juce::AudioFormatManager formatManager;
    };

    // Takes a render as it streams out of the engine and leaves one finished
    // export file in the chosen format. The audio stays in a hidden sibling
    // until commit() installs it, so an abandoned or failed export leaves
    // nothing behind that could pass for a finished one: the destructor
    // removes the sibling. A plain WAV, AIFF or FLAC at the render's own rate
    // is written directly; a rate conversion renders to a float WAV first and
    // hands it to exportTrack.
    class ExportFileStream
    {
    public:
        ExportFileStream (TrackExporter& exporter, const juce::File& destWithoutExtension,
                          const ExportOptions& options, double renderRate, int numChannels);
        ~ExportFileStream();

        bool isOpen() const noexcept { return writer != nullptr; }
        bool write (const juce::AudioBuffer<float>& block, int numSamples);
        bool commit (juce::String& error);

    private:
        TrackExporter& exporter;
        juce::File destWithoutExt, temporary;
        ExportOptions options;
        bool convertOnCommit { false };
        std::shared_ptr<std::atomic<bool>> closeFailed;
        std::unique_ptr<juce::AudioFormatWriter> writer;

        JUCE_DECLARE_NON_COPYABLE (ExportFileStream)
    };
}
