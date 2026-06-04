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

            beginTest ("freshly-grown strips don't inherit a previous layout's edit group / VCA");
            {
                AudioEngine eng;
                eng.setStripCount (2);
                eng.setTrackEditGroup (0, 1);   // persists strip_editgroup_0
                eng.setTrackVcaGroup  (1, 3);   // persists strip_vca_1
                expectEquals (eng.getTrackEditGroup (0), 1);

                // Empty out (as on an empty-on-open relaunch) then add strips
                // back. The new strips must come up CLEAN -- the old edit
                // group / VCA assignments must not revive by array position.
                eng.setStripCount (0);
                eng.setStripCount (2);
                expectEquals (eng.getTrackEditGroup (0), -1);
                expectEquals (eng.getRecorder().getTrack (1).vcaGroup.load(), -1);
            }

            beginTest ("snapSampleToGrid -- Off mode is identity");
            {
                AudioEngine eng;
                eng.setSnapMode (AudioEngine::SnapMode::Off);
                expectEquals (eng.snapSampleToGrid (12345), (juce::int64) 12345);
                expectEquals (eng.snapSampleToGrid (0),     (juce::int64) 0);
                expectEquals (eng.snapSampleToGrid (-5),    (juce::int64) -5);
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

            beginTest ("Time signature drives the metronome bar length (downbeat accent)");
            {
                AudioEngine eng;
                // The metronome's bar length must track the meter's numerator.
                eng.setTimeSignature (4, 4);
                expectEquals (eng.getClickEngine().getBeatsPerBar(), 4);
                eng.setTimeSignature (3, 4);
                expectEquals (eng.getClickEngine().getBeatsPerBar(), 3);
                eng.setTimeSignature (6, 8);
                expectEquals (eng.getClickEngine().getBeatsPerBar(), 6);
                eng.setTimeSignature (4, 4);   // restore default (don't pollute appProps)
            }

            beginTest ("Full strip mix state round-trips through the session file (per-session, not global)");
            {
                auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                               .getChildFile ("zynforge_mix_roundtrip");
                tmp.deleteRecursively();
                tmp.createDirectory();

                AudioEngine eng;
                eng.setStripCount (4);
                eng.setTrackName     (0, "KICK");
                eng.setTrackGainDb   (0, -6.0f);
                eng.setTrackPan      (1, -0.5f);
                eng.getRecorder().getTrack (2).muted.store (true);
                eng.getRecorder().getTrack (3).soloed.store (true);
                eng.setTrackVcaGroup (0, 2);
                eng.setTrackVcaGroup (1, 2);
                eng.setTrackEditGroup (3, 1);
                // isBus + aux send -- the two fields that used to leak across
                // sessions via global appProps. Track 3 becomes a bus; track 0
                // sends to it. Both must round-trip through session_mix.json.
                eng.setTrackIsBus (3, true);
                eng.setTrackSend  (0, /*slot*/ 0, /*bus*/ 3, /*dB*/ -3.0f, /*post*/ true);
                eng.setSessionTempoBpm (142.0f);
                eng.setTimeSignature (6, 8);
                eng.setTempoMap ({ { 0, 120.0f }, { 480000, 140.0f } });
                eng.saveSessionMixTo (tmp);

                // Trash the live state as if a different session had loaded.
                eng.setTrackName   (0, "WRONG");
                eng.setTrackGainDb (0, 0.0f);
                eng.setTrackPan    (1, 0.0f);
                eng.getRecorder().getTrack (2).muted.store (false);
                eng.setTrackVcaGroup (2, 5);
                eng.setTrackIsBus (3, false);                 // un-bus it
                eng.setTrackSend  (0, 0, -1, 0.0f, false);    // clear the send
                eng.setSessionTempoBpm (90.0f);
                eng.setTimeSignature (4, 4);
                eng.setTempoMap ({});

                eng.loadSessionMixFrom (tmp);
                expectEquals (eng.getRecorder().getTrack (0).name, juce::String ("KICK"));
                expectWithinAbsoluteError (eng.getRecorder().getTrack (0).gainDb.load(), -6.0f, 0.01f);
                expectWithinAbsoluteError (eng.getRecorder().getTrack (1).pan.load(), -0.5f, 0.01f);
                expect (eng.getRecorder().getTrack (2).muted.load());
                expect (eng.getRecorder().getTrack (3).soloed.load());
                expectEquals (eng.getRecorder().getTrack (0).vcaGroup.load(), 2);
                expectEquals (eng.getTrackEditGroup (3), 1);
                // Strip 2 was ungrouped in the file -> the leaked VCA 5 is overwritten.
                expectEquals (eng.getRecorder().getTrack (2).vcaGroup.load(), -1);
                // isBus + aux send round-trip (the appProps-leak fix).
                expect (eng.getRecorder().getTrack (3).isBus.load());
                expect (! eng.getRecorder().getTrack (0).isBus.load());
                expectEquals (eng.getRecorder().getTrack (0).sends[0].targetBus.load(), 3);
                expectWithinAbsoluteError (
                    eng.getRecorder().getTrack (0).sends[0].levelDb.load(), -3.0f, 0.01f);
                expect (eng.getRecorder().getTrack (0).sends[0].postFader.load());
                // Tempo state round-trips per session.
                expectWithinAbsoluteError (eng.getSessionTempoBpm(), 142.0f, 0.01f);
                expectEquals (eng.getTimeSignatureNumerator(),   6);
                expectEquals (eng.getTimeSignatureDenominator(), 8);
                expectEquals ((int) eng.getTempoMap().size(), 2);

                tmp.deleteRecursively();
            }
        }
    };

    static EngineStateTests engineStateTestsInstance;
}
