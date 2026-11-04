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

    // A modal swallows the click on STOP and takes Space, so nothing modal
    // opens over a rolling take. A take can still start behind one: a desk,
    // OSC, twin or daemon RECORD, or a punch window. A modal marked here closes
    // when that happens (MainComponent::serviceModalsAroundTakes), as if
    // cancelled. Mark only what may be dropped that way: a settings panel that
    // applies as it goes, or a notice that can be shown again.
    inline const juce::Identifier kClosesForTake { "zfClosesForTake" };

    inline void closesForTake (juce::Component* modal)
    {
        if (modal != nullptr) modal->getProperties().set (kClosesForTake, true);
    }

    inline juce::DialogWindow* launchClosesForTake (juce::DialogWindow::LaunchOptions& opts)
    {
        auto* window = opts.launchAsync();
        closesForTake (window);
        return window;
    }

    // Closes every marked modal that is open. Returns how many.
    inline int closeModalsForTake()
    {
        auto& modals = *juce::ModalComponentManager::getInstance();
        juce::Array<juce::Component::SafePointer<juce::Component>> marked;
        for (int i = 0; i < modals.getNumModalComponents(); ++i)
            if (auto* modal = modals.getModalComponent (i))
                if ((bool) modal->getProperties()[kClosesForTake]) marked.add (modal);
        for (auto& modal : marked)
            if (modal != nullptr) modal->exitModalState (0);
        return marked.size();
    }
}
