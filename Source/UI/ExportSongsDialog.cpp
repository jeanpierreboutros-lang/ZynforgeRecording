#include "ExportSongsDialog.h"

#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
    namespace
    {
        constexpr int kRowH = 30, kHeaderH = 52, kVisibleRows = 8, kWidth = 520;
        enum ContentChoice { kMix = 1, kStems = 2, kBoth = 3 };

        // H:MM:SS, for the range beside each song name.
        juce::String clock (juce::int64 samples, double rate)
        {
            const auto total = (juce::int64) ((double) samples / juce::jmax (1.0, rate));
            return juce::String::formatted ("%d:%02d:%02d", (int) (total / 3600),
                                            (int) ((total / 60) % 60), (int) (total % 60));
        }
    }

    void ExportSongsContent::Row::resized()
    {
        auto r = getLocalBounds().reduced (brand::space::xs, brand::space::xs / 2);
        tick.setBounds (r.removeFromLeft (28));
        span.setBounds (r.removeFromRight (150));
        r.removeFromRight (brand::space::sm);
        name.setBounds (r);
    }

    ExportSongsContent::ExportSongsContent (const std::vector<songexport::Range>& songs, double sessionRate,
                                            ResultCallback onResult)
        : callback (std::move (onResult)), format (sessionRate)
    {
        for (const auto& song : songs)
        {
            auto row = std::make_unique<Row>();
            row->range = song;
            row->tick.setToggleState (true, juce::dontSendNotification);
            row->tick.setColour (juce::ToggleButton::tickColourId, brand::selectFill);
            row->tick.setColour (juce::ToggleButton::tickDisabledColourId, brand::edge);
            row->tick.onClick = [this] { refreshExportEnabled(); };
            row->addAndMakeVisible (row->tick);

            dialog::styleTextEditor (row->name);
            row->name.setText (song.name, false);
            row->name.setSelectAllWhenFocused (true);
            row->name.onReturnKey = [this] { finish (true); };
            row->addAndMakeVisible (row->name);

            row->span.setText (clock (song.start, sessionRate) + " - " + clock (song.end, sessionRate),
                               juce::dontSendNotification);
            row->span.setFont (brand::type::caption());
            row->span.setColour (juce::Label::textColourId, brand::textTertiary);
            row->span.setJustificationType (juce::Justification::centredRight);
            row->addAndMakeVisible (row->span);

            listHolder.addAndMakeVisible (*row);
            rows.push_back (std::move (row));
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

        contentLabel.setText ("Write", juce::dontSendNotification);
        contentLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        addAndMakeVisible (contentLabel);

        contentBox.addItem ("Stereo mix",      kMix);
        contentBox.addItem ("Multitrack stems", kStems);
        contentBox.addItem ("Mix and stems",   kBoth);
        contentBox.setSelectedId (kBoth, juce::dontSendNotification);
        contentBox.onChange = [this]
        { stereoOneFile.setEnabled (contentBox.getSelectedId() != kMix); };
        addAndMakeVisible (contentBox);

        stereoOneFile.setToggleState (true, juce::dontSendNotification);
        stereoOneFile.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
        stereoOneFile.setColour (juce::ToggleButton::tickColourId, brand::selectFill);
        stereoOneFile.setColour (juce::ToggleButton::tickDisabledColourId, brand::edge);
        addAndMakeVisible (stereoOneFile);

        addAndMakeVisible (format);

        dialog::stylePrimary (exportButton);
        exportButton.setButtonText ("Export");
        exportButton.onClick = [this] { finish (true); };
        addAndMakeVisible (exportButton);

        dialog::styleSecondary (cancelButton);
        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { finish (false); };
        addAndMakeVisible (cancelButton);

        refreshExportEnabled();

        // Return = Export, Escape = Cancel, as in every other prompt.
        exportButton.setWantsKeyboardFocus (true);
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<ExportSongsContent> (this)]
            { if (safe != nullptr) safe->exportButton.grabKeyboardFocus(); });

        const int listH = juce::jlimit (kRowH, kRowH * kVisibleRows, (int) rows.size() * kRowH);
        setSize (kWidth, kHeaderH + 28 + listH + 28 + 28 + ExportFormatControls::kPreferredHeight + 32
                             + 4 * brand::space::md + 2 * brand::space::lg);
    }

    void ExportSongsContent::resized()
    {
        auto r = getLocalBounds().reduced (brand::space::lg);
        r.removeFromTop (kHeaderH - brand::space::lg);

        auto top = r.removeFromTop (28);
        selectAllButton.setBounds (top.removeFromLeft (100));
        top.removeFromLeft (brand::space::sm);
        clearButton.setBounds (top.removeFromLeft (80));
        r.removeFromTop (brand::space::sm);

        auto buttons = r.removeFromBottom (32);
        exportButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (brand::space::md);
        cancelButton.setBounds (buttons.removeFromRight (110));
        r.removeFromBottom (brand::space::md);

        format.setBounds (r.removeFromBottom (ExportFormatControls::kPreferredHeight));
        r.removeFromBottom (brand::space::md);
        stereoOneFile.setBounds (r.removeFromBottom (28));
        r.removeFromBottom (brand::space::sm);
        auto content = r.removeFromBottom (28);
        contentLabel.setBounds (content.removeFromLeft (110));
        contentBox.setBounds (content);
        r.removeFromBottom (brand::space::md);

        viewport.setBounds (r);
        listHolder.setSize (r.getWidth() - viewport.getScrollBarThickness(), (int) rows.size() * kRowH);
        int y = 0;
        for (auto& row : rows)
        {
            row->setBounds (0, y, listHolder.getWidth(), kRowH);
            y += kRowH;
        }
    }

    void ExportSongsContent::paint (juce::Graphics& g)
    {
        dialog::paintChrome (g, *this, "EXPORT SONGS");
    }

    songexport::Choice ExportSongsContent::currentChoice() const
    {
        songexport::Choice choice;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (! rows[i]->tick.getToggleState()) continue;
            auto song = rows[i]->range;
            const auto typed = rows[i]->name.getText().trim();
            if (typed.isNotEmpty()) song.name = typed;
            choice.songs.push_back (song);
            choice.numbers.push_back ((int) i + 1);
        }
        const int content = contentBox.getSelectedId();
        choice.mix   = content != kStems;
        choice.stems = content != kMix;
        choice.stereoOneFile = stereoOneFile.getToggleState();
        choice.format = format.getOptions();
        return choice;
    }

    void ExportSongsContent::refreshExportEnabled()
    {
        bool any = false;
        for (const auto& row : rows)
            any = any || row->tick.getToggleState();
        exportButton.setEnabled (any);
    }

    void ExportSongsContent::setAll (bool on)
    {
        for (auto& row : rows)
            row->tick.setToggleState (on, juce::dontSendNotification);
        refreshExportEnabled();
    }

    void ExportSongsContent::finish (bool accepted)
    {
        auto choice = currentChoice();
        if (accepted && choice.songs.empty()) return;
        if (callback)
            callback (accepted ? std::optional<songexport::Choice> (std::move (choice)) : std::nullopt);
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (accepted ? 1 : 0);
    }

    void ExportSongsDialog::launch (const std::vector<songexport::Range>& songs, double sessionRate,
                                    ExportSongsContent::ResultCallback onResult)
    {
        // Wrap so the callback can only fire once.
        auto shared = std::make_shared<ExportSongsContent::ResultCallback> (std::move (onResult));
        auto fireOnce = [shared] (std::optional<songexport::Choice> result)
        {
            if (shared && *shared) { (*shared) (std::move (result)); *shared = nullptr; }
        };

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (new ExportSongsContent (songs, sessionRate, fireOnce));
        opts.dialogTitle                  = "Export Songs";
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
