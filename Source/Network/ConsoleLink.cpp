#include "ConsoleLink.h"

namespace zynforge
{
    ConsoleLink::ConsoleLink()
    {
        receiver.addListener (this);
    }

    ConsoleLink::~ConsoleLink()
    {
        receiver.removeListener (this);
        disconnect();
    }

    void ConsoleLink::setProfile (ConsoleProfile::Kind k)
    {
        if (k == profile.kind) return;
        disconnect();                       // addresses / port change with the desk
        profile = consoleProfileFor (k);
        stagePatch.clear();
        gains.clear();
        patch = Patch::Unknown;
    }

    bool ConsoleLink::connect (const juce::String& targetHost, int port)
    {
        disconnect();
        const int usePort = port > 0 ? port : profile.defaultPort;
        // One UDP socket shared by sender + receiver: the desk replies to
        // the request's source port, so we must listen where we send from.
        // A FRESH socket each time -- shutdown() in disconnect() left the
        // previous one's handle invalid for re-binding.
        socket = std::make_unique<juce::DatagramSocket> (false);
        if (! socket->bindToPort (0))                       { socket.reset(); return false; }
        if (! receiver.connectToSocket (*socket))           { socket.reset(); return false; }
        if (! sender.connectToSocket (*socket, targetHost, usePort))
        {
            // The receiver's reader thread references `socket`; disconnect it
            // BEFORE freeing the socket (disconnect() orders it this way too),
            // else socket.reset() frees a socket the reader thread still uses.
            receiver.disconnect();
            socket.reset();
            return false;
        }
        host      = targetHost;
        connected = true;
        patch     = Patch::Unknown;
        if (onStatus) onStatus (profile.displayName + " link up: "
                                + targetHost + ":" + juce::String (usePort));
        return true;
    }

    void ConsoleLink::disconnect()
    {
        if (! connected) return;
        sender.disconnect();
        receiver.disconnect();
        socket.reset();           // closes + frees the handle; connect() makes a new one
        connected = false;
        host.clear();
        patch = Patch::Unknown;
    }

    juce::String ConsoleLink::inBlockAddress (int blockIdx) const
    {
        return profile.inBlockAddress ? profile.inBlockAddress (blockIdx) : juce::String();
    }

    juce::String ConsoleLink::headampAddress (int headampIdx) const
    {
        return profile.gainAddress ? profile.gainAddress (headampIdx) : juce::String();
    }

