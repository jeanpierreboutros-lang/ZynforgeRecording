// Headless integration test for the companion HTTP server's access control.
// Starts the real server on loopback and drives it with a raw socket to
// prove: (a) every endpoint -- including the audio stream -- 401s without
// a token; (b) the served page threads the per-session token through to its
// sub-requests; (c) the stream is reachable WITH a token. This guards the
// fix where the static page omitted the token on /state.json, /cmd and
// /stream.wav, locking the whole client out under the token regime.

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "../Network/CompanionStreamFormat.h"

#include <atomic>
#include <thread>

namespace zynforge
{
    class CompanionServerTests final : public juce::UnitTest
    {
    public:
        CompanionServerTests() : UnitTest ("Companion server", "zynforge") {}

        // Issue a raw GET on loopback and return what we can read within a
        // short window (status line + a little body). For the streaming
        // endpoint we stop after a small amount so we don't read forever.
        static juce::String httpGet (int port, const juce::String& pathAndQuery, int readMs = 500)
        {
            juce::StreamingSocket sock;
            if (! sock.connect ("127.0.0.1", port, 1000)) return {};
            const auto req = "GET " + pathAndQuery + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                             "Connection: close\r\n\r\n";
            sock.write (req.toRawUTF8(), (int) req.getNumBytesAsUTF8());

            juce::MemoryBlock mb;
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) readMs;
            char buf[2048];
            while (juce::Time::getMillisecondCounter() < deadline)
            {
                const int ready = sock.waitUntilReady (true, 100);
                if (ready < 0) break;
                if (ready == 1)
                {
                    const int n = sock.read (buf, sizeof (buf), false);
                    if (n <= 0) break;                  // peer closed
                    mb.append (buf, (size_t) n);
                    // Enough to capture a full ~8 KB HTML page (so the
                    // token-threading JS near its end is visible); the
                    // streaming endpoint is time-bounded by `deadline`.
                    if (mb.getSize() > 32768) break;
                }
            }

            // A successful /stream.wav response contains arbitrary binary
            // bytes after its ASCII HTTP header.  MemoryBlock::toString()
            // treats the whole response as UTF-8 and asserts when those bytes
            // are not a valid sequence.  Project the response to ASCII for
            // the textual assertions below while preserving RIFF/WAVE tags.
            juce::MemoryBlock ascii (mb);
            auto* bytes = static_cast<juce::uint8*> (ascii.getData());
            for (size_t i = 0; i < ascii.getSize(); ++i)
                if (bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t'
                    && (bytes[i] < 0x20u || bytes[i] > 0x7eu))
                    bytes[i] = (juce::uint8) ' ';
            return ascii.toString();
        }

