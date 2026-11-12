#include "ConsoleLink.h"
#include "ConsoleLog.h"
#include "../Audio/AtomicFile.h"
#include "../Audio/DisplayText.h"

#include <cmath>

namespace zynforge
{
    ConsoleLink::ConsoleLink()  = default;
    ConsoleLink::~ConsoleLink() { disconnect(); }

    void ConsoleLink::setProfile (ConsoleProfile::Kind k)
    {
        if (k == profile.kind) return;
        disconnect();                       // addresses / port / transport all change
        profile = consoleProfileFor (k);
        cancelDeskReads();
        stagePatch.clear();
        sessionsOnSoundcheck.clear();
        gains.clear();
        patch = Patch::Unknown;
        dialectConfirmed = false;
    }

    bool ConsoleLink::connect (const juce::String& targetHost, int port)
    {
        disconnect();
        const int usePort = port > 0 ? port : profile.defaultPort;

        // Only a numeric address. A name is looked up on every UDP send, on this
        // thread, and a lookup that fails is not cached, so one typo stalled the
        // message thread on each heartbeat for as long as the network's resolver
        // took to give up, including during a take.
        const auto address = oscwire::normaliseSourceIp (targetHost);
        if (address.isEmpty()) return false;

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

        transport->onMessage = [this] (const ConsoleMessage& m) { logInbound (m); handleMessage (m); };
        transport->onUnhandledTraffic = [this] (const juce::String& from, const juce::String& what)
        { if (onTraffic) onTraffic ("link <- " + from + "  " + what); };
        if (! transport->connect (address, usePort))
        {
            transport.reset();
            return false;
        }

        host      = address;
        lostHost.clear();
        connectedPort = usePort;
        connected = true;
        dialectConfirmed = false;
        deskAsked = false;
        connectedAtMs = juce::Time::currentTimeMillis();
        lastAnswerMs = 0;
        reportedHealth = Health::NotAnswered;
        neverAnsweredReported = false;

        // HANDSHAKE. Send the dialect's probe; a parseable reply confirms the
        // address model and unlocks the write tier. Nothing blocks on it -- the
        // link is immediately usable read-only either way.
        if (profile.dialect.probe)
        {
            sendMessage (profile.dialect.probe());
            nextProbeMs = connectedAtMs + kProbeMs;
        }
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
            onStatus (profile.displayName + " link open to " + address + ":"
                      + juce::String (usePort) + "  (" + transport->getName() + ") -- "
                      + (profile.dialect.probe ? "waiting for the desk to answer"
                                               : "this link can't tell whether a desk is there"));
        return true;
    }

    void ConsoleLink::disconnect()
    {
        if (! connected) return;
        if (transport != nullptr)
        {
            transport->onMessage = nullptr;   // no late delivery into a dead link
            transport->onUnhandledTraffic = nullptr;
            transport->disconnect();
        }
        transport.reset();
        connected = false;
        dialectConfirmed = false;
        host.clear();
        cancelDeskReads();
        nextSubscribeMs = nextProbeMs = lastAnswerMs = connectedAtMs = 0;
        stopTimer();
    }

    // Nothing is listening any more. A capture left open would finish later
    // from pushed updates, and stands in the way of loading a saved one.
    void ConsoleLink::cancelDeskReads()
    {
        patchRead = PatchRead::None;
        deskRead.clear();
        patchDeadlineMs = 0;
        gainCheckPending = false;
        gainReadBack.clear();
        gainDeadlineMs = 0;
        if (expectedGainReplies > 0)
        {
            expectedGainReplies = 0;
            gains.clear();
            gainCaptureComplete = false;
        }
    }

    void ConsoleLink::sendMessage (const ConsoleMessage& m)
    {
        if (m.isEmpty()) return;
        if (onTraffic)
            onTraffic ("link -> " + host + ":" + juce::String (connectedPort)
                       + "  " + consolelog::describe (m));
        if (msgHook != nullptr) { msgHook (m); return; }
        if (connected && transport != nullptr) transport->send (m);
    }

