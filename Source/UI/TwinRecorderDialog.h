#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/SettingsFile.h"
#include "../Network/TwinLink.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "FloatingLaunch.h"

namespace zynforge
{
    class MixerFixTests;

    // Session > Network & Surfaces > Twin Recorder. A floating panel that can
    // sit open while a show runs, so everything it enables comes from a timer
    // reading live state, never from what was true when it opened.
    class TwinRecorderDialog
    {
    public:
        // Non-modal, like the other tool panels; the caller owns the window.
        static juce::DialogWindow* launch (twin::TwinLink& link, AudioEngine& engine);

        class Content final : public juce::Component, private juce::Timer
        {
        public:
            Content (twin::TwinLink& l, std::function<bool()> rolling, std::function<juce::PropertiesFile*()> settings,
                     std::function<void()> beforeSettingsWrite)
                : link (l), isRolling (std::move (rolling)), getSettings (std::move (settings)),
                  prepareSettingsWrite (std::move (beforeSettingsWrite))
            {
                warning.setText ("Protects against a computer, drive or interface failure. Both Macs run "
                                 "this same app, so it does not protect against an app bug: this is not "
                                 "an independent recorder. The link is authenticated, not encrypted; "
                                 "keep it on a private show network.",
                                 juce::dontSendNotification);
                warning.setFont (brand::type::uiBody());
                warning.setColour (juce::Label::textColourId, brand::alertAmber);
                warning.setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (warning);

                for (auto* field : { &roleLabel, &portLabel, &addressLabel, &codeLabel })
                {
                    dialog::styleFieldLabel (*field);
                    addAndMakeVisible (*field);
                }
                roleLabel.setText ("This Mac is the", juce::dontSendNotification);
                portLabel.setText ("Port", juce::dontSendNotification);
                codeLabel.setText ("Pairing code", juce::dontSendNotification);

                dialog::styleCombo (role);
                role.addItem ("Leader", 1);
                role.addItem ("Follower", 2);
                role.onChange = [this] { onRoleChanged(); };
                addAndMakeVisible (role);

                for (auto* editor : { &port, &address, &code })
                {
                    dialog::styleTextEditor (*editor);
                    addAndMakeVisible (*editor);
                }
                port.setInputRestrictions (5, "0123456789");
                address.setInputRestrictions (15, "0123456789.");
                code.setFont (brand::type::monoCounter());
                code.setInputRestrictions (24);

                regenerate.setButtonText ("New code");
                dialog::styleSecondary (regenerate);
                regenerate.onClick = [this] { onRegenerate(); };
                addAndMakeVisible (regenerate);

                enable.onClick = [this] { onEnableClicked(); };
                addAndMakeVisible (enable);

                closeButton.setButtonText ("Close");
                dialog::styleSecondary (closeButton);
                closeButton.onClick = [this]
                { if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->setVisible (false); };
                addAndMakeVisible (closeButton);

                stateLine.setFont (brand::type::ui (14.0f, true));
                addAndMakeVisible (stateLine);

                details.setMultiLine (true);
                details.setReadOnly (true);
                details.setCaretVisible (false);
                dialog::styleTextEditor (details);
                addAndMakeVisible (details);

                loadFields();
                setSize (580, 520);
                refresh();
                startTimerHz (4);
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, "TWIN RECORDER"); }

            void resized() override
            {
                auto body = dialog::bodyBounds (*this).reduced (brand::space::lg, brand::space::md);
                warning.setBounds (body.removeFromTop (64));
                body.removeFromTop (brand::space::sm);

                const auto row = [&body]
                {
                    auto r = body.removeFromTop (28);
                    body.removeFromTop (brand::space::sm);
                    return r;
                };

                auto first = row();
                roleLabel.setBounds (first.removeFromLeft (120));
                role.setBounds (first.removeFromLeft (130));
                first.removeFromLeft (brand::space::lg);
                portLabel.setBounds (first.removeFromLeft (50));
                port.setBounds (first.removeFromLeft (80));

                auto second = row();
                addressLabel.setBounds (second.removeFromLeft (120));
                address.setBounds (second.removeFromLeft (200));

                auto third = row();
                codeLabel.setBounds (third.removeFromLeft (120));
                code.setBounds (third.removeFromLeft (230));
                third.removeFromLeft (brand::space::sm);
                regenerate.setBounds (third.removeFromLeft (90));

                stateLine.setBounds (row());
                details.setBounds (body);

                auto footer = dialog::footerBounds (*this);
                closeButton.setBounds (footer.removeFromRight (dialog::btnSecond));
                footer.removeFromRight (brand::space::sm);
                enable.setBounds (footer.removeFromRight (dialog::btnPrimary + 30));
            }

