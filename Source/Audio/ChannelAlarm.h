#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <vector>

// Dead-channel alarm. Quiet is normal in a show (a guest vocal can sit silent
// for an hour), so these alarms key on the signature of a FAULT instead: a mic
// or DI always has a noise floor, while a lost path delivers exact digital
// zero. The audio thread only stores where each input last carried signal;
// Monitor turns that into alarms off the audio thread. It observes and never
// touches the take.
namespace zynforge::channelalarm
{
    // Tier 1: an input that carried signal this take reads exact zero this long.
    inline constexpr double kFaultSeconds = 3.0;
    // Tier 2: this many channels entering tier 1 inside this window is one
    // lost stream (MADI, Dante), shown as a single alarm.
    inline constexpr int    kStreamChannels      = 8;
    inline constexpr double kStreamWindowSeconds = 2.0;
    // Tier 3: an input still at exact zero this far into the take.
    inline constexpr double kNoSignalSeconds = 60.0;

    inline const juce::String kChannel  { "channel" };
    inline const juce::String kStream   { "stream" };
    inline const juce::String kNoSignal { "noSignal" };

    struct Alarm
    {
        juce::String kind;
        juce::String name;              // logical strip; a stream names the strips it hit
        int          strip { -1 };      // the strip's first track, -1 for a stream
        int          channels { 1 };    // physical channels a stream took out
        juce::int64  startSample { 0 }; // on the take's timeline
        juce::int64  endSample { -1 };  // -1 while live
        bool         recovered { false };

        juce::var toJson() const
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("kind", kind);
            o->setProperty ("name", name);
            o->setProperty ("strip", strip);
            o->setProperty ("channels", channels);
            o->setProperty ("startSample", startSample);
            o->setProperty ("endSample", endSample);
            o->setProperty ("recovered", recovered);
            return juce::var (o);
        }

