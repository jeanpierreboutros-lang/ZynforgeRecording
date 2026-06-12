// Headless tests for the capture daemon engine (Source/Capture/
// CaptureDaemon.{h,cpp}). Test mode skips the audio device; a real
// CaptureClient drives the daemon over loopback (arm / format / start /
// stop) while the harness feeds synthetic input blocks -- mirroring how
// the GUI suite's CallbackFixture drives AudioEngine.

#include <juce_audio_formats/juce_audio_formats.h>

#include "../Capture/CaptureDaemon.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace zynforge
{
    class CaptureDaemonTests final : public juce::UnitTest
    {
    public:
        CaptureDaemonTests() : juce::UnitTest ("Capture daemon", "zynforge") {}

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

            auto sessionDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getChildFile ("zf-daemon-" + juce::Uuid().toString());

            beginTest ("daemon records a take driven entirely over the wire");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                bool started = false;
                for (int p : { 49720, 49721, 49722, 49723 })
                    if (daemon.start (p, 2)) { started = true; break; }
                expect (started, "daemon failed to start");
                if (! started) return;
                daemon.prepareForTests (48000.0, 256, 2);

                CaptureClient client;
                std::mutex smx;
                EngineStatus last;
                std::atomic<int> statusCount { 0 };
                client.onStatus = [&] (const EngineStatus& s)
                { const std::lock_guard<std::mutex> l (smx); last = s; statusCount.fetch_add (1); };

                expect (client.connect ("127.0.0.1", daemon.getPort()));
                expect (client.hello (2000).ok, "handshake failed");

                // Configure over the wire: 2 tracks, WAV24, arm track 0.
                Command tc;  tc.action = Action::SetTrackCount;    tc.intValue = 2;  expect (client.send (tc));
                Command fmt; fmt.action = Action::SetCaptureFormat; fmt.intValue = 0; expect (client.send (fmt));
                Command arm; arm.action = Action::ArmTrack; arm.trackIndex = 0; arm.boolValue = true;
                expect (client.send (arm));
                expect (waitUntil ([&] { return daemon.getRecorder().getNumTracks() == 2
                                              && daemon.getRecorder().getTrack (0).armed.load(); }, 2000),
                        "configuration commands did not land");

                // Start the take.
                Command rec; rec.action = Action::StartRecording;
                rec.sessionDir = sessionDir.getFullPathName();
                expect (client.send (rec));
                expect (waitUntil ([&] { return daemon.isRecording(); }, 2000),
                        "daemon never started recording");

                // Feed ~1 s of 0.5 DC into channel 0 like the GUI fixture does.
                std::vector<float> ch0 ((size_t) 256, 0.5f), ch1 ((size_t) 256, 0.0f);
                const float* ins[2] = { ch0.data(), ch1.data() };
                for (int b = 0; b < 188; ++b)
                {
                    daemon.processTestBlock (ins, 2, 256);
                    daemon.getRecorder().drainPendingForTests();
                }

                // Status push reflects the rolling take.
                expect (waitUntil ([&] { return statusCount.load() > 0; }, 3000),
                        "no status pushed");
                expect (waitUntil ([&] { const std::lock_guard<std::mutex> l (smx);
                                         return last.recording && last.numTracks == 2; }, 3000),
                        "status never showed recording with 2 tracks");

                // Stop; the take must exist on disk with real audio.
                Command stop; stop.action = Action::StopRecording;
                expect (client.send (stop));
                expect (waitUntil ([&] { return ! daemon.isRecording(); }, 3000),
                        "daemon never stopped");

                const auto audio = sessionDir.getChildFile ("Audio Files");
                const auto t1 = (audio.isDirectory() ? audio : sessionDir).getChildFile ("Track_01.wav");
                expect (t1.existsAsFile(), "Track_01.wav missing");
                if (t1.existsAsFile())
                {
                    juce::AudioFormatManager fm; fm.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (t1));
                    expect (rd != nullptr, "take unreadable");
                    if (rd != nullptr)
                    {
                        expect (rd->lengthInSamples > 40000, "take too short");
                        juce::AudioBuffer<float> buf (1, 4096);
                        rd->read (&buf, 0, 4096, rd->lengthInSamples / 2, true, false);
                        expectWithinAbsoluteError (buf.getSample (0, 2048), 0.5f, 0.02f);
                    }
                }

                client.disconnect();
                daemon.stop();
            }

            beginTest ("playback commands are refused (capture-only daemon)");
            {
                CaptureDaemon daemon;
                daemon.setTestModeNoDevice (true);
                bool started = false;
                for (int p : { 49725, 49726, 49727 })
                    if (daemon.start (p, 1)) { started = true; break; }
                expect (started);
                if (! started) return;

                CaptureClient client;
                std::mutex mx;
                juce::String lastErr;
                std::atomic<int> replies { 0 };
                client.onReply = [&] (const Reply& r)
                { const std::lock_guard<std::mutex> l (mx); if (! r.ok) lastErr = r.error; replies.fetch_add (1); };

                expect (client.connect ("127.0.0.1", daemon.getPort()));
                Command play; play.action = Action::StartPlayback;
                expect (client.send (play));
                expect (waitUntil ([&] { return replies.load() > 0; }, 2000), "no refusal reply");
                {
                    const std::lock_guard<std::mutex> l (mx);
                    expect (lastErr.containsIgnoreCase ("capture-only"), "wrong refusal: " + lastErr);
                }
                client.disconnect();
                daemon.stop();
            }

            sessionDir.deleteRecursively();
        }
    };

    static CaptureDaemonTests captureDaemonTests;
}
