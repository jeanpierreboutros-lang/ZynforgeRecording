#include "CompanionServer.h"
#include "../Theme/BrandColors.h"

#include <cstring>
#include <thread>

namespace zynforge
{
    namespace
    {
        // Read one HTTP request from the socket -- naive parser, enough
        // for browser-issued GET/POST. Returns empty string on error.
        juce::String readRequest (juce::StreamingSocket& s, juce::String& bodyOut)
        {
            juce::MemoryBlock head;
            char buf[1024];
            // 5 s overall deadline so a half-open client doesn't wedge us.
            const auto deadline = juce::Time::getMillisecondCounter() + 5000;
            for (;;)
            {
                if (juce::Time::getMillisecondCounter() >= deadline) return {};
                const int ready = s.waitUntilReady (true, 200);
                if (ready < 0) return {};        // socket error / closed
                if (ready == 0) continue;        // timed out, loop until deadline

                const int got = s.read (buf, sizeof (buf), false);
                if (got <= 0) return {};
                head.append (buf, (std::size_t) got);

                const auto str = juce::String::fromUTF8 ((const char*) head.getData(),
                                                          (int) head.getSize());
                const int hdrEnd = str.indexOf ("\r\n\r\n");
                if (hdrEnd < 0) continue;

                const auto headers = str.substring (0, hdrEnd);
                int contentLength = 0;
                for (auto& line : juce::StringArray::fromLines (headers))
                    if (line.startsWithIgnoreCase ("Content-Length:"))
                        contentLength = line.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();

                int bodyHave = (int) head.getSize() - (hdrEnd + 4);
                bodyOut = str.substring (hdrEnd + 4);
                while (bodyHave < contentLength
                       && juce::Time::getMillisecondCounter() < deadline)
                {
                    const int r2 = s.waitUntilReady (true, 200);
                    if (r2 < 0) break;
                    if (r2 == 0) continue;
                    const int got2 = s.read (buf, sizeof (buf), false);
                    if (got2 <= 0) break;
                    bodyOut += juce::String::fromUTF8 (buf, got2);
                    bodyHave += got2;
                }
                return headers;
            }
        }

