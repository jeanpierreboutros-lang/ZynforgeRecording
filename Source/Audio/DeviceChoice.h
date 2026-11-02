#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "DisplayText.h"

// Which device a take records from, and whether that is the one the engineer
// chose. A wrong device looks perfectly healthy: the meters move and the files
// are good, they just hold the room mic. So this says nothing about signal. It
// compares names and ids, and every rule here is plain data in, answer out, so
// each case can be tested with no hardware.
namespace zynforge::devicechoice
{
    // One device as the system lists it right now. `name` is what JUCE calls it
    // (it numbers duplicates), `uid` is CoreAudio's id, empty when unknown.
    struct Listed
    {
        juce::String name, uid;
    };
    using DeviceList = std::vector<Listed>;

    // `saved` is what separates "never chose" from a choice of no input.
    struct Chosen
    {
        bool saved { false };
        juce::String input, output;
        juce::String inputUid, outputUid;
        int inputs { 0 };   // active inputs when chosen
    };

    inline Chosen load (const juce::PropertySet& props)
    {
        Chosen c;
        c.saved     = props.containsKey ("chosenInputDevice");
        c.input     = props.getValue ("chosenInputDevice");
        c.output    = props.getValue ("chosenOutputDevice");
        c.inputUid  = props.getValue ("chosenInputUid");
        c.outputUid = props.getValue ("chosenOutputUid");
        c.inputs    = props.getIntValue ("chosenInputCount");
        return c;
    }

    inline void save (juce::PropertySet& props, const Chosen& c)
    {
        props.setValue ("chosenInputDevice",  c.input);
        props.setValue ("chosenOutputDevice", c.output);
        props.setValue ("chosenInputUid",     c.inputUid);
        props.setValue ("chosenOutputUid",    c.outputUid);
        props.setValue ("chosenInputCount",   c.inputs);
    }

    // The name a saved device goes by now, or empty when it is not here. The id
    // wins: it survives a rename, and it tells two identical units apart when
    // JUCE's numbering swaps them. A name never matches loosely, because a near
    // match that is wrong is the failure this file exists to catch.
    inline juce::String nameNow (const juce::String& name, const juce::String& uid, const DeviceList& listed)
    {
        if (uid.isNotEmpty())
            for (const auto& device : listed)
                if (device.uid == uid) return device.name;
        for (const auto& device : listed)
            if (device.name == name) return device.name;
        return {};
    }

    struct OpenNames
    {
        juce::String input, output;
    };

    // What to open at launch. Each side stands alone: JUCE fails the whole open
    // over one missing name, and a monitor output that went away must not cost
    // the input the take records from.
    inline OpenNames planOpen (const Chosen& chosen, const DeviceList& inputs, const DeviceList& outputs,
                               const juce::String& defaultInput, const juce::String& defaultOutput)
    {
        OpenNames open;
        const auto inputNow  = nameNow (chosen.input,  chosen.inputUid,  inputs);
        const auto outputNow = nameNow (chosen.output, chosen.outputUid, outputs);
        open.input  = chosen.input.isEmpty()  ? juce::String()
                    : inputNow.isNotEmpty()   ? inputNow  : defaultInput;
        open.output = outputNow.isNotEmpty()  ? outputNow : defaultOutput;
        return open;
    }

    enum class Verdict
    {
        onChosen,        // the chosen device is the open input
        nothingChosen,   // no choice has been saved, so this is the Mac's default
        chosenMissing,   // the chosen device is not connected
        chosenNotOpen    // connected, but something else is open
    };

    struct DeviceState
    {
        Verdict verdict { Verdict::nothingChosen };
        juce::String active;       // the open input device, empty when none
        juce::String chosen;       // as saved
        juce::String chosenNow;    // what it is called now, empty when missing
        int liveInputs { 0 };
        int chosenInputs { 0 };

        // An aggregate that lost a member stays listed under its name with fewer
        // inputs, and its channels move down: nothing wraps, every strip just
        // hears a different channel. Count is the one sign of that.
        bool inputsShrank() const noexcept
        {
            return verdict == Verdict::onChosen && chosenInputs > 0 && liveInputs < chosenInputs;
        }
    };

    inline DeviceState assess (const Chosen& chosen, const juce::String& openInput,
                               const DeviceList& inputs, int liveInputs)
    {
        DeviceState state;
        state.active = openInput;
        state.chosen = chosen.input;
        state.liveInputs = liveInputs;
        state.chosenInputs = chosen.inputs;
        if (! chosen.saved) return state;
        if (chosen.input.isEmpty())
        {
            if (openInput.isEmpty()) state.verdict = Verdict::onChosen;
            return state;
        }
        state.chosenNow = nameNow (chosen.input, chosen.inputUid, inputs);
        state.verdict = state.chosenNow.isEmpty() ? Verdict::chosenMissing
                      : state.chosenNow == openInput ? Verdict::onChosen
                                                     : Verdict::chosenNotOpen;
        return state;
    }

    // The screen stays calm for a device nobody has chosen yet: RECORD still
    // asks, but nothing is wrong until a choice exists to be wrong about.
    inline bool isWrongDevice (const DeviceState& state) noexcept
    {
        return state.verdict == Verdict::chosenMissing || state.verdict == Verdict::chosenNotOpen
            || state.inputsShrank();
    }

    // Marked as well as coloured, so it doesn't rest on colour alone.
    inline juce::String labelText (const DeviceState& state)
    {
        const auto device = state.active.isNotEmpty() ? state.active : juce::String ("no device");
        return (isWrongDevice (state) ? "! In: " : "In: ") + device;
    }