    void ConsoleLink::logInbound (const ConsoleMessage& m)
    {
        if (! onTraffic) return;
        onTraffic ("link <- " + (m.source.isNotEmpty() ? m.source : host + ":" + juce::String (connectedPort))
                   + "  " + (m.wire.isNotEmpty() ? m.wire : consolelog::describe (m)));
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

    bool ConsoleLink::guardDeskChange (const char* what)
    {
        if (takeIsHoldingDesk())
        {
            if (onStatus) onStatus ("Console changes wait until the take has stopped");
            return false;
        }
        return guardWrite (what);
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
        injectReply (cm);
    }

    void ConsoleLink::requestChannelNames (int count)
    {
        if (! connected || ! profile.canReadNames || ! profile.dialect.queryChannelName) return;
        for (int ch = 1; ch <= juce::jlimit (0, 128, count); ++ch)
            sendMessage (profile.dialect.queryChannelName (ch));
        if (onStatus) onStatus ("Requested " + juce::String (count) + " channel names...");
    }

    int ConsoleLink::requestStereoLinks()
    {
        if (! connected || ! profile.canReadStereoLinks || ! profile.dialect.queryStereoLink) return 0;
        int asked = 0;
        for (int left = 1;; left += 2)
        {
            const auto query = profile.dialect.queryStereoLink (left);
            if (query.isEmpty()) return asked;
            sendMessage (query);
            ++asked;
        }
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
        if (! guardDeskChange ("repatch")) return;

        // Reading the card returns as a show patch is how the real one is lost.
        if (isOnSoundcheckPatch())
        {
            if (onStatus) onStatus ("Saved stage patch retained -- go back to STAGE, or check the desk's "
                                    "patch if it was put back by hand.");
            return;
        }

        // Phase 1: query the current routing of every block. The replies stash
        // the show patch; once all are in, phase 2 flips the blocks to card.
        startPatchRead (PatchRead::Stash);
        if (onStatus) onStatus ("Reading console patch...");
    }

    void ConsoleLink::exitSoundcheck()
    {
        if (! connected || ! profile.canRepatch) return;
        if (! guardDeskChange ("repatch")) return;
        if (! hasStagePatch())
        {
            if (onStatus) onStatus ("No stashed stage patch -- enter soundcheck first.");
            return;
        }
        for (const auto& [block, value] : stagePatch)
            if (profile.dialect.setInBlock)
                sendMessage (profile.dialect.setInBlock (block, value));
        // UDP gives no receipt, and a desk that isn't there looks exactly like
        // one that took it. Only the desk's own answer ends SOUNDCHECK.
        startPatchRead (PatchRead::Restore);
        if (onStatus) onStatus ("Sent the STAGE patch -- waiting for the desk to confirm it...");
    }

    void ConsoleLink::checkDeskPatch()
    {
        if (! connected) return;
        if (! profile.canRepatch)
        {
            if (onStatus) onStatus (profile.displayName + " cannot report its routing to this app.");
            return;
        }
        deskAsked = true;
        startPatchRead (PatchRead::Check);
        if (onStatus) onStatus ("Reading the desk's patch...");
    }

    void ConsoleLink::startPatchRead (PatchRead kind)
    {
        patchRead = kind;
        deskRead.clear();
        for (int b = 0; b < profile.numInBlocks; ++b)
            if (profile.dialect.queryInBlock)
                sendMessage (profile.dialect.queryInBlock (b));
        patchDeadlineMs = juce::Time::currentTimeMillis() + kPatchTimeoutMs;
        startHeartbeat();    // abort if a routing reply is lost
    }

    void ConsoleLink::finishPatchRead()
    {
        const auto finished = patchRead;
        patchRead = PatchRead::None;
        patchDeadlineMs = 0;
        switch (finished)
        {
            case PatchRead::Stash:   finishPatchStash();   break;
            case PatchRead::Restore: finishPatchRestore(); break;
            case PatchRead::Check:   finishPatchCheck();   break;
            case PatchRead::None:    break;
        }
    }

    // What the screen must keep saying is the state as it stands: nothing here
    // moved it, because silence proves nothing about where a desk is.
    void ConsoleLink::patchReadTimedOut()
    {
        const auto timedOut = patchRead;
        const auto heard = juce::String ((int) deskRead.size()) + " of " + juce::String (profile.numInBlocks);
        patchRead = PatchRead::None;
        deskRead.clear();
        if (! onStatus) return;
        switch (timedOut)
        {
            case PatchRead::Stash:
                onStatus ("Console patch read timed out (lost reply) -- try again.");
                break;
            case PatchRead::Restore:
                onStatus ("The desk did not confirm STAGE (" + heard + " input blocks answered), so it still "
                          "shows as on SOUNDCHECK. Check the connection and go back to STAGE again.");
                break;
            case PatchRead::Check:
                onStatus ("The desk did not answer (" + heard + " input blocks), so its patch is not known"
                          + (patch == Patch::Soundcheck ? ". It still shows as on SOUNDCHECK." : "."));
                break;
            case PatchRead::None:
                break;
        }
    }

    void ConsoleLink::finishPatchRestore()
    {
        int wrong = 0;
        for (const auto& [block, value] : stagePatch)
            if (deskRead[block] != value) ++wrong;
        deskRead.clear();
        if (wrong > 0)
        {
            if (onStatus) onStatus ("The desk did not take the STAGE patch on " + juce::String (wrong) + " of "
                                    + juce::String (profile.numInBlocks) + " input blocks, so it still shows as "
                                    "on SOUNDCHECK. Go back to STAGE again, or set the routing on the desk.");
            return;
        }
        patch = Patch::Stage;
        const bool saved = persistPatch();
        if (onStatus) onStatus (saved ? "Console back on STAGE, confirmed by the desk (restored show patch)."
                                      : "Console back on STAGE, confirmed by the desk, but the session could not be updated.");
    }

    void ConsoleLink::finishPatchCheck()
    {
        // A show patch can use the card on purpose, so only a block that has
        // moved away from the saved patch counts as a soundcheck.
        int onCard = 0;
        for (const auto& [block, value] : deskRead)
        {
            const auto saved = stagePatch.find (block);
            if (value == profile.cardBlockFirst + block
                && (saved == stagePatch.end() || saved->second != value))
                ++onCard;
        }
        deskRead.clear();

        const auto wasOnSoundcheck = patch == Patch::Soundcheck;
        if (onCard == 0)
        {
            patch = Patch::Stage;
            const bool saved = ! wasOnSoundcheck || persistPatch();
            if (onStatus) onStatus (! wasOnSoundcheck ? juce::String ("The desk is on its stage inputs.")
                                    : saved ? "The desk is on its stage inputs, so SOUNDCHECK is cleared. Nothing was written to it."
                                            : "The desk is on its stage inputs, but the session could not be updated.");
            return;
        }
        const auto blocks = juce::String (onCard) + " of " + juce::String (profile.numInBlocks) + " input blocks";
        if (! hasStagePatch())
        {
            patch = Patch::Unknown;
            if (onStatus) onStatus ("The desk has card returns on " + blocks + ", and this session has no show patch "
                                    "to go back to. Open the session that saved it, or set the routing on the desk.");
            return;
        }
        patch = Patch::Soundcheck;
        if (! wasOnSoundcheck) persistPatch();
        if (onStatus) onStatus ("The desk has card returns on " + blocks + ".");
    }

    juce::String ConsoleLink::deskPatchText() const
    {
        if (! profile.canRepatch)
            return profile.hasNativeVsc ? "not visible here: check the console's own Virtual Soundcheck"
                                        : "not visible here: this link reads no routing";
        if (isOnSoundcheckPatch())  return "on SOUNDCHECK (card returns)";
        if (patch == Patch::Stage)  return "stage inputs";
        return "not read from the desk";
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
        gainCheckPending = false;
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
        if (! guardDeskChange ("head-amp restore")) return;
        if (gains.empty())
        {
            if (onStatus) onStatus ("No captured gains to restore.");
            return;
        }
        if (! gainCaptureComplete)
        {
            if (onStatus) onStatus ("Partial head-amp capture cannot be restored -- capture every requested gain first.");
            return;
        }
        for (const auto& [idx, value] : gains)
            if (profile.dialect.setHeadAmp)
                sendMessage (profile.dialect.setHeadAmp (idx, value));

        // As with the patch: a restore counts when the desk reads it back.
        gainReadBack.clear();
        gainCheckPending = true;
        for (const auto& entry : gains)
            if (profile.dialect.queryHeadAmp)
                sendMessage (profile.dialect.queryHeadAmp (entry.first));
        gainDeadlineMs = juce::Time::currentTimeMillis() + kGainTimeoutMs;
        startHeartbeat();
        if (onStatus) onStatus ("Sent " + counted ((int) gains.size(), "head-amp gain")
                                + " -- waiting for the desk to confirm them...");
    }

    void ConsoleLink::finishGainCheck()
    {
        gainCheckPending = false;
        gainDeadlineMs = 0;
        int wrong = 0;
        for (const auto& [idx, value] : gainReadBack)
            if (std::abs (gainToDb (value) - gainToDb (gains[idx])) > kGainMatchDb) ++wrong;
        const int total = (int) gains.size();
        gainReadBack.clear();
        if (onStatus) onStatus (wrong == 0
            ? counted (total, "head-amp gain") + " restored to show settings, confirmed by the desk."
            : "The desk did not take " + juce::String (wrong) + " of " + counted (total, "head-amp gain")
              + ". Restore again, or set them on the desk.");
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
                lastAnswerMs = juce::Time::currentTimeMillis();
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

            case ConsoleEvent::Type::StereoLink:
                if (onStereoLink) onStereoLink (ev.index, ev.intValue != 0);
                return;

            // ── WRITE-TIER REPLIES ───────────────────────────────────────
            case ConsoleEvent::Type::InputBlockRouting:
            {
                if (patchRead == PatchRead::None) return;
                // Range-check the off-wire index, exactly like HeadAmpGain below.
                // Without it a junk index became a stagePatch key, and
                // exitSoundcheck later wrote that garbage back to the desk --
                // the one thing the whole write-gate design exists to prevent.
                // A value out of range is refused here rather than at the next
                // load: the desk must not move on a way back that won't reload.
                if (ev.index < 0 || ev.index >= profile.numInBlocks || ! isRoutingValue (ev.intValue)) return;
                deskRead[ev.index] = ev.intValue;
                // Completion is "every DISTINCT block is stashed", not "N replies
                // arrived". We subscribe to pushed updates on connect, so an
                // unsolicited routing message during the read window used to
                // decrement the counter for a block already held: the count hit
                // zero with an INCOMPLETE stash, phase 2 flipped the desk to card
                // anyway, and exitSoundcheck then refused to restore -- leaving
                // the desk on card returns with the show patch nowhere but in the
                // engineer's head.
                if ((int) deskRead.size() >= profile.numInBlocks) finishPatchRead();
                return;
            }

            case ConsoleEvent::Type::HeadAmpGain:
            {
                // The raw wire value is normalised 0..1 and restoreGains sends
                // it back to the desk verbatim. A NaN is no reading at all
                // (the capture stays incomplete and cannot be restored);
                // anything else is clamped to the wire range, as loadJson does.
                if (! std::isfinite (ev.value)) return;
                if (gainCheckPending)
                {
                    if (gains.count (ev.index) == 0) return;
                    gainReadBack[ev.index] = juce::jlimit (0.0f, 1.0f, ev.value);
                    if (gainReadBack.size() >= gains.size()) finishGainCheck();
                    return;
                }
                if (expectedGainReplies <= 0) return; // preserve the completed capture snapshot
                // Only the headamps queried by this capture can complete it.
                // Subscription updates for other headamps are not replies.
                if (ev.index < 0 || ev.index >= expectedGainReplies) return;
                gains[ev.index] = juce::jlimit (0.0f, 1.0f, ev.value);
                if (expectedGainReplies > 0 && (int) gains.size() >= expectedGainReplies)
                {
                    gainDeadlineMs = 0;
                    expectedGainReplies = 0;
                    gainCaptureComplete = true;
                    if (onStatus) onStatus (juce::String ((int) gains.size())
                                            + " head-amp gains captured.");
                    if (onGainCaptureFinished) onGainCaptureFinished (true);
                }
                return;
            }
        }
    }

