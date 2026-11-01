// The dead-channel alarm. The tiers run through the real audio callback, with
// the same status snapshot the UI timer and the daemon link read; the Monitor's
// arithmetic is pinned at its boundaries; the JSON stays additive.

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/ChannelAlarm.h"
#include "../Capture/CaptureDaemon.h"
#include "../Network/CaptureProtocol.h"
#include "../Network/RemoteControl.h"

#include <functional>
#include <mutex>

namespace zynforge
{
    class ChannelAlarmTests final : public juce::UnitTest
    {
    public:
        ChannelAlarmTests() : juce::UnitTest ("Dead-channel alarm", "zynforge") {}

        void runTest() override
        {
            // Before the first engine exists: it must never open a real device.
            AudioEngine::setTestModeSkipAudioInit (true);
            monitorBoundaries();
            tiersThroughTheCallback();
            streamFaults();
            stereoPairs();
            takeIsUntouched();
            reportedAndCarried();
            daemon();
            statusFields();
            statusSnapshot();
        }

    private:
        using Alarm = channelalarm::Alarm;
        static constexpr double kRate  = 48000.0;
        static constexpr int    kBlock = 4800;   // 100 ms, one UI timer tick

        static juce::int64 frames (double seconds) { return (juce::int64) std::llround (seconds * kRate); }

        static juce::File tempDir()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("zf-alarm-" + juce::Uuid().toString());
            dir.createDirectory();
            return dir;
        }

        static std::vector<Alarm> ofKind (const std::vector<Alarm>& alarms, const juce::String& kind)
        {
            std::vector<Alarm> out;
            for (const auto& a : alarms) if (a.kind == kind) out.push_back (a);
            return out;
        }

        static juce::var waitForReport (const juce::File& dir)
        {
            const auto file = dir.getChildFile ("session.report.json");
            for (int i = 0; i < 3000; ++i)
            {
                const auto report = juce::JSON::parse (file);
                if (report.isObject() && ! (bool) report.getProperty ("sha256Pending", true))
                    return report;
                juce::Thread::sleep (10);
            }
            return juce::JSON::parse (file);
        }

        static std::vector<Alarm> latchedIn (const juce::File& dir)
        {
            const auto report = waitForReport (dir);
            std::vector<Alarm> alarms;
            if (const auto* list = report.getProperty ("channelAlarms", {}).getArray())
                for (const auto& alarm : *list) alarms.push_back (Alarm::fromJson (alarm));
            return alarms;
        }

        // What the daemon's own status loop has latched, read without evaluating
        // anything: a test that evaluated would be doing the loop's job for it.
        static std::vector<Alarm> latchedByLoop (MultitrackRecorder& recorder)
        {
            const juce::ScopedLock sl (recorder.alarmLock);
            return recorder.alarmMonitor.history (0);
        }

        // ── The engine harness ────────────────────────────────────────────
        struct Rig
        {
            explicit Rig (int strips)
                : dir (tempDir()), in (strips, kBlock), out (2, kBlock), levels ((size_t) strips, 0.0f)
            {
                engine.setStripCount (strips);
                engine.prepareForTests (kRate, kBlock);
                engine.getRecorder().setCaptureFormat (CaptureFormat::Wav16);
                for (int i = 0; i < strips; ++i)
                {
                    auto& t = engine.getRecorder().getTrack (i);
                    t.armed.store (true);
                    t.setNameThreadSafe ("Strip " + juce::String (i + 1));
                    engine.setTrackInputRouting (i, i);
                    engine.setTrackOutputRouting (i, -1);
                }
            }

            ~Rig() { MultitrackRecorder::deleteSessionAfterCancellingReports (dir); }

            void level (float value) { std::fill (levels.begin(), levels.end(), value); }

            // Runs the callback for `count` frames, polling status per block
            // as the 10 Hz timer would. Leaves the last poll in `live`.
            void run (juce::int64 count)
            {
                while (count > 0)
                {
                    const int n = (int) juce::jmin ((juce::int64) kBlock, count);
                    for (int ch = 0; ch < in.getNumChannels(); ++ch)
                        juce::FloatVectorOperations::fill (in.getWritePointer (ch), levels[(size_t) ch], n);
                    out.clear();
                    engine.audioDeviceIOCallbackWithContext (in.getArrayOfReadPointers(), in.getNumChannels(),
                                                             out.getArrayOfWritePointers(), 2, n, {});
                    engine.getRecorder().drainPendingForTests();
                    live = engine.captureStatus().channelAlarms;
                    count -= n;
                }
            }