            // Everything that can be greyed is decided here, from live state.
            void refresh()
            {
                const auto snapshot = link.snapshot();
                const bool enabled = snapshot.enabled;
                const bool rolling = isRolling();
                const bool follower = enabled ? snapshot.role == twin::Role::Follower : role.getSelectedId() == 2;

                // A role or a new code must not change under a take: the code
                // is what a lost link needs to come back. A leader with no link
                // has nothing to protect, though. The code isn't kept across a
                // restart, so with a daemon take still rolling, these two fields
                // are the only way back to the follower.
                const bool editable = ! enabled && ! rolling;
                role.setEnabled (editable);
                port.setReadOnly (! editable);
                address.setReadOnly (enabled || follower);
                code.setReadOnly (enabled || follower);
                regenerate.setVisible (follower);
                regenerate.setEnabled (follower && ! rolling && snapshot.state != twin::LinkState::Linked);

                addressLabel.setText (follower ? "This Mac's address" : "Follower's address",
                                      juce::dontSendNotification);
                if (follower)
                {
                    const auto mine = localAddresses();
                    if (address.getText() != mine) address.setText (mine, false);
                }

                if (disconnectArmedMs != 0 && (! enabled || armedOut())) disconnectArmedMs = 0;
                enable.setButtonText (! enabled             ? (follower ? "Listen for leader" : "Connect")
                                      : disconnectArmedMs != 0 ? "Press again"
                                                               : "Disconnect");
                enable.setEnabled (true);
                if (enabled) dialog::styleSecondary (enable); else dialog::stylePrimary (enable);

                const auto [text, colour] = stateText (snapshot);
                stateLine.setText (text, juce::dontSendNotification);
                stateLine.setColour (juce::Label::textColourId, colour);

                const auto report = detailText (snapshot);
                if (details.getText() != report) details.setText (report, false);
            }

            // Test seams: the same paths the controls take.
            void pressEnable()                { onEnableClicked(); }
            void setRole (twin::Role r)       { role.setSelectedId (r == twin::Role::Follower ? 2 : 1, juce::sendNotificationSync); }
            void setPeerAddress (const juce::String& a) { address.setText (a, false); }
            void setPort (int p)              { port.setText (juce::String (p), false); }
            void setCode (const juce::String& c) { code.setText (c, false); }
            juce::String getCode() const      { return code.getText(); }
            bool roleEditable() const         { return role.isEnabled(); }
            bool codeEditable() const         { return ! code.isReadOnly(); }
            bool canRegenerate() const        { return regenerate.isEnabled(); }
            juce::String getStateLine() const { return stateLine.getText(); }
            juce::String getDetails() const   { return details.getText(); }
            juce::String getWarning() const   { return warning.getText(); }
            void pressRegenerate()            { onRegenerate(); }

        private:
            friend class zynforge::MixerFixTests;
            void timerCallback() override { refresh(); }

            void loadFields()
            {
                if (link.isEnabled())
                {
                    const auto& config = link.getConfig();
                    role.setSelectedId (config.role == twin::Role::Follower ? 2 : 1, juce::dontSendNotification);
                    port.setText (juce::String (config.port), false);
                    address.setText (config.peerAddress, false);
                    code.setText (config.pairingCode, false);
                    return;
                }
                auto* settings = getSettings();
                const int savedRole = settings != nullptr ? settings->getIntValue ("twinRole", 0) : 0;
                role.setSelectedId (savedRole == 1 ? 2 : 1, juce::dontSendNotification);
                port.setText (juce::String (settings != nullptr ? settings->getIntValue ("twinPort", twin::kDefaultPort)
                                                                 : twin::kDefaultPort), false);
                address.setText (settings != nullptr ? settings->getValue ("twinPeerAddress") : juce::String(), false);
                if (savedRole == 1) code.setText (twin::generatePairingCode(), false);
            }

            void onRoleChanged()
            {
                const bool follower = role.getSelectedId() == 2;
                code.setText (follower ? twin::generatePairingCode() : juce::String(), false);
                address.setText (! follower && getSettings() != nullptr
                                     ? getSettings()->getValue ("twinPeerAddress") : juce::String(), false);
                refresh();
            }

            void onRegenerate()
            {
                if (isRolling() || link.snapshot().state == twin::LinkState::Linked) return;
                code.setText (twin::generatePairingCode(), false);
                if (link.isEnabled()) startLink();
                refresh();
            }

            // While either Mac rolls, Disconnect beside Close takes a second press
            // within 2 s, as STOP does: the follower would roll on with no STOP
            // to reach it.
            void onEnableClicked()
            {
                error.clear();
                if (link.isEnabled())
                {
                    const auto peer = link.snapshot().peer;
                    const bool takeRolling = isRolling() || (peer.has_value() && peer->recording);
                    if (takeRolling && (disconnectArmedMs == 0 || armedOut()))
                        disconnectArmedMs = juce::jmax ((juce::uint32) 1, juce::Time::getMillisecondCounter());
                    else
                    {
                        disconnectArmedMs = 0;
                        link.stop();
                    }
                }
                else startLink();
                refresh();
            }