    void ConsoleLink::sendMessage (const juce::OSCMessage& m)
    {
        if (sendHook != nullptr) { sendHook (m); return; }
        if (connected) sender.send (m);
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
        // Phase 1: query the current routing of every block. The replies
        // stash the show patch; once all are in, phase 2 (in
        // oscMessageReceived) flips the blocks to the card returns.
        stagePatch.clear();
        pendingPatchQueries = profile.numInBlocks;
        for (int b = 0; b < profile.numInBlocks; ++b)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (inBlockAddress (b))));
        if (onStatus) onStatus ("Reading console patch...");
        startTimer (1200);   // abort if a routing reply is lost
    }

    void ConsoleLink::exitSoundcheck()
    {
        if (! connected || ! profile.canRepatch) return;
        if (stagePatch.size() < (size_t) profile.numInBlocks)
        {
            if (onStatus) onStatus ("No stashed stage patch -- enter soundcheck first.");
            return;
        }
        for (const auto& [block, value] : stagePatch)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (inBlockAddress (block)),
                                           (juce::int32) value));
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
        gains.clear();
        gainCaptureComplete = false;
        expectedGainReplies = juce::jlimit (0, 128, numHeadamps);
        for (int i = 0; i < expectedGainReplies; ++i)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (headampAddress (i))));
        if (onStatus) onStatus ("Capturing " + juce::String (expectedGainReplies)
                                + " head-amp gains...");
        startTimer (1500);   // abort if a head-amp reply is lost
    }

    void ConsoleLink::restoreGains()
    {
        if (! connected || ! profile.canCaptureGains) return;
        if (gains.empty())
        {
            if (onStatus) onStatus ("No captured gains to restore.");
            return;
        }
        for (const auto& [idx, value] : gains)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (headampAddress (idx)),
                                           value));
        if (onStatus) onStatus (juce::String ((int) gains.size())
                                + " head-amp gains restored to show settings.");
    }

    void ConsoleLink::oscMessageReceived (const juce::OSCMessage& m)
    {
        const auto addr = m.getAddressPattern().toString();

        // Routing-block reply (query response carries the enum as int).
        for (int b = 0; b < profile.numInBlocks; ++b)
        {
            if (addr != inBlockAddress (b)) continue;
            if (m.size() < 1 || ! m[0].isInt32()) return;
            if (pendingPatchQueries > 0)
            {
                stagePatch[b] = (int) m[0].getInt32();
                if (--pendingPatchQueries == 0)
                {
                    stopTimer();   // all replies in -- cancel the watchdog
                    // Phase 2: full show patch stashed -> flip to card.
                    for (int blk = 0; blk < profile.numInBlocks; ++blk)
                        sendMessage (juce::OSCMessage (
                            juce::OSCAddressPattern (inBlockAddress (blk)),
                            (juce::int32) (profile.cardBlockFirst + blk)));
                    patch = Patch::Soundcheck;
                    if (onStatus) onStatus ("Console repatched to SOUNDCHECK (card returns); show patch stashed.");
                }
            }
            return;
        }

        // Head-amp gain reply: /headamp/NNN/gain ,f
        if (addr.startsWith ("/headamp/") && addr.endsWith ("/gain")
            && m.size() >= 1 && m[0].isFloat32())
        {
            const int idx = addr.fromFirstOccurrenceOf ("/headamp/", false, false)
                                .upToFirstOccurrenceOf ("/", false, false).getIntValue();
            // The index comes straight off the wire -- range-check it before
            // using it as a map key so a malformed / spoofed reply can't seed
            // the gains map with a garbage (e.g. huge or negative) head-amp
            // number that we'd later blindly write back to the desk (finding
            // #8b). captureGains only ever polls [0, 128).
            if (idx < 0 || idx >= 128) return;
            gains[idx] = m[0].getFloat32();
            if (expectedGainReplies > 0 && (int) gains.size() >= expectedGainReplies)
            {
                stopTimer();   // all replies in -- cancel the watchdog
                expectedGainReplies = 0;
                gainCaptureComplete = true;   // every requested reply landed
                if (onStatus) onStatus (juce::String ((int) gains.size())
                                        + " head-amp gains captured.");
            }
        }
    }

    void ConsoleLink::timerCallback()
    {
        // Reply-timeout watchdog (see header). A query is still outstanding ->
        // a UDP reply was lost; abort cleanly so the UI doesn't sit forever on
        // "Reading console patch..." / "Capturing head-amp gains...".
        stopTimer();
        if (pendingPatchQueries > 0)
        {
            pendingPatchQueries = 0;
            stagePatch.clear();
            if (onStatus) onStatus ("Console patch read timed out (lost reply) -- try again.");
        }
        if (expectedGainReplies > 0)
        {
            const int got = (int) gains.size();
            expectedGainReplies = 0;
            gainCaptureComplete = false;   // partial -- must NOT be persisted as the show's gains
            if (onStatus) onStatus ("Head-amp capture timed out (" + juce::String (got)
                                    + " of expected received) -- try again.");
        }
    }

    // ── Persistence ─────────────────────────────────────────────────────

    juce::var ConsoleLink::toJson() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("host", host);
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
                    // ever reaches restoreGains -> the desk. The wire path already
                    // clamps; this path did not.
                    const int ha = (int) g.getProperty ("headamp", 0);
                    if (ha < 0 || ha >= 128) continue;
                    // gains hold the RAW normalised 0..1 wire value (sent back
                    // verbatim by restoreGains), so clamp in THAT domain -- a
                    // dB-range clamp (-12..60) was a no-op that passed
                    // dB-looking garbage (e.g. 24) straight to the desk.
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
