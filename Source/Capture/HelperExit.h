#pragma once

#include <sys/types.h>

namespace zynforge::capture::helperexit
{
    // When a signal ends the helper's take at once, rather than waiting for the
    // window's STOP. One SIGTERM or SIGINT from a person (Activity Monitor,
    // kill, Ctrl-C in the window's Terminal) leaves a rolling take alone, as a
    // refused Quit does; a second one insists. launchd sends one SIGTERM at
    // logout, restart and shutdown and a SIGKILL a few seconds later, never a
    // second SIGTERM: waiting then would only lose the take's ending.
    inline bool finishTakeNow (int signalsSeen, bool fromSystem) noexcept
    {
        return signalsSeen >= 2 || fromSystem;
    }

    // launchd is process 1.
    inline bool fromSystem (pid_t sender) noexcept
    {
        return sender == 1;
    }
}