            juce::File dir;
            AudioEngine engine;
            juce::AudioBuffer<float> in, out;
            std::vector<float> levels;
            std::vector<Alarm> live;
        };

        // ── Monitor boundaries ────────────────────────────────────────────
        void monitorBoundaries()
        {
            using namespace channelalarm;
            const auto fault = frames (kFaultSeconds);

            const auto monitorOf = [] (int channels)
            {
                Monitor m;
                std::vector<Monitor::Input> inputs;
                for (int i = 0; i < channels; ++i) inputs.push_back ({ i, "Ch " + juce::String (i + 1) });
                m.begin (std::move (inputs), kRate);
                return m;
            };

            beginTest ("Thresholds are the agreed ones");
            {
                expectEquals (kFaultSeconds, 3.0);
                expectEquals (kStreamWindowSeconds, 2.0);
                expectEquals (kStreamChannels, 8);
                expectEquals (kNoSignalSeconds, 60.0);
            }

            beginTest ("Tier 1 fires at exactly the threshold");
            {
                auto m = monitorOf (1);
                m.update ({ 1000 }, 1000 + fault - 1);
                expect (m.live (0).empty(), "one frame short must not fire");
                m.update ({ 1000 }, 1000 + fault);
                expectEquals ((int) m.live (0).size(), 1);
                expectEquals (m.live (0)[0].kind, kChannel);
                expectEquals (m.live (0)[0].startSample, (juce::int64) 1000);
                expectEquals (m.live (500)[0].startSample, (juce::int64) 1500);
            }

            beginTest ("Tier 3 fires at exactly the threshold, for an input with no signal only");
            {
                auto m = monitorOf (2);
                const auto never = frames (kNoSignalSeconds);
                m.update ({ -1, never - 1 }, never - 1);
                expect (m.live (0).empty());
                m.update ({ -1, never }, never);
                expectEquals ((int) m.live (0).size(), 1);
                expectEquals (m.live (0)[0].kind, kNoSignal);
                expectEquals (m.live (0)[0].strip, 0);
            }

            beginTest ("Tier 2 needs eight channels: seven stay individual");
            {
                auto seven = monitorOf (7);
                seven.update (std::vector<juce::int64> (7, 0), fault);
                expectEquals ((int) ofKind (seven.live (0), kChannel).size(), 7);
                expect (ofKind (seven.live (0), kStream).empty());

                auto eight = monitorOf (8);
                eight.update (std::vector<juce::int64> (8, 0), fault);
                expectEquals ((int) eight.live (0).size(), 1);
                expectEquals (eight.live (0)[0].kind, kStream);
                expectEquals (eight.live (0)[0].channels, 8);
            }

            beginTest ("Tier 2's window is two seconds, inclusive");
            {
                const auto window = frames (kStreamWindowSeconds);
                std::vector<juce::int64> ends (8, 1000);
                ends[7] = 1000 + window;
                auto inside = monitorOf (8);
                inside.update (ends, 1000 + window + fault);
                expectEquals ((int) ofKind (inside.live (0), kStream).size(), 1, "exactly two seconds is inside");

                ends[7] = 1000 + window + 1;
                auto outside = monitorOf (8);
                outside.update (ends, 1000 + window + 1 + fault);
                expect (ofKind (outside.live (0), kStream).empty(), "one frame past the window is outside");
                expectEquals ((int) ofKind (outside.live (0), kChannel).size(), 8);
            }

            beginTest ("A stream that shrinks below eight hands back to its channels");
            {
                auto m = monitorOf (10);
                std::vector<juce::int64> ends (10, 0);
                m.update (ends, fault);
                expectEquals ((int) ofKind (m.live (0), kStream).size(), 1);
                expectEquals (m.live (0)[0].channels, 10);

                const auto later = fault + 100;
                for (int i = 0; i < 4; ++i) ends[(size_t) i] = later;
                m.update (ends, later);
                const auto now = m.live (0);
                expect (ofKind (now, kStream).empty());
                expectEquals ((int) ofKind (now, kChannel).size(), 6);

                const auto all = m.history (0);
                const auto streams = ofKind (all, kStream);
                expectEquals ((int) streams.size(), 1);
                expectEquals (streams[0].endSample, later);
                expect (streams[0].recovered);
            }

            beginTest ("Stereo strips inside a stream stay inside its one alarm");
            {
                Monitor m;
                std::vector<Monitor::Input> inputs;
                for (int strip = 0; strip < 4; ++strip)
                    for (int side = 0; side < 2; ++side)
                        inputs.push_back ({ strip * 2, "Pair " + juce::String (strip + 1) });
                m.begin (std::move (inputs), kRate);
                m.update (std::vector<juce::int64> (8, 0), fault);
                expectEquals ((int) m.live (0).size(), 1);
                expectEquals (m.live (0)[0].kind, kStream);
                expectEquals (m.live (0)[0].channels, 8);
            }

            beginTest ("The take's end closes whatever is live, unrecovered");
            {
                auto m = monitorOf (1);
                m.update ({ 0 }, fault);
                m.finish (fault + 50);
                expect (m.live (0).empty());
                expectEquals ((int) m.history (0).size(), 1);
                expectEquals (m.history (0)[0].endSample, fault + 50);
                expect (! m.history (0)[0].recovered);
            }

            beginTest ("The banner ranks a stream first and keeps long lists short");
            {
                std::vector<Alarm> alarms;
                for (int i = 0; i < 6; ++i)
                {
                    Alarm a; a.kind = kChannel; a.name = "C" + juce::String (i);
                    alarms.push_back (a);
                }
                Alarm notice; notice.kind = kNoSignal; notice.name = "Guest";
                alarms.push_back (notice);
                Alarm stream; stream.kind = kStream; stream.channels = 16; stream.name = "S1, S2";
                alarms.push_back (stream);

                const auto text = bannerText (alarms);
                expect (text.startsWith ("!! INPUT STREAM LOST -- 16 channels"), text);
                expect (text.contains ("C0, C1, C2, C3 +2 more"), text);
                expect (text.contains ("No signal yet this take: Guest"), text);
                expect (bannerText ({}).isEmpty());
            }
        }

