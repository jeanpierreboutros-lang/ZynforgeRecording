#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
#include "../Audio/MidiClockOut.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    // Control Surfaces hub: a list of every supported control / console
    // protocol with an Enable tick, and a per-protocol settings dialog.
    //  - OSC family (OSC / DiGiCo / Allen & Heath / SSL Live / Yamaha) is
    //    fully wired: ticking one starts the OSC receiver on that dialect
    //    (one receiver, so ticking a second switches the active console).
    //  - MIDI surfaces (Mackie Control / PreSonus FaderPort) save their
    //    device config here; the live surface I/O layer is staged.
    class ControlSurfacesDialog
    {
    public:
        // createFromConsole is invoked by the DiGiCo panel's "Create session
        // from console" button (the host builds the session). Optional.
        static void launch (AudioEngine& engine, std::function<void()> createFromConsole = {});
    };

    namespace cs_detail
    {
        struct ProtoInfo
        {
            juce::String name;
            bool         osc;        // OSC-receiver protocol vs MIDI surface
            int          dialect;    // OSC dialect index (0..4) or -1
            const char*  key;        // appProps key stem
            const char*  prefix;     // OSC address root (info only)
        };

        inline const std::vector<ProtoInfo>& protocols()
        {
            static const std::vector<ProtoInfo> p = {
                { "Open Sound Control (OSC)", true,  0, "osc",       "/zynforge/" },
                { "DiGiCo",                   true,  1, "digico",    "/Console/"  },
                { "Allen & Heath",            true,  2, "ah",        "/sq/"       },
                { "SSL Live",                 true,  3, "ssl",       "/sslnet/"   },
                { "Yamaha (DM7 / RIVAGE)",    true,  4, "yamaha",    "/Yamaha/"   },
                { "Mackie Control",           false, -1, "mackie",   "" },
                { "PreSonus FaderPort",       false, -1, "faderport","" },
            };
            return p;
        }

        inline int oscPort (AudioEngine& e)
        {
            if (auto* p = e.getAppProps())
                return juce::jlimit (1, 65535, p->getIntValue ("oscListenPort", 8000));
            return 8000;
        }

        inline bool protoEnabled (AudioEngine& e, const ProtoInfo& pi)
        {
            if (pi.osc)
                return e.isOscListening() && e.getOscDialect() == pi.dialect;
            if (auto* p = e.getAppProps())
                return p->getBoolValue (juce::String ("cs_") + pi.key + "_enabled", false);
            return false;
        }

        inline void setProtoEnabled (AudioEngine& e, const ProtoInfo& pi, bool on)
        {
            if (pi.osc)
            {
                if (on) e.startOsc (oscPort (e), pi.dialect);   // switches the single receiver
                else    e.stopOsc();
            }
            else if (auto* p = e.getAppProps())
            {
                p->setValue (juce::String ("cs_") + pi.key + "_enabled", on);
                p->saveIfNeeded();
            }
        }

        // ── OSC-family settings ──────────────────────────────────────────
        class OscSettings final : public juce::Component
        {
        public:
            OscSettings (AudioEngine& e, ProtoInfo pi, std::function<void()> createFromConsole)
                : engine (e), info (std::move (pi)), onCreateFromConsole (std::move (createFromConsole))
            {
                auto lab = [this] (juce::Label& l, const juce::String& t, juce::Colour c, bool b = false)
                { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, c);
                  l.setFont (brand::type::ui (13.0f, b)); addAndMakeVisible (l); };

                const auto host = juce::SystemStats::getComputerName() + ".local";
                lab (connKey, "Connection",  brand::textSecondary);
                lab (connVal, "osc.udp://" + host + ":" + juce::String (oscPort (e)) + "/", brand::textPrimary, true);
                lab (portKey, "Listen Port", brand::textSecondary);
                lab (addrKey, "Address root", brand::textSecondary);
                lab (addrVal, juce::String (info.prefix) + "*", brand::accentStatus, true);

                dialog::styleTextEditor (portEd);
                portEd.setInputRestrictions (5, "0123456789");
                portEd.setText (juce::String (oscPort (e)), juce::dontSendNotification);
                addAndMakeVisible (portEd);

                digico = (info.dialect == 1);
                if (digico)   // DiGiCo: bidirectional console link
                {
                    auto* ap = e.getAppProps();
                    lab (ipKey, "Console IP", brand::textSecondary);
                    dialog::styleTextEditor (ipEd);
                    ipEd.setText (ap ? ap->getValue ("cs_digico_ip") : juce::String(), juce::dontSendNotification);
                    addAndMakeVisible (ipEd);

                    lab (rxKey, "Console Receive Port", brand::textSecondary);
                    dialog::styleTextEditor (rxEd);
                    rxEd.setInputRestrictions (5, "0123456789");
                    rxEd.setText (juce::String (ap ? ap->getIntValue ("cs_digico_port", 8001) : 8001), juce::dontSendNotification);
                    addAndMakeVisible (rxEd);

                    reqB.setButtonText ("Request ALL channel names from console");
                    dialog::stylePrimary (reqB);
                    reqB.onClick = [this] { requestNames(); };
                    createB.setButtonText ("Create session from console");
                    dialog::styleSecondary (createB);
                    createB.setEnabled (onCreateFromConsole != nullptr);
                    createB.onClick = [this]
                    {
                        persist();
                        if (onCreateFromConsole) onCreateFromConsole();
                        if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0);
                    };
                    addAndMakeVisible (reqB); addAndMakeVisible (createB);

                    statusL.setFont (brand::type::caption());
                    statusL.setColour (juce::Label::textColourId, brand::textMuted);
                    statusL.setJustificationType (juce::Justification::topLeft);
                    addAndMakeVisible (statusL);
                }

                applyB.setButtonText ("Apply"); dialog::stylePrimary (applyB);
                closeB.setButtonText ("Close"); dialog::styleSecondary (closeB);
                applyB.onClick = [this] { persist(); rebind(); };
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (applyB); addAndMakeVisible (closeB);

                setSize (520, digico ? 420 : 250);
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, info.name.toUpperCase() + " -- OSC"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                auto row = [&] (juce::Label& k, juce::Component& v, int h = 24)
                { auto r = b.removeFromTop (h); k.setBounds (r.removeFromLeft (170)); v.setBounds (r); b.removeFromTop (10); };
                row (connKey, connVal);
                row (portKey, portEd, 30);
                if (digico)
                {
                    row (ipKey, ipEd, 30);
                    row (rxKey, rxEd, 30);
                    reqB.setBounds (b.removeFromTop (30)); b.removeFromTop (8);
                    createB.setBounds (b.removeFromTop (30)); b.removeFromTop (8);
                    statusL.setBounds (b.removeFromTop (32));
                }
                row (addrKey, addrVal);
                auto br = getLocalBounds().removeFromBottom (50).reduced (24, 10);
                applyB.setBounds (br.removeFromRight (90)); br.removeFromRight (10);
                closeB.setBounds (br.removeFromRight (90));
            }

        private:
            void persist()
            {
                if (auto* p = engine.getAppProps())
                {
                    p->setValue ("oscListenPort", juce::jlimit (1, 65535, portEd.getText().getIntValue()));
                    if (digico)
                    {
                        p->setValue ("cs_digico_ip", ipEd.getText().trim());
                        p->setValue ("cs_digico_port", juce::jlimit (1, 65535, rxEd.getText().getIntValue()));
                    }
                    p->saveIfNeeded();
                }
            }
            void rebind()
            {
                const int port = juce::jlimit (1, 65535, portEd.getText().getIntValue());
                if (engine.isOscListening() && engine.getOscDialect() == info.dialect)
                { engine.stopOsc(); engine.startOsc (port, info.dialect); }
                connVal.setText ("osc.udp://" + juce::SystemStats::getComputerName() + ".local:"
                                 + juce::String (port) + "/", juce::dontSendNotification);
            }
            void requestNames()
            {
                persist();
                if (! engine.isOscListening() || engine.getOscDialect() != 1)
                    engine.startOsc (juce::jlimit (1, 65535, portEd.getText().getIntValue()), 1);   // ensure we're listening for replies
                const auto ip = ipEd.getText().trim();
                const int  rx = juce::jlimit (1, 65535, rxEd.getText().getIntValue());
                const bool ok = engine.requestConsoleChannelNames (ip, rx);
                statusL.setText (ok ? "Request sent to " + ip + ":" + juce::String (rx)
                                      + " -- channel names fill in as the console replies."
                                    : "Couldn't send -- check Console IP + Receive Port.",
                                 juce::dontSendNotification);
                statusL.setColour (juce::Label::textColourId, ok ? brand::accentStatus : brand::accentRecord);
            }

            AudioEngine& engine;
            ProtoInfo info;
            std::function<void()> onCreateFromConsole;
            bool digico { false };
            juce::Label connKey, connVal, portKey, addrKey, addrVal, ipKey, rxKey, statusL;
            juce::TextEditor portEd, ipEd, rxEd;
            juce::TextButton applyB, closeB, reqB, createB;
        };

        // ── MIDI control-surface settings ────────────────────────────────
        class MidiSurfaceSettings final : public juce::Component
        {
        public:
            MidiSurfaceSettings (AudioEngine& e, ProtoInfo pi) : engine (e), info (std::move (pi))
            {
                auto lab = [this] (juce::Label& l, const juce::String& t, juce::Colour c, bool b = false)
                { l.setText (t, juce::dontSendNotification); l.setColour (juce::Label::textColourId, c);
                  l.setFont (brand::type::ui (13.0f, b)); addAndMakeVisible (l); };

                lab (typeKey, "Device Type",        brand::textSecondary);
                lab (inKey,   "Surface receives via", brand::textSecondary);
                lab (outKey,  "Surface sends via",  brand::textSecondary);

                dialog::styleCombo (typeBox);
                const auto types = juce::String (info.key) == "mackie"
                    ? juce::StringArray { "Mackie Control", "Mackie Control Universal Pro" }
                    : juce::StringArray { "FaderPort", "FaderPort 2", "FaderPort 8", "FaderPort 16" };
                for (int i = 0; i < types.size(); ++i) typeBox.addItem (types[i], i + 1);
                typeBox.setSelectedId (juce::jmax (1, prop ("type", 1)), juce::dontSendNotification);
                addAndMakeVisible (typeBox);

                fillMidi (inBox,  engine.getMidiInputNames(),       "midiin");
                fillMidi (outBox, zynforge::MidiClockOut::listOutputs(), "midiout");

                note.setText ("Device configuration is saved with the app. Live fader / transport / "
                              "metering control over MIDI is on the roadmap; OSC consoles work today.",
                              juce::dontSendNotification);
                note.setColour (juce::Label::textColourId, brand::textMuted);
                note.setFont (brand::type::caption());
                note.setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (note);

                applyB.setButtonText ("Save"); dialog::stylePrimary (applyB);
                closeB.setButtonText ("Close"); dialog::styleSecondary (closeB);
                applyB.onClick = [this] { save(); };
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (applyB); addAndMakeVisible (closeB);
                setSize (520, 300);
            }

            void paint (juce::Graphics& g) override
            { dialog::paintChrome (g, *this, info.name.toUpperCase() + " -- MIDI CONTROL SURFACE"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                auto row = [&] (juce::Label& k, juce::Component& v)
                { auto r = b.removeFromTop (30); k.setBounds (r.removeFromLeft (160)); v.setBounds (r); b.removeFromTop (10); };
                row (typeKey, typeBox); row (inKey, inBox); row (outKey, outBox);
                b.removeFromTop (6);
                note.setBounds (b.removeFromTop (54));
                auto br = getLocalBounds().removeFromBottom (50).reduced (24, 10);
                applyB.setBounds (br.removeFromRight (90)); br.removeFromRight (10);
                closeB.setBounds (br.removeFromRight (90));
            }

        private:
            int prop (const char* k, int dflt) const
            { return engine.getAppProps() ? engine.getAppProps()->getIntValue (juce::String ("cs_") + info.key + "_" + k, dflt) : dflt; }

            void fillMidi (juce::ComboBox& box, const juce::StringArray& names, const char* k)
            {
                dialog::styleCombo (box);
                box.addItem ("Disconnected", 1);
                for (int i = 0; i < names.size(); ++i) box.addItem (names[i], i + 2);
                const auto saved = engine.getAppProps() ? engine.getAppProps()->getValue (juce::String ("cs_") + info.key + "_" + k) : juce::String();
                const int idx = names.indexOf (saved);
                box.setSelectedId (idx >= 0 ? idx + 2 : 1, juce::dontSendNotification);
                addAndMakeVisible (box);
            }

            void save()
            {
                if (auto* p = engine.getAppProps())
                {
                    p->setValue (juce::String ("cs_") + info.key + "_type", typeBox.getSelectedId());
                    p->setValue (juce::String ("cs_") + info.key + "_midiin",  inBox.getSelectedId()  > 1 ? inBox.getText()  : juce::String());
                    p->setValue (juce::String ("cs_") + info.key + "_midiout", outBox.getSelectedId() > 1 ? outBox.getText() : juce::String());
                    p->saveIfNeeded();
                }
                if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0);
            }

            AudioEngine& engine;
            ProtoInfo info;
            juce::Label typeKey, inKey, outKey, note;
            juce::ComboBox typeBox, inBox, outBox;
            juce::TextButton applyB, closeB;
        };

        // ── Hub list ─────────────────────────────────────────────────────
        class Hub final : public juce::Component, public juce::ListBoxModel
        {
        public:
            Hub (AudioEngine& e, std::function<void()> createFromConsole)
                : engine (e), onCreateFromConsole (std::move (createFromConsole))
            {
                list.setModel (this);
                list.setRowHeight (32);
                list.setColour (juce::ListBox::backgroundColourId, brand::bgDeep);
                addAndMakeVisible (list);

                title.setText ("Control Surfaces", juce::dontSendNotification);
                title.setFont (brand::type::ui (15.0f, true));
                title.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (title);

                hint.setText ("Tick to enable a protocol, then open its settings. OSC consoles are "
                              "live now; MIDI surfaces save their config.", juce::dontSendNotification);
                hint.setFont (brand::type::caption());
                hint.setColour (juce::Label::textColourId, brand::textMuted);
                addAndMakeVisible (hint);

                settingsB.setButtonText ("Protocol Settings...");
                dialog::stylePrimary (settingsB);
                settingsB.onClick = [this] { openSettings (list.getSelectedRow()); };
                addAndMakeVisible (settingsB);

                closeB.setButtonText ("Close");
                dialog::styleSecondary (closeB);
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (closeB);

                setSize (560, 460);
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, "CONTROL SURFACES"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                title.setBounds (b.removeFromTop (22));
                hint.setBounds (b.removeFromTop (32));
                auto bottom = b.removeFromBottom (44);
                list.setBounds (b.reduced (0, 6));
                settingsB.setBounds (bottom.removeFromLeft (180));
                closeB.setBounds (bottom.removeFromRight (90));
            }

            int getNumRows() override { return (int) protocols().size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
            {
                if (row < 0 || row >= getNumRows()) return;
                const auto& pi = protocols()[(size_t) row];
                if (selected) { g.setColour (brand::brandOrange.withAlpha (0.18f)); g.fillRect (0, 0, w, h); }

                // checkbox
                const auto box = juce::Rectangle<float> (12.0f, h * 0.5f - 8.0f, 16.0f, 16.0f);
                g.setColour (brand::edge); g.drawRoundedRectangle (box, 3.0f, 1.2f);
                if (protoEnabled (engine, pi))
                {
                    g.setColour (brand::accentStatus);
                    g.fillRoundedRectangle (box.reduced (3.0f), 2.0f);
                }
                g.setColour (brand::textPrimary);
                g.setFont (brand::type::ui (14.0f, false));
                g.drawText (pi.name, 40, 0, w - 48, h, juce::Justification::centredLeft, true);
            }

            void listBoxItemClicked (int row, const juce::MouseEvent& e) override
            {
                if (row < 0 || row >= getNumRows()) return;
                if (e.x < 36)   // checkbox column -> toggle enable
                {
                    const auto& pi = protocols()[(size_t) row];
                    setProtoEnabled (engine, pi, ! protoEnabled (engine, pi));
                    list.repaint();
                }
            }
            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { openSettings (row); }

        private:
            void openSettings (int row)
            {
                if (row < 0 || row >= getNumRows()) return;
                const auto& pi = protocols()[(size_t) row];
                juce::DialogWindow::LaunchOptions opts;
                opts.dialogTitle = pi.name;
                if (pi.osc) opts.content.setOwned (new OscSettings (engine, pi, onCreateFromConsole));
                else        opts.content.setOwned (new MidiSurfaceSettings (engine, pi));
                opts.dialogBackgroundColour       = brand::bgPanel;
                opts.escapeKeyTriggersCloseButton = true;
                opts.useNativeTitleBar            = true;
                opts.resizable                    = false;
                opts.launchAsync();
            }

            AudioEngine& engine;
            std::function<void()> onCreateFromConsole;
            juce::ListBox list { "protocols", nullptr };
            juce::Label title, hint;
            juce::TextButton settingsB, closeB;
        };
    }

    inline void ControlSurfacesDialog::launch (AudioEngine& engine, std::function<void()> createFromConsole)
    {
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Control Surfaces";
        opts.content.setOwned (new cs_detail::Hub (engine, std::move (createFromConsole)));
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