        static Alarm fromJson (const juce::var& v)
        {
            Alarm a;
            a.kind        = v.getProperty ("kind", "").toString();
            a.name        = v.getProperty ("name", "").toString();
            a.strip       = (int) v.getProperty ("strip", -1);
            a.channels    = (int) v.getProperty ("channels", 1);
            a.startSample = (juce::int64) v.getProperty ("startSample", 0);
            a.endSample   = (juce::int64) v.getProperty ("endSample", -1);
            a.recovered   = (bool) v.getProperty ("recovered", false);
            return a;
        }
    };

    // First few names, so a long list cannot crowd out the rest of the banner.
    inline juce::String joinNames (const juce::StringArray& names, int limit = 4)
    {
        juce::StringArray shown;
        for (int i = 0; i < juce::jmin (limit, names.size()); ++i) shown.add (names[i]);
        auto text = shown.joinIntoString (", ");
        if (names.size() > limit) text << " +" << (names.size() - limit) << " more";
        return text;
    }

    // One status-line string for the live alarms, loudest first.
    inline juce::String bannerText (const std::vector<Alarm>& live)
    {
        juce::String text;
        juce::StringArray faults, notices;
        for (const auto& a : live)
        {
            if (a.kind == kStream)
                text << "!! INPUT STREAM LOST -- " << a.channels
                     << " channels at digital zero: " << a.name << "  ";
            else if (a.kind == kChannel) faults.add (a.name);
            else if (a.kind == kNoSignal) notices.add (a.name);
        }
        if (! faults.isEmpty())
            text << "! CHANNEL FAULT -- input lost, digital zero: " << joinNames (faults) << "  ";
        if (! notices.isEmpty())
            text << "No signal yet this take: " << joinNames (notices) << "  ";
        return text;
    }

    class Monitor
    {
    public:
        // One entry per physical input. Both halves of a stereo pair share the
        // strip index and name, so a pair raises one alarm, not two.
        struct Input { int strip { -1 }; juce::String name; };

        void begin (std::vector<Input> channelInputs, double sampleRateHz)
        {
            inputs = std::move (channelInputs);
            rate   = sampleRateHz;
            alarms.clear();
            openAlarm.assign (inputs.size(), -1);
            inStream.assign (inputs.size(), 0);
            openStream = -1;
        }

        // signalEnd[i]: the frame just after the last block with signal on
        // input i, or -1 if it has had none. now: frames delivered so far.
        // Reading the two at slightly different moments is harmless: the
        // skew is a block or two against thresholds of whole seconds.
        void update (const std::vector<juce::int64>& signalEnd, juce::int64 now)
        {
            jassert (signalEnd.size() == inputs.size());
            const auto faultFrames  = framesFor (kFaultSeconds);
            const auto windowFrames = framesFor (kStreamWindowSeconds);

            std::vector<Fault> faults;
            for (std::size_t i = 0; i < inputs.size(); ++i)
                if (signalEnd[i] >= 0 && now - signalEnd[i] >= faultFrames)
                    faults.push_back ({ signalEnd[i], i });
            std::sort (faults.begin(), faults.end(),
                       [] (const Fault& a, const Fault& b) { return a.start < b.start; });

            std::fill (inStream.begin(), inStream.end(), (char) 0);
            for (std::size_t i = 0; i + kStreamChannels <= faults.size(); ++i)
                if (faults[i + kStreamChannels - 1].start - faults[i].start <= windowFrames)
                    for (std::size_t m = i; m < i + kStreamChannels; ++m)
                        inStream[faults[m].input] = 1;

            for (std::size_t first = 0, last = 0; first < inputs.size(); first = last)
            {
                last = first + 1;
                while (last < inputs.size() && inputs[last].strip == inputs[first].strip) ++last;
                updateStrip (first, last, signalEnd, now, faultFrames);
            }
            updateStream (faults, now);
        }

        // The take ended: whatever is still live ends with it, unrecovered.
        void finish (juce::int64 now)
        {
            for (auto& a : alarms)
                if (a.endSample < 0) a.endSample = now;
            std::fill (openAlarm.begin(), openAlarm.end(), -1);
            std::fill (inStream.begin(), inStream.end(), (char) 0);
            openStream = -1;
        }

        // What should show right now. A stream's member channels stay inside
        // its one alarm. `origin` turns frames into timeline positions.
        std::vector<Alarm> live (juce::int64 origin) const
        {
            std::vector<Alarm> out;
            if (openStream >= 0) out.push_back (positioned (alarms[(std::size_t) openStream], origin));
            for (std::size_t first = 0, last = 0; first < inputs.size(); first = last)
            {
                bool streamed = false;
                for (last = first; last < inputs.size() && inputs[last].strip == inputs[first].strip; ++last)
                    if (inStream[last] != 0) streamed = true;
                if (openAlarm[first] >= 0 && ! streamed)
                    out.push_back (positioned (alarms[(std::size_t) openAlarm[first]], origin));
            }
            std::stable_sort (out.begin(), out.end(), [] (const Alarm& a, const Alarm& b)
                              { return rank (a) < rank (b); });
            return out;
        }

        // Every alarm raised this take, recovered or not, in the order raised.
        std::vector<Alarm> history (juce::int64 origin) const
        {
            std::vector<Alarm> out;
            for (const auto& a : alarms) out.push_back (positioned (a, origin));
            return out;
        }

    private:
        struct Fault { juce::int64 start; std::size_t input; };

        static int rank (const Alarm& a) noexcept
        { return a.kind == kStream ? 0 : a.kind == kChannel ? 1 : 2; }

        juce::int64 framesFor (double seconds) const noexcept
        { return (juce::int64) std::ceil (seconds * rate); }

        static Alarm positioned (Alarm a, juce::int64 origin) noexcept
        {
            a.startSample += origin;
            if (a.endSample >= 0) a.endSample += origin;
            return a;
        }

        void updateStrip (std::size_t first, std::size_t last,
                          const std::vector<juce::int64>& signalEnd,
                          juce::int64 now, juce::int64 faultFrames)
        {
            juce::String kind;
            juce::int64 start = 0, latestSignal = -1;
            for (auto i = first; i < last; ++i)
            {
                const auto end = signalEnd[i];
                latestSignal = juce::jmax (latestSignal, end);
                if (end >= 0 && now - end >= faultFrames)
                {
                    start = kind == kChannel ? juce::jmin (start, end) : end;
                    kind  = kChannel;
                }
                else if (end < 0 && now >= framesFor (kNoSignalSeconds) && kind.isEmpty())
                    kind = kNoSignal;
            }

            auto& open = openAlarm[first];
            if (open >= 0 && alarms[(std::size_t) open].kind != kind)
            {
                auto& ended = alarms[(std::size_t) open];
                ended.endSample = latestSignal >= 0 ? latestSignal : now;
                ended.recovered = true;
                open = -1;
            }
            if (kind.isNotEmpty() && open < 0)
            {
                open = (int) alarms.size();
                Alarm raised;
                raised.kind = kind;
                raised.name = inputs[first].name;
                raised.strip = inputs[first].strip;
                raised.startSample = start;
                alarms.push_back (std::move (raised));
            }
        }

        void updateStream (const std::vector<Fault>& faults, juce::int64 now)
        {
            int members = 0;
            juce::int64 earliest = now;
            juce::StringArray names;
            for (const auto& fault : faults)
                if (inStream[fault.input] != 0)
                {
                    ++members;
                    earliest = juce::jmin (earliest, fault.start);
                    names.addIfNotAlreadyThere (inputs[fault.input].name);
                }

            if (members >= kStreamChannels)
            {
                if (openStream < 0)
                {
                    openStream = (int) alarms.size();
                    Alarm raised;
                    raised.kind = kStream;
                    raised.startSample = earliest;
                    alarms.push_back (std::move (raised));
                }
                auto& stream = alarms[(std::size_t) openStream];
                stream.channels = juce::jmax (stream.channels, members);
                stream.name = joinNames (names);
            }
            else if (openStream >= 0)
            {
                auto& stream = alarms[(std::size_t) openStream];
                stream.endSample = now;
                stream.recovered = true;
                openStream = -1;
            }
        }

        std::vector<Input> inputs;
        double rate { 48000.0 };
        std::vector<Alarm> alarms;
        std::vector<int>   openAlarm;   // by a strip's first input, -1 = none
        std::vector<char>  inStream;    // by input
        int                openStream { -1 };
    };
}
