#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace zynforge
{
    // Launch DialogWindow content as a NON-MODAL floating window.
    //
    // DialogWindow::LaunchOptions::launchAsync() calls enterModalState(), which
    // makes the window application-modal: it blocks the main window AND
    // swallows clicks meant for other controls -- so a header "tab" button
    // can't be clicked again to toggle the window closed, and the engineer
    // can't keep using the mixer while the panel is open. These tool panels
    // (meterbridge, patch, device) are meant to float alongside the app, so
    // launch them non-modally instead: create the same window, add it to the
    // desktop, and show it -- no modal state.
    //
    // The default close box just hides the window (DefaultDialogWindow::
    // closeButtonPressed -> setVisible(false)); the owner is responsible for
    // deleting it (e.g. on the next toggle, or when it polls isVisible()).
    inline juce::DialogWindow* launchFloating (juce::DialogWindow::LaunchOptions& opts)
    {
        auto* d = opts.create();
        if (! d->isOnDesktop())
            d->addToDesktop (d->getDesktopWindowStyleFlags());
        d->setVisible (true);
        d->toFront (true);
        return d;
    }
}
