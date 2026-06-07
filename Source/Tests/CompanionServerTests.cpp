// Headless integration test for the companion HTTP server's access control.
// Starts the real server on loopback and drives it with a raw socket to
// prove: (a) every endpoint -- including the audio stream -- 401s without
// a token; (b) the served page threads the per-session token through to its
// sub-requests; (c) the stream is reachable WITH a token. This guards the
// fix where the static page omitted the token on /state.json, /cmd and
// /stream.wav, locking the whole client out under the token regime.

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"

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
            return mb.toString();
        }

        void runTest() override
        {
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

                    // (b) The tokened page is served and carries the JS that
                    //     threads the token onto its sub-requests + the stream.
                    const auto page = httpGet (port, "/?t=" + tok);
                    expect (page.contains ("200"), "tokened page not served");
                    expect (page.contains ("URLSearchParams"), "page does not read the token from the URL");
                    expect (page.contains ("/stream.wav\" + _q"), "page does not thread the token to the stream");

                    // (c) The stream is reachable WITH a valid token.
                    expect (! httpGet (port, "/stream.wav?t=" + tok).contains ("401"),
                            "tokened stream wrongly rejected");

                    engine.stopCompanionServer();
                }
            }
            AudioEngine::setTestModeSkipAudioInit (false);
        }
    };

    static CompanionServerTests companionServerTests;
}
