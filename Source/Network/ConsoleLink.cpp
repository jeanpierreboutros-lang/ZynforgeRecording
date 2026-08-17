#include "ConsoleLink.h"

namespace zynforge
{
    ConsoleLink::ConsoleLink()  = default;
    ConsoleLink::~ConsoleLink() { disconnect(); }

    void ConsoleLink::setProfile (ConsoleProfile::Kind k)
    {
        if (k == profile.kind) return;
        disconnect();                       // addresses / port / transport all change
        profile = consoleProfileFor (k);
        stagePatch.clear();
        gains.clear();
        patch = Patch::Unknown;
        dialectConfirmed = false;
    }

    bool ConsoleLink::connect (const juce::String& targetHost, int port)
    {
        disconnect();
        const int usePort = port > 0 ? port : profile.defaultPort;

        switch (profile.transport)
        {
            case ConsoleProfile::Transport::TextLineTcp:
                transport = std::make_unique<TextLineTransport>(); break;
            case ConsoleProfile::Transport::MidiTcp:
                transport = std::make_unique<MidiTcpTransport>();  break;
            case ConsoleProfile::Transport::OscUdp:
            default:
                transport = std::make_unique<OscUdpTransport>();   break;
        }

        transport->onMessage = [this] (const ConsoleMessage& m) { handleMessage (m); };
        if (! transport->connect (targetHost, usePort))
        {
            transport.reset();
            return false;
        }

        host      = targetHost;
        connected = true;
        patch     = Patch::Unknown;
        dialectConfirmed = false;

        // HANDSHAKE. Send the dialect's probe; a parseable reply confirms the
        // address model and unlocks the write tier. Nothing blocks on it -- the
        // link is immediately usable read-only either way.
        if (profile.dialect.probe)
            sendMessage (profile.dialect.probe());
        // Ask for pushed updates (names / scene changes) where supported.
        // Subscriptions EXPIRE (the X32's /xremote lasts ~10 s), so this is the
        // first of a repeating series -- see startHeartbeat.
        if (profile.dialect.subscribe)
        {
            sendMessage (profile.dialect.subscribe());
            nextSubscribeMs = juce::Time::currentTimeMillis() + kResubscribeMs;
        }
        startHeartbeat();

        if (onStatus)
            onStatus (profile.displayName + " link up: " + targetHost + ":"
                      + juce::String (usePort) + "  (" + transport->getName() + ")");
        return true;
    }

    void ConsoleLink::disconnect()
    {
        if (! connected) return;
        if (transport != nullptr)
        {
            transport->onMessage = nullptr;   // no late delivery into a dead link
            transport->disconnect();
        }
        transport.reset();
        connected = false;
        dialectConfirmed = false;
        host.clear();
        patch = Patch::Unknown;
        patchDeadlineMs = gainDeadlineMs = nextSubscribeMs = 0;
        stopTimer();
    }

    void ConsoleLink::sendMessage (const ConsoleMessage& m)
    {
        if (m.isEmpty()) return;
        if (msgHook != nullptr) { msgHook (m); return; }
        if (connected && transport != nullptr) transport->send (m);
    }

    // Every WRITE goes through here. Two independent gates: the profile has to
    // claim the capability at all, and the dialect has to be trustworthy --
    // either verified by hand or confirmed by the connect-time handshake.
    // Refusing loudly beats writing a guess into a desk mid-show.
    bool ConsoleLink::guardWrite (const char* what)
    {
        if (! connected) return false;
        if (canWrite()) return true;
        if (onStatus)
            onStatus (juce::String (profile.displayName)
                      + ": " + what + " blocked -- the console didn't answer the "
                      "handshake, so its control dialect is unconfirmed. Read-only.");
        return false;
    }

    void ConsoleLink::setSendHook (std::function<void (const juce::OSCMessage&)> hook)
    {
        if (hook == nullptr) { msgHook = nullptr; return; }
        // Translate the neutral message back into OSC so the long-standing X32
        // tests keep asserting on juce::OSCMessage unchanged.
        setMessageHook ([hook] (const ConsoleMessage& m)
        {
            juce::OSCMessage osc { juce::OSCAddressPattern (m.address) };
            for (const auto& a : m.args)
            {
                if      (a.isInt() || a.isInt64()) osc.addInt32 ((juce::int32) (int) a);
                else if (a.isDouble())             osc.addFloat32 ((float) (double) a);
                else                               osc.addString (a.toString());
            }
            hook (osc);
        });
    }