    // Phase 2 of enterSoundcheck. The desk changes only once the way back is on
    // disk: after a crash this file is all that knows the show patch.
    void ConsoleLink::finishPatchStash()
    {
        // The read can land after RECORD, and a press said nothing about that.
        if (! guardDeskChange ("repatch"))
        {
            deskRead.clear();
            return;
        }

        bool alreadyOnCard = true;
        for (const auto& [block, value] : deskRead)
            alreadyOnCard = alreadyOnCard && value == profile.cardBlockFirst + block;
        if (alreadyOnCard)
        {
            // Saving this as the show patch is how the real one gets lost for good.
            deskRead.clear();
            stagePatch.clear();
            patch = Patch::Unknown;
            if (onStatus) onStatus ("The console is already on card returns, so there is no show patch to save. "
                                    "Open the session that saved it, or set the routing on the desk.");
            return;
        }

        const auto stageBefore = patch;
        stagePatch = std::move (deskRead);
        deskRead.clear();
        patch = Patch::Soundcheck;
        deskAsked = true;
        if (! persistPatch())
        {
            patch = stageBefore;
            stagePatch.clear();
            if (onStatus) onStatus ("Could not save the show patch to the session -- console left as it was.");
            return;
        }

        for (int blk = 0; blk < profile.numInBlocks; ++blk)
            if (profile.dialect.setInBlock)
                sendMessage (profile.dialect.setInBlock (blk, profile.cardBlockFirst + blk));
        if (onStatus)
            onStatus ("Console repatched to SOUNDCHECK (card returns); show patch saved with the session.");
    }