        // ── Through the callback ──────────────────────────────────────────
        void tiersThroughTheCallback()
        {
            beginTest ("Exact zero for three seconds after signal raises a channel fault, then recovers");
            {
                Rig rig (1);
                rig.engine.getRecorder().getTrack (0).setNameThreadSafe ("Kick In");
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0) - 1);
                expect (rig.live.empty(), "fired a frame early");
                rig.run (1);
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty())
                {
                    expectEquals (rig.live[0].kind, channelalarm::kChannel);
                    expectEquals (rig.live[0].name, juce::String ("Kick In"));
                    expectEquals (rig.live[0].startSample, frames (1.0));
                    expectEquals (rig.live[0].endSample, (juce::int64) -1);
                }
                rig.run (frames (2.0));
                expectEquals ((int) rig.live.size(), 1, "the alarm did not hold through the fault");

                rig.level (0.3f);
                rig.run (kBlock);
                expect (rig.live.empty(), "recovery did not clear the live alarm");
                rig.level (0.0f);
                rig.run (kBlock);
                rig.engine.stopRecording();

                const auto alarms = latchedIn (rig.dir);
                expectEquals ((int) alarms.size(), 1, "recovery lost the latched alarm");
                if (alarms.size() == 1)
                {
                    const auto& a = alarms[0];
                    expectEquals (a.kind, channelalarm::kChannel);
                    expectEquals (a.name, juce::String ("Kick In"));
                    expectEquals (a.startSample, frames (1.0));
                    expectEquals (a.endSample, frames (6.0) + kBlock);
                    expect (a.recovered);
                }
            }

            beginTest ("A quiet input with a noise floor never alarms");
            {
                Rig rig (1);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (kBlock);
                rig.level (1.0e-7f);   // below any meter, above exact zero
                rig.run (frames (65.0));
                expect (rig.live.empty(), "quiet is not a fault");
                rig.engine.stopRecording();
            }

            beginTest ("An armed input never carrying signal is a quiet notice at sixty seconds");
            {
                Rig rig (1);
                rig.engine.getRecorder().getTrack (0).setNameThreadSafe ("Guest Vox");
                expect (rig.engine.startRecording (rig.dir));
                rig.run (frames (60.0) - 1);
                expect (rig.live.empty(), "fired a frame early");
                rig.run (1);
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty())
                {
                    expectEquals (rig.live[0].kind, channelalarm::kNoSignal);
                    expectEquals (rig.live[0].name, juce::String ("Guest Vox"));
                    expect (! channelalarm::bannerText (rig.live).contains ("!"), "a notice must stay quiet");
                }

                // The guest walks on. Their channel then behaves like any other.
                rig.level (0.2f);
                rig.run (kBlock);
                expect (rig.live.empty());
                rig.level (0.0f);
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty()) expectEquals (rig.live[0].kind, channelalarm::kChannel);
                rig.engine.stopRecording();

                const auto alarms = latchedIn (rig.dir);
                expectEquals ((int) alarms.size(), 2);
                if (alarms.size() == 2)
                {
                    expectEquals (alarms[0].kind, channelalarm::kNoSignal);
                    expect (alarms[0].recovered);
                    expectEquals (alarms[1].kind, channelalarm::kChannel);
                    expect (! alarms[1].recovered, "still dead when the take ended");
                }
            }

            beginTest ("A muted strip is still watched: recording is pre-fader");
            {
                Rig rig (1);
                rig.engine.getRecorder().getTrack (0).muted.store (true);
                rig.engine.getRecorder().getTrack (0).gainDb.store (-60.0f);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (kBlock);
                rig.level (0.0f);
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1);
                rig.engine.stopRecording();
            }

            beginTest ("An unarmed input raises nothing");
            {
                Rig rig (2);
                rig.engine.getRecorder().getTrack (1).armed.store (false);
                expect (rig.engine.startRecording (rig.dir));
                rig.levels[0] = 0.3f;
                rig.run (frames (61.0));
                expect (rig.live.empty());
                rig.engine.stopRecording();
            }

            beginTest ("Positions sit on the take's timeline, pre-roll included");
            {
                Rig rig (1);
                rig.engine.getRecorder().setPreRollSeconds (1);
                rig.level (0.3f);
                rig.run (frames (2.0));   // fills the history; nothing is being captured
                expect (rig.engine.startRecording (rig.dir));
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty())
                    expectEquals (rig.live[0].startSample, frames (2.0), "one second of pre-roll, one of take");
                rig.engine.stopRecording();
            }
        }

        void streamFaults()
        {
            beginTest ("Eight channels dropping together are one loud stream alarm");
            {
                Rig rig (10);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                for (int ch = 0; ch < 8; ++ch) rig.levels[(size_t) ch] = 0.0f;
                rig.run (frames (3.0) - 1);
                expect (rig.live.empty());
                rig.run (1);
                expectEquals ((int) rig.live.size(), 1, "N small alarms instead of one");
                if (! rig.live.empty())
                {
                    expectEquals (rig.live[0].kind, channelalarm::kStream);
                    expectEquals (rig.live[0].channels, 8);
                    expect (rig.live[0].name.contains ("Strip 1"));
                    expect (channelalarm::bannerText (rig.live).startsWith ("!! INPUT STREAM LOST"));
                }
                rig.level (0.3f);
                rig.run (kBlock);
                expect (rig.live.empty(), "the stream did not recover");
                rig.engine.stopRecording();

                const auto latched = latchedIn (rig.dir);
                expectEquals ((int) ofKind (latched, channelalarm::kStream).size(), 1);
                expectEquals ((int) ofKind (latched, channelalarm::kChannel).size(), 8,
                              "the report must still name each channel");
            }

            beginTest ("Seven channels dropping together stay seven channel faults");
            {
                Rig rig (10);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                for (int ch = 0; ch < 7; ++ch) rig.levels[(size_t) ch] = 0.0f;
                rig.run (frames (3.0));
                expectEquals ((int) ofKind (rig.live, channelalarm::kChannel).size(), 7);
                expect (ofKind (rig.live, channelalarm::kStream).empty());
                rig.engine.stopRecording();
            }

            beginTest ("Channels dying more than two seconds apart are not one stream");
            {
                Rig rig (8);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                for (int ch = 0; ch < 7; ++ch) rig.levels[(size_t) ch] = 0.0f;
                rig.run (frames (2.0) + 1);
                rig.levels[7] = 0.0f;
                rig.run (frames (3.0));
                expectEquals ((int) ofKind (rig.live, channelalarm::kChannel).size(), 8);
                expect (ofKind (rig.live, channelalarm::kStream).empty());
                rig.engine.stopRecording();
            }
        }

        void stereoPairs()
        {
            const auto pairRig = [] (Rig& rig)
            {
                auto& left = rig.engine.getRecorder().getTrack (0);
                left.isStereo.store (true);
                left.setNameThreadSafe ("Overheads");
                rig.engine.getRecorder().getTrack (1).setNameThreadSafe ("Overheads R");
            };

            beginTest ("A stereo pair losing both sides raises one alarm under the strip's name");
            {
                Rig rig (2);
                pairRig (rig);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty())
                {
                    expectEquals (rig.live[0].name, juce::String ("Overheads"));
                    expectEquals (rig.live[0].strip, 0);
                }
                rig.engine.stopRecording();
            }

            beginTest ("One dead side of a pair alarms the strip; it clears when that side returns");
            {
                Rig rig (2);
                pairRig (rig);
                rig.engine.getRecorder().getTrack (1).armed.store (false);   // R follows L's arm
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.levels[1] = 0.0f;
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1, "the right half is not watched");
                if (! rig.live.empty()) expectEquals (rig.live[0].name, juce::String ("Overheads"));
                rig.levels[1] = 0.3f;
                rig.run (kBlock);
                expect (rig.live.empty());
                rig.engine.stopRecording();

                expectEquals ((int) latchedIn (rig.dir).size(), 1);
            }

            beginTest ("Four stereo pairs are eight channels of a stream");
            {
                Rig rig (8);
                for (int i = 0; i < 8; i += 2)
                    rig.engine.getRecorder().getTrack (i).isStereo.store (true);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0));
                expectEquals ((int) rig.live.size(), 1);
                if (! rig.live.empty())
                {
                    expectEquals (rig.live[0].kind, channelalarm::kStream);
                    expectEquals (rig.live[0].channels, 8);
                }
                rig.engine.stopRecording();
            }
        }

        void takeIsUntouched()
        {
            beginTest ("An alarm neither stops the take nor changes a sample of it");
            {
                Rig rig (2);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.levels[0] = 0.0f;
                rig.run (frames (4.0));
                expectEquals ((int) rig.live.size(), 1);
                expect (rig.engine.isRecording(), "the alarm stopped the take");
                rig.levels[0] = 0.3f;
                rig.run (frames (1.0));
                rig.engine.stopRecording();

                const auto audio = rig.dir.getChildFile ("Audio Files");
                juce::AudioFormatManager fm; fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (audio.getChildFile ("Track_01.wav")));
                expect (reader != nullptr);
                if (reader != nullptr)
                {
                    expectEquals (reader->lengthInSamples, frames (6.0), "frames were lost or added");
                    juce::AudioBuffer<float> take (1, (int) reader->lengthInSamples);
                    reader->read (&take, 0, take.getNumSamples(), 0, true, false);
                    expectWithinAbsoluteError (take.getSample (0, (int) frames (0.5)), 0.3f, 0.001f);
                    expectEquals (take.getSample (0, (int) frames (2.0)), 0.0f);
                    expectWithinAbsoluteError (take.getSample (0, (int) frames (5.5)), 0.3f, 0.001f);
                }
                const auto report = waitForReport (rig.dir);
                expect (! (bool) report.getProperty ("primaryFailed", true));
                expectEquals ((int) report.getProperty ("missedSamples", -1), 0);
            }
        }

        void reportedAndCarried()
        {
            beginTest ("A clean continue keeps the earlier pass's alarms in the report");
            {
                Rig rig (1);
                rig.engine.getRecorder().getTrack (0).setNameThreadSafe ("Bass DI");
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0));
                rig.level (0.3f);
                rig.run (kBlock);
                rig.engine.stopRecording();
                waitForReport (rig.dir);

                rig.engine.getRecorder().armContinue (0);
                expect (rig.engine.startRecording (rig.dir));
                rig.run (kBlock);
                expect (rig.live.empty(), "a new pass must start with no live alarm");
                rig.engine.stopRecording();

                const auto alarms = latchedIn (rig.dir);
                expectEquals ((int) alarms.size(), 1, "the continue dropped the earlier alarm");
                if (alarms.size() == 1)
                    expectEquals (alarms[0].name, juce::String ("Bass DI"));
            }

            beginTest ("Alarms stop being live when the take stops");
            {
                Rig rig (1);
                expect (rig.engine.startRecording (rig.dir));
                rig.level (0.3f);
                rig.run (frames (1.0));
                rig.level (0.0f);
                rig.run (frames (3.0));
                expect (! rig.live.empty());
                rig.engine.stopRecording();
                expect (rig.engine.captureStatus().channelAlarms.empty());
            }
        }

        void daemon()
        {
            using namespace zynforge::capture;
            const auto waitUntil = [] (std::function<bool()> done, int timeoutMs)
            {
                for (int t = 0; t < timeoutMs && ! done(); t += 10) juce::Thread::sleep (10);
                return done();
            };

            beginTest ("The daemon's status carries the alarm to its client");
            {
                CaptureDaemon capture; capture.setTestModeNoDevice (true);
                bool listening = false;
                for (int port : { 49770, 49771, 49772 })
                    if (capture.start (port, 1)) { listening = true; break; }
                expect (listening);
                if (! listening) return;
                capture.prepareForTests (kRate, kBlock, 1);
                capture.getRecorder().getTrack (0).armed.store (true);
                capture.getRecorder().getTrack (0).setNameThreadSafe ("Snare");

                CaptureClient client;
                std::mutex lock;
                EngineStatus latest;
                client.onStatus = [&] (const EngineStatus& s) { const std::lock_guard<std::mutex> l (lock); latest = s; };
                expect (client.connect ("127.0.0.1", capture.getPort()));
                expect (client.hello (2000).ok);
                const auto dir = tempDir();
                Command start; start.action = Action::StartRecording; start.sessionDir = dir.getFullPathName();
                expect (client.request (start, 3000).ok);

                std::vector<float> loud ((size_t) kBlock, 0.3f), zero ((size_t) kBlock, 0.0f);
                const float* signal[] { loud.data() };
                const float* silence[] { zero.data() };
                capture.processTestBlock (signal, 1, kBlock);
                for (int b = 0; b < 30; ++b) capture.processTestBlock (silence, 1, kBlock);
                capture.getRecorder().drainPendingForTests();
                expect (waitUntil ([&]
                {
                    const std::lock_guard<std::mutex> l (lock);
                    return ! latest.channelAlarms.empty();
                }, 3000), "the alarm did not reach the client");
                {
                    const std::lock_guard<std::mutex> l (lock);
                    if (! latest.channelAlarms.empty())
                        expectEquals (latest.channelAlarms[0].name, juce::String ("Snare"));
                }
                Command stop; stop.action = Action::StopRecording;
                expect (client.request (stop, 5000).ok, "an alarm must not make the stop look unclean");
                client.disconnect();
                capture.stop();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }

            beginTest ("With no client attached the daemon still latches a fault that comes and goes");
            {
                CaptureDaemon capture; capture.setTestModeNoDevice (true);
                bool listening = false;
                for (int port : { 49773, 49774, 49775 })
                    if (capture.start (port, 1)) { listening = true; break; }
                expect (listening);
                if (! listening) return;
                capture.prepareForTests (kRate, kBlock, 1);
                capture.getRecorder().getTrack (0).armed.store (true);

                const auto dir = tempDir();
                {
                    CaptureClient client;
                    expect (client.connect ("127.0.0.1", capture.getPort()));
                    expect (client.hello (2000).ok);
                    Command start; start.action = Action::StartRecording; start.sessionDir = dir.getFullPathName();
                    expect (client.request (start, 3000).ok);
                    client.disconnect();
                }

                std::vector<float> loud ((size_t) kBlock, 0.3f), zero ((size_t) kBlock, 0.0f);
                const float* signal[] { loud.data() };
                const float* silence[] { zero.data() };
                capture.processTestBlock (signal, 1, kBlock);
                for (int b = 0; b < 30; ++b) capture.processTestBlock (silence, 1, kBlock);
                capture.getRecorder().drainPendingForTests();
                // A tick right after the disconnect can spend its write budget on
                // the dead socket, so wait for the loop rather than a fixed time.
                expect (waitUntil ([&] { return ! latchedByLoop (capture.getRecorder()).empty(); }, 10000),
                        "the status loop never saw the fault");
                capture.processTestBlock (signal, 1, kBlock);
                expect (waitUntil ([&]
                {
                    const auto alarms = latchedByLoop (capture.getRecorder());
                    return ! alarms.empty() && alarms[0].recovered;
                }, 10000), "the status loop never saw the recovery");
                capture.getRecorder().stopRecording();

                const auto alarms = latchedIn (dir);
                expectEquals ((int) alarms.size(), 1, "the fault was never seen");
                if (alarms.size() == 1)
                    expect (alarms[0].recovered, "the recovery was never seen");
                capture.stop();
                MultitrackRecorder::deleteSessionAfterCancellingReports (dir);
            }
        }

        void statusFields()
        {
            using namespace zynforge::capture;

            const auto sample = []
            {
                EngineStatus status;
                status.recording = true;
                Alarm stream;
                stream.kind = channelalarm::kStream; stream.name = "A, B"; stream.channels = 12;
                stream.startSample = 5000000000;
                status.channelAlarms.push_back (stream);
                return status;
            };

            beginTest ("Alarms round-trip through the wire encoding on protocol 4");
            {
                expectEquals (kProtocolVersion, 4, "the alarm is additive: no version bump");
                const auto line = frame (encodeStatus (sample()));
                expect (line.endsWith ("\n") && ! line.trimEnd().containsChar ('\n'), "not one line");
                const auto back = decodeStatus (juce::JSON::parse (line));
                expectEquals ((int) back.channelAlarms.size(), 1);
                if (! back.channelAlarms.empty())
                {
                    const auto& a = back.channelAlarms[0];
                    expectEquals (a.kind, channelalarm::kStream);
                    expectEquals (a.name, juce::String ("A, B"));
                    expectEquals (a.channels, 12);
                    expectEquals (a.startSample, (juce::int64) 5000000000);
                    expectEquals (a.endSample, (juce::int64) -1);
                }
            }

            beginTest ("A status from an older peer has no alarms and decodes as before");
            {
                auto json = encodeStatus (sample());
                json.getDynamicObject()->removeProperty ("channelAlarms");
                const auto back = decodeStatus (juce::JSON::parse (juce::JSON::toString (json)));
                expect (back.channelAlarms.empty());
                expect (back.recording);
            }

            beginTest ("An older peer's decoder is unaffected by the new field");
            {
                // Everything that peer reads is unchanged, and it reads by
                // name, so the extra array is simply never looked at.
                auto withAlarm = encodeStatus (sample()), without = encodeStatus (sample());
                without.getDynamicObject()->removeProperty ("channelAlarms");
                for (const auto& property : without.getDynamicObject()->getProperties())
                    expect (withAlarm.hasProperty (property.name), property.name.toString());
                auto status = sample();
                status.channelAlarms.clear();
                auto stripped = encodeStatus (status);
                stripped.getDynamicObject()->removeProperty ("channelAlarms");
                expectEquals (juce::JSON::toString (stripped), juce::JSON::toString (without));
            }
        }

        void statusSnapshot()
        {
            beginTest ("The status snapshot carries the alarm, on a daemon take as on a local one");
            {
                AudioEngine engine;
                engine.setStripCount (1);
                EngineStatus status;
                status.recording = true;
                Alarm alarm;
                alarm.kind = channelalarm::kChannel; alarm.name = "Kick In";
                status.channelAlarms.push_back (alarm);
                engine.setExternalCaptureStatus (status);
                engine.setExternalRecording (true);

                const auto live = engine.captureStatus();
                expectEquals ((int) live.channelAlarms.size(), 1, "the snapshot lost the alarm");
                const auto state = juce::JSON::toString (live.toJson());
                expect (state.contains ("channelAlarms"), "the status JSON has no alarm field");
                expect (state.contains ("Kick In"), "the status JSON lost the alarm");
                engine.setExternalRecording (false);
            }
        }
    };

    static ChannelAlarmTests channelAlarmTests;
}