    void ConsoleLink::injectReply (const juce::OSCMessage& m)
    {
        ConsoleMessage cm { m.getAddressPattern().toString() };
        for (const auto& a : m)
        {
            if      (a.isInt32())   cm.args.emplace_back ((int) a.getInt32());
            else if (a.isFloat32()) cm.args.emplace_back ((double) a.getFloat32());
            else if (a.isString())  cm.args.emplace_back (a.getString());
        }
        handleMessage (cm);
    }

    void ConsoleLink::requestChannelNames (int count)
    {
        if (! connected || ! profile.canReadNames || ! profile.dialect.queryChannelName) return;
        for (int ch = 1; ch <= juce::jlimit (0, 128, count); ++ch)
            sendMessage (profile.dialect.queryChannelName (ch));
        if (onStatus) onStatus ("Requested " + juce::String (count) + " channel names...");
    }

    void ConsoleLink::enterSoundcheck()
    {
        if (! connected) return;
        if (! profile.canRepatch)
        {
            if (onStatus) onStatus (profile.hasNativeVsc
                ? profile.displayName + ": use the console's Virtual Soundcheck mode."
                : profile.displayName + ": repatch over the wire isn't supported.");
            return;
        }
        if (! guardWrite ("repatch")) return;

        // Phase 1: query the current routing of every block. The replies stash
        // the show patch; once all are in, phase 2 flips the blocks to card.
        stagePatch.clear();
        pendingPatchQueries = profile.numInBlocks;
        for (int b = 0; b < profile.numInBlocks; ++b)
            if (profile.dialect.queryInBlock)
                sendMessage (profile.dialect.queryInBlock (b));
        if (onStatus) onStatus ("Reading console patch...");
        patchDeadlineMs = juce::Time::currentTimeMillis() + kPatchTimeoutMs;
        startHeartbeat();    // abort if a routing reply is lost
    }

    void ConsoleLink::exitSoundcheck()
    {
        if (! connected || ! profile.canRepatch) return;
        if (! guardWrite ("repatch")) return;
        if (stagePatch.size() < (size_t) profile.numInBlocks)
        {
            if (onStatus) onStatus ("No stashed stage patch -- enter soundcheck first.");
            return;
        }
        for (const auto& [block, value] : stagePatch)
            if (profile.dialect.setInBlock)
                sendMessage (profile.dialect.setInBlock (block, value));
        patch = Patch::Stage;
        if (onStatus) onStatus ("Console repatched to STAGE (restored show patch).");
    }

    void ConsoleLink::captureGains (int numHeadamps)
    {
        if (! connected) return;
        if (! profile.canCaptureGains)
        {
            if (onStatus) onStatus (profile.displayName + ": head-amp gain capture isn't supported.");
            return;
        }
        // Capture is a READ, but it feeds restoreGains() which is a write, and
        // a half-understood reply format would poison the stash. Gate it too.
        if (! guardWrite ("head-amp capture")) return;

        gains.clear();
        gainCaptureComplete = false;
        expectedGainReplies = juce::jlimit (0, 128, numHeadamps);
        for (int i = 0; i < expectedGainReplies; ++i)
            if (profile.dialect.queryHeadAmp)
                sendMessage (profile.dialect.queryHeadAmp (i));
        if (onStatus) onStatus ("Capturing " + juce::String (expectedGainReplies)
                                + " head-amp gains...");
        gainDeadlineMs = juce::Time::currentTimeMillis() + kGainTimeoutMs;
        startHeartbeat();    // abort if a head-amp reply is lost
    }

