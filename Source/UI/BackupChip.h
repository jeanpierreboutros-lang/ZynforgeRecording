#pragma once

#include <juce_graphics/juce_graphics.h>

#include "../Theme/BrandColors.h"

// The header's BACKUP chip. "Backup not configured" is the first problem
// pre-flight names, so the chip says at a glance whether there is a backup and
// how it is going: a state dot beside the word. The chip itself stays neutral,
// as every header chip does until engaged; only the dot carries colour.
namespace zynforge::backupchip
{
    enum class State
    {
        Off,       // no backup folder: amber, BACKUP OFF
        Idle,      // configured, its drive answering, no take: steel
        Writing,   // a take is writing to it: green
        Trouble,   // configured but its drive isn't answering, or a take isn't writing it: amber, BACKUP !
        Failed     // a backup write failed (the banner's BACKUP WRITE FAILED): red, BACKUP !
    };

    struct Facts
    {
        bool configured { false };
        bool recording { false };      // a take is rolling
        bool backupActive { false };   // the take's backup copy is open and writing
        bool backupFailed { false };   // a backup write failed, this take or the last one
        bool driveAnswering { false }; // between takes: the probe read the folder recently
    };

    inline State stateFor (const Facts& f) noexcept
    {
        if (! f.configured)  return State::Off;
        if (f.backupFailed)  return State::Failed;
        if (f.recording)     return f.backupActive ? State::Writing : State::Trouble;
        return f.driveAnswering ? State::Idle : State::Trouble;
    }

    inline juce::String labelFor (State s)
    {
        switch (s)
        {
            case State::Off:     return "BACKUP OFF";
            case State::Trouble:
            case State::Failed:  return "BACKUP !";
            case State::Idle:
            case State::Writing: break;
        }
        return "BACKUP";
    }

    // What VoiceOver says, since Idle and Writing share a word.
    inline juce::String describe (State s)
    {
        switch (s)
        {
            case State::Off:     return "No backup folder is set";
            case State::Idle:    return "Backup folder ready";
            case State::Writing: return "Backup is writing";
            case State::Trouble: return "Backup is not writing or its drive is not answering";
            case State::Failed:  return "A backup write failed";
        }
        return {};
    }

    inline juce::Colour dotFor (State s) noexcept
    {
        switch (s)
        {
            case State::Off:
            case State::Trouble: return brand::alertAmber;
            case State::Writing: return brand::accentStatus;
            case State::Failed:  return brand::accentRecord;
            case State::Idle:    break;
        }
        return brand::debossFace;
    }
}
