#include "AudioDeviceDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    namespace
    {
        // ─── Reusable labelled card ────────────────────────────────────────
        // Paints a header bar with a coloured accent stripe + a body area
        // where the caller drops its widgets. The card has a dark gradient
        // background and a subtle border.
        class Card final : public juce::Component
        {
        public:
            Card (const juce::String& heading, juce::Colour accentBar)
                : title (heading), accent (accentBar) {}

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced (1.0f);

                // Card body -- vertical gradient (lighter top → darker bottom).
                g.setGradientFill (brand::verticalGradient (brand::bgStrip, r, 0.10f, 0.18f));
                g.fillRoundedRectangle (r, brand::radius::lg);

                g.setColour (brand::borderSubtle);
                g.drawRoundedRectangle (r, brand::radius::lg, 1.0f);

                // Header strip -- 22 px tall with a coloured 3 px accent
                // bar on the left, then the title text.
                auto header = r.removeFromTop (24.0f);
                g.setColour (accent);
                g.fillRoundedRectangle (header.withWidth (3.0f), 1.5f);
                g.setColour (brand::textSecondary);
                g.setFont (brand::type::uiLabel());
                g.drawText (title.toUpperCase(),
                            header.withTrimmedLeft (brand::space::md).toNearestInt(),
                            juce::Justification::centredLeft, false);
            }

            juce::Rectangle<int> bodyArea() const
            {
                return getLocalBounds().reduced (brand::space::md)
                                       .withTrimmedTop (24);
            }

        private:
            juce::String title;
            juce::Colour accent;
        };

        // ─── Mini input level meter (single bar) ───────────────────────────
        class InputMeter final : public juce::Component, private juce::Timer
        {
        public:
            explicit InputMeter (AudioEngine& eng) : engine (eng) { startTimerHz (24); }
            ~InputMeter() override { stopTimer(); }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced (1.0f);
                g.setColour (brand::bgDeep);
                g.fillRoundedRectangle (r, brand::radius::sm);

                // Read level from the first active input channel (cheap).
                float level = 0.0f;
                auto& rec = engine.getRecorder();
                for (int i = 0; i < rec.getNumTracks(); ++i)
                {
                    const auto p = rec.getTrack (i).peak.load (std::memory_order_relaxed);
                    if (p > level) level = p;
                }
                const float lit = juce::jlimit (0.0f, 1.0f, level);
                auto bar = r.withWidth (r.getWidth() * lit);
                g.setGradientFill (juce::ColourGradient (
                    brand::meterGreen, bar.getX(), bar.getY(),
                    lit > 0.85f ? brand::meterRed : brand::meterAmber,
                    bar.getRight(), bar.getY(), false));
                g.fillRoundedRectangle (bar, brand::radius::sm);

                g.setColour (brand::edge);
                g.drawRoundedRectangle (r, brand::radius::sm, 1.0f);
            }

        private:
            void timerCallback() override
            {
                // Sample the peak level. Only repaint when the lit
                // bar would move by at least 1 px (~1.5 % at the
                // dialog's input-meter width). Without this the
                // meter repaints 24 Hz even when the level is
                // perfectly stable.
                float level = 0.0f;
                auto& rec = engine.getRecorder();
                for (int i = 0; i < rec.getNumTracks(); ++i)
                {
                    const auto p = rec.getTrack (i).peak.load (std::memory_order_relaxed);
                    if (p > level) level = p;
                }
                const float quantised = std::round (juce::jlimit (0.0f, 1.0f, level) * 100.0f) / 100.0f;
                if (std::abs (quantised - lastLevel) >= 0.005f)
                {
                    lastLevel = quantised;
                    repaint();
                }
            }

            AudioEngine& engine;
            float        lastLevel { -1.0f };   // -1 = never painted, force first paint
        };

        // ─── The main dialog content ───────────────────────────────────────
        class DialogContent final : public juce::Component,
                                    private juce::ChangeListener
        {
        public:
            explicit DialogContent (AudioEngine& eng)
                : engine (eng),
                  outputCard  ("Output",       brand::accentPlay),
                  inputCard   ("Input",        brand::accentRecord),
                  rateCard    ("Sample Rate",  brand::featureEngaged),
                  bufferCard  ("Buffer Size",  brand::engagedAmber),
                  monitorCard ("Monitor Bus",  brand::brandOrange),
                  inputMeter (eng)
            {
                auto& dm = engine.getDeviceManager();
                snapshot = dm.createStateXml();
                dm.addChangeListener (this);

                auto styleCombo = [] (juce::ComboBox& c)
                {
                    c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep);
                    c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                    c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                    c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
                };
                styleCombo (outputBox);
                styleCombo (inputBox);
                styleCombo (rateBox);
                styleCombo (bufferBox);
                styleCombo (monitorOutLBox);
                styleCombo (monitorOutRBox);

                outputBox.onChange     = [this] { onDeviceChanged (false); };
                inputBox .onChange     = [this] { onDeviceChanged (true);  };
                rateBox  .onChange     = [this] { onSampleRateChanged();   };
                bufferBox.onChange     = [this] { onBufferSizeChanged();   };
                monitorOutLBox.onChange = [this] { onMonitorOutChanged(); };
                monitorOutRBox.onChange = [this] { onMonitorOutChanged(); };

                addAndMakeVisible (outputCard);
                addAndMakeVisible (inputCard);
                addAndMakeVisible (rateCard);
                addAndMakeVisible (bufferCard);
                addAndMakeVisible (monitorCard);
                addAndMakeVisible (outputBox);
                addAndMakeVisible (inputBox);
                addAndMakeVisible (rateBox);
                addAndMakeVisible (bufferBox);
                addAndMakeVisible (monitorOutLBox);
                addAndMakeVisible (monitorOutRBox);

                monitorLLabel.setText ("Monitor L", juce::dontSendNotification);
                monitorRLabel.setText ("Monitor R", juce::dontSendNotification);
                for (auto* l : { &monitorLLabel, &monitorRLabel })
                {
                    l->setFont (brand::type::caption());
                    l->setColour (juce::Label::textColourId, brand::textTertiary);
                    addAndMakeVisible (*l);
                }

                outChanLabel.setText ("Active outputs:", juce::dontSendNotification);
                inChanLabel .setText ("Active inputs:",  juce::dontSendNotification);
                outChanValue.setText ("--", juce::dontSendNotification);
                inChanValue .setText ("--", juce::dontSendNotification);
                for (auto* l : { &outChanLabel, &inChanLabel })
                {
                    l->setFont (brand::type::caption());
                    l->setColour (juce::Label::textColourId, brand::textTertiary);
                    addAndMakeVisible (*l);
                }
                // Recessed boxed field -- bgDeep body + edge outline, the
                // same finish as the device combos above, so the active
                // channel list reads as a contained value rather than
                // free text that clips at the card edge.
                for (auto* l : { &outChanValue, &inChanValue })
                {
                    l->setFont (brand::type::mono (brand::type::h_caption, true));
                    l->setColour (juce::Label::textColourId,       brand::textPrimary);
                    l->setColour (juce::Label::backgroundColourId, brand::bgDeep);
                    l->setColour (juce::Label::outlineColourId,    brand::edge);
                    l->setJustificationType (juce::Justification::centredLeft);
                    l->setBorderSize (juce::BorderSize<int> (2, brand::space::sm, 2, brand::space::sm));
                    l->setMinimumHorizontalScale (0.85f);
                    addAndMakeVisible (*l);
                }

                addAndMakeVisible (inputMeter);

                testButton.setTooltip ("Play a short 440 Hz tone through the selected output.");
                testButton.setColour (juce::TextButton::buttonColourId, brand::featureEngaged.withAlpha (0.25f));
                testButton.setColour (juce::TextButton::textColourOffId, brand::featureEngaged);
                testButton.onClick = [this] { engine.getDeviceManager().playTestSound(); };
                addAndMakeVisible (testButton);

                statusLabel.setText ("Pick a device, sample rate and buffer -- then Apply.",
                                     juce::dontSendNotification);
                statusLabel.setJustificationType (juce::Justification::centredLeft);
                statusLabel.setColour (juce::Label::textColourId, brand::textTertiary);
                statusLabel.setFont (brand::type::statusBar());
                addAndMakeVisible (statusLabel);

                applyButton.setColour (juce::TextButton::buttonColourId,
                                       brand::accentStatus.withAlpha (brand::alpha::prominent));
                applyButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
                applyButton.onClick = [this] { applyAndClose(); };
                addAndMakeVisible (applyButton);

                cancelButton.setColour (juce::TextButton::buttonColourId, brand::bgElevated);
                cancelButton.setColour (juce::TextButton::textColourOffId, brand::textSecondary);
                cancelButton.onClick = [this] { cancelAndClose(); };
                addAndMakeVisible (cancelButton);

                refreshAll();
                setSize (640, 560);
            }

            ~DialogContent() override
            {
                engine.getDeviceManager().removeChangeListener (this);

                if (! applied && snapshot != nullptr)
                    engine.getDeviceManager().initialise (0, 256, snapshot.get(), true);
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "AUDIO DEVICE");
            }

            void resized() override
            {
                auto r = getLocalBounds();
                r.removeFromTop (44);
                auto footer = r.removeFromBottom (60).reduced (brand::space::md);
                r.reduce (brand::space::md, brand::space::sm);

                // 3-row card layout:
                //   row 1: Output | Input
                //   row 2: Sample Rate | Buffer Size
                //   row 3: Monitor Bus (full width)
                const int totalH      = r.getHeight();
                const int rowGap      = brand::space::sm;
                const int row3H       = 96;                              // Monitor card -- single short row
                const int dualRowsH   = totalH - row3H - rowGap * 2;     // height shared by rows 1 + 2
                const int cardH       = (dualRowsH - rowGap) / 2;

                auto topRow = r.removeFromTop (cardH);
                r.removeFromTop (rowGap);
                auto midRow = r.removeFromTop (cardH);
                r.removeFromTop (rowGap);
                auto monRow = r.removeFromTop (row3H);

                const int half = (topRow.getWidth() - rowGap) / 2;
                outputCard.setBounds (topRow.removeFromLeft (half));
                topRow.removeFromLeft (rowGap);
                inputCard .setBounds (topRow);

                rateCard  .setBounds (midRow.removeFromLeft (half));
                midRow.removeFromLeft (rowGap);
                bufferCard.setBounds (midRow);

                monitorCard.setBounds (monRow);

                // Card contents
                auto outBody = outputCard.bodyArea().translated (outputCard.getX(), outputCard.getY());
                outputBox   .setBounds (outBody.removeFromTop (brand::space::ctrlH));
                outBody.removeFromTop (brand::space::sm);
                outChanLabel.setBounds (outBody.removeFromTop (brand::space::xl).withWidth (110));
                outChanValue.setBounds (outBody.removeFromTop (brand::space::ctrlH));
                outBody.removeFromTop (brand::space::xs);
                testButton  .setBounds (outBody.removeFromTop (brand::space::btnH).reduced (0, 0));

                auto inBody = inputCard.bodyArea().translated (inputCard.getX(), inputCard.getY());
                inputBox   .setBounds (inBody.removeFromTop (brand::space::ctrlH));
                inBody.removeFromTop (brand::space::sm);
                inChanLabel.setBounds (inBody.removeFromTop (brand::space::xl).withWidth (110));
                inChanValue.setBounds (inBody.removeFromTop (brand::space::ctrlH));
                inBody.removeFromTop (brand::space::xs);
                inputMeter .setBounds (inBody.removeFromTop (14));

                auto rateBody = rateCard.bodyArea().translated (rateCard.getX(), rateCard.getY());
                rateBox  .setBounds (rateBody.removeFromTop (brand::space::ctrlH));

                auto bufBody = bufferCard.bodyArea().translated (bufferCard.getX(), bufferCard.getY());
                bufferBox.setBounds (bufBody.removeFromTop (brand::space::ctrlH));

                // Monitor card -- 2 columns of label + combo. The L / R
                // pickers drive engine.setMasterOutputs, which all the
                // monitor-bound feeds (master sum, click, NDI, companion
                // stream) follow.
                auto monBody = monitorCard.bodyArea().translated (monitorCard.getX(),
                                                                  monitorCard.getY());
                const int halfMon = (monBody.getWidth() - brand::space::md) / 2;
                auto leftCol  = monBody.removeFromLeft (halfMon);
                monBody.removeFromLeft (brand::space::md);
                auto rightCol = monBody;

                monitorLLabel  .setBounds (leftCol .removeFromTop (brand::space::xl).withWidth (halfMon));
                monitorOutLBox .setBounds (leftCol .removeFromTop (brand::space::ctrlH));
                monitorRLabel  .setBounds (rightCol.removeFromTop (brand::space::xl).withWidth (halfMon));
                monitorOutRBox .setBounds (rightCol.removeFromTop (brand::space::ctrlH));

                applyButton .setBounds (footer.removeFromRight (110).reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::sm);
                cancelButton.setBounds (footer.removeFromRight (90).reduced (0, brand::space::xs));
                footer.removeFromRight (brand::space::md);
                statusLabel.setBounds (footer);
            }

        private:
            void changeListenerCallback (juce::ChangeBroadcaster*) override
            {
                refreshAll();
            }

            void refreshAll()
            {
                if (refreshing) return;
                const juce::ScopedValueSetter<bool> guard (refreshing, true);

                auto& dm = engine.getDeviceManager();
                auto* type = dm.getCurrentDeviceTypeObject();
                if (type == nullptr) return;
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                dm.getAudioDeviceSetup (setup);

                // Output devices
                outputBox.clear (juce::dontSendNotification);
                const auto outNames = type->getDeviceNames (false);
                for (int i = 0; i < outNames.size(); ++i)
                    outputBox.addItem (outNames[i], i + 1);
                outputBox.setSelectedId (
                    juce::jmax (1, outNames.indexOf (setup.outputDeviceName) + 1),
                    juce::dontSendNotification);

                // Input devices
                inputBox.clear (juce::dontSendNotification);
                const auto inNames = type->getDeviceNames (true);
                inputBox.addItem ("(none)", 1);
                for (int i = 0; i < inNames.size(); ++i)
                    inputBox.addItem (inNames[i], i + 2);
                const int selectedInputId = setup.inputDeviceName.isEmpty()
                                            ? 1
                                            : inNames.indexOf (setup.inputDeviceName) + 2;
                inputBox.setSelectedId (juce::jmax (1, selectedInputId),
                                        juce::dontSendNotification);

                // Sample rate + buffer come from the active device
                rateBox  .clear (juce::dontSendNotification);
                bufferBox.clear (juce::dontSendNotification);
                if (auto* dev = dm.getCurrentAudioDevice())
                {
                    const auto rates = dev->getAvailableSampleRates();
                    for (int i = 0; i < rates.size(); ++i)
                        rateBox.addItem (juce::String (juce::roundToInt (rates[i])) + " Hz",
                                         i + 1);
                    const int rateIdx = rates.indexOf (setup.sampleRate);
                    rateBox.setSelectedId (juce::jmax (1, rateIdx + 1), juce::dontSendNotification);

                    const auto buffers = dev->getAvailableBufferSizes();
                    for (int i = 0; i < buffers.size(); ++i)
                    {
                        const double ms = 1000.0 * buffers[i] / juce::jmax (1.0, setup.sampleRate);
                        bufferBox.addItem (juce::String (buffers[i]) + " samples ("
                                           + juce::String (ms, 1) + " ms)",
                                           i + 1);
                    }
                    const int bufIdx = buffers.indexOf (setup.bufferSize);
                    bufferBox.setSelectedId (juce::jmax (1, bufIdx + 1), juce::dontSendNotification);

                    outChanValue.setText (channelsToString (dev->getActiveOutputChannels()),
                                          juce::dontSendNotification);
                    inChanValue .setText (channelsToString (dev->getActiveInputChannels()),
                                          juce::dontSendNotification);

                    // Monitor L / R combos. Listed item per active
                    // hardware output (1-based to match console
                    // numbering, e.g. "Out 1", "Out 2"). The current
                    // engine.masterOutL / masterOutR get selected.
                    monitorOutLBox.clear (juce::dontSendNotification);
                    monitorOutRBox.clear (juce::dontSendNotification);
                    const auto active = dev->getActiveOutputChannels();
                    const int numOut  = dev->getOutputChannelNames().size();
                    for (int i = 0; i < numOut; ++i)
                    {
                        const auto label = "Out " + juce::String (i + 1)
                                         + (active[i] ? juce::String() : juce::String (" (off)"));
                        monitorOutLBox.addItem (label, i + 1);
                        monitorOutRBox.addItem (label, i + 1);
                    }
                    monitorOutLBox.setSelectedId (
                        juce::jlimit (1, juce::jmax (1, numOut),
                                      engine.getMasterOutputL() + 1),
                        juce::dontSendNotification);
                    monitorOutRBox.setSelectedId (
                        juce::jlimit (1, juce::jmax (1, numOut),
                                      engine.getMasterOutputR() + 1),
                        juce::dontSendNotification);
                }
                else
                {
                    outChanValue.setText ("--", juce::dontSendNotification);
                    inChanValue .setText ("--", juce::dontSendNotification);
                    monitorOutLBox.clear (juce::dontSendNotification);
                    monitorOutRBox.clear (juce::dontSendNotification);
                }
            }

            void onMonitorOutChanged()
            {
                if (refreshing) return;
                const int l = juce::jmax (0, monitorOutLBox.getSelectedId() - 1);
                const int r = juce::jmax (0, monitorOutRBox.getSelectedId() - 1);
                engine.setMasterOutputs (l, r);
                statusLabel.setText (
                    "Monitor bus -> Out " + juce::String (l + 1)
                    + " / Out " + juce::String (r + 1),
                    juce::dontSendNotification);
            }

            // Collapse the active-channel bitmask into compact ranges plus
            // a total count, e.g. "1-64  (64 ch)" or "1-8, 17-24  (16 ch)".
            // A Dante card exposes 64+ channels; the old comma-joined list
            // overran the single-line field and clipped to "...".
            static juce::String channelsToString (const juce::BigInteger& bits)
            {
                juce::StringArray ranges;
                int total = 0;
                for (int i = 0; i < 128; )
                {
                    if (! bits[i]) { ++i; continue; }
                    const int start = i;
                    while (i < 128 && bits[i]) { ++total; ++i; }
                    const int end = i - 1;
                    ranges.add (start == end
                                    ? juce::String (start + 1)
                                    : juce::String (start + 1) + "-" + juce::String (end + 1));
                }
                if (total == 0) return "none";
                return ranges.joinIntoString (", ") + "  (" + juce::String (total) + " ch)";
            }

            void onDeviceChanged (bool isInput)
            {
                if (refreshing) return;
                auto& dm = engine.getDeviceManager();
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                dm.getAudioDeviceSetup (setup);

                if (isInput)
                {
                    const int id = inputBox.getSelectedId();
                    if (id <= 1)
                    {
                        setup.inputDeviceName = {};
                    }
                    else
                    {
                        auto* type = dm.getCurrentDeviceTypeObject();
                        if (type != nullptr)
                            setup.inputDeviceName = type->getDeviceNames (true)[id - 2];
                    }
                    setup.useDefaultInputChannels = true;
                }
                else
                {
                    auto* type = dm.getCurrentDeviceTypeObject();
                    if (type != nullptr)
                        setup.outputDeviceName = type->getDeviceNames (false)[outputBox.getSelectedId() - 1];
                    setup.useDefaultOutputChannels = true;
                }

                const auto err = dm.setAudioDeviceSetup (setup, true);
                statusLabel.setText (err.isEmpty() ? juce::String ("Device set -- Apply to keep it.")
                                                  : err,
                                     juce::dontSendNotification);
            }

            void onSampleRateChanged()
            {
                if (refreshing) return;
                auto& dm = engine.getDeviceManager();
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                dm.getAudioDeviceSetup (setup);
                if (auto* dev = dm.getCurrentAudioDevice())
                {
                    const auto rates = dev->getAvailableSampleRates();
                    const int idx = rateBox.getSelectedId() - 1;
                    if (idx >= 0 && idx < rates.size())
                        setup.sampleRate = rates[idx];
                }
                dm.setAudioDeviceSetup (setup, true);
            }

            void onBufferSizeChanged()
            {
                if (refreshing) return;
                auto& dm = engine.getDeviceManager();
                juce::AudioDeviceManager::AudioDeviceSetup setup;
                dm.getAudioDeviceSetup (setup);
                if (auto* dev = dm.getCurrentAudioDevice())
                {
                    const auto buffers = dev->getAvailableBufferSizes();
                    const int idx = bufferBox.getSelectedId() - 1;
                    if (idx >= 0 && idx < buffers.size())
                        setup.bufferSize = buffers[idx];
                }
                dm.setAudioDeviceSetup (setup, true);
            }

            void applyAndClose()
            {
                applied = true;
                statusLabel.setText ("Applied.", juce::dontSendNotification);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (1);
            }
            void cancelAndClose()
            {
                applied = false;
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            AudioEngine& engine;

            Card outputCard, inputCard, rateCard, bufferCard, monitorCard;
            juce::ComboBox outputBox, inputBox, rateBox, bufferBox;
            juce::ComboBox monitorOutLBox, monitorOutRBox;
            juce::Label    outChanLabel, outChanValue, inChanLabel, inChanValue;
            juce::Label    monitorLLabel, monitorRLabel;
            InputMeter     inputMeter;
            juce::TextButton testButton   { "Test" };
            juce::TextButton applyButton  { "Apply" };
            juce::TextButton cancelButton { "Cancel" };
            juce::Label      statusLabel;
            std::unique_ptr<juce::XmlElement> snapshot;
            bool applied    { false };
            bool refreshing { false };
        };
    }

    juce::DialogWindow* AudioDeviceDialog::launch (AudioEngine& engine)
    {
        auto content = std::make_unique<DialogContent> (engine);

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Audio Device";
        opts.content.setOwned (content.release());
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = true;
        return opts.launchAsync();
    }
}