    void ConsoleLink::restoreGains()
    {
        if (! connected || ! profile.canCaptureGains) return;
        if (! guardWrite ("head-amp restore")) return;
        if (gains.empty())
        {
            if (onStatus) onStatus ("No captured gains to restore.");
            return;
        }
        for (const auto& [idx, value] : gains)
            if (profile.dialect.setHeadAmp)
                sendMessage (profile.dialect.setHeadAmp (idx, value));
        if (onStatus) onStatus (juce::String ((int) gains.size())
                                + " head-amp gains restored to show settings.");
    }

    void ConsoleLink::handleMessage (const ConsoleMessage& m)
    {
        if (! profile.dialect.parse) return;
        const auto ev = profile.dialect.parse (m);

        switch (ev.type)
        {
            case ConsoleEvent::Type::None:
                return;

            case ConsoleEvent::Type::HandshakeOk:
                if (! dialectConfirmed)
                {
                    dialectConfirmed = true;
                    if (onStatus)
                        onStatus (profile.displayName + ": dialect confirmed -- console control enabled.");
                }
                return;

            // ── READ TIER ────────────────────────────────────────────────
            case ConsoleEvent::Type::ChannelName:
                if (onChannelName && ev.index >= 1) onChannelName (ev.index, ev.text);
                return;

            case ConsoleEvent::Type::SceneRecalled:
                if (onSceneRecalled) onSceneRecalled (ev.index, ev.text);
                return;

            // ── WRITE-TIER REPLIES ───────────────────────────────────────
            case ConsoleEvent::Type::InputBlockRouting:
            {
                if (pendingPatchQueries <= 0) return;
                // Range-check the off-wire index, exactly like HeadAmpGain below.
                // Without it a junk index became a stagePatch key, and
                // exitSoundcheck later wrote that garbage back to the desk --
                // the one thing the whole write-gate design exists to prevent.
                if (ev.index < 0 || ev.index >= profile.numInBlocks) return;
                stagePatch[ev.index] = ev.intValue;
                // Completion is "every DISTINCT block is stashed", not "N replies
                // arrived". We subscribe to pushed updates on connect, so an
                // unsolicited routing message during the read window used to
                // decrement the counter for a block already held: the count hit
                // zero with an INCOMPLETE stash, phase 2 flipped the desk to card
                // anyway, and exitSoundcheck then refused to restore -- leaving
                // the desk on card returns with the show patch nowhere but in the
                // engineer's head.
                if ((int) stagePatch.size() >= profile.numInBlocks)
                {
                    pendingPatchQueries = 0;
                    patchDeadlineMs = 0;
                    // Phase 2: full show patch stashed -> flip to card.
                    for (int blk = 0; blk < profile.numInBlocks; ++blk)
                        if (profile.dialect.setInBlock)
                            sendMessage (profile.dialect.setInBlock (
                                blk, profile.cardBlockFirst + blk));
                    patch = Patch::Soundcheck;
                    if (onStatus)
                        onStatus ("Console repatched to SOUNDCHECK (card returns); show patch stashed.");
                }
                return;
            }

            case ConsoleEvent::Type::HeadAmpGain:
            {
                if (ev.index < 0 || ev.index >= 128) return;   // range-check off-wire indices
                gains[ev.index] = ev.value;
                if (expectedGainReplies > 0 && (int) gains.size() >= expectedGainReplies)
                {
                    gainDeadlineMs = 0;
                    expectedGainReplies = 0;
                    gainCaptureComplete = true;
                    if (onStatus) onStatus (juce::String ((int) gains.size())
                                            + " head-amp gains captured.");
                }
                return;
            }
        }
    }

    void ConsoleLink::startHeartbeat()
    {
        if (! isTimerRunning()) startTimer (kHeartbeatMs);
    }

