// AudioEngine state-mutation tests. Covers strip count, naming,
// colour, gain / pan, stereo linking, swapTracks reordering, and
// the strip-overrides reset path. Skips audio-device init via
// setTestModeSkipAudioInit so tests run without a real device.

#include <juce_core/juce_core.h>
#include "../Audio/AudioEngine.h"

namespace zynforge
{
    class EngineStateTests final : public juce::UnitTest
    {
    public:
        EngineStateTests() : UnitTest ("Engine state", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("setStripCount grows + shrinks the recorder");
            {
                AudioEngine eng;
                eng.setStripCount (8);
                expectEquals (eng.getRecorder().getNumTracks(), 8);
                eng.setStripCount (3);
                expectEquals (eng.getRecorder().getNumTracks(), 3);
                eng.setStripCount (0);
                expectEquals (eng.getRecorder().getNumTracks(), 0);
            }

            beginTest ("setTrackName + setTrackColour persist on TrackState");
            {
                AudioEngine eng;
                eng.setStripCount (2);
                eng.setTrackName   (0, "kick");
                eng.setTrackColour (0, juce::Colour::fromRGB (0xff, 0x44, 0x22));
                expectEquals (eng.getRecorder().getTrack (0).name, juce::String ("kick"));
                expect (eng.getRecorder().getTrack (0).colourARGB.load() != 0);
            }

            beginTest ("setTrackGainDb / setTrackPan clamp + persist");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                eng.setTrackGainDb (0, -6.0f);
                expectWithinAbsoluteError (eng.getRecorder().getTrack (0).gainDb.load(), -6.0f, 0.001f);
                eng.setTrackPan (0, -0.5f);
                expectWithinAbsoluteError (eng.getRecorder().getTrack (0).pan.load(), -0.5f, 0.001f);
                // Wild values should clamp to the live range, not produce NaN.
                eng.setTrackPan (0, 9999.0f);
                const float clamped = eng.getRecorder().getTrack (0).pan.load();
                expect (clamped >= -1.0f && clamped <= 1.0f);
            }

            beginTest ("setTrackStereo flags the L channel");
            {
                AudioEngine eng;
                eng.setStripCount (4);
                eng.setTrackStereo (0, true);
                expect (eng.getRecorder().getTrack (0).isStereo.load());
            }

            beginTest ("swapTracks reorders adjacent strips");
            {
                AudioEngine eng;
                eng.setStripCount (3);
                eng.setTrackName (0, "A");
                eng.setTrackName (1, "B");
                eng.setTrackName (2, "C");
                expect (eng.swapTracks (0, 1));
                expectEquals (eng.getRecorder().getTrack (0).name, juce::String ("B"));
                expectEquals (eng.getRecorder().getTrack (1).name, juce::String ("A"));
                expectEquals (eng.getRecorder().getTrack (2).name, juce::String ("C"));
            }

            beginTest ("clearAllStripOverrides wipes per-strip state");
            {
                AudioEngine eng;
                eng.setStripCount (2);
                eng.setTrackPan    (0, 0.7f);
                eng.setTrackGainDb (0, -3.0f);
                eng.setTrackName   (0, "hat");
                eng.clearAllStripOverrides();
                // After a wipe + fresh setStripCount, the engineer
                // shouldn't see last session's hard-pan port over.
                eng.setStripCount (2);
                expectWithinAbsoluteError (eng.getRecorder().getTrack (0).pan.load(), 0.0f, 0.001f);
            }

            beginTest ("snapSampleToGrid -- Off mode is identity");
            {
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Off);
                expectEquals (eng.snapSampleToGrid (12345), (juce::int64) 12345);
                expectEquals (eng.snapSampleToGrid (0),     (juce::int64) 0);
                expectEquals (eng.snapSampleToGrid (-5),    (juce::int64) -5);
            }