    void ConsoleLink::startHeartbeat()
    {
        if (! isTimerRunning()) startTimer (kHeartbeatMs);
    }

    void ConsoleLink::timerCallback()
    {
        if (! connected) { stopTimer(); return; }
        const auto now = juce::Time::currentTimeMillis();

        // 0. LINK WATCHDOG. A TCP transport's reader exits when the desk goes
        //    away (power cycle, switch reboot, idle timeout) and the transport
        //    then reports !isConnected() -- but nothing asked, so the menu
        //    still said "disconnect <host>" while names and scene markers had
        //    stopped for the rest of the show. Say so and drop the link; a
        //    reconnect stays the engineer's call (TCP connect blocks this
        //    thread for up to 2 s). The stashed patch + gains are kept.
        if (transport != nullptr && ! transport->isConnected())
        {
            const auto dropped = host;
            disconnect();
            lostHost = dropped;
            if (onStatus)
                onStatus (profile.displayName + " link to " + dropped
                          + " lost -- the console stopped answering. Use Console: connect... to reconnect.");
            return;
        }

        // 1. Renew the push subscription BEFORE it expires. Without this the
        //    desk stops pushing ~10 s after connect and every scene recall
        //    silently stops dropping a marker for the rest of the show.
        if (profile.dialect.subscribe && now >= nextSubscribeMs)
        {
            sendMessage (profile.dialect.subscribe());
            nextSubscribeMs = now + kResubscribeMs;
        }

        // 4. Ask again, then say what the answers add up to. First, so a timed-out
        //    restore below is the line left on screen.
        if (nextProbeMs != 0 && now >= nextProbeMs)
        {
            sendMessage (profile.dialect.probe());
            nextProbeMs = now + kProbeMs;
        }
        reportHealthChange (now);

        // 2. Reply-timeout watchdogs. A query is still outstanding past its
        //    deadline -> a reply was lost; abort cleanly so the UI doesn't sit
        //    forever on "Reading console patch..." / "Capturing head-amp gains".
        if (patchDeadlineMs != 0 && now >= patchDeadlineMs)
        {
            patchDeadlineMs = 0;
            patchReadTimedOut();
        }
        if (gainDeadlineMs != 0 && now >= gainDeadlineMs)
        {
            gainDeadlineMs = 0;
            if (gainCheckPending)
            {
                gainCheckPending = false;
                if (onStatus) onStatus ("The desk did not confirm the head-amp restore ("
                                        + juce::String ((int) gainReadBack.size()) + " of "
                                        + juce::String ((int) gains.size())
                                        + " answered). Check the connection and restore again.");
                gainReadBack.clear();
            }
            else if (expectedGainReplies > 0)
            {
                const int got = (int) gains.size();
                expectedGainReplies = 0;
                gainCaptureComplete = false;   // partial -- must NOT be persisted as the show's gains
                if (onStatus) onStatus ("Head-amp capture timed out (" + juce::String (got)
                                        + " of expected received) -- try again.");
                if (onGainCaptureFinished) onGainCaptureFinished (false);
            }
        }

        // 3. A saved SOUNDCHECK is only a claim until this desk has been asked.
        if (! deskAsked && patchRead == PatchRead::None)
        {
            deskAsked = true;
            if (isOnSoundcheckPatch()) startPatchRead (PatchRead::Check);
        }

        // The watchdog above beats for as long as the link is up, so the
        // timer only stops in disconnect().
    }