            bool armedOut() const
            {
                constexpr juce::uint32 kArmWindowMs = 2000;
                return juce::Time::getMillisecondCounter() - disconnectArmedMs > kArmWindowMs;
            }

            void startLink()
            {
                twin::Config config;
                config.role = role.getSelectedId() == 2 ? twin::Role::Follower : twin::Role::Leader;
                config.port = port.getText().getIntValue();
                config.peerAddress = config.role == twin::Role::Leader ? address.getText().trim() : juce::String();
                config.pairingCode = code.getText();
                config.localName = juce::SystemStats::getComputerName();
                if (config.port < 1024)
                {
                    error = "The port must be between 1024 and 65535";
                    return;
                }
                if (link.start (config, error)) saveFields (config);
            }

            void saveFields (const twin::Config& config)
            {
                if (auto* settings = getSettings())
                {
                    prepareSettingsWrite();
                    settings->setValue ("twinRole", (int) config.role);
                    settings->setValue ("twinPort", config.port);
                    if (config.role == twin::Role::Leader)
                        settings->setValue ("twinPeerAddress", config.peerAddress);
                    settings->saveIfNeeded();
                }
            }

            static juce::String localAddresses()
            {
                juce::StringArray found;
                for (const auto& ip : juce::IPAddress::getAllAddresses())
                    if (! ip.isIPv6 && ip != juce::IPAddress::local())
                        found.add (ip.toString());
                return found.joinIntoString (", ");
            }

            static std::pair<juce::String, juce::Colour> stateText (const twin::Snapshot& s)
            {
                using S = twin::LinkState;
                switch (s.state)
                {
                    case S::Linked:
                        return { "LINKED to " + s.peerName, s.warnings.isEmpty() ? brand::accentStatus
                                                                                 : brand::alertAmber };
                    case S::Waiting:
                        return { s.role == twin::Role::Follower ? "WAITING for the leader"
                                                                : "CONNECTING to the follower",
                                 brand::alertAmber };
                    case S::Lost:    return { "TWIN LOST", brand::textDanger };
                    case S::Refused:
                    case S::Failed:  return { "NOT LINKED", brand::textDanger };
                    case S::Off:     break;
                }
                return { "Off", brand::textSecondary };
            }

            juce::String detailText (const twin::Snapshot& s) const
            {
                juce::StringArray lines;
                if (error.isNotEmpty()) lines.add (error);
                if (s.detail.isNotEmpty()) lines.add (s.detail);
                for (const auto& warningLine : s.warnings) lines.add ("! " + warningLine);
                if (s.peer.has_value())
                {
                    const auto& peer = *s.peer;
                    lines.add (juce::String (peer.recording ? "Other Mac: RECORDING  " : "Other Mac: idle  ")
                               + "session \"" + peer.sessionName + "\", "
                               + juce::String (peer.armedTracks) + " armed, "
                               + juce::String (peer.sampleRate, 0) + " Hz");
                    lines.add ("Other Mac health: missed " + juce::String (peer.missedSamples)
                               + ", write failures " + juce::String (peer.writeFailures)
                               + ", disk " + (peer.diskMinutesLeft > 0 ? juce::String (peer.diskMinutesLeft) + " min"
                                                                       : juce::String ("unknown"))
                               + ", backup " + peer.backupHealth);
                }
                if (s.clockValid)
                    lines.add ("Clock offset to other Mac: " + juce::String (s.clockOffsetMs, 1)
                               + " ms (round trip " + juce::String (s.clockRttMs, 1) + " ms)");
                if (s.lastEvent.isNotEmpty()) lines.add (s.lastEvent);
                if (s.compareLine.isNotEmpty()) lines.add (s.compareLine);
                if (s.failedAuth > 0)
                    lines.add ("Refused connection attempts: " + juce::String (s.failedAuth));
                return lines.joinIntoString ("\n");
            }

            twin::TwinLink& link;
            std::function<bool()> isRolling;
            juce::uint32 disconnectArmedMs { 0 };   // the first press of a Disconnect under a take
            std::function<juce::PropertiesFile*()> getSettings;
            std::function<void()> prepareSettingsWrite;
            juce::String error;

            juce::Label warning, roleLabel, portLabel, addressLabel, codeLabel, stateLine;
            juce::ComboBox role;
            juce::TextEditor port, address, code, details;
            juce::TextButton regenerate, enable, closeButton;

            JUCE_DECLARE_NON_COPYABLE (Content)
        };
    };

    inline juce::DialogWindow* TwinRecorderDialog::launch (twin::TwinLink& link, AudioEngine& engine)
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle                  = "Twin Recorder";
        options.content.setOwned (new Content (link,
            [&engine] { return engine.isRecording(); },
            [&engine] { return engine.getAppProps(); },
            [&engine] { if (auto* settings = engine.getAppProps()) reloadSettingsReplacing (*settings); }));
        options.dialogBackgroundColour       = brand::bgPanel;
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar            = true;
        options.resizable                    = false;
        return launchFloating (options);
    }
}
