#include <juce_core/juce_core.h>
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
