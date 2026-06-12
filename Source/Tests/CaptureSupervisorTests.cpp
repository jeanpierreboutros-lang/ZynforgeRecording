// Headless tests for the GUI-side daemon supervisor (Source/Capture/
// CaptureSupervisor.{h,cpp}) -- Phases 1d + 2 semantics:
//   * attach-first (a rolling daemon is re-adopted, never duplicated)
//   * launch the REAL ZynforgeCapture binary when nothing answers,
//     record a take over the wire, and LEAVE IT ROLLING on shutdown()
//   * the GUI-side watchdog reports a dead daemon (loud when mid-take)

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Capture/CaptureSupervisor.h"
#include "../Capture/CaptureDaemon.h"

#include <atomic>
#include <functional>

namespace zynforge
{
    class CaptureSupervisorTests final : public juce::UnitTest
    {
    public:
        CaptureSupervisorTests() : juce::UnitTest ("Capture supervisor", "zynforge") {}

        static bool waitUntil (std::function<bool()> pred, int timeoutMs)
        {
            for (int t = 0; t < timeoutMs; t += 10)
            {
                if (pred()) return true;
                juce::Thread::sleep (10);
            }
            return pred();
        }

        void runTest() override
        {
            using namespace zynforge::capture;

            beginTest ("attaches to an existing daemon instead of launching a twin (reattach)");
            {
                // In-process daemon stands in for one left rolling by a dead GUI.
                CaptureDaemon existing;
                existing.setTestModeNoDevice (true);
                int port = 0;
                for (int p : { 49740, 49741, 49742 })
                    if (existing.start (p, 2)) { port = p; break; }
                expect (port > 0);
                if (port == 0) return;
                existing.prepareForTests (48000.0, 256, 2);

                CaptureSupervisor sup;
                expect (sup.connectOrLaunch (port), "attach failed");
                expect (sup.isAttached());
                expect (! sup.didLaunch(), "launched a twin instead of attaching");

                // Status flows: the supervisor sees the daemon's state.
                expect (waitUntil ([&] { return sup.hasStatus(); }, 3000), "no status received");

                sup.disconnect();
                existing.stop();
            }

            beginTest ("launches the real binary, records a take, and leaves it rolling on shutdown");
            {
                const auto bin = CaptureSupervisor::discoverBinary();
                expect (bin.existsAsFile(),
                        "ZynforgeCapture binary not found relative to the test executable");
                if (! bin.existsAsFile()) return;

                auto sessionDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                      .getChildFile ("zf-sup-" + juce::Uuid().toString());

                CaptureSupervisor sup;
                sup.setBinaryPath (bin);
                expect (sup.connectOrLaunch (49745, { "--no-device", "--inputs", "2" }),
                        "launch+connect failed");
                expect (sup.didLaunch(), "should have launched (nothing was on the port)");

                juce::BigInteger arms; arms.setBit (0);
                expect (sup.startRecording (sessionDir, 2, /*Wav24*/ 0, arms));
                expect (waitUntil ([&] { return sup.isDaemonRecording(); }, 4000),
                        "daemon never reported recording");

                // Phase 2 semantic: shutdown() with a ROLLING take leaves the
                // daemon alive. Reattach with a fresh supervisor and stop it.
                sup.shutdown();
                juce::Thread::sleep (300);

                CaptureSupervisor again;
                expect (again.connectOrLaunch (49745), "reattach to rolling daemon failed");
                expect (! again.didLaunch(), "daemon was killed by shutdown mid-take");
                expect (waitUntil ([&] { return again.isDaemonRecording(); }, 3000),
                        "rolling take lost across GUI handover");

                expect (again.stopRecording());
                expect (waitUntil ([&] { return again.hasStatus()
                                              && ! again.isDaemonRecording(); }, 4000),
                        "stop never landed");

                // The take exists on disk (headers written even with no audio fed).
                const auto audio = sessionDir.getChildFile ("Audio Files");
                expect ((audio.isDirectory() ? audio : sessionDir)
                            .getChildFile ("Track_01.wav").existsAsFile(),
                        "no take on disk");

                // Idle now -- but `again` ATTACHED (didn't launch), so its
                // shutdown must leave the daemon alone.
                again.shutdown();
                juce::Thread::sleep (200);
                CaptureSupervisor third;
                expect (third.connectOrLaunch (49745), "attached supervisor wrongly killed the daemon");
                expect (! third.didLaunch());

                // Cleanup over the wire: Quit (accepted -- idle), then the
                // port must come free so the NEXT suite run launches fresh.
                expect (third.requestQuit(), "quit send failed");
                expect (waitUntil ([&]
                {
                    CaptureClient probe;
                    const bool up = probe.connect ("127.0.0.1", 49745) && probe.hello (300).ok;
                    probe.disconnect();
                    return ! up;
                }, 5000), "daemon did not exit after Quit -- would leak across runs");
                third.disconnect();
                sessionDir.deleteRecursively();
            }

            beginTest ("watchdog reports a dead daemon (loud when it was recording)");
            {
                CaptureDaemon fake;
                fake.setTestModeNoDevice (true);
                int port = 0;
                for (int p : { 49750, 49751, 49752 })
                    if (fake.start (p, 1)) { port = p; break; }
                expect (port > 0);
                if (port == 0) return;
                fake.prepareForTests (48000.0, 256, 1);

                CaptureSupervisor sup;
                std::atomic<int>  deaths { 0 };
                std::atomic<bool> deadWhileRecording { false };
                sup.onDaemonDied = [&] (bool wasRecording)
                { deadWhileRecording.store (wasRecording); deaths.fetch_add (1); };

                expect (sup.connectOrLaunch (port));
                expect (waitUntil ([&] { return sup.hasStatus(); }, 3000));

                // Kill the "daemon": the link drops; tick() must notice once.
                fake.stop();
                expect (waitUntil ([&] { sup.tick(); return deaths.load() > 0; }, 4000),
                        "watchdog never fired");
                sup.tick(); sup.tick();
                expectEquals (deaths.load(), 1, "death reported more than once");
                expect (! deadWhileRecording.load(), "idle daemon misreported as mid-take");

                sup.disconnect();
            }
        }
    };

    static CaptureSupervisorTests captureSupervisorTests;
}