    inline juce::String wrongDeviceLine (const DeviceState& state)
    {
        const auto active = state.active.isNotEmpty() ? state.active : juce::String ("no device");
        switch (state.verdict)
        {
            case Verdict::chosenMissing:
                return state.chosen + " is not connected -- using " + active;
            case Verdict::chosenNotOpen:
                return state.chosenNow + " is connected but not open -- using " + active;
            case Verdict::onChosen:
                if (state.inputsShrank())
                    return active + " has " + counted (state.liveInputs, "input") + ", down from "
                         + juce::String (state.chosenInputs);
                break;
            case Verdict::nothingChosen: break;
        }
        return {};
    }

    struct ArmedStrip
    {
        int strip { 0 };
        int requested { 0 };   // TrackState::inputRouting
    };

    // Armed strips that would record an input other than the one they ask for:
    // the wrap onto a smaller device, or a strip with no input past its end,
    // which applyRecordAutoRouting sends the same way. Two strips patched to one
    // input on purpose are not counted. A wrap that lands on an unused input is,
    // because that strip still records the wrong channel.
    inline int stripsThroughWrap (const std::vector<ArmedStrip>& armed, int numInputs) noexcept
    {
        if (numInputs <= 0) return 0;
        int count = 0;
        for (const auto& s : armed)
            if (s.requested >= 0 ? s.requested >= numInputs : s.strip >= numInputs) ++count;
        return count;
    }

    // Each answer is good for one session on one device. It belongs to the
    // device the question named, so a question left open while the device
    // changed answers nothing about the new one.
    class Answers
    {
    public:
        bool contains (const juce::String& session, const juce::String& device) const
        {
            for (const auto& answer : answers)
                if (answer.first == session && answer.second == device) return true;
            return false;
        }

        void add (const juce::String& session, const juce::String& device)
        {
            if (! contains (session, device)) answers.emplace_back (session, device);
        }

        // A fresh take has no session until it starts.
        void move (const juce::String& from, const juce::String& to, const juce::String& device)
        {
            answers.erase (std::remove (answers.begin(), answers.end(), std::make_pair (from, device)), answers.end());
            add (to, device);
        }

        void clear() { answers.clear(); }

    private:
        std::vector<std::pair<juce::String, juce::String>> answers;
    };

    struct RecordQuestion
    {
        bool needed { false };     // RECORD must not roll on this press
        bool answered { false };   // it would have been needed, and was answered
        juce::String device;       // the device an answer belongs to
        juce::String text;         // the question
        juce::String refusal;      // the same thing in a line, for a press from elsewhere
    };

    inline RecordQuestion recordQuestion (const DeviceState& state, int wrappedStrips,
                                          const Answers& answers, const juce::String& session)
    {
        RecordQuestion question;
        juce::StringArray body;
        juce::String line;
        const auto active = state.active.isNotEmpty() ? state.active : juce::String ("no device");

        if (state.verdict == Verdict::nothingChosen)
        {
            body.add ("No input device has been chosen yet. RECORD would capture from " + active + ".");
            line = "no input device has been chosen";
        }
        else if (isWrongDevice (state))
        {
            line = wrongDeviceLine (state);
            body.add (state.verdict == Verdict::chosenMissing
                          ? "The device you chose, " + state.chosen + ", is not connected."
                      : state.verdict == Verdict::chosenNotOpen
                          ? "The device you chose, " + state.chosenNow + ", is connected but is not the one open."
                          : line + ". Its channels may not be where you patched them.");
            if (state.verdict != Verdict::onChosen)
                body.add ("RECORD would capture from " + active + ".");
        }
        if (wrappedStrips > 0)
        {
            const auto wrap = counted (wrappedStrips, "armed strip") + " ask for inputs " + active + " does not have";
            body.add (wrap + ". It has " + counted (state.liveInputs, "input")
                      + ", so those strips would record whichever input is left, and can share one.");
            if (line.isEmpty()) line = wrap;
        }
        if (body.isEmpty()) return question;

        question.device = state.active;
        if (answers.contains (session, state.active))
        {
            question.answered = true;
            return question;
        }
        question.needed = true;
        question.text = body.joinIntoString ("\n\n");
        question.refusal = line;
        return question;
    }

    // Reopening restarts the device, so it waits until nothing could notice:
    // never during a take, and never over a panel the engineer is trying
    // devices in, which would be fighting them for it.
    inline bool shouldReopen (const DeviceState& state, bool transportQuiet) noexcept
    {
        return transportQuiet && state.verdict == Verdict::chosenNotOpen;
    }

    // A device can be listed before it can run, and each attempt that fails
    // interrupts whatever is open. So a failure backs off rather than repeats.
    class ReopenPacing
    {
    public:
        bool due (juce::uint32 nowMs) const noexcept
        {
            return retryAtMs == 0 || (juce::int32) (nowMs - retryAtMs) >= 0;
        }

        void failed (juce::uint32 nowMs) noexcept
        {
            delayMs = delayMs == 0 ? kFirstRetryMs : juce::jmin (delayMs * 2, kLongestRetryMs);
            retryAtMs = juce::jmax ((juce::uint32) 1, nowMs + delayMs);
        }

        void reset() noexcept { retryAtMs = 0; delayMs = 0; }

    private:
        static constexpr juce::uint32 kFirstRetryMs = 2000, kLongestRetryMs = 60000;
        juce::uint32 retryAtMs { 0 }, delayMs { 0 };
    };
}
