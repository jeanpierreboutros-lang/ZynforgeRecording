#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/ClickTrackRenderer.h"

namespace zynforge
{
    class ClickTrackRendererTests final : public juce::UnitTest
    {
    public:
        ClickTrackRendererTests() : juce::UnitTest ("Click-track renderer", "zynforge") {}

        static bool writeSentinel (const juce::File& file)
        {
            return file.replaceWithText ("keep the previous click");
        }

        void runTest() override
        {
            const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("zf-click-" + juce::Uuid().toString());
            expect (root.createDirectory().wasOk());
            const auto destination = root.getChildFile ("Track_01.wav");

            clickrender::Settings settings;
            settings.sampleRate = 48000.0;
            settings.totalSamples = 48000;
            settings.initialBpm = 120.0f;
            settings.beatsPerBar = 4;
            settings.voice1 = ClickEngine::getVoicePreset (ClickEngine::Voice::Click);
            settings.voice2 = ClickEngine::getVoicePreset (ClickEngine::Voice::Click);
            settings.subdivision1 = ClickEngine::Subdivision::Quarter;
            settings.subdivision2 = ClickEngine::Subdivision::Quarter;

            beginTest ("successful render atomically replaces the destination with a valid WAV");
            expect (writeSentinel (destination));
            expect (clickrender::render (destination, settings) == clickrender::Result::succeeded);
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (destination));
            expect (reader != nullptr);
            if (reader != nullptr)
            {
                expectEquals ((int) reader->numChannels, 1);
                expectEquals (reader->lengthInSamples, (juce::int64) 48000);
                expectWithinAbsoluteError (reader->sampleRate, 48000.0, 0.1);
            }
            expectEquals (root.findChildFiles (juce::File::findFiles, false).size(), 1);

            beginTest ("pre-cancelled render preserves the previous click and leaves no partial file");
            expect (writeSentinel (destination));
            std::atomic<bool> cancelled { true };
            expect (clickrender::render (destination, settings, &cancelled)
                    == clickrender::Result::cancelled);
            expectEquals (destination.loadFileAsString(), juce::String ("keep the previous click"));
            expectEquals (root.findChildFiles (juce::File::findFiles, false).size(), 1);

            beginTest ("invalid render request preserves the previous click");
            auto invalid = settings;
            invalid.totalSamples = 0;
            expect (clickrender::render (destination, invalid) == clickrender::Result::failed);
            expectEquals (destination.loadFileAsString(), juce::String ("keep the previous click"));

            root.deleteRecursively();
        }
    };

    static ClickTrackRendererTests clickTrackRendererTests;
}