    void ConsoleLink::timerCallback()
    {
        if (! connected) { stopTimer(); return; }
        const auto now = juce::Time::currentTimeMillis();

        // 1. Renew the push subscription BEFORE it expires. Without this the
        //    desk stops pushing ~10 s after connect and every scene recall
        //    silently stops dropping a marker for the rest of the show.
        if (profile.dialect.subscribe && now >= nextSubscribeMs)
        {
            sendMessage (profile.dialect.subscribe());
            nextSubscribeMs = now + kResubscribeMs;
        }

        // 2. Reply-timeout watchdogs. A query is still outstanding past its
        //    deadline -> a reply was lost; abort cleanly so the UI doesn't sit
        //    forever on "Reading console patch..." / "Capturing head-amp gains".
        if (patchDeadlineMs != 0 && now >= patchDeadlineMs)
        {
            patchDeadlineMs = 0;
            if (pendingPatchQueries > 0)
            {
                pendingPatchQueries = 0;
                stagePatch.clear();
                if (onStatus) onStatus ("Console patch read timed out (lost reply) -- try again.");
            }
        }
        if (gainDeadlineMs != 0 && now >= gainDeadlineMs)
        {
            gainDeadlineMs = 0;
            if (expectedGainReplies > 0)
            {
                const int got = (int) gains.size();
                expectedGainReplies = 0;
                gainCaptureComplete = false;   // partial -- must NOT be persisted as the show's gains
                if (onStatus) onStatus ("Head-amp capture timed out (" + juce::String (got)
                                        + " of expected received) -- try again.");
            }
        }

        // Nothing left to do and no subscription to renew -> stop beating.
        if (profile.dialect.subscribe == nullptr
            && patchDeadlineMs == 0 && gainDeadlineMs == 0)
            stopTimer();
    }

    // ── Persistence ─────────────────────────────────────────────────────

    juce::var ConsoleLink::toJson() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("host", host);
        obj->setProperty ("console", (int) profile.kind);
        obj->setProperty ("capturedAt", juce::Time::getCurrentTime().toISO8601 (true));

        juce::Array<juce::var> patchArr;
        for (int b = 0; b < profile.numInBlocks; ++b)
        {
            auto it = stagePatch.find (b);
            patchArr.add (it != stagePatch.end() ? it->second : -1);
        }
        obj->setProperty ("stagePatch", patchArr);

        juce::Array<juce::var> gainArr;
        for (const auto& [idx, value] : gains)
        {
            auto* g = new juce::DynamicObject();
            g->setProperty ("headamp", idx);
            g->setProperty ("gain", value);
            g->setProperty ("dB", gainToDb (value));
            gainArr.add (juce::var (g));
        }
        obj->setProperty ("gains", gainArr);
        return juce::var (obj);
    }

    bool ConsoleLink::loadJson (const juce::var& v)
    {
        if (! v.isObject()) return false;
        if (auto* patchArr = v.getProperty ("stagePatch", {}).getArray())
        {
            stagePatch.clear();
            for (int b = 0; b < patchArr->size(); ++b)
                if ((int) (*patchArr)[b] >= 0)
                    stagePatch[b] = (int) (*patchArr)[b];
        }
        if (auto* gainArr = v.getProperty ("gains", {}).getArray())
        {
            gains.clear();
            gainCaptureComplete = true;   // a saved capture is by definition finished
            for (const auto& g : *gainArr)
                if (g.hasProperty ("headamp") && g.hasProperty ("gain"))
                {
                    // Range-check a hand-edited / corrupted state file before it
                    // ever reaches restoreGains -> the desk. The wire path also
                    // clamps; this path did not.
                    const int ha = (int) g.getProperty ("headamp", 0);
                    if (ha < 0 || ha >= 128) continue;
                    // gains hold the RAW normalised 0..1 wire value (sent back
                    // verbatim by restoreGains), so clamp in THAT domain -- a
                    // dB-range clamp (-12..60) was a no-op that passed
                    // dB-looking garbage straight to the desk.
                    gains[ha] = juce::jlimit (0.0f, 1.0f,
                                              (float) (double) g.getProperty ("gain", 0.0));
                }
        }
        return true;
    }

    bool ConsoleLink::saveTo (const juce::File& sessionDir) const
    {
        if (! sessionDir.isDirectory()) return false;
        return sessionDir.getChildFile (kStateFileName)
                   .replaceWithText (juce::JSON::toString (toJson()));
    }

    bool ConsoleLink::loadFrom (const juce::File& sessionDir)
    {
        const auto f = sessionDir.getChildFile (kStateFileName);
        if (! f.existsAsFile()) return false;
        return loadJson (juce::JSON::parse (f));
    }
}
