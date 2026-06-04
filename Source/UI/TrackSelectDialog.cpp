#include "TrackSelectDialog.h"
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    namespace
    {
        class TrackSelectContent final : public juce::Component
        {
        public:
            TrackSelectContent (const std::vector<juce::String>& names,
                                TrackSelectDialog::ResultCallback cb)
                : callback (std::move (cb))
            {
                // One toggle per track, hosted in a scrollable list so a
                // 32-track session doesn't overflow the dialog. Default to
                // all ticked -- the common case is "export these takes" and
                // un-ticking a few is quicker than ticking many.
                for (int i = 0; i < (int) names.size(); ++i)
                {
                    auto tb = std::make_unique<juce::ToggleButton> (names[(size_t) i]);
                    tb->setToggleState (true, juce::dontSendNotification);
                    tb->setColour (juce::ToggleButton::textColourId,   brand::textPrimary);
                    tb->setColour (juce::ToggleButton::tickColourId,   brand::accentPlay);
                    tb->setColour (juce::ToggleButton::tickDisabledColourId, brand::edge);
                    tb->onClick = [this] { refreshExportEnabled(); };
                    listHolder.addAndMakeVisible (*tb);
                    toggles.push_back (std::move (tb));
                }

                viewport.setViewedComponent (&listHolder, false);
                viewport.setScrollBarsShown (true, false);
                addAndMakeVisible (viewport);

                dialog::styleSecondary (selectAllButton);
                selectAllButton.setButtonText ("Select all");
                selectAllButton.onClick = [this] { setAll (true); };
                addAndMakeVisible (selectAllButton);

                dialog::styleSecondary (clearButton);
                clearButton.setButtonText ("Clear");
                clearButton.onClick = [this] { setAll (false); };
                addAndMakeVisible (clearButton);

                dialog::stylePrimary (exportButton);
                exportButton.setButtonText ("Export");
                exportButton.onClick = [this] { onAccept(); };
                addAndMakeVisible (exportButton);

                dialog::styleSecondary (cancelButton);
                cancelButton.setButtonText ("Cancel");
                cancelButton.onClick = [this] { onCancel(); };
                addAndMakeVisible (cancelButton);

                refreshExportEnabled();

                // Height: header + list (capped) + button rows.
                const int rows      = juce::jmax (1, (int) toggles.size());
                const int listH     = juce::jlimit (kRowH, kRowH * 12, rows * kRowH);
                setSize (380, kHeaderH + listH + 96);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (16);
                r.removeFromTop (kHeaderH - 16);   // clear the EXPORT chrome title band

                // Top row: Select all / Clear.
                auto topRow = r.removeFromTop (28);
                selectAllButton.setBounds (topRow.removeFromLeft (100));
                topRow.removeFromLeft (brand::space::sm);
                clearButton.setBounds (topRow.removeFromLeft (80));
                r.removeFromTop (brand::space::sm);

                // Bottom: Export / Cancel.
                auto btnRow = r.removeFromBottom (32);
                exportButton.setBounds (btnRow.removeFromRight (110));
                btnRow.removeFromRight (brand::space::md);
                cancelButton.setBounds (btnRow.removeFromRight (110));
                r.removeFromBottom (brand::space::md);

                // The rest is the scrollable track list.
                viewport.setBounds (r);
                listHolder.setSize (r.getWidth() - 10, (int) toggles.size() * kRowH);
                int y = 0;
                for (auto& tb : toggles)
                {
                    tb->setBounds (4, y, listHolder.getWidth() - 8, kRowH);
                    y += kRowH;
                }
            }

            void paint (juce::Graphics& g) override
            {
                dialog::paintChrome (g, *this, "EXPORT TRACKS");
            }

        private:
            static constexpr int kRowH    = 26;
            static constexpr int kHeaderH = 52;

            void setAll (bool on)
            {
                for (auto& tb : toggles)
                    tb->setToggleState (on, juce::dontSendNotification);
                refreshExportEnabled();
            }

            void refreshExportEnabled()
            {
                bool any = false;
                for (auto& tb : toggles)
                    if (tb->getToggleState()) { any = true; break; }
                exportButton.setEnabled (any);
            }

            void onAccept()
            {
                std::vector<int> chosen;
                for (int i = 0; i < (int) toggles.size(); ++i)
                    if (toggles[(size_t) i]->getToggleState())
                        chosen.push_back (i);

                if (chosen.empty()) return;   // Export is disabled, but guard anyway
                if (callback) callback (chosen);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (1);
            }

            void onCancel()
            {
                if (callback) callback (std::nullopt);
                if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);
            }

            TrackSelectDialog::ResultCallback callback;

            juce::Viewport  viewport;
            juce::Component listHolder;
            std::vector<std::unique_ptr<juce::ToggleButton>> toggles;
            juce::TextButton selectAllButton, clearButton, exportButton, cancelButton;
        };
    }

    void TrackSelectDialog::launch (const juce::String& title,
                                    const std::vector<juce::String>& names,
                                    ResultCallback cb)
    {
        // Wrap so the callback can only fire once.
        auto shared = std::make_shared<ResultCallback> (std::move (cb));
        auto fireOnce = [shared] (std::optional<std::vector<int>> result)
        {
            if (shared && *shared) { (*shared)(std::move (result)); *shared = nullptr; }
        };

        auto content = std::make_unique<TrackSelectContent> (names, fireOnce);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (content.release());
        opts.dialogTitle                  = title;
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