        const char* kHtmlPage = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no"/>
<title>ZynForge Recording</title>
<style>
:root { --bg:#0d0d12; --panel:#1e1e1e; --text:#fff; --muted:#7a8a9a;
        --record:#ff3b3b; --play:#4ad878; --solo:#ffd64d; --accent:#ff7733; }
* { box-sizing:border-box; }
body { margin:0; padding:0; background:var(--bg); color:var(--text);
       font-family:-apple-system,system-ui,sans-serif; }
header { background:linear-gradient(180deg,#1a1a22,#0d0d12);
         padding:14px 18px; border-bottom:1px solid #2a2a32;
         display:flex; align-items:center; gap:16px; }
header .brand { color:var(--accent); font-weight:700; font-size:18px; letter-spacing:.05em; }
header .status { color:var(--muted); font-size:13px; }
.transport { padding:14px 18px; border-bottom:1px solid #26282e; display:flex; gap:12px; }
.transport button { padding:12px 18px; font-size:14px; font-weight:700; border:none;
                    border-radius:6px; background:linear-gradient(180deg,#2c2e34,#16181c);
                    color:var(--text); min-height:48px; min-width:80px; }
.transport button.rec  { color:var(--record); }
.transport button.play { color:var(--play); }
#strips { padding:12px; display:grid;
          grid-template-columns:repeat(auto-fill,minmax(140px,1fr)); gap:10px; }
.strip { background:linear-gradient(180deg,#1f1f26,#15151a);
         border:1px solid #2a2a32; border-radius:8px; padding:10px;
         display:flex; flex-direction:column; gap:8px; min-height:200px; }
.strip .name { font-weight:700; font-size:14px; }
.strip .swatch { width:100%; height:6px; border-radius:3px; }
.strip .btns { display:grid; grid-template-columns:1fr 1fr 1fr; gap:6px; }
.strip .btns button { padding:8px 0; border:none; border-radius:4px;
                      background:#26262c; color:var(--muted); font-weight:700;
                      font-size:11px; min-height:36px; }
.strip .btns button.on.rec  { background:var(--record); color:#fff; }
.strip .btns button.on.mute { background:var(--record); color:#fff; }
.strip .btns button.on.solo { background:var(--solo);  color:#000; }
.strip .meter { height:60px; background:#0d0d12; border-radius:4px;
                position:relative; overflow:hidden; }
.strip .meter .bar { position:absolute; bottom:0; left:0; right:0;
                     background:linear-gradient(0deg,var(--play),var(--solo),var(--record));
                     transition:height 60ms linear; }
.audition { margin:12px 18px; padding:10px; background:var(--panel);
            border-radius:6px; }
.audition audio { width:100%; }
</style></head>
<body>
<header><span class="brand">ZYNFORGE  RECORDING</span>
        <span class="status" id="status">connecting...</span></header>
<div class="transport">
  <button id="playBtn" class="play">▶ PLAY</button>
  <button id="stopBtn">■ STOP</button>
  <button id="recBtn" class="rec">● RECORD</button>
</div>
<div class="audition">
  <strong style="color:var(--accent)">REMOTE AUDITION</strong><br/>
  <audio controls preload="none">
    <source src="/stream.wav" type="audio/wav">
  </audio>
</div>
<div id="strips"></div>
<script>
let lastTag = "";
// The page is opened as /?t=<token>; every sub-request (state poll,
// command POST, and the audio stream) must carry that same access token
// or the server 401s it. The <audio> element can't send an Authorization
// header, so the token rides in the query string for all of them.
const _t = new URLSearchParams(location.search).get("t") || "";
const _q = _t ? ("?t=" + encodeURIComponent(_t)) : "";
// Point the remote-audition stream at the tokened URL (preload="none"
// means nothing is fetched until the engineer hits play, by which time
// this src is set).
(function () {
    const src = document.querySelector(".audition source");
    if (src) { src.src = "/stream.wav" + _q;
               const a = src.parentElement; if (a && a.load) a.load(); }
})();
async function cmd(action, channel, value) {
    try {
        await fetch("/cmd" + _q, { method:"POST", headers:{"Content-Type":"application/json"},
                              body: JSON.stringify({action, channel, value}) });
    } catch (e) { document.getElementById("status").textContent = "send failed"; }
}
document.getElementById("playBtn").onclick = () => cmd("play");
document.getElementById("stopBtn").onclick = () => cmd("stop");
document.getElementById("recBtn") .onclick = () => cmd("record");
function renderStrips(state) {
    const el = document.getElementById("strips");
    const tag = state.tracks.map(t => t.name+"|"+t.colour).join(",");
    if (tag !== lastTag) {
        el.innerHTML = "";
        for (let i = 0; i < state.tracks.length; ++i) {
            const t = state.tracks[i];
            const card = document.createElement("div");
            card.className = "strip";
            card.innerHTML = `
                <div class="swatch" style="background:${t.colour}"></div>
                <div class="name">${t.name}</div>
                <div class="btns">
                  <button class="rec  ${t.armed?"on":""}"  data-i="${i}" data-a="arm" >REC</button>
                  <button class="mute ${t.muted?"on":""}"  data-i="${i}" data-a="mute">MUTE</button>
                  <button class="solo ${t.soloed?"on":""}" data-i="${i}" data-a="solo">SOLO</button>
                </div>
                <div class="meter"><div class="bar" id="mtr${i}"></div></div>`;
            el.appendChild(card);
        }
        el.querySelectorAll("button[data-a]").forEach(b => {
            b.onclick = () => cmd(b.dataset.a, parseInt(b.dataset.i)+1, !b.classList.contains("on"));
        });
        lastTag = tag;
    }
    // Live meter + button toggle state
    for (let i = 0; i < state.tracks.length; ++i) {
        const t = state.tracks[i];
        const m = document.getElementById("mtr"+i);
        if (m) m.style.height = (Math.min(1, t.peak) * 100) + "%";
        const card = el.children[i];
        if (!card) continue;
        const btns = card.querySelectorAll("button[data-a]");
        if (btns[0]) btns[0].classList.toggle("on", t.armed);
        if (btns[1]) btns[1].classList.toggle("on", t.muted);
        if (btns[2]) btns[2].classList.toggle("on", t.soloed);
    }
    document.getElementById("status").textContent =
        (state.recording?"REC ":"") + (state.playing?"PLAY ":"") +
        state.tracks.length + " channels";
}
async function tick() {
    try {
        const r = await fetch("/state.json" + _q, { cache:"no-store" });
        renderStrips(await r.json());
    } catch (e) { document.getElementById("status").textContent = "disconnected"; }
}
setInterval(tick, 500); tick();
</script></body></html>)HTML";
    }

    CompanionServer::CompanionServer (AudioEngine& eng) : engine (eng)
    {
        streamRing.allocate (48000 * 4);   // 1 s of stereo @ 48k as a basic floor
    }

    CompanionServer::~CompanionServer() { stop(); }

    bool CompanionServer::start (int port, const juce::String& bindAddress)
    {
        if (running.load()) return true;
        bind = bindAddress.isEmpty() ? juce::String ("127.0.0.1") : bindAddress;

        // Generate a fresh 32-hex-char access token each start. The
        // token is the only thing keeping someone else on the same
        // Wi-Fi from arming tracks when bound to 0.0.0.0 -- mint a
        // new one per server lifetime so an old token from a prior
        // session doesn't grant access to a fresh one.
        {
            std::lock_guard<std::mutex> g (tokenLock);
            juce::Random rng (juce::Time::currentTimeMillis());
            accessToken.clear();
            for (int i = 0; i < 32; ++i)
            {
                const int v = rng.nextInt (16);
                accessToken += juce::String::toHexString (v);
            }
        }

        listener = std::make_unique<juce::StreamingSocket>();
        if (! listener->createListener (port, bind))
        {
            // Surface WHY the bind failed (port busy, etc.) -- DBG() is compiled
            // out in Release, so use the always-on logger.
            juce::Logger::writeToLog ("[companion] createListener FAILED port=" + juce::String (port)
                                      + " bind=" + bind + " errno=" + juce::String (errno)
                                      + " (" + juce::String (std::strerror (errno)) + ")");
            listener.reset();
            return false;
        }
        listenPort.store (port);
        running.store (true);
        acceptThread = std::thread ([this] { acceptLoop(); });
        return true;
    }

    void CompanionServer::stop()
    {
        if (! running.exchange (false)) return;
        if (listener != nullptr) listener->close();
        if (acceptThread.joinable()) acceptThread.join();
        listener.reset();
        listenPort.store (-1);
        {
            std::lock_guard<std::mutex> g (tokenLock);
            accessToken.clear();
        }
    }

    juce::String CompanionServer::getAccessToken() const
    {
        std::lock_guard<std::mutex> g (tokenLock);
        return accessToken;
    }

    juce::String CompanionServer::getAccessUrl() const
    {
        if (! running.load()) return {};
        std::lock_guard<std::mutex> g (tokenLock);
        const auto host = (bind == "127.0.0.1" || bind == "::1")
                              ? juce::String ("localhost")
                              : juce::IPAddress::getLocalAddress().toString();
        return "http://" + host + ":" + juce::String (listenPort.load())
             + "/?t=" + accessToken;
    }

    bool CompanionServer::isExposedOnLan() const
    {
        return running.load() && bind != "127.0.0.1" && bind != "::1";
    }

    void CompanionServer::feedStreamSamples (const float* L, const float* R, int n) noexcept
    {
        if (! streamRing.active.load (std::memory_order_relaxed) || streamRing.capacity() == 0)
            return;
        const auto cap = streamRing.capacity();
        for (int i = 0; i < n; ++i)
        {
            const auto idx = (streamRing.writeIdx.load (std::memory_order_relaxed) + (std::size_t) i) % cap;
            streamRing.dataL[idx] = L != nullptr ? L[i] : 0.0f;
            streamRing.dataR[idx] = R != nullptr ? R[i] : 0.0f;
        }
        streamRing.writeIdx.fetch_add ((std::size_t) n, std::memory_order_release);
    }

    void CompanionServer::acceptLoop()
    {
        while (running.load() && listener != nullptr)
        {
            auto client = std::unique_ptr<juce::StreamingSocket> (listener->waitForNextConnection());
            if (client == nullptr) break;
            // Detach a thread per client. Connections are short -- clients
            // re-poll every 500 ms -- except the /stream.wav reader which
            // holds the socket open for the duration of audition.
            std::thread ([this, c = std::move (client)] () mutable
            {
                handleClient (std::move (c));
            }).detach();
        }
    }

    void CompanionServer::handleClient (std::unique_ptr<juce::StreamingSocket> client)
    {
        if (client == nullptr) return;
        juce::String body;
        const auto headers = readRequest (*client, body);
        if (headers.isEmpty()) return;

        // First line is the request line: METHOD PATH HTTP/1.1
        const auto firstLine = headers.upToFirstOccurrenceOf ("\r\n", false, false);
        const auto bits      = juce::StringArray::fromTokens (firstLine, " ", "");
        if (bits.size() < 2) return;
        const auto& method   = bits[0];
        const auto& fullPath = bits[1];
        const auto path      = fullPath.upToFirstOccurrenceOf ("?", false, false);
        const auto query     = fullPath.fromFirstOccurrenceOf ("?", false, false);

        // Token check on every endpoint. Accept either `?t=<token>`
        // in the query OR `Authorization: Bearer <token>` in the
        // headers (lets the engineer paste the URL into a browser
        // bookmark OR use a script-driven client with a header).
        // The 401 body is plain text so misconfigured clients have
        // a chance of surfacing what went wrong.
        const auto expected = getAccessToken();
        if (expected.isEmpty())
        {
            writeRaw (*client, "503 Service Unavailable", "text/plain",
                      "companion server starting");
            return;
        }
        juce::String provided;
        {
            auto pieces = juce::StringArray::fromTokens (query, "&", "");
            for (auto& p : pieces)
                if (p.startsWith ("t="))
                    provided = p.fromFirstOccurrenceOf ("=", false, false);
            const auto authLine = headers.fromFirstOccurrenceOf ("\nAuthorization:", false, true)
                                          .upToFirstOccurrenceOf ("\n", false, false)
                                          .trim();
            if (authLine.startsWithIgnoreCase ("Bearer "))
                provided = authLine.substring (7).trim();
        }
        if (provided != expected)
        {
            writeRaw (*client, "401 Unauthorized", "text/plain",
                      "missing or invalid access token (?t=<token>)");
            return;
        }

        if (method == "GET" && (path == "/" || path == "/index.html"))
            return writeHtmlPage (*client);
        if (method == "GET" && path == "/state.json")
            return writeStateJson (*client);
        if (method == "GET" && path == "/stream.wav")
            return writeStreamWav (*client);
        if (method == "POST" && path == "/cmd")
            return handleCommand (*client, body);

        writeRaw (*client, "404 Not Found", "text/plain", "not found");
    }

    void CompanionServer::writeHtmlPage (juce::StreamingSocket& s)
    {
        writeRaw (s, "200 OK", "text/html; charset=utf-8", juce::String::fromUTF8 (kHtmlPage));
    }

    void CompanionServer::writeStateJson (juce::StreamingSocket& s)
    {
        juce::DynamicObject::Ptr root (new juce::DynamicObject());
        root->setProperty ("recording", engine.isRecording());
        root->setProperty ("playing",   engine.getPlayer().isPlaying());
        const int n = engine.getRecorder().getNumTracks();
        juce::Array<juce::var> arr;
        for (int i = 0; i < n; ++i)
        {
            auto& t = engine.getRecorder().getTrack (i);
            juce::DynamicObject::Ptr td (new juce::DynamicObject());
            td->setProperty ("name",   t.name);
            td->setProperty ("armed",  t.armed .load());
            td->setProperty ("muted",  t.muted .load());
            td->setProperty ("soloed", t.soloed.load());
            td->setProperty ("peak",   t.peak  .load());
            const auto argb = t.colourARGB.load();
            const auto col  = argb != 0 ? juce::Colour ((juce::uint32) argb)
                                        : zynforge::brand::swatchGraphite;
            td->setProperty ("colour", "#" + col.toString().substring (2));
            arr.add (juce::var (td.get()));
        }
        root->setProperty ("tracks", juce::var (arr));
        writeRaw (s, "200 OK", "application/json", juce::JSON::toString (juce::var (root.get())));
    }

    void CompanionServer::handleCommand (juce::StreamingSocket& s, const juce::String& body)
    {
        const auto parsed = juce::JSON::parse (body);
        if (! parsed.isObject()) { writeRaw (s, "400 Bad Request", "text/plain", "json expected"); return; }
        const auto action = parsed.getProperty ("action", "").toString();
        const int channel = (int) parsed.getProperty ("channel", 0);
        const bool value  = (bool) parsed.getProperty ("value", true);

        auto setBoolOnTrack = [&] (auto setter)
        {
            const int idx = channel - 1;
            if (idx < 0 || idx >= engine.getRecorder().getNumTracks()) return;
            setter (engine.getRecorder().getTrack (idx), value);
        };

        if      (action == "play")   { if (engine.getPlayer().isPlaying()) engine.stopPlayback();  else engine.startPlayback(); }
        else if (action == "stop")   { engine.stopPlayback(); if (engine.isRecording()) engine.stopRecording(); engine.getPlayer().rewind(); }
        else if (action == "record") { if (engine.isRecording()) engine.stopRecording(); }
        else if (action == "mute")   setBoolOnTrack ([] (auto& t, bool v) { t.muted .store (v); });
        else if (action == "solo")   setBoolOnTrack ([] (auto& t, bool v) { t.soloed.store (v); });
        else if (action == "arm")    setBoolOnTrack ([] (auto& t, bool v) { t.armed .store (v); });

        writeRaw (s, "200 OK", "application/json", "{\"ok\":true}");
    }

    void CompanionServer::writeStreamWav (juce::StreamingSocket& s)
    {
        // Mark the ring as active so the audio thread starts pushing.
        streamRing.active.store (true);

        // Long-living HTTP response with a WAV header claiming ~24 h of
        // audio. Browsers / iOS Safari treat this as a streaming source
        // and start playback as bytes arrive.
        const int    sr           = 48000;
        const int    bitsPerSamp  = 16;
        const int    numChannels  = 2;
        const juce::uint32 fakeBytes = (juce::uint32) (sr * numChannels * (bitsPerSamp / 8) * 60 * 60 * 24);

        auto u32 = [] (juce::uint32 v) { return juce::String ((juce::int64) v); };
        juce::ignoreUnused (u32);

        juce::String head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: audio/wav\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n"
            "\r\n";
        s.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());

        // 44-byte WAV header (PCM 16-bit, stereo, 48 kHz).
        auto write_le32 = [&] (juce::uint32 v)
        {
            const juce::uint8 b[4] = { (juce::uint8)(v),       (juce::uint8)(v >> 8),
                                       (juce::uint8)(v >> 16), (juce::uint8)(v >> 24) };
            s.write (b, 4);
        };
        auto write_le16 = [&] (juce::uint16 v)
        {
            const juce::uint8 b[2] = { (juce::uint8)(v), (juce::uint8)(v >> 8) };
            s.write (b, 2);
        };
        s.write ("RIFF", 4); write_le32 (fakeBytes + 36);
        s.write ("WAVE", 4);
        s.write ("fmt ", 4); write_le32 (16);
        write_le16 (1);                                                       // PCM
        write_le16 ((juce::uint16) numChannels);
        write_le32 ((juce::uint32) sr);
        write_le32 ((juce::uint32) (sr * numChannels * (bitsPerSamp / 8)));   // byte rate
        write_le16 ((juce::uint16) (numChannels * (bitsPerSamp / 8)));        // block align
        write_le16 ((juce::uint16) bitsPerSamp);
        s.write ("data", 4); write_le32 (fakeBytes);

        // Drain the ring into the socket. Stop on socket error.
        std::size_t readIdx = streamRing.writeIdx.load (std::memory_order_acquire);
        std::vector<juce::int16> outChunk;
        while (running.load())
        {
            const auto wIdx = streamRing.writeIdx.load (std::memory_order_acquire);
            if (wIdx == readIdx) { juce::Thread::sleep (10); continue; }

            const auto cap   = streamRing.capacity();
            const auto avail = wIdx - readIdx;
            const auto take  = juce::jmin ((std::size_t) 1024, (std::size_t) avail);
            outChunk.resize (take * 2);
            for (std::size_t i = 0; i < take; ++i)
            {
                const auto idx = (readIdx + i) % cap;
                const float l = streamRing.dataL[idx];
                const float r = streamRing.dataR[idx];
                outChunk[i * 2 + 0] = (juce::int16) juce::jlimit (-32768, 32767, (int) (l * 32767.0f));
                outChunk[i * 2 + 1] = (juce::int16) juce::jlimit (-32768, 32767, (int) (r * 32767.0f));
            }
            const auto bytes = (int) (outChunk.size() * sizeof (juce::int16));
            if (s.write (outChunk.data(), bytes) != bytes) break;
            readIdx += take;
        }

        streamRing.active.store (false);
    }

    void CompanionServer::writeRaw (juce::StreamingSocket& s,
                                    const juce::String& status,
                                    const juce::String& contentType,
                                    const juce::String& body)
    {
        const auto utf8 = body.toUTF8();
        const auto bodyLen = (int) utf8.sizeInBytes() - 1;
        const auto head = "HTTP/1.1 " + status + "\r\n"
                          "Content-Type: " + contentType + "\r\n"
                          "Content-Length: " + juce::String (bodyLen) + "\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Connection: close\r\n\r\n";
        s.write (head.toRawUTF8(), (int) head.getNumBytesAsUTF8());
        s.write (utf8.getAddress(), bodyLen);
    }
}
