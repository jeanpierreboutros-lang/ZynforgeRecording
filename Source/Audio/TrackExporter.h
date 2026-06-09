#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace zynforge
{
    enum class ExportFormat
    {
        Wav24,
        Aiff24,
        Flac24,
        Mp3
    };

    struct ExportOptions
    {
        ExportFormat format        { ExportFormat::Wav24 };
        double       sampleRate    { 48000.0 };
        int          bitsPerSample { 24 };    // 16 / 24 / 32 (32 = IEEE float)
        int          mp3Bitrate    { 256 };   // kbps, used only for MP3
    };

    // Reads a source audio file, optionally resamples it, and writes it
    // to `dest` in the requested format. For MP3 the audio is first
    // rendered to a temporary WAV, then `lame` is invoked to produce
    // the final file. Returns true on success.
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

        // Resolves a path to the `lame` binary if installed (Homebrew /opt
        // or /usr/local, or whatever `which lame` returns). Returns
        // an invalid File if unavailable.
        static juce::File findLameBinary();

    private:
        // Shared MP3 tail: encodes an already-written temp WAV to .mp3 via
        // lame, then deletes the temp. False (+ outError) if lame is missing
        // or fails.
        bool encodeMp3 (const juce::File& tempWav,
                        const juce::File& destWithoutExt,
                        const ExportOptions&,
                        juce::String& outError);

        juce::AudioFormatManager formatManager;
    };
}