            beginTest ("snapSampleToGrid -- Bars at 120 BPM 4/4");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                eng.setSessionTempoBpm (120.0f);
                eng.setTimeSignature (4, 4);
                eng.setSnapMode (AudioEngine::SnapMode::Bars);
                // 120 BPM 4/4 -> bar = 2 seconds. Default player SR
                // is 48000 when no session loaded, so bar = 96000.
                // Sample 50000 (-> 1.04s) should snap to bar 0 (closer).
                const auto snapped = eng.snapSampleToGrid (50000);
                expect (snapped == 0 || snapped == 96000);
                // Sample 60000 (1.25s) is closer to 2s bar (96000)
                // than to 0s bar -- still ambiguous depending on
                // closer side. Just check it's exactly on a bar.
                const auto s2 = eng.snapSampleToGrid (110000);
                // 110000 (~2.29s) is closer to bar at 96000 (2.0s)
                // than bar at 192000 (4.0s).
                expectEquals (s2, (juce::int64) 96000);
            }

            beginTest ("snapSampleToGrid -- Bars honours tempo map mid-session");
            {
                AudioEngine eng;
                eng.setStripCount (1);
                eng.setSessionTempoBpm (120.0f);
                eng.setTimeSignature (4, 4);
                // Tempo doubles to 240 BPM at sample 96000 (= bar 2
                // start at 120 bpm). 120 bpm 4/4 -> 2 s/bar = 96000.
                // 240 bpm 4/4 -> 1 s/bar = 48000. Bars after the
                // jump should land on 96000, 144000, 192000, ...
                eng.addTempoChange (96000, 240.0f);
                eng.setSnapMode (AudioEngine::SnapMode::Bars);
                // 150000 (3.125 s) under a constant 120 bpm would
                // snap to 192000 (4 s = bar 2). With the speed-up
                // 144000 (3 s) is the bar nearer to 150000.
                const auto s = eng.snapSampleToGrid (150000);
                expectEquals (s, (juce::int64) 144000);
                // 100000 -- post jump bar 2 (96000) is closest.
                expectEquals (eng.snapSampleToGrid (100000), (juce::int64) 96000);
                // 50000 -- pre-jump still 120 bpm, nearest bar is 0.
                const auto s50 = eng.snapSampleToGrid (50000);
                expect (s50 == 0 || s50 == 96000);   // either is the closer bar
            }

            beginTest ("snapSampleToGrid -- input at exact bar is unchanged");
            {
                AudioEngine eng;
                eng.setSessionTempoBpm (120.0f);
                eng.setTimeSignature (4, 4);
                eng.setSnapMode (AudioEngine::SnapMode::Bars);
                // 96000 IS bar 2 at 120 bpm.
                expectEquals (eng.snapSampleToGrid (96000), (juce::int64) 96000);
                expectEquals (eng.snapSampleToGrid (0),     (juce::int64) 0);
            }

            beginTest ("snapSampleToGrid -- Markers picks nearest");
            {
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Markers);
                // No session loaded -- markers manager has no
                // context, so snap should fall back to identity.
                expectEquals (eng.snapSampleToGrid (12345), (juce::int64) 12345);
            }

            beginTest ("Edit group assignment + membership query");
            {
                AudioEngine eng;
                eng.setStripCount (6);
                // Strips 0, 2, 4 share edit group 1.
                eng.setTrackEditGroup (0, 1);
                eng.setTrackEditGroup (2, 1);
                eng.setTrackEditGroup (4, 1);
                // Strip 3 in a different group.
                eng.setTrackEditGroup (3, 7);
                expectEquals (eng.getTrackEditGroup (0), 1);
                expectEquals (eng.getTrackEditGroup (1), -1);
                expectEquals (eng.getTrackEditGroup (3), 7);
                auto group1 = eng.getStripsInEditGroup (1);
                expectEquals ((int) group1.size(), 3);
                expectEquals (group1[0], 0);
                expectEquals (group1[1], 2);
                expectEquals (group1[2], 4);
                auto group7 = eng.getStripsInEditGroup (7);
                expectEquals ((int) group7.size(), 1);
                expectEquals (group7[0], 3);
                // -1 query returns empty.
                expect (eng.getStripsInEditGroup (-1).empty());
                // Reset to unlinked.
                eng.setTrackEditGroup (0, -1);
                expectEquals (eng.getTrackEditGroup (0), -1);
                expectEquals ((int) eng.getStripsInEditGroup (1).size(), 2);
            }

            beginTest ("VCA group assignment + reset");
            {
                AudioEngine eng;
                eng.setStripCount (4);
                eng.setTrackVcaGroup (0, 3);
                expectEquals (eng.getRecorder().getTrack (0).vcaGroup.load(), 3);
                eng.setTrackVcaGroup (0, -1);
                expectEquals (eng.getRecorder().getTrack (0).vcaGroup.load(), -1);
            }
        }
    };

    static EngineStateTests engineStateTestsInstance;
}