    ConsoleLink::Health ConsoleLink::getHealth() const
    {
        if (! profile.dialect.probe) return Health::CannotTell;
        if (lastAnswerMs == 0)       return Health::NotAnswered;
        return juce::Time::currentTimeMillis() - lastAnswerMs > kDeskSilentMs ? Health::Silent
                                                                              : Health::Answering;
    }

    juce::String ConsoleLink::healthText() const
    {
        switch (getHealth())
        {
            case Health::CannotTell:  return "this link can't tell whether a desk is there";
            case Health::NotAnswered: return "no answer from the desk yet";
            case Health::Answering:   return "desk answering";
            case Health::Silent:      return "desk not answering";
        }
        return {};
    }

    // Once per change: this runs every second for as long as a link is open.
    void ConsoleLink::reportHealthChange (juce::int64 now)
    {
        const auto health = getHealth();
        const auto who = profile.displayName + " at " + host;
        if (health == Health::NotAnswered)
        {
            if (! neverAnsweredReported && connectedAtMs != 0 && now - connectedAtMs > kDeskSilentMs)
            {
                neverAnsweredReported = true;
                if (onStatus) onStatus (who + " has not answered. Names and scene markers need a desk at "
                                        "that address.");
            }
            return;
        }
        if (health == reportedHealth) return;
        const auto previous = reportedHealth;
        reportedHealth = health;
        if (! onStatus) return;
        if (health == Health::Silent)
            onStatus (who + " stopped answering -- names and scene markers have stopped.");
        else if (health == Health::Answering && previous == Health::Silent)
            onStatus (who + " is answering again.");
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
        if (patch == Patch::Stage)           obj->setProperty ("deskPatch", "stage");
        else if (patch == Patch::Soundcheck) obj->setProperty ("deskPatch", "soundcheck");

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

        // What this run knows about the desk outranks any file. The file's array
        // is usually -1s (a capture with no repatch), so loading it over a live
        // stash used to erase the only record of the show patch.
        const auto* patchArr = v.getProperty ("stagePatch", {}).getArray();
        const auto deskPatch = v.getProperty ("deskPatch", "").toString();
        if (! hasStagePatch() && patchRead == PatchRead::None && profile.numInBlocks > 0
            && patchArr != nullptr && patchArr->size() == profile.numInBlocks)
        {
            std::map<int, int> saved;
            for (int b = 0; b < patchArr->size(); ++b)
                if (const auto& value = (*patchArr)[b]; value.isInt() && isRoutingValue ((int) value))
                    saved[b] = (int) value;
            if ((int) saved.size() == profile.numInBlocks)
                stagePatch = std::move (saved);
            else if (deskPatch == "soundcheck" && onStatus)
                onStatus ("This session says the desk is on SOUNDCHECK, but its saved show patch is damaged, "
                          "so STAGE can't restore it. Set the routing on the desk.");
        }
        // A file from before deskPatch has a stash only if a soundcheck began,
        // and nothing says it ended.
        if (patch == Patch::Unknown && hasStagePatch())
            patch = deskPatch == "stage" ? Patch::Stage : Patch::Soundcheck;

        const auto* gainArr = v.getProperty ("gains", {}).getArray();
        if (gains.empty() && expectedGainReplies == 0 && gainArr != nullptr)
        {
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

    void ConsoleLink::setSessionDir (const juce::File& dir, bool mayReadDisk)
    {
        if (dir == sessionDir)
        {
            if (mayReadDisk && stateLoadPending)
            {
                stateLoadPending = false;
                loadSessionState (dir);
            }
            return;
        }
        sessionDir = dir;

        // Gains and any saved patch belong to the session just left. A desk on
        // card returns is different: it is still there whatever session is open,
        // and this stash is what brings it back.
        gains.clear();
        gainCaptureComplete = false;
        expectedGainReplies = 0;
        gainCheckPending = false;
        gainReadBack.clear();
        gainDeadlineMs = 0;
        if (patch == Patch::Soundcheck)
        {
            if (onStatus) onStatus ("The console is still on SOUNDCHECK from the last session. "
                                    "Going back to STAGE restores that session's show patch.");
        }
        else
        {
            stagePatch.clear();
            patchRead = PatchRead::None;
            deskRead.clear();
            patchDeadlineMs = 0;
            patch = Patch::Unknown;
        }
        stateLoadPending = ! mayReadDisk;
        if (mayReadDisk) loadSessionState (dir);
    }

    void ConsoleLink::loadSessionState (const juce::File& dir)
    {
        const bool carried = patch == Patch::Soundcheck;
        if (dir.isDirectory()) loadFrom (dir);
        // A relaunch from here has only this file to go on.
        if (carried) persistPatch();
    }

    // The patch half of the state file, written when the desk moves. Gains keep
    // their own rule (only a finished capture is saved), so an unfinished one
    // here must not replace what the file holds. During a take this stays off
    // the thread that STOP runs on.
    bool ConsoleLink::persistPatch()
    {
        const bool onSoundcheck = patch == Patch::Soundcheck;
        juce::Array<juce::File> others;
        if (! onSoundcheck)
        {
            others.swapWith (sessionsOnSoundcheck);
            others.removeAllInstancesOf (sessionDir);
        }

        const auto save = [dir = sessionDir, state = toJson(), keepFileGains = ! gainCaptureComplete, others]
        {
            for (const auto& other : others)
            {
                const auto f = other.getChildFile (kStateFileName);
                const auto saved = juce::JSON::parse (f);
                if (auto* object = saved.getDynamicObject())
                {
                    object->setProperty ("deskPatch", "stage");
                    atomicfile::writeText (f, juce::JSON::toString (saved));
                }
            }
            if (dir == juce::File()) return true;
            if (! dir.isDirectory()) return false;
            const auto f = dir.getChildFile (kStateFileName);
            if (keepFileGains)
                if (auto* object = state.getDynamicObject())
                    object->setProperty ("gains", juce::JSON::parse (f).getProperty ("gains", juce::Array<juce::var>()));
            return atomicfile::writeText (f, juce::JSON::toString (state));
        };

        const bool saved = takeIsHoldingDesk() ? juce::Thread::launch (save) : save();
        if (saved && onSoundcheck && sessionDir != juce::File())
            sessionsOnSoundcheck.addIfNotAlreadyThere (sessionDir);
        return saved;
    }

    bool ConsoleLink::saveTo (const juce::File& sessionDir) const
    {
        if (! sessionDir.isDirectory()) return false;
        return atomicfile::writeText (sessionDir.getChildFile (kStateFileName),
                                      juce::JSON::toString (toJson()));
    }

    void ConsoleLink::saveInBackground (const juce::File& sessionDir) const
    {
        juce::Thread::launch ([file = sessionDir.getChildFile (kStateFileName),
                               json = juce::JSON::toString (toJson())]
                              { atomicfile::writeText (file, json); });
    }

    bool ConsoleLink::loadFrom (const juce::File& sessionDir)
    {
        const auto f = sessionDir.getChildFile (kStateFileName);
        if (! f.existsAsFile()) return false;
        const bool loaded = loadJson (juce::JSON::parse (f));
        if (patch == Patch::Soundcheck)
        {
            sessionsOnSoundcheck.addIfNotAlreadyThere (sessionDir);
            deskAsked = false;
        }
        return loaded;
    }
}
