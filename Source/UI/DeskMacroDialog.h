#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../Audio/AudioEngine.h"
#include "../Audio/SettingsFile.h"
#include "../Network/DeskMacro.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // Setup for the desk macro listener (Source/Network/DeskMacro.h): the port,
    // the one desk it answers, whether it is on, and the addresses to paste into
    // a console macro. Listening never survives a relaunch; the port and desk
    // address do.
    class DeskMacroDialog
    {
    public:
        static void launch (AudioEngine& engine);
    };

    namespace deskmacro_detail
    {
        inline juce::String addressList()
        {
            juce::StringArray lines;
            for (const auto& entry : deskmacro::kAddresses) lines.add (entry.address);
            return lines.joinIntoString ("\n");
        }

        class Panel final : public juce::Component, private juce::Timer
        {
        public:
            explicit Panel (AudioEngine& e) : engine (e)
            {
                auto label = [this] (juce::Label& l, const juce::String& text)
                {
                    l.setText (text, juce::dontSendNotification);
                    dialog::styleFieldLabel (l);
                    addAndMakeVisible (l);
                };
                label (portKey, "Listen Port");
                label (ipKey, "Desk IP");
                label (addressKey, "Addresses for the console macro");

                auto* props = engine.getAppProps();
                dialog::styleTextEditor (portEd);
                portEd.setInputRestrictions (5, "0123456789");
                portEd.setText (juce::String (props != nullptr ? props->getIntValue ("deskMacroPort", deskmacro::kDefaultPort)
                                                               : deskmacro::kDefaultPort),
                                juce::dontSendNotification);
                portEd.setTitle ("Desk macro listen port");
                addAndMakeVisible (portEd);

                dialog::styleTextEditor (ipEd);
                ipEd.setText (props != nullptr ? props->getValue ("deskMacroIp") : juce::String(),
                              juce::dontSendNotification);
                ipEd.setTitle ("Desk IP address");
                ipEd.setDescription ("Datagrams from any other address are dropped and counted.");
                addAndMakeVisible (ipEd);

                enableToggle.setButtonText ("Listen for desk macros");
                enableToggle.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                enableToggle.setColour (juce::ToggleButton::tickColourId, brand::accentStatus);
                enableToggle.onClick = [this] { applyEnable(); };
                addAndMakeVisible (enableToggle);

                stateL.setFont (brand::type::caption());
                stateL.setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (stateL);

                addressView.setMultiLine (true);
                addressView.setReadOnly (true);
                addressView.setCaretVisible (false);
                addressView.setFont (brand::type::mono (12.0f));
                dialog::styleTextEditor (addressView);
                addressView.setText (addressList(), false);
                addressView.setTitle ("Desk macro addresses");
                addAndMakeVisible (addressView);

                copyB.setButtonText ("Copy Addresses");
                dialog::styleSecondary (copyB);
                copyB.onClick = [] { juce::SystemClipboard::copyTextToClipboard (addressList()); };
                addAndMakeVisible (copyB);

                noteL.setText ("Send each as an address alone or with an integer. Integer messages are the "
                               "most reliable type on DiGiCo. 0 is ignored (a button release); any other "
                               "integer fires. marker/goto takes the marker number, from 1.",
                               juce::dontSendNotification);
                noteL.setFont (brand::type::caption());
                noteL.setColour (juce::Label::textColourId, brand::textMuted);
                noteL.setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (noteL);

                closeB.setButtonText ("Close");
                dialog::styleSecondary (closeB);
                closeB.onClick = [this]
                { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (closeB);

                setSize (520, 520);
                refresh();
                startTimerHz (4);
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, "DESK MACRO CONTROL"); }

            void resized() override
            {
                auto b = dialog::bodyBounds (*this).reduced (brand::space::lg, brand::space::md);
                auto row = [&] (juce::Label& key, juce::Component& value)
                {
                    auto r = b.removeFromTop (brand::space::ctrlH);
                    key.setBounds (r.removeFromLeft (120));
                    value.setBounds (r);
                    b.removeFromTop (brand::space::sm);
                };
                row (portKey, portEd);
                row (ipKey, ipEd);
                enableToggle.setBounds (b.removeFromTop (brand::space::ctrlH));
                stateL.setBounds (b.removeFromTop (44));
                b.removeFromTop (brand::space::xs);
                {
                    auto r = b.removeFromTop (brand::space::ctrlH);
                    copyB.setBounds (r.removeFromRight (130));
                    addressKey.setBounds (r);
                }
                b.removeFromTop (brand::space::xs);
                noteL.setBounds (b.removeFromBottom (48));
                b.removeFromBottom (brand::space::xs);
                addressView.setBounds (b);

                auto footer = dialog::footerBounds (*this);
                closeB.setBounds (footer.removeFromRight (dialog::btnSecond));
            }

        private:
            void timerCallback() override { refresh(); }

            int port() const { return portEd.getText().getIntValue(); }

            void applyEnable()
            {
                auto& remotes = engine.getRemotes();
                problem.clear();
                if (! enableToggle.getToggleState())
                {
                    remotes.stopDeskMacro();
                    refresh();
                    return;
                }
                const auto ip = deskmacro::normaliseSourceIp (ipEd.getText());
                if (engine.areControlsLocked())
                    problem = "The session is locked. Unlock it first.";
                else if (port() < 1024 || port() > 65535)
                    problem = "Enter a UDP port from 1024 to 65535.";
                else if (ip.isEmpty())
                    problem = "Enter the desk's IPv4 address, for example 192.168.1.20.";
                else if (remotes.startDeskMacro (port(), ip))
                    persist (ip);
                else
                    problem = "Could not bind UDP port " + juce::String (port()) + ".";
                refresh();
            }

            void persist (const juce::String& ip)
            {
                ipEd.setText (ip, juce::dontSendNotification);
                if (auto* props = engine.getAppProps())
                {
                    reloadSettingsReplacing (*props);
                    props->setValue ("deskMacroPort", port());
                    props->setValue ("deskMacroIp", ip);
                    props->saveIfNeeded();
                }
            }

            // Always from the engine: LOCK can engage, and the listener can
            // stop, while this is open.
            void refresh()
            {
                const auto& remotes = engine.getRemotes();
                const bool listening = remotes.isDeskMacroListening();
                const bool locked    = engine.areControlsLocked();
                enableToggle.setToggleState (listening, juce::dontSendNotification);
                enableToggle.setEnabled (! locked);
                portEd.setEnabled (! listening && ! locked);
                ipEd.setEnabled (! listening && ! locked);

                juce::String state;
                if (problem.isNotEmpty() && ! listening) state = problem;
                else if (listening)
                {
                    state = "Listening on UDP " + juce::String (remotes.getDeskMacroPort())
                          + " for " + remotes.getDeskMacroSource() + " only.";
                    const auto dropped = remotes.getDeskMacroDropped();
                    if (dropped > 0)
                        state << " Dropped " << dropped << " from elsewhere, latest "
                              << remotes.getDeskMacroLastDropped() << ".";
                }
                else state = "Off. Nothing is listening on this port.";

                if (state != stateL.getText())
                {
                    stateL.setText (state, juce::dontSendNotification);
                    stateL.setColour (juce::Label::textColourId,
                                      problem.isNotEmpty() && ! listening ? brand::accentRecord
                                      : listening                         ? brand::accentStatus
                                                                          : brand::textMuted);
                }
            }

            AudioEngine& engine;
            juce::String problem;
            juce::Label portKey, ipKey, addressKey, stateL, noteL;
            juce::TextEditor portEd, ipEd, addressView;
            juce::ToggleButton enableToggle;
            juce::TextButton copyB, closeB;
        };
    }

    inline void DeskMacroDialog::launch (AudioEngine& engine)
    {
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle = "Desk Macro Control";
        opts.content.setOwned (new deskmacro_detail::Panel (engine));
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
