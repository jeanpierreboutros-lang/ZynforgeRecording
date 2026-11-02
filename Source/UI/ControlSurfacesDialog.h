#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../Audio/AudioEngine.h"
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
        // consoleLinkUp says whether the connected console link can supply the
        // session instead; the hub's own button is the way there for a desk
        // with no OSC panel here, such as an X32.
        static void launch (AudioEngine& engine, std::function<void()> createFromConsole = {},
                            std::function<bool()> consoleLinkUp = {});
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
                return e.getRemotes().isOscListening() && e.getRemotes().getOscDialect() == pi.dialect;
            return e.getRemotes().getActiveSurfaceKey() == pi.key;   // MIDI surface is live
        }

        inline juce::String portTakenText (int port)
        {
            return "Could not listen on UDP port " + juce::String (port)
                 + ". Another app may hold it, so pick another in Protocol Settings.";
        }

        // Empty when it did what was asked, otherwise why it didn't.
        inline juce::String setProtoEnabled (AudioEngine& e, const ProtoInfo& pi, bool on)
        {
            if (pi.osc)
            {
                if (! on)
                {
                    e.getRemotes().stopOsc();
                    return {};
                }
                // Switches the single receiver.
                return e.getRemotes().startOsc (oscPort (e), pi.dialect) ? juce::String() : portTakenText (oscPort (e));
            }
            // MIDI surface: open it on the saved MIDI in/out (one surface at a time).
            if (on)
            {
                auto* p = e.getAppProps();
                const auto in  = p ? p->getValue (juce::String ("cs_") + pi.key + "_midiin")  : juce::String();
                const auto out = p ? p->getValue (juce::String ("cs_") + pi.key + "_midiout") : juce::String();
                if (! e.getRemotes().enableControlSurface (pi.key, in, out))
                    return "Could not open that surface's MIDI input. Check it in Protocol Settings.";
            }
            else
                e.getRemotes().disableControlSurface();
            return {};
        }

        // Live OSC traffic log -- so the engineer can verify the request /
        // reply strings against their actual console.
        class OscLogContent final : public juce::Component, private juce::Timer
        {
        public:
            explicit OscLogContent (AudioEngine& e) : engine (e)
            {
                view.setMultiLine (true); view.setReadOnly (true); view.setCaretVisible (false);
                view.setFont (brand::type::mono (brand::type::h_caption));
                view.setColour (juce::TextEditor::backgroundColourId, brand::bgDeep);
                view.setColour (juce::TextEditor::textColourId, brand::textPrimary);
                addAndMakeVisible (view);
                clearB.setButtonText ("Clear"); dialog::styleSecondary (clearB);
                clearB.onClick = [this] { engine.getRemotes().clearOscLog(); refresh(); };
                closeB.setButtonText ("Close"); dialog::styleSecondary (closeB);
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (clearB); addAndMakeVisible (closeB);
                setSize (660, 440); startTimerHz (5); refresh();
            }
            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, "OSC TRAFFIC LOG"); }
            void resized() override
            {
                auto b = getLocalBounds().reduced (16).withTrimmedTop (40);
                auto bottom = b.removeFromBottom (40);
                view.setBounds (b);
                clearB.setBounds (bottom.removeFromLeft (90));
                closeB.setBounds (bottom.removeFromRight (90));
            }
        private:
            // The log can hold a whole show. Joining and re-laying out all of
            // it five times a second would stall the message thread, and Save
            // Console Log is the way to read the lot.
            static constexpr int kShownLines = 500;
            void timerCallback() override { refresh(); }
            void refresh()
            {
                const auto revision = engine.getRemotes().getOscLogRevision();
                if (revision == shownRevision) return;
                shownRevision = revision;
                view.setText (engine.getRemotes().getOscLogTail (kShownLines).joinIntoString ("\n"), false);
                view.moveCaretToEnd();
            }
            AudioEngine& engine;
            juce::int64 shownRevision { -1 };
            juce::TextEditor view;
            juce::TextButton clearB, closeB;
        };

        inline void launchOscLog (AudioEngine& e)
        {
            juce::DialogWindow::LaunchOptions o;
            o.dialogTitle = "OSC Traffic Log";
            o.content.setOwned (new OscLogContent (e));
            o.dialogBackgroundColour = brand::bgPanel;
            o.escapeKeyTriggersCloseButton = true; o.useNativeTitleBar = true; o.resizable = true;
            o.launchAsync();
        }

        // ── OSC-family settings ──────────────────────────────────────────
        class OscSettings final : public juce::Component,
                                  private juce::Timer
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
                lab (addrVal, juce::String (info.prefix) + "*", brand::textPrimary, true);

                dialog::styleTextEditor (portEd);
                portEd.setInputRestrictions (5, "0123456789");
                portEd.setText (juce::String (oscPort (e)), juce::dontSendNotification);
                addAndMakeVisible (portEd);

                digico = (info.dialect >= 1);   // any real console (not the generic OSC schema)
                if (! digico)   // the Generic schema refuses a command without this token
                {
                    lab (tokenKey, "Access token", brand::textSecondary);
                    lab (tokenVal, {}, brand::textPrimary, true);
                    tokenVal.setTitle ("OSC access token");
                    copyTokenB.setButtonText ("Copy");
                    dialog::styleSecondary (copyTokenB);
                    copyTokenB.onClick = [this]
                    { juce::SystemClipboard::copyTextToClipboard (engine.getRemotes().getOscAccessToken()); };
                    addAndMakeVisible (copyTokenB);
                    refreshToken();
                    startTimerHz (2);   // the token changes with each start of the listener
                }
                if (digico)   // bidirectional console link
                {
                    auto* ap = e.getAppProps();
                    lab (ipKey, "Console IP", brand::textSecondary);
                    dialog::styleTextEditor (ipEd);
                    ipEd.setText (ap ? ap->getValue (ipPropKey()) : juce::String(), juce::dontSendNotification);
                    addAndMakeVisible (ipEd);

                    lab (rxKey, "Console Receive Port", brand::textSecondary);
                    dialog::styleTextEditor (rxEd);
                    rxEd.setInputRestrictions (5, "0123456789");
                    rxEd.setText (juce::String (ap ? ap->getIntValue (portPropKey(), 8001) : 8001), juce::dontSendNotification);
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
                        // Tell the host which console to pull from.
                        updateSettings (engine.getAppProps(), [&] (juce::PropertiesFile& p)
                        { p.setValue ("cs_console_dialect", info.dialect); p.setValue ("cs_console_key", info.key); });
                        if (onCreateFromConsole) onCreateFromConsole();
                        if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0);
                    };
                    addAndMakeVisible (reqB); addAndMakeVisible (createB);

                    debugToggle.setButtonText ("Debug: log OSC traffic");
                    dialog::styleCheckRow (debugToggle);
                    debugToggle.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                    debugToggle.setColour (juce::ToggleButton::tickColourId, brand::selectFill);
                    debugToggle.setToggleState (engine.getRemotes().isOscDebug(), juce::dontSendNotification);
                    debugToggle.onClick = [this] { engine.getRemotes().setOscDebug (debugToggle.getToggleState()); };
                    logB.setButtonText ("View OSC Log...");
                    dialog::styleSecondary (logB);
                    logB.onClick = [this] { launchOscLog (engine); };
                    addAndMakeVisible (debugToggle); addAndMakeVisible (logB);

                    statusL.setFont (brand::type::caption());
                    statusL.setColour (juce::Label::textColourId, brand::textTertiary);
                    statusL.setJustificationType (juce::Justification::topLeft);
                    addAndMakeVisible (statusL);
                }

                applyB.setButtonText ("Apply"); dialog::stylePrimary (applyB);
                closeB.setButtonText ("Close"); dialog::styleSecondary (closeB);
                applyB.onClick = [this] { persist(); rebind(); };
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (applyB); addAndMakeVisible (closeB);

                setSize (520, digico ? 470 : 284);
            }

            void timerCallback() override { refreshToken(); }

            void refreshToken()
            {
                const auto token = engine.getRemotes().getOscAccessToken();
                tokenVal.setText (token.isNotEmpty() ? token : juce::String ("Shown once OSC is listening"),
                                  juce::dontSendNotification);
                copyTokenB.setEnabled (token.isNotEmpty());
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, info.name.toUpperCase() + " -- OSC"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                auto row = [&] (juce::Label& k, juce::Component& v, int h = 24)
                { auto r = b.removeFromTop (h); k.setBounds (r.removeFromLeft (170)); v.setBounds (r); b.removeFromTop (10); };
                row (connKey, connVal);
                row (portKey, portEd, 30);
                if (! digico)
                {
                    auto tr = b.removeFromTop (24);
                    tokenKey.setBounds (tr.removeFromLeft (170));
                    copyTokenB.setBounds (tr.removeFromRight (64));
                    tokenVal.setBounds (tr);
                    b.removeFromTop (10);
                }
                if (digico)
                {
                    row (ipKey, ipEd, 30);
                    row (rxKey, rxEd, 30);
                    reqB.setBounds (b.removeFromTop (30)); b.removeFromTop (8);
                    createB.setBounds (b.removeFromTop (30)); b.removeFromTop (8);
                    {
                        auto dr = b.removeFromTop (28);
                        debugToggle.setBounds (dr.removeFromLeft (220));
                        logB.setBounds (dr.removeFromLeft (140));
                    }
                    b.removeFromTop (6);
                    statusL.setBounds (b.removeFromTop (28));
                }
                row (addrKey, addrVal);
                auto br = getLocalBounds().removeFromBottom (50).reduced (24, 10);
                applyB.setBounds (br.removeFromRight (90)); br.removeFromRight (10);
                closeB.setBounds (br.removeFromRight (90));
            }

        private:
            juce::String ipPropKey()   const { return juce::String ("cs_") + info.key + "_ip"; }
            juce::String portPropKey() const { return juce::String ("cs_") + info.key + "_port"; }

            void persist()
            {
                updateSettings (engine.getAppProps(), [&] (juce::PropertiesFile& p)
                {
                    p.setValue ("oscListenPort", juce::jlimit (1, 65535, portEd.getText().getIntValue()));
                    if (digico)
                    {
                        p.setValue (ipPropKey(),   ipEd.getText().trim());
                        p.setValue (portPropKey(), juce::jlimit (1, 65535, rxEd.getText().getIntValue()));
                    }
                });
            }
            void rebind()
            {
                const int port = juce::jlimit (1, 65535, portEd.getText().getIntValue());
                bool listening = true;
                if (engine.getRemotes().isOscListening() && engine.getRemotes().getOscDialect() == info.dialect)
                {
                    engine.getRemotes().stopOsc();
                    listening = engine.getRemotes().startOsc (port, info.dialect);
                }
                connVal.setText (listening ? "osc.udp://" + juce::SystemStats::getComputerName() + ".local:"
                                                 + juce::String (port) + "/"
                                           : portTakenText (port),
                                 juce::dontSendNotification);
                connVal.setColour (juce::Label::textColourId, listening ? brand::textPrimary : brand::textDanger);
            }
            void requestNames()
            {
                persist();
                // Listen for replies on this dialect. A request nobody can hear is no request.
                const int port = juce::jlimit (1, 65535, portEd.getText().getIntValue());
                if ((! engine.getRemotes().isOscListening() || engine.getRemotes().getOscDialect() != info.dialect)
                    && ! engine.getRemotes().startOsc (port, info.dialect))
                {
                    statusL.setText (portTakenText (port), juce::dontSendNotification);
                    statusL.setColour (juce::Label::textColourId, brand::textDanger);
                    return;
                }
                const auto ip = ipEd.getText().trim();
                const int  rx = juce::jlimit (1, 65535, rxEd.getText().getIntValue());
                const bool ok = engine.getRemotes().requestConsoleChannelNames (ip, rx, info.dialect);
                statusL.setText (ok ? "Request sent to " + ip + ":" + juce::String (rx)
                                      + " -- channel names fill in as the console replies."
                                    : "Couldn't send -- check Console IP + Receive Port.",
                                 juce::dontSendNotification);
                statusL.setColour (juce::Label::textColourId, ok ? brand::accentStatus : brand::textDanger);
            }

            AudioEngine& engine;
            ProtoInfo info;
            std::function<void()> onCreateFromConsole;
            bool digico { false };
            juce::Label connKey, connVal, portKey, addrKey, addrVal, ipKey, rxKey, statusL, tokenKey, tokenVal;
            juce::TextButton copyTokenB;
            juce::TextEditor portEd, ipEd, rxEd;
            juce::TextButton applyB, closeB, reqB, createB, logB;
            juce::ToggleButton debugToggle;
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

                lab (inKey,   "Surface receives via", brand::textSecondary);
                lab (outKey,  "Surface sends via",  brand::textSecondary);

                fillMidi (inBox,  engine.getMidiInputNames(),       "midiin");
                fillMidi (outBox, midiOutputNames(),                  "midiout");

                note.setText ("Saved with the app. Tick this surface in the Control Surfaces list to open it: "
                              "faders, mute, solo, arm, transport and jog work live.",
                              juce::dontSendNotification);
                note.setColour (juce::Label::textColourId, brand::textTertiary);
                note.setFont (brand::type::caption());
                note.setJustificationType (juce::Justification::topLeft);
                addAndMakeVisible (note);

                applyB.setButtonText ("Save"); dialog::stylePrimary (applyB);
                closeB.setButtonText ("Close"); dialog::styleSecondary (closeB);
                applyB.onClick = [this] { save(); };
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (applyB); addAndMakeVisible (closeB);
                setSize (520, 260);
            }

            void paint (juce::Graphics& g) override
            { dialog::paintChrome (g, *this, info.name.toUpperCase() + " -- MIDI CONTROL SURFACE"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                auto row = [&] (juce::Label& k, juce::Component& v)
                { auto r = b.removeFromTop (30); k.setBounds (r.removeFromLeft (160)); v.setBounds (r); b.removeFromTop (10); };
                row (inKey, inBox); row (outKey, outBox);
                b.removeFromTop (6);
                note.setBounds (b.removeFromTop (54));
                auto br = getLocalBounds().removeFromBottom (50).reduced (24, 10);
                applyB.setBounds (br.removeFromRight (90)); br.removeFromRight (10);
                closeB.setBounds (br.removeFromRight (90));
            }

        private:
            static juce::StringArray midiOutputNames()
            {
                juce::StringArray names;
                for (const auto& device : juce::MidiOutput::getAvailableDevices())
                    names.add (device.name);
                return names;
            }

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
                updateSettings (engine.getAppProps(), [&] (juce::PropertiesFile& p)
                {
                    p.setValue (juce::String ("cs_") + info.key + "_midiin",  inBox.getSelectedId()  > 1 ? inBox.getText()  : juce::String());
                    p.setValue (juce::String ("cs_") + info.key + "_midiout", outBox.getSelectedId() > 1 ? outBox.getText() : juce::String());
                });
                if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0);
            }

            AudioEngine& engine;
            ProtoInfo info;
            juce::Label inKey, outKey, note;
            juce::ComboBox inBox, outBox;
            juce::TextButton applyB, closeB;
        };

        // ── Hub list ─────────────────────────────────────────────────────
        class Hub final : public juce::Component, public juce::ListBoxModel, private juce::Timer
        {
            static constexpr const char* kHintText = "Tick a surface to switch it on. Protocol Settings sets "
                                                     "its network port or its MIDI in and out.";
        public:
            Hub (AudioEngine& e, std::function<void()> createFromConsole, std::function<bool()> linkUp)
                : engine (e), onCreateFromConsole (std::move (createFromConsole)), consoleLinkUp (std::move (linkUp))
            {
                list.setModel (this);
                list.setRowHeight (32);
                list.setColour (juce::ListBox::backgroundColourId, brand::bgDeep);
                addAndMakeVisible (list);

                title.setText ("Control Surfaces", juce::dontSendNotification);
                title.setFont (brand::type::ui (brand::type::h_title, true));
                title.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (title);

                hint.setText (kHintText, juce::dontSendNotification);
                hint.setFont (brand::type::caption());
                hint.setColour (juce::Label::textColourId, brand::textTertiary);
                addAndMakeVisible (hint);

                settingsB.setButtonText ("Protocol Settings...");
                dialog::stylePrimary (settingsB);
                settingsB.onClick = [this] { openSettings (list.getSelectedRow()); };
                addAndMakeVisible (settingsB);

                createB.setButtonText ("Create Session from Connected Console");
                dialog::styleSecondary (createB);
                createB.onClick = [this]
                {
                    if (onCreateFromConsole) onCreateFromConsole();
                    if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0);
                };
                addAndMakeVisible (createB);

                closeB.setButtonText ("Close");
                dialog::styleSecondary (closeB);
                closeB.onClick = [this] { if (auto* d = findParentComponentOfClass<juce::DialogWindow>()) d->exitModalState (0); };
                addAndMakeVisible (closeB);

                setSize (560, 460);
                timerCallback();
                startTimerHz (2);
            }

            void paint (juce::Graphics& g) override { dialog::paintChrome (g, *this, "CONTROL SURFACES"); }

            void resized() override
            {
                auto b = getLocalBounds().reduced (24).withTrimmedTop (44);
                title.setBounds (b.removeFromTop (22));
                hint.setBounds (b.removeFromTop (32));
                auto bottom = b.removeFromBottom (44);
                list.setBounds (b.reduced (0, 6));
                settingsB.setBounds (bottom.removeFromLeft (150));
                bottom.removeFromLeft (brand::space::sm);
                closeB.setBounds (bottom.removeFromRight (80));
                bottom.removeFromRight (brand::space::sm);
                createB.setBounds (bottom);
            }

            int getNumRows() override { return (int) protocols().size(); }

            void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
            {
                if (row < 0 || row >= getNumRows()) return;
                const auto& pi = protocols()[(size_t) row];
                if (selected) { g.setColour (brand::selectFill.withAlpha (zynforge::brand::alpha::ghost)); g.fillRect (0, 0, w, h); }

                const auto box = juce::Rectangle<float> (12.0f, h * 0.5f - 8.0f, 16.0f, 16.0f);
                g.setColour (brand::edge); g.drawRoundedRectangle (box, brand::radius::md, 1.2f);
                if (protoEnabled (engine, pi))
                {
                    g.setColour (brand::selectFill);
                    g.fillRoundedRectangle (box.reduced (3.0f), 2.0f);
                }
                g.setColour (brand::textPrimary);
                g.setFont (brand::type::ui (14.0f, false));
                g.drawText (pi.name, 40, 0, w - 48, h, juce::Justification::centredLeft, true);
                if (! pi.osc)
                {
                    g.setColour (brand::textTertiary);
                    g.setFont (brand::type::caption());
                    g.drawText ("set MIDI in/out in settings", 40, 0, w - 56, h,
                                juce::Justification::centredRight, true);
                }
            }

            void listBoxItemClicked (int row, const juce::MouseEvent& e) override
            {
                if (row < 0 || row >= getNumRows()) return;
                const auto& pi = protocols()[(size_t) row];
                if (e.x < 36)   // checkbox column -> toggle enable
                {
                    const auto refusal = setProtoEnabled (engine, pi, ! protoEnabled (engine, pi));
                    hint.setText (refusal.isNotEmpty() ? refusal : juce::String (kHintText), juce::dontSendNotification);
                    hint.setColour (juce::Label::textColourId, refusal.isNotEmpty() ? brand::textDanger : brand::textTertiary);
                    list.repaint();
                }
            }
            void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { openSettings (row); }

        private:
            // A link can connect or drop while this is open.
            void timerCallback() override
            { createB.setEnabled (onCreateFromConsole != nullptr && consoleLinkUp != nullptr && consoleLinkUp()); }

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
            std::function<bool()> consoleLinkUp;
            juce::ListBox list { "protocols", nullptr };
            juce::Label title, hint;
            juce::TextButton settingsB, createB, closeB;
        };
    }

    inline void ControlSurfacesDialog::launch (AudioEngine& engine, std::function<void()> createFromConsole,
                                               std::function<bool()> consoleLinkUp)
    {
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Control Surfaces";
        opts.content.setOwned (new cs_detail::Hub (engine, std::move (createFromConsole), std::move (consoleLinkUp)));
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
