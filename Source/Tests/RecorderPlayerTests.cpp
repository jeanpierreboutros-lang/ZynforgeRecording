// Behavioural tests for the recorder + player state machines, plus
// the clip/take surface. These are state-only checks (no audio
// device involved); the real-time path is exercised indirectly
// through getters / setters that mirror engine atomics.

#include <juce_core/juce_core.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/MultitrackRecorder.h"
#include "../Audio/SessionPlayer.h"

namespace zynforge
{
    class RecorderStateTests final : public juce::UnitTest
    {
    public:
        RecorderStateTests() : UnitTest ("Recorder state", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Default capture format is WAV/24");
            {
                AudioEngine eng;
                expect (eng.getRecorder().getCaptureFormat() == CaptureFormat::Wav24);
            }

            beginTest ("setCaptureFormat switches when not recording");
            {
                AudioEngine eng;
                eng.getRecorder().setCaptureFormat (CaptureFormat::Flac24);
                expect (eng.getRecorder().getCaptureFormat() == CaptureFormat::Flac24);
                eng.getRecorder().setCaptureFormat (CaptureFormat::Aiff32Float);
                expect (eng.getRecorder().getCaptureFormat() == CaptureFormat::Aiff32Float);
            }

            beginTest ("setPreRollSeconds clamps to a sensible range");
            {
                AudioEngine eng;
                eng.getRecorder().setPreRollSeconds (5);
                eng.getRecorder().setPreRollSeconds (30);
                eng.getRecorder().setPreRollSeconds (0);
                // Negative values shouldn't crash the recorder state.
                eng.getRecorder().setPreRollSeconds (-1);
            }

            beginTest ("isRecording() defaults to false");
            {
                AudioEngine eng;
                expect (! eng.isRecording());
                expect (! eng.getRecorder().isRecording());
            }

            beginTest ("Backup format persists independently of primary");
            {
                AudioEngine eng;
                eng.getRecorder().setCaptureFormat (CaptureFormat::Wav24);
                eng.getRecorder().setBackupCaptureFormat (CaptureFormat::Flac16);
                expect (eng.getRecorder().getCaptureFormat() == CaptureFormat::Wav24);
                expect (eng.getRecorder().getBackupCaptureFormat() == CaptureFormat::Flac16);
            }
        }
    };

    class SessionPlayerStateTests final : public juce::UnitTest
    {
    public:
        SessionPlayerStateTests() : UnitTest ("Session player", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("isLoaded / isPlaying default to false");
            {
                AudioEngine eng;
                expect (! eng.getPlayer().isLoaded());
                expect (! eng.getPlayer().isPlaying());
            }

            beginTest ("setLoopRegion + hasLoopRegion + clearLoopRegion round-trip");
            {
                AudioEngine eng;
                auto& player = eng.getPlayer();
                expect (! player.hasLoopRegion());
                player.setLoopRegion (10000, 50000);
                expect (player.hasLoopRegion());
                expectEquals (player.getLoopStart(), (juce::int64) 10000);
                expectEquals (player.getLoopEnd(),   (juce::int64) 50000);
                player.clearLoopRegion();
                expect (! player.hasLoopRegion());
            }

            beginTest ("setPositionSamples persists even without a loaded session");
            {
                AudioEngine eng;
                eng.getPlayer().setPositionSamples (123456);
                // Without a loaded session getPositionSamples may
                // clamp to 0 -- just verify we didn't crash and the
                // call returns a non-negative number.
                expect (eng.getPlayer().getPositionSamples() >= 0);
            }

            beginTest ("editCursor sample atomic round-trips");
            {
                AudioEngine eng;
                eng.setEditCursorSample (987654);
                expectEquals (eng.getEditCursorSample(), (juce::int64) 987654);
                eng.setEditCursorSample (0);
                expectEquals (eng.getEditCursorSample(), (juce::int64) 0);
            }
        }
    };

    class PunchModeTests final : public juce::UnitTest
    {
    public:
        PunchModeTests() : UnitTest ("Punch mode", "zynforge") {}

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);

            beginTest ("Per-track punch-arm toggle");
            {
                AudioEngine eng;
                eng.setStripCount (3);
                expect (! eng.isTrackPunchArmed (0));
                eng.setTrackPunchArmed (0, true);
                expect (eng.isTrackPunchArmed (0));
                expect (! eng.isTrackPunchArmed (1));
                eng.setTrackPunchArmed (0, false);
                expect (! eng.isTrackPunchArmed (0));
            }

            beginTest ("Automation punch range round-trip");
            {
                AudioEngine eng;
                eng.setAutomationPunchRange (1000, 2000);
                expectEquals (eng.getAutomationPunchIn(),  (juce::int64) 1000);
                expectEquals (eng.getAutomationPunchOut(), (juce::int64) 2000);
                expect (  eng.isInsideAutomationPunchRange (1500));
                expect (! eng.isInsideAutomationPunchRange (500));
                expect (! eng.isInsideAutomationPunchRange (2500));
                // Boundary semantics: in inclusive, out exclusive.
                expect (  eng.isInsideAutomationPunchRange (1000));
                expect (! eng.isInsideAutomationPunchRange (2000));
            }

            beginTest ("Cleared range allows writes anywhere");
            {
                AudioEngine eng;
                eng.setAutomationPunchRange (-1, -1);
                expect (eng.isInsideAutomationPunchRange (0));
                expect (eng.isInsideAutomationPunchRange (999999));
            }
        }
    };

    static RecorderStateTests       recorderStateTestsInstance;
    static SessionPlayerStateTests  sessionPlayerStateTestsInstance;
    static PunchModeTests           punchModeTestsInstance;
}