        // POST a JSON body and return the response (status + body).
        static juce::String httpPost (int port, const juce::String& path, const juce::String& json)
        {
            juce::StreamingSocket sock;
            if (! sock.connect ("127.0.0.1", port, 1000)) return {};
            const auto body = json;
            const auto req = "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                             "Content-Type: application/json\r\nContent-Length: "
                             + juce::String (body.getNumBytesAsUTF8()) + "\r\nConnection: close\r\n\r\n" + body;
            sock.write (req.toRawUTF8(), (int) req.getNumBytesAsUTF8());

            juce::MemoryBlock mb;
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) 600;
            char buf[2048];
            while (juce::Time::getMillisecondCounter() < deadline)
            {
                const int ready = sock.waitUntilReady (true, 100);
                if (ready < 0) break;
                if (ready == 1)
                {
                    const int n = sock.read (buf, sizeof (buf), false);
                    if (n <= 0) break;
                    mb.append (buf, (size_t) n);
                    if (mb.getSize() > 4096) break;
                }
            }
            return mb.toString();
        }

        static void sendInvalidUtf8Request (int port)
        {
            juce::StreamingSocket sock;
            if (! sock.connect ("127.0.0.1", port, 1000)) return;
            const char request[] = {
                'P','O','S','T',' ','/','c','m','d',' ','H','T','T','P','/','1','.','1','\r','\n',
                'H','o','s','t',':',' ','1','2','7','.','0','.','0','.','1','\r','\n',
                'C','o','n','t','e','n','t','-','L','e','n','g','t','h',':',' ','1','\r','\n','\r','\n',
                (char) 0xff
            };
            sock.write (request, (int) sizeof (request));
        }

        void runTest() override
        {
            beginTest ("Live WAV placeholder sizes never overflow RIFF's 32-bit fields");
            {
                const auto bytes48k = companionstream::placeholderDataBytes (48000, 2, 16);
                const auto bytes384k = companionstream::placeholderDataBytes (384000, 2, 16);
                const auto maxField = std::numeric_limits<juce::uint32>::max();
                expect (bytes48k <= maxField - 36u);
                expect (bytes384k <= maxField - 36u);
                expectEquals ((int) (bytes48k % 4u), 0);
                expectEquals ((int) (bytes384k % 4u), 0);
                expectEquals ((juce::int64) companionstream::placeholderDataBytes (1, 1, 8, 1),
                              (juce::int64) 3600);
            }

            beginTest ("Every endpoint -- including /stream.wav -- is token-gated; page threads the token");

            AudioEngine::setTestModeSkipAudioInit (true);
            {
                AudioEngine engine;

                int port = 0;
                for (int p = 19234; p < 19244 && port == 0; ++p)
                    if (engine.startCompanionServer (p)) port = p;

                expect (port != 0, "companion server failed to start on loopback");
                if (port != 0)
                {
                    const auto url = engine.getCompanionAccessUrl();      // http://127.0.0.1:port/?t=<tok>
                    const auto tok = url.fromFirstOccurrenceOf ("t=", false, false).trim();
                    expect (tok.isNotEmpty(), "no access token minted");

                    // (a) No token -> 401 on the stream and the state poll.
                    expect (httpGet (port, "/stream.wav").contains ("401"), "tokenless stream not rejected");
                    expect (httpGet (port, "/state.json").contains ("401"), "tokenless state not rejected");
                    expect (httpGet (port, "/confidence").contains ("401"), "tokenless confidence page not rejected");

                    // (b) The tokened page is served and carries the JS that
                    //     threads the token onto its sub-requests + the stream.
                    const auto page = httpGet (port, "/?t=" + tok);
                    expect (page.contains ("200"), "tokened page not served");
                    expect (page.contains ("URLSearchParams"), "page does not read the token from the URL");
                    expect (page.contains ("/stream.wav\" + _q"), "page does not thread the token to the stream");
                    expect (page.contains ("/confidence\" + _q"), "page does not link to tokened confidence monitor");
                    expect (page.contains ("result.error") && page.contains ("commandNotice"),
                            "remote STOP/refusal reason is hidden by the companion page");

                    const auto confidence = httpGet (port, "/confidence?t=" + tok);
                    expect (confidence.contains ("200"), "tokened confidence page not served");
                    expect (confidence.contains ("READ ONLY"), "confidence page does not identify itself as read-only");
                    expect (confidence.contains ("/state.json?t="), "confidence page does not use the status feed");
                    expect (! confidence.contains ("/cmd"), "confidence page must not expose command controls");

                    // (c) The stream is reachable WITH a valid token.
                    expect (! httpGet (port, "/stream.wav?t=" + tok).contains ("401"),
                            "tokened stream wrongly rejected");

                    engine.stopCompanionServer();
                }
            }

            beginTest ("Malformed unauthenticated UTF-8 is rejected without killing the server");
            {
                AudioEngine engine;
                int port = 0;
                for (int p = 19244; p < 19250 && port == 0; ++p)
                    if (engine.startCompanionServer (p)) port = p;
                expect (port != 0, "companion server failed to start");
                if (port != 0)
                {
                    sendInvalidUtf8Request (port);
                    juce::Thread::sleep (100);
                    const auto tok = engine.getCompanionAccessUrl()
                                         .fromFirstOccurrenceOf ("t=", false, false).trim();
                    expect (httpGet (port, "/state.json?t=" + tok).contains ("200"),
                            "server stopped responding after malformed input");
                    engine.stopCompanionServer();
                }
            }

            beginTest ("Every endpoint is wired to the engine (state reflects it, /cmd mutates it, /stream serves audio)");
            {
                AudioEngine engine;
                engine.setStripCount (4);
                engine.setTrackName (0, "KickTest");
                engine.getRecorder().getTrack (1).armed.store (true);

                int port = 0;
                for (int p = 19250; p < 19260 && port == 0; ++p)
                    if (engine.startCompanionServer (p)) port = p;
                expect (port != 0, "companion server failed to start");

                if (port != 0)
                {
                    const auto tok = engine.getCompanionAccessUrl()
                                         .fromFirstOccurrenceOf ("t=", false, false).trim();

                    // /state.json reflects real engine state (names, armed, counts).
                    const auto state = httpGet (port, "/state.json?t=" + tok);
                    expect (state.contains ("200"),        "state.json not served");
                    expect (state.contains ("KickTest"),   "state.json missing the track name -> not wired to engine");
                    expect (state.contains ("\"armed\":true") || state.contains ("\"armed\": true"),
                            "state.json doesn't reflect the armed track");
                    expect (state.contains ("\"recording\":false") || state.contains ("\"recording\": false"),
                            "state.json missing transport state");

                    EngineStatus daemon;
                    daemon.recording = true;
                    daemon.missedSamples = 17;
                    daemon.numTracks = 4;
                    daemon.tracks.resize (4);
                    engine.setExternalCaptureStatus (daemon);
                    engine.setExternalRecording (true);
                    engine.setTrackMuted (0, true);
                    engine.setTrackSoloed (0, true);
                    const auto daemonState = httpGet (port, "/state.json?t=" + tok);
                    expect (daemonState.contains ("\"source\":\"daemon\"")
                            || daemonState.contains ("\"source\": \"daemon\""),
                            "daemon status was not selected for remote confidence");
                    expect (daemonState.contains ("\"missedSamples\":17")
                            || daemonState.contains ("\"missedSamples\": 17"),
                            "remote confidence did not carry daemon capture metrics");
                    expect (daemonState.contains ("KickTest"), "daemon status lost the GUI track name");
                    expect (daemonState.contains ("\"muted\":true") || daemonState.contains ("\"muted\": true"),
                            "daemon companion state hid the GUI mute");
                    expect (daemonState.contains ("\"soloed\":true") || daemonState.contains ("\"soloed\": true"),
                            "daemon companion state hid the GUI solo");
                    engine.setTrackMuted (0, false);
                    engine.setTrackSoloed (0, false);
                    const auto refusedArm = httpPost (port, "/cmd?t=" + tok,
                        "{\"action\":\"arm\",\"channel\":1,\"value\":true}");
                    expect (refusedArm.contains ("409"), "refused arm was ACKed as success");
                    engine.setExternalCaptureStatus (daemon, juce::Time::currentTimeMillis() - 5000);
                    expectEquals (engine.captureStatus().source, juce::String ("daemon-unavailable"),
                                  "stale daemon status was presented as fresh");
                    engine.setExternalCaptureStatus (daemon);
                    engine.setExternalRecording (false);
                    engine.setExternalRecording (true);
                    const auto unavailable = httpGet (port, "/state.json?t=" + tok);
                    expect (unavailable.contains ("daemon-unavailable"),
                            "a new daemon take reused stale metrics from the prior take");
                    engine.setExternalRecording (false);

                    // /cmd actually mutates engine state (mute ch 1 on, then off).
                    expect (! engine.getRecorder().getTrack (0).muted.load(), "precondition: ch1 not muted");
                    httpPost (port, "/cmd?t=" + tok, "{\"action\":\"mute\",\"channel\":1,\"value\":true}");
                    expect (engine.getRecorder().getTrack (0).muted.load(), "/cmd mute did not reach the engine");
                    httpPost (port, "/cmd?t=" + tok, "{\"action\":\"mute\",\"channel\":1,\"value\":false}");
                    expect (! engine.getRecorder().getTrack (0).muted.load(), "/cmd unmute did not reach the engine");
                    const auto invalidMute = httpPost (port, "/cmd?t=" + tok,
                        "{\"action\":\"mute\",\"channel\":999,\"value\":true}");
                    expect (invalidMute.contains ("409"), "invalid channel was ACKed as success");

                    // /cmd arm on a different channel.
                    httpPost (port, "/cmd?t=" + tok, "{\"action\":\"arm\",\"channel\":3,\"value\":true}");
                    expect (engine.getRecorder().getTrack (2).armed.load(), "/cmd arm did not reach the engine");

                    // A tokenless /cmd must NOT mutate.
                    httpPost (port, "/cmd", "{\"action\":\"mute\",\"channel\":3,\"value\":true}");
                    expect (! engine.getRecorder().getTrack (2).muted.load(), "tokenless /cmd wrongly mutated state");

                    // /stream.wav serves a WAV stream (RIFF/RF64 header in the body).
                    const auto stream = httpGet (port, "/stream.wav?t=" + tok, 700);
                    expect (stream.contains ("200"), "stream not served");
                    expect (stream.contains ("audio/wav") || stream.contains ("RIFF") || stream.contains ("WAVE"),
                            "stream response carries no WAV payload");

                    engine.stopCompanionServer();
                }
            }

            beginTest ("Stopping the server cancels a transport command queued on the message thread");
            {
                AudioEngine engine;
                int port = 0;
                for (int p = 19270; p < 19280 && port == 0; ++p)
                    if (engine.startCompanionServer (p)) port = p;
                expect (port != 0, "companion server failed to start");

                if (port != 0)
                {
                    const auto tok = engine.getCompanionAccessUrl()
                                         .fromFirstOccurrenceOf ("t=", false, false).trim();
                    std::atomic<bool> requestStarted { false };
                    juce::String response;
                    std::thread requester ([&]
                    {
                        requestStarted.store (true, std::memory_order_release);
                        response = httpPost (port, "/cmd?t=" + tok,
                                             "{\"action\":\"play\"}");
                    });

                    while (! requestStarted.load (std::memory_order_acquire))
                        juce::Thread::yield();
                    juce::Thread::sleep (150); // allow acceptLoop to queue the command

                    const auto started = juce::Time::getMillisecondCounterHiRes();
                    engine.stopCompanionServer();
                    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - started;
                    requester.join();

                    expectLessThan (elapsedMs, 1500.0,
                                    "stop waited for the five-second transport timeout");
                    expect (response.contains ("409") || response.isEmpty(),
                            "cancelled transport returned an unexpected response");
                }
            }
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static CompanionServerTests companionServerTests;
}
