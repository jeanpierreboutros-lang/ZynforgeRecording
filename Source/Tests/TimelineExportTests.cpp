#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/TimelineExport.h"

namespace zynforge
{
    class TimelineExportTests final : public juce::UnitTest
    {
    public:
        TimelineExportTests() : UnitTest ("Timeline export", "zynforge") {}

        void runTest() override
        {
            using namespace timelineexport;

            beginTest ("samplesToTimecode maps samples to HH:MM:SS:FF");
            {
                // 1 s @ 48k, 30 fps -> 00:00:01:00.
                expectEquals (samplesToTimecode (48000, 48000.0, 30), juce::String ("00:00:01:00"));
                // Half a second -> 15 frames at 30 fps.
                expectEquals (samplesToTimecode (24000, 48000.0, 30), juce::String ("00:00:00:15"));
                // 1 h 2 m 3 s exactly.
                const juce::int64 s = (juce::int64) ((1 * 3600 + 2 * 60 + 3) * 48000);
                expectEquals (samplesToTimecode (s, 48000.0, 30), juce::String ("01:02:03:00"));
                // Guards.
                expectEquals (samplesToTimecode (1000, 0.0, 30), juce::String ("00:00:00:00"));
                expectEquals (samplesToTimecode (1000, 48000.0, 0), juce::String ("00:00:00:00"));
            }

            beginTest ("csvField quotes + escapes embedded quotes");
            {
                expectEquals (csvField ("Kick"),        juce::String ("\"Kick\""));
                expectEquals (csvField ("Lead \"Vox\""), juce::String ("\"Lead \"\"Vox\"\"\""));
                expectEquals (csvField ("a,b"),         juce::String ("\"a,b\""));
            }

            beginTest ("csvField neutralises spreadsheet formula injection");
            {
                expectEquals (csvField ("=WEBSERVICE(\"https://example.invalid\")"),
                              juce::String ("\"'=WEBSERVICE(\"\"https://example.invalid\"\")\""));
                expectEquals (csvField ("+cmd|' /C calc'!A0"),
                              juce::String ("\"'+cmd|' /C calc'!A0\""));
                expectEquals (csvField ("@SUM(1,2)"), juce::String ("\"'@SUM(1,2)\""));
                expectEquals (csvField ("-6 dB"),     juce::String ("\"'-6 dB\""));
            }

            beginTest ("buildCsv emits header, tracks, markers, cues sections");
            {
                std::vector<TrackEntry> tracks { { 1, "Kick", "Track_01.wav" },
                                                 { 2, "Snare, top", "Track_02.wav" } };
                std::vector<MarkEntry>  markers { { 48000, "Verse", "user" } };
                std::vector<MarkEntry>  cues    { { 96000, "Song 1", "" } };

                const auto csv = buildCsv ("My Show", 48000.0, 30, tracks, markers, cues);

                expect (csv.contains ("Session,\"My Show\""));
                expect (csv.contains ("SampleRate,48000"));
                expect (csv.contains ("FrameRate,30"));
                expect (csv.contains ("[Tracks]"));
                expect (csv.contains ("1,\"Kick\",\"Track_01.wav\""));
                expect (csv.contains ("2,\"Snare, top\",\"Track_02.wav\""));   // comma stays inside the quoted field
                expect (csv.contains ("[Markers]"));
                expect (csv.contains ("00:00:01:00,48000,\"user\",\"Verse\""));
                expect (csv.contains ("[Cues]"));
                expect (csv.contains ("00:00:02:00,96000,\"Song 1\""));
            }

            beginTest ("Track rows name the files a strip really reads, every part included");
            {
                AudioEngine::setTestModeSkipAudioInit (true);
                const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("zf-timeline-" + juce::Uuid().toString());
                const auto audio = dir.getChildFile ("Audio Files");
                audio.createDirectory();
                const auto write = [&audio] (const juce::String& name, int channels)
                {
                    juce::WavAudioFormat wav;
                    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (
                        audio.getChildFile (name).createOutputStream().release(),
                        48000.0, (unsigned) channels, 24, {}, 0));
                    juce::AudioBuffer<float> b (channels, 480);
                    b.clear();
                    return w != nullptr && w->writeFromAudioSampleBuffer (b, 0, 480);
                };
                expect (write ("Track_01.wav", 2));          // native stereo: strips 1 (L) + 2 (R)
                expect (write ("Track_03.wav", 1));          // mono take with a continuation
                expect (write ("Track_03_part02.wav", 1));   // strip 4 was never recorded

                AudioEngine engine;
                engine.setActiveSessionDir (dir);
                const auto rows = trackEntries ({ "Keys L", "Keys R", "Vox", "Spare" },
                    [&engine] (int track, int& channel) { return engine.getTrackAudioFile (track, &channel); });

                const auto has = [&rows] (int index1, const juce::String& file, int channel1, int part)
                {
                    for (const auto& r : rows)
                        if (r.index1 == index1 && r.file == file && r.channel1 == channel1 && r.part == part)
                            return true;
                    return false;
                };
                expect (has (1, "Track_01.wav", 1, 1), "L strip must read channel 1 of the stereo file");
                expect (has (2, "Track_01.wav", 2, 1), "R strip pointed at a Track_02 file that doesn't exist");
                expect (has (3, "Track_03.wav", 0, 1));
                expect (has (3, "Track_03_part02.wav", 0, 2), "the continuation part is missing");
                expect (has (4, {}, 0, 1), "an unrecorded strip must not name a file");
                for (const auto& r : rows)
                    expect (r.file.isEmpty() || audio.getChildFile (r.file).existsAsFile(),
                            "listed file does not exist: " + r.file);

                const auto csv = buildCsv ("Show", 48000.0, 25, rows, {}, {});
                expect (csv.contains ("Index,Name,File,Channel,Part"));
                expect (csv.contains ("2,\"Keys R\",\"Track_01.wav\",2,1"));
                expect (csv.contains ("3,\"Vox\",\"Track_03_part02.wav\",,2"));
                dir.deleteRecursively();
            }

            beginTest ("Frame rate follows learned house timecode, 30 fps when none");
            {
                expectEquals (frameRateFor (0.0f), 30);
                expectEquals (frameRateFor (24.0f), 24);
                expectEquals (frameRateFor (25.0f), 25);
                expectEquals (frameRateFor (29.97f), 30);
                expectEquals (frameRateFor (30.0f), 30);
            }

            beginTest ("buildCsv with no markers/cues still emits the section headers");
            {
                const auto csv = buildCsv ("Empty", 48000.0, 25, {}, {}, {});
                expect (csv.contains ("[Tracks]"));
                expect (csv.contains ("[Markers]"));
                expect (csv.contains ("[Cues]"));
                expect (csv.contains ("FrameRate,25"));
            }
        }
    };

    static TimelineExportTests timelineExportTests;
}
