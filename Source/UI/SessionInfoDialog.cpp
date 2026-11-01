#include "SessionInfoDialog.h"
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        static void styleLabel (juce::Label& l)
        {
            l.setFont (brand::type::caption());
            l.setColour (juce::Label::textColourId, brand::textSecondary);
        }

        static void styleReadOnly (juce::Label& l)
        {
            l.setFont (brand::type::channelName());
            l.setColour (juce::Label::textColourId, brand::accentStatus);
        }

        class Content final : public juce::Component
        {
        public:
            explicit Content (const SessionInfoDialog::Fields& fields)
            {
                title.setText ("Session Info", juce::dontSendNotification);
                title.setFont (brand::type::headline());
                title.setColour (juce::Label::textColourId, brand::textPrimary);
                title.setJustificationType (juce::Justification::centred);
                addAndMakeVisible (title);

                // The session name is the FOLDER name, which is what the title
                // bar, the status line, the recent list and the .zfproj all use.
                // Renaming a session means moving the folder and its .zfproj
                // while the engine holds it open; that belongs in Save As.
                nameLabel.setText ("Session name", juce::dontSendNotification);
                styleLabel (nameLabel);
                addAndMakeVisible (nameLabel);
                nameValue.setText (fields.name, juce::dontSendNotification);
                styleReadOnly (nameValue);
                addAndMakeVisible (nameValue);
                nameHint.setText ("Use File > Save As to write the session under a different name",
                                  juce::dontSendNotification);
                nameHint.setFont (brand::type::caption());
                nameHint.setColour (juce::Label::textColourId, brand::textTertiary);
                addAndMakeVisible (nameHint);

                configHeader.setText ("Audio configuration", juce::dontSendNotification);
                configHeader.setFont (brand::type::captionBold());
                configHeader.setColour (juce::Label::textColourId, brand::textMuted);
                addAndMakeVisible (configHeader);

                auto setupRO = [&] (juce::Label& cap, juce::Label& val,
                                    const juce::String& capText, const juce::String& valText)
                {
                    cap.setText (capText, juce::dontSendNotification);
                    styleLabel (cap);
                    addAndMakeVisible (cap);

                    val.setText (valText.isNotEmpty() ? valText : juce::String ("--"),
                                 juce::dontSendNotification);
                    styleReadOnly (val);
                    addAndMakeVisible (val);
                };
                setupRO (sampleRateCap, sampleRateVal, "Sample rate", fields.sampleRate);
                setupRO (bitDepthCap,   bitDepthVal,   "Bit depth",   fields.bitDepth);
                setupRO (formatCap,     formatVal,     "File type",   fields.captureFormat);

                closeButton.setButtonText ("Close");
                dialog::stylePrimary (closeButton);
                closeButton.onClick = [this]
                {
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState (0);
                };
                addAndMakeVisible (closeButton);

                setSize (520, 256);
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "SESSION INFO");
            }

            void resized() override
            {
                auto b = getLocalBounds();
                title.setBounds (b.removeFromTop (40));
                b.reduce (24, 12);

                {
                    auto line = b.removeFromTop (30 + 18);
                    nameLabel.setBounds (line.removeFromTop (brand::space::xl));
                    nameValue.setBounds (line.removeFromTop (30));
                    nameHint .setBounds (b.removeFromTop (16));
                    b.removeFromTop (brand::space::sm);
                }

                b.removeFromTop (brand::space::md);
                configHeader.setBounds (b.removeFromTop (18));
                b.removeFromTop (brand::space::xs);

                auto cfg = b.removeFromTop (44);
                const int colW = cfg.getWidth() / 3;
                {
                    auto col = cfg.removeFromLeft (colW);
                    sampleRateCap.setBounds (col.removeFromTop (brand::space::xl));
                    sampleRateVal.setBounds (col.removeFromTop (brand::space::btnH));
                }
                {
                    auto col = cfg.removeFromLeft (colW);
                    bitDepthCap.setBounds (col.removeFromTop (brand::space::xl));
                    bitDepthVal.setBounds (col.removeFromTop (brand::space::btnH));
                }
                {
                    formatCap.setBounds (cfg.removeFromTop (brand::space::xl));
                    formatVal.setBounds (cfg.removeFromTop (brand::space::btnH));
                }

                auto footer = getLocalBounds().removeFromBottom (56).reduced (24, 12);
                closeButton.setBounds (footer.removeFromRight (100));
            }

        private:
            juce::Label title;
            juce::Label nameLabel, nameValue, nameHint;
            juce::Label configHeader;
            juce::Label sampleRateCap, bitDepthCap, formatCap;
            juce::Label sampleRateVal, bitDepthVal, formatVal;
            juce::TextButton closeButton;

            JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Content)
        };
    }

    void SessionInfoDialog::launch (Fields fields)
    {
        if (launchForTests) { launchForTests (std::move (fields)); return; }
        auto* content = new Content (fields);

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle                  = "Session Info";
        opts.content.setOwned (content);
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
