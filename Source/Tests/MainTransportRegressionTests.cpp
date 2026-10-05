#include <juce_audio_formats/juce_audio_formats.h>

#include "../UI/MainComponent.h"
#include "../Capture/CaptureDaemon.h"
#include "../Audio/SessionBackup.h"

#include <array>
#include <functional>
#include <thread>
#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

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

        static bool pumpUntil (const std::function<bool()>& predicate, int timeoutMs)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
            while (! predicate() && juce::Time::getMillisecondCounterHiRes() < end)
            {
               #if JUCE_MAC
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
               #elif JUCE_MODAL_LOOPS_PERMITTED
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
               #else
                juce::Thread::sleep (10);
               #endif
            }
            return predicate();
        }

       #if JUCE_MAC
        struct MetadataProbe
        {
            juce::File session = juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("zf-save-lifecycle-" + juce::Uuid().toString());
            std::unique_ptr<MainComponent> host;
            juce::WaitableEvent entered, release, interventionHandled;
            std::atomic<int> snapshotsEntered { 0 };
            std::atomic<bool> blocked { false }, hookFinished { false }, saveReturned { false };
            std::atomic<bool> interventionRan { false }, interventionDuringIo { false };

            ~MetadataProbe()
            {
                release.signal();
                if (host != nullptr) host->engine.clearAllStripOverrides();
                host.reset();
                session.deleteRecursively();
            }
        };

        std::shared_ptr<MetadataProbe> probePendingSave (
            std::function<void (MainComponent&, const juce::File&)> intervention,
            bool destroyHost = false, int stripCount = 1, bool stopTake = false)
        {
            auto probe = std::make_shared<MetadataProbe>();
            expect (probe->session.createDirectory().wasOk());
            probe->host = std::make_unique<MainComponent>();
            probe->host->engine.clearAllStripOverrides();
            probe->host->engine.setStripCount (stripCount);
            probe->host->engine.setTrackName (0, "Revision A");
            probe->host->engine.setActiveSessionDir (probe->session);
            probe->host->statusLabel.setText ("Not saved yet", juce::dontSendNotification);
            if (stopTake)
            {
                auto& recorder = probe->host->engine.getRecorder();
                recorder.prepare (48000.0, 256, stripCount);
                std::vector<float> audio ((size_t) stripCount * 256, 0.1f);
                std::vector<const float*> inputs;
                for (int channel = 0; channel < stripCount; ++channel)
                {
                    recorder.getTrack (channel).armed.store (true);
                    inputs.push_back (audio.data() + channel * 256);
                }
                expect (recorder.startRecording (probe->session), "synthetic capture did not start");
                for (int block = 0; block < 8; ++block)
                    recorder.processBlock (inputs.data(), stripCount, 256);
            }
            const auto hook = std::make_shared<const std::function<void()>> ([probe]
            {
                if (probe->snapshotsEntered.fetch_add (1) != 0) return;
                probe->blocked.store (true);
                probe->entered.signal();
                probe->release.wait (5000);
                probe->blocked.store (false);
                probe->hookFinished.store (true);
            });
            std::atomic_store (&sessionbackup::beforeSnapshotForTests, hook);
            const juce::ScopeGuard clearHook { [&]
            {
                probe->release.signal();
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::shared_ptr<const std::function<void()>>());
            } };
            std::thread watchdog ([probe, intervention, destroyHost]
            {
                if (! probe->entered.wait (5000)) { probe->release.signal(); return; }
                juce::MessageManager::callAsync ([probe, intervention, destroyHost]
                {
                    probe->interventionDuringIo.store (probe->blocked.load());
                    if (probe->host != nullptr) intervention (*probe->host, probe->session);
                    if (destroyHost && probe->host != nullptr)
                    {
                        probe->host->engine.clearAllStripOverrides();
                        probe->host.reset();
                    }
                    probe->interventionRan.store (true);
                    probe->release.signal();
                    probe->interventionHandled.signal();
                });
                if (! probe->interventionHandled.wait (2000)) probe->release.signal();
            });
            expect (juce::MessageManager::callAsync ([probe, stopTake]
            {
                if (probe->host != nullptr)
                {
                    if (stopTake) probe->host->stopActiveCaptureAsync (false);
                    else probe->host->onSaveSessionState();
                }
                probe->saveReturned.store (true);
            }));
            const bool completed = pumpUntil ([probe]
            {
                return probe->hookFinished.load() && probe->saveReturned.load()
                    && probe->interventionRan.load();
            }, 8000);
            probe->release.signal();
            watchdog.join();
            expect (completed, "metadata lifecycle fixture did not complete");
            expect (probe->interventionDuringIo.load(),
                    "UI could not exercise the lifecycle operation while metadata I/O was pending");
            return probe;
        }
       #endif

        void runTest() override
        {
            beginTest ("Isolated daemon transport fixture starts without an audio device");
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

                beginTest ("Native Save menu refreshes when finalization and metadata ownership change");
                {
                    struct MenuObserver final : juce::MenuBarModel::Listener
                    {
                        int changes = 0;
                        void menuBarItemsChanged (juce::MenuBarModel*) override { ++changes; }
                        void menuCommandInvoked (juce::MenuBarModel*,
                            const juce::ApplicationCommandTarget::InvocationInfo&) override {}
                    } observer;
                    main.addListener (&observer);
                    const juce::ScopeGuard restore { [&]
                    {
                        main.removeListener (&observer);
                        main.sessionIoBusy.store (false);
                        main.pendingMetadataSaves = 0;
                        main.captureMetadataPending = false;
                    } };
                    const auto saveEnabled = [&]
                    {
                        const auto menu = main.getMenuForIndex (0, "File");
                        for (juce::PopupMenu::MenuItemIterator item (menu); item.next();)
                            if (item.getItem().itemID == 2) return item.getItem().isEnabled;
                        return false;
                    };
                    main.sessionIoBusy.store (true);
                    main.pendingMetadataSaves = 0;
                    main.captureMetadataPending = false;
                    main.refreshMenuStateIfChanged();
                    expect (pumpUntil ([&] { return observer.changes != 0; }, 1000));
                    expect (! saveEnabled());
                    observer.changes = 0;
                    main.pendingMetadataSaves = 1; // busy remains true; ownership alone changes
                    main.refreshMenuStateIfChanged();
                    expect (pumpUntil ([&] { return observer.changes != 0; }, 500),
                            "native menu was not invalidated when metadata-only Save became available");
                    expect (saveEnabled());
                    observer.changes = 0;
                    main.captureMetadataPending = true;
                    main.refreshMenuStateIfChanged();
                    expect (pumpUntil ([&] { return observer.changes != 0; }, 500),
                            "native menu was not invalidated when capture finalization reserved Save");
                    expect (! saveEnabled());
                    observer.changes = 0;
                    main.captureMetadataPending = false;
                    main.refreshMenuStateIfChanged();
                    expect (pumpUntil ([&] { return observer.changes != 0; }, 500),
                            "native menu stayed stale after capture finalization completed");
                    expect (saveEnabled());
                }

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
                beginTest ("Repeated STOP stays pending and finalizes metadata exactly once");
                auto stopEntered = std::make_shared<juce::WaitableEvent>();
                auto stopRelease = std::make_shared<juce::WaitableEvent>();
                auto stopSnapshots = std::make_shared<std::atomic<int>> (0);
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::make_shared<const std::function<void()>> ([stopEntered, stopRelease, stopSnapshots]
                    {
                        ++*stopSnapshots;
                        stopEntered->signal();
                        stopRelease->wait (5000);
                    }));
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error),
                        "confirmed STOP acknowledged success before metadata completion");
                expect (error.containsIgnoreCase ("pending"), error);
                expect (stopEntered->wait (3000), "STOP did not enqueue its metadata snapshot");
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error));
                expect (error.containsIgnoreCase ("pending"), error);
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StartPlay, error));
                expect (error.containsIgnoreCase ("pending"), error);
                stopRelease->signal();
                expect (pumpUntil ([&] { return ! main.captureMetadataPending && main.pendingMetadataSaves == 0; }, 5000),
                        "confirmed STOP never completed metadata persistence");
                std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                    std::shared_ptr<const std::function<void()>>());
                expectEquals (stopSnapshots->load(), 1, "duplicate STOP finalized the same take again");
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

                beginTest ("remote STOP reports finalization failure after capture has stopped");
                auto remoteFailure = makeSession();
                expect (main.captureSupervisor.startRecording (remoteFailure, 2, 0, armed));
                expect (waitUntil ([&] { return main.captureSupervisor.isDaemonRecording(); }, 3000));
                main.engine.setExternalRecording (true);
                auto missingRemoteMetadata = makeSession();
                expect (missingRemoteMetadata.createDirectory().wasOk());
                main.engine.setActiveSessionDir (missingRemoteMetadata);
                expect (missingRemoteMetadata.deleteRecursively());
                main.stopArmedAtMs = 0;
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error));
                expect (error.containsIgnoreCase ("armed"), "first STOP did not retain its safety guard");
                expect (main.engine.isRecording(), "first STOP cut the take");
                error.clear();
                expect (! main.engine.performRemoteTransport (
                            AudioEngine::RemoteTransportAction::StopRecord, error),
                        "remote STOP acknowledged success despite mandatory session-save failure");
                expect (error.isNotEmpty(), "remote finalization failure had no explanation");
                expect (waitUntil ([&] { return ! main.captureSupervisor.isDaemonRecording(); }, 3000));
                expect (! main.engine.isRecording(), "capture itself should stop even when metadata cannot save");
                expect (pumpUntil ([&] { return ! main.captureMetadataPending && main.pendingMetadataSaves == 0; }, 5000));
                expect (main.lastCaptureFinalizationError.isNotEmpty(), "failed finalization was not latched");
                for (int retry = 0; retry < 2; ++retry)
                {
                    error.clear();
                    expect (! main.engine.performRemoteTransport (
                                AudioEngine::RemoteTransportAction::StopRecord, error),
                            "a later STOP falsely acknowledged failed persistence");
                    expect (error.containsIgnoreCase ("could not") || error.containsIgnoreCase ("failed"), error);
                }
                expect (remoteFailure.getChildFile ("Audio Files/Track_01.wav").existsAsFile(),
                        "failed metadata persistence lost the captured audio");

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
                expect (pumpUntil ([&] { return ! main.captureMetadataPending && main.pendingMetadataSaves == 0; }, 5000));
                expect (main.lastCaptureFinalizationError.isEmpty(), main.lastCaptureFinalizationError);
                expect (local.getChildFile ("Audio Files/Track_01.wav").existsAsFile());
                expect (local.getChildFile (local.getFileName() + ".zfproj").existsAsFile(),
                        "local stop did not persist the session document");
                expect (local.getChildFile ("session_mix.json").existsAsFile(),
                        "local stop did not persist track/mixer state");
                expect (main.engine.getActiveSessionDir() == local,
                        "local stop switched away from the recorded session");

                for (const juce::String failure : { "primary", "device", "backup", "mirror", "skipped mirror",
                                                    "recovery marker", "punch", "stereo mix", "integrity report" })
                {
                    beginTest ("Local asynchronous STOP retains the failed capture outcome: " + failure);
                    const auto failureSession = makeSession();
                    const juce::ScopeGuard cleanup { [&] { failureSession.deleteRecursively(); } };
                    MainComponent failed;
                    failed.engine.clearAllStripOverrides();
                    auto& rec = failed.engine.getRecorder();
                    rec.prepare (48000.0, 256, 1);
                    rec.getTrack (0).armed.store (true);
                    failed.engine.setActiveSessionDir (failureSession);
                    expect (rec.startRecording (failureSession));
                    std::array<float, 256> samples {};
                    const float* inputBlock = samples.data();
                    rec.processBlock (&inputBlock, 1, 256);
                    rec.drainPendingForTests();
                    // The captured media is valid before injecting the already
                    // existing outcome latch. This isolates STOP truthfulness
                    // from filesystem/device fault mechanics tested elsewhere.
                    if      (failure == "primary")          rec.primaryFailed.store (true);
                    else if (failure == "device")           rec.markCaptureDeviceLost();
                    else if (failure == "backup")           rec.backupFailed.store (true);
                    else if (failure == "mirror")           rec.mirrorFailed.store (true);
                    else if (failure == "skipped mirror")   rec.mirrorsSkippedAtStart.store (1);
                    else if (failure == "recovery marker")  rec.recoveryMarkerFailed.store (true);
                    else if (failure == "punch")            rec.punchSpliceFailed.store (true);
                    else if (failure == "stereo mix")       failed.engine.stereoMixWriteFailed.store (true);
                    else                                    rec.reportAsyncState->failed.store (true);
                    bool completed = false, successful = true;
                    failed.stopActiveCaptureAsync (true, [&] (bool ok)
                    {
                        successful = ok;
                        completed = true;
                    });
                    expect (pumpUntil ([&] { return completed && failed.pendingMetadataSaves == 0; }, 5000),
                            "local finalization never completed");
                    expect (! successful, "local STOP acknowledged a take with a latched " + failure + " failure");
                    expect (! failed.captureMetadataPending && ! failed.engine.isRecording());
                    expect (failed.lastCaptureFinalizationError.isNotEmpty(), "capture failure was not retained");
                    const auto expectedWord = failure == "skipped mirror" ? juce::String ("mirror")
                                            : failure == "stereo mix" ? juce::String ("stereo")
                                            : failure == "integrity report" ? juce::String ("report") : failure;
                    expect (failed.statusLabel.getText().containsIgnoreCase (expectedWord),
                            "final STOP status lost the reason: " + failed.statusLabel.getText());
                    juce::String stopError;
                    expect (! failed.engine.performRemoteTransport (
                                AudioEngine::RemoteTransportAction::StopRecord, stopError),
                            "a later remote STOP falsely acknowledged the failed take");
                    expect (failureSession.getChildFile ("session_mix.json").existsAsFile(),
                            "failed media outcome skipped required session metadata persistence");
                    failed.engine.clearAllStripOverrides();
                }

                beginTest ("Cue update reports persistence failure and retains the in-memory cue");
                auto cueSession = makeSession();
                expect (cueSession.createDirectory().wasOk());
                main.engine.setActiveSessionDir (cueSession);
                // A directory where the project must be installed fails reliably
                // even for privileged test users; no chmod or real volume needed.
                const auto blockedProject = cueSession.getChildFile (cueSession.getFileName() + ".zfproj");
                expect (blockedProject.createDirectory().wasOk());
                expect (blockedProject.getChildFile ("keep.txt").replaceWithText ("existing data"));
                main.cues.clear();
                SetlistBar::Cue cue;
                cue.name = "Unsaved cue";
                main.cues.push_back (cue);
                main.currentCueIndex = 0;
                main.statusLabel.setText ("", juce::dontSendNotification);
                main.updateCueAtTransport();
                expect (pumpUntil ([&] { return main.pendingMetadataSaves == 0; }, 5000));
                const auto cueStatus = main.statusLabel.getText();
                expect (cueStatus.containsIgnoreCase ("failed")
                            || cueStatus.containsIgnoreCase ("could not")
                            || cueStatus.containsIgnoreCase ("not saved"),
                        "cue persistence failed but the host reported only success: " + cueStatus);
                expectEquals ((int) main.cues.size(), 1, "failed persistence discarded the in-memory edit");
                expectEquals (blockedProject.getChildFile ("keep.txt").loadFileAsString(),
                              juce::String ("existing data"));

               #if JUCE_MAC
                beginTest ("Saving session metadata services UI messages before slow snapshot I/O completes");
                auto slowSaveSession = makeSession();
                expect (slowSaveSession.createDirectory().wasOk());
                main.engine.setActiveSessionDir (slowSaveSession);
                main.statusLabel.setText ("Save has not completed", juce::dontSendNotification);
                struct SaveGate
                {
                    juce::WaitableEvent entered, release, sentinelHandled;
                    std::atomic<bool> blocked { false }, hookFinished { false };
                    std::atomic<bool> saveCallReturned { false }, sentinelRan { false };
                    std::atomic<bool> sentinelDuringIo { false }, prematureSuccess { false };
                    std::atomic<bool> enteredObserved { false }, sentinelQueued { false };
                };
                auto gate = std::make_shared<SaveGate>();
                const auto hook = std::make_shared<const std::function<void()>> ([gate]
                {
                    gate->blocked.store (true);
                    gate->entered.signal();
                    gate->release.wait (5000); // bounded even if fixture dispatch fails
                    gate->blocked.store (false);
                    gate->hookFinished.store (true);
                });
                std::atomic_store (&sessionbackup::beforeSnapshotForTests, hook);
                const juce::ScopeGuard clearHook { [&]
                {
                    gate->release.signal();
                    std::atomic_store (&sessionbackup::beforeSnapshotForTests,
                        std::shared_ptr<const std::function<void()>>());
                } };
                juce::Component::SafePointer<MainComponent> safeMain (&main);
                std::thread watchdog ([gate, safeMain]
                {
                    if (! gate->entered.wait (5000)) { gate->release.signal(); return; }
                    gate->enteredObserved.store (true);
                    gate->sentinelQueued.store (juce::MessageManager::callAsync ([gate, safeMain]
                    {
                        const bool persistencePending = gate->blocked.load();
                        gate->sentinelDuringIo.store (persistencePending);
                        gate->prematureSuccess.store (persistencePending && safeMain != nullptr
                            && safeMain->statusLabel.getText().startsWith ("Saved session state"));
                        gate->sentinelRan.store (true);
                        gate->release.signal();
                        gate->sentinelHandled.signal();
                    }));
                    // The old synchronous save needs this escape hatch: its
                    // message thread cannot run the sentinel until I/O returns.
                    if (! gate->sentinelHandled.wait (2000)) gate->release.signal();
                });
                expect (juce::MessageManager::callAsync ([gate, safeMain]
                {
                    if (safeMain != nullptr) safeMain->onSaveSessionState();
                    gate->saveCallReturned.store (true);
                }));
                const auto pumpDeadline = juce::Time::getMillisecondCounterHiRes() + 8000.0;
                while (juce::Time::getMillisecondCounterHiRes() < pumpDeadline)
                {
                    // JUCE's macOS message queue is a CFRunLoop source. Pump it
                    // here because the test runner executes during initialise().
                    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, true);
                    if (gate->hookFinished.load() && gate->saveCallReturned.load()
                        && gate->sentinelRan.load()
                        && main.statusLabel.getText().startsWith ("Saved session state")) break;
                }
                gate->release.signal();
                watchdog.join();
                expect (gate->enteredObserved.load(), "the slow snapshot seam was never exercised");
                expect (gate->sentinelQueued.load() && gate->sentinelRan.load(), "the UI sentinel did not run");
                expect (gate->sentinelDuringIo.load(), "snapshot disk work blocked the message thread");
                expect (! gate->prematureSuccess.load(), "Save reported success before mandatory snapshot I/O completed");
                expect (gate->hookFinished.load() && gate->saveCallReturned.load(), "Save never completed");
                expect (main.statusLabel.getText().startsWith ("Saved session state"),
                        "successful completed persistence was not reported");
                expect (slowSaveSession.getChildFile ("Session File Backups").isDirectory());
                slowSaveSession.deleteRecursively();

                beginTest ("A pending metadata save refuses opening a different session");
                {
                    const auto otherSession = makeSession();
                    expect (otherSession.createDirectory().wasOk());
                    const auto project = otherSession.getChildFile (otherSession.getFileName() + ".zfproj");
                    const juce::String originalProject ("{\"formatVersion\":3,\"sentinel\":\"foreign session\"}");
                    expect (project.replaceWithText (originalProject));
                    auto attempted = std::make_shared<std::atomic<bool>> (false);
                    auto accepted = std::make_shared<std::atomic<bool>> (false);
                    auto probe = probePendingSave ([otherSession, attempted, accepted]
                        (MainComponent& host, const juce::File&)
                    {
                        attempted->store (true);
                        accepted->store (host.openSessionDocument (otherSession, false));
                    });
                    expect (attempted->load());
                    expect (! accepted->load(), "session replacement crossed a pending mandatory save");
                    expect (probe->host != nullptr && probe->host->engine.getActiveSessionDir() == probe->session,
                            "pending save lost ownership of its session");
                    expectEquals (project.loadFileAsString(), originalProject, "save changed another session's project");
                    otherSession.deleteRecursively();
                }

                beginTest ("A newer explicit save preserves edits made during an older pending snapshot");
                {
                    auto probe = probePendingSave ([] (MainComponent& host, const juce::File&)
                    {
                        host.engine.setTrackName (0, "Revision B");
                        host.menuItemSelected (2, 0); // File > Save remains usable while a save is pending
                    });
                    const auto hasNewRevision = [probe]
                    {
                        const auto mix = juce::JSON::parse (probe->session.getChildFile ("session_mix.json"));
                        const auto strips = mix.getProperty ("strips", juce::var());
                        return strips.isArray() && strips.size() == 1
                            && strips[0].getProperty ("name", "").toString() == "Revision B"
                            && probe->host != nullptr
                            && probe->host->statusLabel.getText().startsWith ("Saved session state");
                    };
                    expect (pumpUntil (hasNewRevision, 5000), "older completion lost or falsely acknowledged newer edits");
                    bool savedA = false, savedB = false;
                    const auto backups = probe->session.getChildFile ("Session File Backups");
                    for (const auto& folder : backups.findChildFiles (juce::File::findDirectories, false))
                    {
                        const auto saved = folder.getChildFile ("session_mix.json").loadFileAsString();
                        savedA = savedA || saved.contains ("Revision A");
                        savedB = savedB || saved.contains ("Revision B");
                    }
                    expect (savedA && savedB, "accepted explicit saves did not retain their own immutable revisions");
                }

                beginTest ("Non-undo mixer edits stay dirty when an older save completes");
                {
                    auto probe = probePendingSave ([] (MainComponent& host, const juce::File&)
                    {
                        host.engine.setTrackName (0, "Unsaved newer name");
                    });
                    expect (pumpUntil ([probe] { return probe->host->pendingMetadataSaves == 0; }, 5000));
                    const auto saved = probe->session.getChildFile ("session_mix.json").loadFileAsString();
                    expect (saved.contains ("Revision A") && ! saved.contains ("Unsaved newer name"),
                            "the worker read mutable engine state instead of its captured revision");
                    expect (probe->host->statusLabel.getText().containsIgnoreCase ("newer changes remain unsaved"),
                            "an older save falsely marked non-undo mixer edits clean");
                    expectEquals (probe->host->lastSavedUndoUnits, -1);
                }

                beginTest ("Pending layout saves keep navigation available and bounded without saving unrelated edits");
                {
                    auto inspected = std::make_shared<std::atomic<bool>> (false);
                    auto navigationWorked = std::make_shared<std::atomic<bool>> (false);
                    auto bounded = std::make_shared<std::atomic<bool>> (false);
                    auto probe = probePendingSave ([inspected, navigationWorked, bounded]
                        (MainComponent& host, const juce::File&)
                    {
                        host.timerCallback();
                        const auto previousView = host.currentView;
                        host.keyPressed (juce::KeyPress ('=', juce::ModifierKeys::commandModifier, 0), nullptr);
                        navigationWorked->store (host.currentView != previousView
                            && host.editPage != nullptr && host.editPage->isEnabled());
                        host.engine.setTrackName (0, "Unsaved layout-only edit");
                        for (int event = 0; event < 128; ++event) host.requestUILayoutSave();
                        bounded->store (host.pendingMetadataSaves <= 2);
                        inspected->store (true);
                    });
                    expect (inspected->load() && navigationWorked->load(),
                            "metadata saving disabled view navigation or the whole editor");
                    expect (bounded->load(), "coalesced layout saves accumulated completion obligations");
                    expect (pumpUntil ([probe] { return probe->host->pendingMetadataSaves == 0; }, 5000));
                    const auto saved = probe->session.getChildFile ("session_mix.json").loadFileAsString();
                    expect (saved.contains ("Revision A") && ! saved.contains ("Unsaved layout-only edit"),
                            "a layout-only save unexpectedly persisted an unrelated mixer edit");
                    expect (! probe->host->statusLabel.getText().startsWith ("Saving session metadata"),
                            "layout revision suppressed the full-save completion status");
                    expectEquals (probe->snapshotsEntered.load(), 1, "layout saves created full metadata backups");
                }

                beginTest ("A 27-track STOP services UI while its mandatory metadata snapshot is stalled");
                {
                    auto pendingObserved = std::make_shared<std::atomic<bool>> (false);
                    auto probe = probePendingSave ([pendingObserved] (MainComponent& host, const juce::File&)
                    {
                        pendingObserved->store (host.captureMetadataPending && ! host.engine.isRecording()
                            && host.engine.getRecorder().getNumTracks() == 27);
                        host.timerCallback();
                        host.engine.setTrackName (0, "Edited while finalizing");
                    }, false, 27, true);
                    expect (pendingObserved->load(), "27-track STOP did not yield while finalization was pending");
                    expect (pumpUntil ([probe]
                    {
                        return ! probe->host->captureMetadataPending && probe->host->pendingMetadataSaves == 0;
                    }, 5000));
                    expect (probe->host->lastCaptureFinalizationError.isEmpty(),
                            probe->host->lastCaptureFinalizationError);
                    const auto mix = juce::JSON::parse (probe->session.getChildFile ("session_mix.json"));
                    expectEquals ((int) mix["trackCount"], 27);
                    const auto recordedFiles = probe->session.getChildFile ("Audio Files")
                        .findChildFiles (juce::File::findFiles, false, "Track_*.wav");
                    expectEquals (recordedFiles.size(), 27);
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    for (const auto& file : recordedFiles)
                    {
                        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                        expect (reader != nullptr, "a finalized 27-track file is unreadable");
                        if (reader != nullptr)
                            expectEquals (reader->lengthInSamples, (juce::int64) 2048,
                                          "asynchronous STOP lost accepted capture frames");
                    }
                    expect (probe->host->statusLabel.getText().containsIgnoreCase ("newer changes remain unsaved"),
                            "successful capture completion hid newer unsaved metadata edits");
                    expectEquals (probe->snapshotsEntered.load(), 1);
                }

                beginTest ("Destroying a host with a pending metadata save retires callbacks safely");
                {
                    auto probe = probePendingSave ([] (MainComponent&, const juce::File&) {}, true);
                    expect (probe->host == nullptr, "host remained alive after requested destruction");
                    // Flush already-posted completion work after destruction;
                    // ASan/TSan exercise the same real callback lifetime path.
                    auto drained = std::make_shared<std::atomic<bool>> (false);
                    juce::MessageManager::callAsync ([drained] { drained->store (true); });
                    expect (pumpUntil ([drained] { return drained->load(); }, 2000));
                    expect (probe->session.getChildFile ("session_mix.json").existsAsFile(),
                            "host teardown discarded accepted persistence work");
                }
               #endif

                first.deleteRecursively();
                second.deleteRecursively();
                third.deleteRecursively();
                remoteFailure.deleteRecursively();
                local.deleteRecursively();
                cueSession.deleteRecursively();
            }

            daemon.stop();
            MainComponent::s_testConstruct = false;
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static MainTransportRegressionTests mainTransportRegressionTests;
}
