#pragma once

#include <juce_core/juce_core.h>

#include "../Audio/EngineStatus.h"

namespace zynforge::capture
{
    // Wire protocol for the headless capture daemon <-> GUI client (Phase 1 of
    // the capture-process split; see decisions.md). Newline-delimited JSON
    // over a local socket: each message is one JSON object on its own line,
    // tagged with "type" (cmd / reply / status). Versioned via a Hello
    // handshake so a GUI and a daemon built at different versions fail LOUDLY
    // instead of silently mis-recording.
    //
    // This is the CONTRACT only -- the socket transport and the daemon that
    // owns the recorder come in later Phase-1 chunks. Header-only + engine-free
    // (depends only on the serialisable EngineStatus), so both processes share
    // it and it's testable in isolation -- the same transport-seam-first
    // approach used for ConsoleLink before any hardware existed.

    inline constexpr int kProtocolVersion = 1;

    enum class Action
    {
        Hello,            // version handshake (client -> daemon, daemon replies)
        StartRecording,   // sessionDir
        StopRecording,
        StartPlayback,
        StopPlayback,
        ArmTrack,         // trackIndex, boolValue
        SetCaptureFormat, // intValue = CaptureFormat
        SetTrackCount,    // intValue = number of tracks
        SetSessionDir,    // sessionDir
        Ping,             // liveness
        Quit              // ask an IDLE daemon to exit (refused mid-take)
    };

    inline juce::String actionToString (Action a)
    {
        switch (a)
        {
            case Action::Hello:            return "hello";
            case Action::StartRecording:   return "startRecording";
            case Action::StopRecording:    return "stopRecording";
            case Action::StartPlayback:    return "startPlayback";
            case Action::StopPlayback:     return "stopPlayback";
            case Action::ArmTrack:         return "armTrack";
            case Action::SetCaptureFormat: return "setCaptureFormat";
            case Action::SetTrackCount:    return "setTrackCount";
            case Action::SetSessionDir:    return "setSessionDir";
            case Action::Ping:             return "ping";
            case Action::Quit:             return "quit";
        }
        return "ping";
    }

    inline Action actionFromString (const juce::String& s, bool& ok)
    {
        ok = true;
        if (s == "hello")            return Action::Hello;
        if (s == "startRecording")   return Action::StartRecording;
        if (s == "stopRecording")    return Action::StopRecording;
        if (s == "startPlayback")    return Action::StartPlayback;
        if (s == "stopPlayback")     return Action::StopPlayback;
        if (s == "armTrack")         return Action::ArmTrack;
        if (s == "setCaptureFormat") return Action::SetCaptureFormat;
        if (s == "setTrackCount")    return Action::SetTrackCount;
        if (s == "setSessionDir")    return Action::SetSessionDir;
        if (s == "ping")             return Action::Ping;
        if (s == "quit")             return Action::Quit;
        ok = false;
        return Action::Ping;
    }

    // GUI -> daemon control message.
    struct Command
    {
        Action       action     { Action::Ping };
        juce::String sessionDir;
        int          trackIndex { -1 };
        bool         boolValue  { false };
        int          intValue   { 0 };
        int          version    { kProtocolVersion };   // meaningful for Hello
        int          id         { 0 };   // request-correlation id (0 = none)

        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("type",   "cmd");
            o->setProperty ("action", actionToString (action));
            if (sessionDir.isNotEmpty()) o->setProperty ("sessionDir", sessionDir);
            if (trackIndex >= 0)         o->setProperty ("trackIndex", trackIndex);
            o->setProperty ("boolValue", boolValue);
            o->setProperty ("intValue",  intValue);
            if (action == Action::Hello) o->setProperty ("version", version);
            if (id > 0)                  o->setProperty ("id", id);
            return juce::var (o);
        }

        static Command fromJson (const juce::var& v, bool& ok)
        {
            Command c;
            ok = false;
            if (v.getProperty ("type", "").toString() != "cmd") return c;
            bool actionOk = false;
            c.action = actionFromString (v.getProperty ("action", "").toString(), actionOk);
            if (! actionOk) return c;
            c.sessionDir = v.getProperty ("sessionDir", "").toString();
            c.trackIndex = (int) v.getProperty ("trackIndex", -1);
            c.boolValue  = (bool) v.getProperty ("boolValue", false);
            c.intValue   = (int) v.getProperty ("intValue", 0);
            c.version    = (int) v.getProperty ("version", kProtocolVersion);
            c.id         = (int) v.getProperty ("id", 0);
            ok = true;
            return c;
        }
    };

    // daemon -> GUI acknowledgement (esp. the Hello reply).
    struct Reply
    {
        bool         ok       { false };
        int          version  { kProtocolVersion };
        juce::String error;
        int          id       { 0 };   // echoes the Command::id this replies to

        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("type",    "reply");
            o->setProperty ("ok",      ok);
            o->setProperty ("version", version);
            if (error.isNotEmpty()) o->setProperty ("error", error);
            if (id > 0)             o->setProperty ("id", id);
            return juce::var (o);
        }

        static Reply fromJson (const juce::var& v)
        {
            Reply r;
            r.ok      = (bool) v.getProperty ("ok", false);
            r.version = (int) v.getProperty ("version", kProtocolVersion);
            r.error   = v.getProperty ("error", "").toString();
            r.id      = (int) v.getProperty ("id", 0);
            return r;
        }
    };

    // Discriminator on a parsed line: "cmd" / "reply" / "status".
    inline juce::String messageType (const juce::var& v)
    {
        return v.getProperty ("type", "").toString();
    }

    // Status push: an EngineStatus tagged type:"status".
    inline juce::var encodeStatus (const EngineStatus& s)
    {
        auto v = s.toJson();
        if (auto* o = v.getDynamicObject()) o->setProperty ("type", "status");
        return v;
    }
    inline EngineStatus decodeStatus (const juce::var& v) { return EngineStatus::fromJson (v); }

    // "bye": the daemon tells a client its socket is being closed on PURPOSE
    // (superseded by a newer connection), so the client can distinguish an
    // intentional kick from an actual daemon death and not raise a false
    // mid-take "daemon died" alarm.
    inline juce::var encodeBye (const juce::String& reason)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", "bye");
        if (reason.isNotEmpty()) o->setProperty ("reason", reason);
        return juce::var (o);
    }

    // Framing: one compact JSON object per line.
    inline juce::String frame (const juce::var& v) { return juce::JSON::toString (v, true) + "\n"; }

    // Hello handshake: v1 requires an exact version match (the daemon refuses
    // a client it can't speak to rather than risk a silent mis-record).
    inline bool versionsCompatible (int clientVersion, int daemonVersion)
    {
        return clientVersion == daemonVersion;
    }
}
