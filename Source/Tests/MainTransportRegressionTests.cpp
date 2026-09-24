#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../Capture/CaptureDaemon.h"

#include <functional>

namespace zynforge
{
    class MainTransportRegressionTests final : public juce::UnitTest
    {
    public:
        MainTransportRegressionTests()
            : juce::UnitTest ("Main daemon transport regressions", "zynforge") {}

        static bool waitUntil (std::function<bool()> predicate, int timeoutMs)
        {
            for (int elapsed = 0; elapsed < timeoutMs; elapsed += 10)
            {
                if (predicate()) return true;
                juce::Thread::sleep (10);
            }
            return predicate();
        }

        void runTest() override
        {
            AudioEngine::setTestModeSkipAudioInit (true);
            MainComponent::s_testConstruct = true;

            capture::CaptureDaemon daemon;
            daemon.setTestModeNoDevice (true);
            int port = 0;
            for (int candidate : { 49760, 49761, 49762 })
                if (daemon.start (candidate, 2)) { port = candidate; break; }
            expect (port > 0, "test daemon failed to bind");
            if (port == 0)
            {
                MainComponent::s_testConstruct = false;
                AudioEngine::setTestModeSkipAudioInit (false);
                return;
            }
            daemon.prepareForTests (48000.0, 256, 2);

            {
                MainComponent main;
                beginTest ("LOCK disables EDIT controls and refuses remote playback");
                main.onLockToggled();
                expect (main.editPage != nullptr && ! main.editPage->isEnabled());
                expect (main.automationToolbar != nullptr && ! main.automationToolbar->isEnabled());
                juce::String lockError;
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StartPlay, lockError));
                expect (lockError.containsIgnoreCase ("locked"), lockError);
                main.onLockToggled();

                main.useCaptureDaemon = true;
                expect (main.captureSupervisor.connectOrLaunch (port), "supervisor attach failed");
                expect (waitUntil ([&] { return main.captureSupervisor.hasStatus(); }, 3000));

                juce::BigInteger armed;
                armed.setBit (0);
                auto makeSession = []
                {
                    return juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("zf-main-transport-" + juce::Uuid().toString());
                };

                beginTest ("remote STOP needs confirmation before finalising the capture daemon");
                auto first = makeSession();
                expect (main.captureSupervisor.startRecording (first, 2, 0, armed));
                expect (waitUntil ([&] { return main.captureSupervisor.isDaemonRecording(); }, 3000));
                main.engine.setExternalRecording (true);
                main.engine.setActiveSessionDir (first);
                juce::String error;
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error));
                expect (error.containsIgnoreCase ("armed"), error);
                expect (main.engine.isRecording(), "first remote STOP ended the take");
                error.clear();
                expect (main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error), error);
                expect (waitUntil ([&] { return main.captureSupervisor.hasStatus()
                                             && ! main.captureSupervisor.isDaemonRecording(); }, 3000));
                expect (! main.engine.isRecording());
                expect (first.getChildFile ("Audio Files").getChildFile ("Track_01.wav").existsAsFile());

                beginTest ("confirmed quit stop helper does not orphan a rolling daemon take");
                auto second = makeSession();
                expect (main.captureSupervisor.startRecording (second, 2, 0, armed));
                expect (waitUntil ([&] { return main.captureSupervisor.isDaemonRecording(); }, 3000));
                main.engine.setExternalRecording (true);
                main.engine.setActiveSessionDir (second);
                const auto stale = main.captureSupervisor.lastStatus();
                main.captureSupervisor.disconnect();
                main.engine.setExternalCaptureStatus (stale, juce::Time::currentTimeMillis() - 5000);
                main.timerCallback();
                expectEquals (main.engine.captureStatus().source, juce::String ("daemon-unavailable"));
                expect (main.statusLabel.getText().containsIgnoreCase ("DAEMON STATUS UNAVAILABLE"),
                        "main dashboard bypassed status age and showed cached healthy metrics");
                expect (main.captureSupervisor.connectOrLaunch (port), "could not reattach to rolling daemon");
                expect (waitUntil ([&] { return main.captureSupervisor.isDaemonRecording(); }, 3000));
                expect (main.stopActiveCapture (false));
                expect (waitUntil ([&] { return main.captureSupervisor.hasStatus()
                                             && ! main.captureSupervisor.isDaemonRecording(); }, 3000));
                expect (! main.engine.isRecording(), "Stop & Quit path left external recording set");

                beginTest ("daemon stop reports session-finalization failure instead of false success");
                auto third = makeSession();
                expect (main.captureSupervisor.startRecording (third, 2, 0, armed));
                expect (waitUntil ([&] { return main.captureSupervisor.isDaemonRecording(); }, 3000));
                main.engine.setExternalRecording (true);
                expect (main.engine.getActiveSessionDir() == second,
                        "the preceding take should still be the pinned session before the new host pin");
                // Simulate a session volume disappearing after record starts.
                // The daemon still finalises its already-open media in `third`,
                // but the host has nowhere to write mandatory project state.
                auto vanishedMetadataTarget = makeSession();
                expect (vanishedMetadataTarget.createDirectory());
                main.engine.setActiveSessionDir (vanishedMetadataTarget);
                expect (main.engine.getActiveSessionDir() == vanishedMetadataTarget,
                        "an explicit daemon-session pin was hidden by the previously loaded take");
                expect (vanishedMetadataTarget.deleteRecursively());
                expect (main.engine.getActiveSessionDir() == vanishedMetadataTarget,
                        "a vanished recording volume silently fell back to the preceding take");
                expect (! main.stopActiveCapture (false),
                        "metadata write failure was reported as a clean stop");
                expect (waitUntil ([&] { return ! main.captureSupervisor.isDaemonRecording(); }, 3000));
                expect (! main.engine.isRecording(), "capture itself did not stop on metadata failure");

                beginTest ("selected daemon mode never silently falls back after disconnect");
                main.captureSupervisor.disconnect();
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StartRecord, error));
                expect (error.containsIgnoreCase ("daemon"));
                expect (! main.engine.getRecorder().isRecording());

                beginTest ("local remote RECORD uses host preflight, not a new timestamped session");
                main.useCaptureDaemon = false;
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StartRecord, error));
                expect (error.containsIgnoreCase ("pre-flight"), error);

                beginTest ("one manual punch-out saves media and project in the same session");
                auto local = makeSession();
                auto& recorder = main.engine.getRecorder();
                recorder.prepare (48000.0, 256, 1);
                recorder.getTrack (0).armed.store (true, std::memory_order_relaxed);
                expect (recorder.startRecording (local));
                std::vector<float> audio (256, 0.2f);
                const float* input = audio.data();
                for (int b = 0; b < 20; ++b)
                    recorder.processBlock (&input, 1, 256);
                main.manualPunchActive = true;
                main.onStopClicked();
                expect (! recorder.isRecording(), "manual punch-out still needed a second STOP tap");
                expect (! main.manualPunchActive, "manual punch state survived stop");
                expect (local.getChildFile ("Audio Files/Track_01.wav").existsAsFile());
                expect (local.getChildFile (local.getFileName() + ".zfproj").existsAsFile(),
                        "local stop did not persist the session document");
                expect (local.getChildFile ("session_mix.json").existsAsFile(),
                        "local stop did not persist track/mixer state");
                expect (main.engine.getActiveSessionDir() == local,
                        "local stop switched away from the recorded session");

                first.deleteRecursively();
                second.deleteRecursively();
                third.deleteRecursively();
                local.deleteRecursively();
            }

            daemon.stop();
            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static MainTransportRegressionTests mainTransportRegressionTests;
}
