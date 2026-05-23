#include "ClickSettingsDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        static const juce::StringArray voiceLabels = {
            "Sine",  "Beep",  "Cowbell",  "Wood Block",  "Classic Click"
        };

        // Plain fraction labels — the Unicode music-symbol glyphs (𝅗𝅥,
        // 𝅘𝅥, …) only render with a music font (Bravura etc.) that we
        // don't ship; on the default macOS UI font they fall back to
        // 'tofu' boxes. Fractions are bundled with every system font
        // and read unambiguously at small button size.
        static const char* subdivisionGlyphs[] = {
            "1",         // whole
            "1/2",       // half
            "1/4",       // quarter
            "1/8",       // eighth
            "1/16",      // sixteenth
            "3",         // triplet
            "\xe2\x80\x94"  // em dash = silent
        };

        class VoiceRow final : public juce::Component
        {
        public:
            VoiceRow (const juce::String& heading,
                      ClickSettings::Voice1Then2 init,
                      std::function<void (const ClickSettings::Voice1Then2&)> onChange)
                : changed (std::move (onChange))
            {
                title.setText (heading, juce::dontSendNotification);
                title.setFont (brand::type::captionBold());
                title.setColour (juce::Label::textColourId, brand::textPrimary);
                addAndMakeVisible (title);

                volume.setSliderStyle (juce::Slider::LinearHorizontal);
                volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                volume.setRange (-60.0, 12.0, 0.1);
                volume.setDoubleClickReturnValue (true, 0.0);
                volume.setValue (init.volumeDb, juce::dontSendNotification);
                volume.setColour (juce::Slider::thumbColourId,        brand::accentStatus);
                volume.setColour (juce::Slider::trackColourId,        brand::edge);
                volume.setColour (juce::Slider::backgroundColourId,   brand::bgDeep);
                volume.onValueChange = [this] { broadcast(); };
                addAndMakeVisible (volume);

                voice.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff000000));
                voice.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                voice.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                voice.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
                for (int i = 0; i < voiceLabels.size(); ++i)
                    voice.addItem (voiceLabels[i], i + 1);
                voice.setSelectedId ((int) init.voice + 1, juce::dontSendNotification);
                voice.onChange = [this] { broadcast(); };
                addAndMakeVisible (voice);

                for (int i = 0; i < (int) std::size (subdivisionGlyphs); ++i)
                {
                    auto* b = new juce::TextButton (juce::String::fromUTF8 (subdivisionGlyphs[i]));
                    b->setClickingTogglesState (true);
                    b->setRadioGroupId (1001);
                    b->setColour (juce::TextButton::buttonColourId,   brand::bgElevated);
                    b->setColour (juce::TextButton::buttonOnColourId, brand::accentSolo);
                    b->setColour (juce::TextButton::textColourOffId,  brand::textPrimary);
                    b->setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
                    if (i == (int) init.sub)
                        b->setToggleState (true, juce::dontSendNotification);
                    b->onClick = [this, i]
                    {
                        sub = (ClickSettings::Subdivision) i;
                        broadcast();
                    };
                    subButtons.add (b);
                    addAndMakeVisible (*b);
                }

                vol1   = init.volumeDb;
                voice1 = init.voice;
                sub    = init.sub;
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced (1.0f);
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.10f));
                g.fillRoundedRectangle (r, brand::radius::md);
                g.setColour (brand::edge);
                g.drawRoundedRectangle (r, brand::radius::md, 1.0f);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (10, 6);

                auto topRow = r.removeFromTop (24);
                title  .setBounds (topRow.removeFromLeft (60));
                topRow.removeFromLeft (brand::space::lg);
                voice  .setBounds (topRow.removeFromRight (160));
                topRow.removeFromRight (brand::space::lg);
                volume .setBounds (topRow);

                r.removeFromTop (brand::space::xs);
                auto btnRow = r;
                const int n  = subButtons.size();
                const int bw = btnRow.getWidth() / juce::jmax (1, n);
                for (auto* b : subButtons)
                    b->setBounds (btnRow.removeFromLeft (bw).reduced (2, 2));
            }

        private:
            void broadcast()
            {
                ClickSettings::Voice1Then2 cur;
                cur.volumeDb = (float) volume.getValue();
                cur.voice    = (ClickSettings::Voice) (voice.getSelectedId() - 1);
                cur.sub      = sub;
                if (changed) changed (cur);
            }

            juce::Label  title;
            juce::Slider volume;
            juce::ComboBox voice;
            juce::OwnedArray<juce::TextButton> subButtons;
            ClickSettings::Subdivision sub { ClickSettings::Subdivision::Quarter };
            float vol1 { 0.0f };
            ClickSettings::Voice voice1 { ClickSettings::Voice::Click };

            std::function<void (const ClickSettings::Voice1Then2&)> changed;
            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceRow)
        };

        class Content final : public juce::Component
        {
        public:
            Content (ClickSettings initial,
                     float bpm,
                     ClickSettingsDialog::SaveCallback save,
                     ClickSettingsDialog::GenerateCallback gen)
                : settings (initial),
                  onSave   (std::move (save)),
                  onGenerate (std::move (gen))
            {
                // Tempo readout in a single label: '120 BPM', large
                // accent-coloured number with 'BPM' beside it. No
                // separate BEAT / FOLLOW METER chips.
                bpmValue.setText (juce::String (bpm, 0) + " BPM", juce::dontSendNotification);
                // Big tabular numeral so the BPM digit doesn't dance.
                bpmValue.setFont (brand::type::mono (44.0f, true));
                bpmValue.setColour (juce::Label::textColourId, brand::accentStatus);
                bpmValue.setJustificationType (juce::Justification::centred);
                addAndMakeVisible (bpmValue);

                onButton.setButtonText (initial.on ? "ON" : "OFF");
                onButton.setClickingTogglesState (true);
                onButton.setToggleState (initial.on, juce::dontSendNotification);
                onButton.setColour (juce::TextButton::buttonOnColourId, brand::accentStatus);
                onButton.setColour (juce::TextButton::buttonColourId,   brand::bgElevated);
                onButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::black);
                onButton.setColour (juce::TextButton::textColourOffId,  brand::textPrimary);
                onButton.onClick = [this]
                {
                    settings.on = onButton.getToggleState();
                    onButton.setButtonText (settings.on ? "ON" : "OFF");
                    if (onSave) onSave (settings);
                };
                addAndMakeVisible (onButton);

                row1 = std::make_unique<VoiceRow> ("CLICK 1", initial.click1,
                    [this] (const ClickSettings::Voice1Then2& v)
                {
                    settings.click1 = v;
                    if (onSave) onSave (settings);
                });
                addAndMakeVisible (*row1);

                row2 = std::make_unique<VoiceRow> ("CLICK 2", initial.click2,
                    [this] (const ClickSettings::Voice1Then2& v)
                {
                    settings.click2 = v;
                    if (onSave) onSave (settings);
                });
                addAndMakeVisible (*row2);

                generateButton.setButtonText ("Generate click track");
                generateButton.setColour (juce::TextButton::buttonColourId,  brand::accentRecord);
                generateButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
                generateButton.onClick = [this]
                {
                    // Generating commits a click WAV to the session.
                    // The real-time click engine should NOT also be
                    // running on top — flip it off and reflect that
                    // back to the settings before the dialog closes.
                    settings.on = false;
                    onButton.setToggleState (false, juce::dontSendNotification);
                    onButton.setButtonText ("OFF");
                    if (onSave)     onSave (settings);
                    if (onGenerate) onGenerate();
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState (1);
                };
                addAndMakeVisible (generateButton);

                closeButton.setButtonText ("Close");
                closeButton.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
                closeButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
                closeButton.onClick = [this]
                {
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState (0);
                };
                addAndMakeVisible (closeButton);

                setSize (640, 460);
            }

            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat();
                g.setGradientFill (brand::verticalGradient (brand::bgPanel, r, 0.04f, 0.14f));
                g.fillAll();
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (16, 12);

                // Top banner: [ '120 BPM' centred ] [ ON pill on the right ].
                auto banner = r.removeFromTop (72);
                auto onArea = banner.removeFromRight (90);
                onButton.setBounds (onArea.reduced (4, 14));
                bpmValue.setBounds (banner);

                r.removeFromTop (12);
                row1->setBounds (r.removeFromTop (80));
                r.removeFromTop (brand::space::md);
                row2->setBounds (r.removeFromTop (80));

                auto footer = r.removeFromBottom (40);
                closeButton   .setBounds (footer.removeFromLeft (90));
                footer.removeFromLeft (brand::space::md);
                generateButton.setBounds (footer.removeFromRight (180));
            }

        private:
            ClickSettings settings;
            ClickSettingsDialog::SaveCallback     onSave;
            ClickSettingsDialog::GenerateCallback onGenerate;

            juce::Label bpmValue;
            juce::TextButton onButton { "ON" };
            std::unique_ptr<VoiceRow> row1, row2;
            juce::TextButton generateButton, closeButton;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
        };
    }

    void ClickSettingsDialog::launch (ClickSettings  initial,
                                      float          bpm,
                                      SaveCallback   onSave,
                                      GenerateCallback onGenerate)
    {
        auto* content = new Content (initial, bpm, std::move (onSave), std::move (onGenerate));
        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Click";
        opts.content.setOwned (content);
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
