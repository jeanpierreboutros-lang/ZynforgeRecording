#include "ConsoleLink.h"

namespace zynforge
{
    // X32 input-routing block addresses, in block order 0..3.
    static const char* const kInBlockAddr[ConsoleLink::kNumInBlocks] =
    {
        "/config/routing/IN/1-8",
        "/config/routing/IN/9-16",
        "/config/routing/IN/17-24",
        "/config/routing/IN/25-32",
    };

    ConsoleLink::ConsoleLink()
    {
        receiver.addListener (this);
    }

    ConsoleLink::~ConsoleLink()
    {
        receiver.removeListener (this);
        disconnect();
    }

    bool ConsoleLink::connect (const juce::String& targetHost, int port)
    {
        disconnect();
        // One UDP socket shared by sender + receiver: the X32 replies to
        // the request's source port, so we must listen where we send from.
        // A FRESH socket each time -- shutdown() in disconnect() left the
        // previous one's handle invalid for re-binding.
        socket = std::make_unique<juce::DatagramSocket> (false);
        if (! socket->bindToPort (0))                       { socket.reset(); return false; }
        if (! receiver.connectToSocket (*socket))           { socket.reset(); return false; }
        if (! sender.connectToSocket (*socket, targetHost, port)) { socket.reset(); return false; }
        host      = targetHost;
        connected = true;
        patch     = Patch::Unknown;
        if (onStatus) onStatus ("Console link up: " + targetHost + ":" + juce::String (port));
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

    juce::String ConsoleLink::inBlockAddress (int blockIdx)
    {
        return blockIdx >= 0 && blockIdx < kNumInBlocks ? kInBlockAddr[blockIdx]
                                                        : juce::String();
    }

    juce::String ConsoleLink::headampAddress (int headampIdx)
    {
        return "/headamp/" + juce::String (headampIdx).paddedLeft ('0', 3) + "/gain";
    }

    void ConsoleLink::sendMessage (const juce::OSCMessage& m)
    {
        if (sendHook != nullptr) { sendHook (m); return; }
        if (connected) sender.send (m);
    }

    void ConsoleLink::enterSoundcheck()
    {
        if (! connected) return;
        // Phase 1: query the current routing of all four blocks. The
        // replies stash the show patch; once all four are in, phase 2
        // (in oscMessageReceived) flips the blocks to the card returns.
        stagePatch.clear();
        pendingPatchQueries = kNumInBlocks;
        for (int b = 0; b < kNumInBlocks; ++b)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (inBlockAddress (b))));
        if (onStatus) onStatus ("Reading console patch...");
    }

    void ConsoleLink::exitSoundcheck()
    {
        if (! connected) return;
        if (stagePatch.size() < (size_t) kNumInBlocks)
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
        gains.clear();
        expectedGainReplies = juce::jlimit (0, 128, numHeadamps);
        for (int i = 0; i < expectedGainReplies; ++i)
            sendMessage (juce::OSCMessage (juce::OSCAddressPattern (headampAddress (i))));
        if (onStatus) onStatus ("Capturing " + juce::String (expectedGainReplies)
                                + " head-amp gains...");
    }

    void ConsoleLink::restoreGains()
    {
        if (! connected) return;
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
        for (int b = 0; b < kNumInBlocks; ++b)
        {
            if (addr != inBlockAddress (b)) continue;
            if (m.size() < 1 || ! m[0].isInt32()) return;
            if (pendingPatchQueries > 0)
            {
                stagePatch[b] = (int) m[0].getInt32();
                if (--pendingPatchQueries == 0)
                {
                    // Phase 2: full show patch stashed -> flip to card.
                    for (int blk = 0; blk < kNumInBlocks; ++blk)
                        sendMessage (juce::OSCMessage (
                            juce::OSCAddressPattern (inBlockAddress (blk)),
                            (juce::int32) (kCardBlockFirst + blk)));
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
            gains[idx] = m[0].getFloat32();
            if (expectedGainReplies > 0 && (int) gains.size() >= expectedGainReplies)
            {
                expectedGainReplies = 0;
                if (onStatus) onStatus (juce::String ((int) gains.size())
                                        + " head-amp gains captured.");
            }
        }
    }

    // ── Persistence ─────────────────────────────────────────────────────

    juce::var ConsoleLink::toJson() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("host", host);
        obj->setProperty ("capturedAt", juce::Time::getCurrentTime().toISO8601 (true));

        juce::Array<juce::var> patchArr;
        for (int b = 0; b < kNumInBlocks; ++b)
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
            for (int b = 0; b < juce::jmin (kNumInBlocks, patchArr->size()); ++b)
                if ((int) (*patchArr)[b] >= 0)
                    stagePatch[b] = (int) (*patchArr)[b];
        }
        if (auto* gainArr = v.getProperty ("gains", {}).getArray())
        {
            gains.clear();
            for (const auto& g : *gainArr)
                if (g.hasProperty ("headamp") && g.hasProperty ("gain"))
                    gains[(int) g.getProperty ("headamp", 0)] =
                        (float) (double) g.getProperty ("gain", 0.0);
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
