#include "ExportDialog.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "../Theme/DialogChrome.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"

namespace zynforge
{
    ExportFormatControls::ExportFormatControls (double sessionRate)
    {
        // Format
        formatLabel.setText ("Format", juce::dontSendNotification);
        formatLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        addAndMakeVisible (formatLabel);

        formatBox.addItem ("WAV",  1);
        formatBox.addItem ("AIFF", 2);
        formatBox.addItem ("FLAC", 3);
        formatBox.setSelectedId (1);
        formatBox.onChange = [this] { refreshAvailability(); };
        addAndMakeVisible (formatBox);

        // Sample rate
        rateLabel.setText ("Sample rate", juce::dontSendNotification);
        rateLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        addAndMakeVisible (rateLabel);

        // Standard rates, plus the session's own rate when it isn't one
        // of them (88.2 kHz sessions exist). The session rate is
        // preselected and marked, so "just export it" is a straight copy
        // and choosing a conversion is a deliberate act.
        rateHz = { 44100.0, 48000.0, 96000.0, 192000.0 };
        const double sr = sessionRate > 0.0 ? sessionRate : 48000.0;
        if (std::find_if (rateHz.begin(), rateHz.end(),
                          [sr] (double r) { return std::abs (r - sr) < 1.0; })
            == rateHz.end())
            rateHz.push_back (sr);
        std::sort (rateHz.begin(), rateHz.end());

        int selectId = 1;
        for (size_t i = 0; i < rateHz.size(); ++i)
        {
            const bool isSession = std::abs (rateHz[i] - sr) < 1.0;
            auto label = juce::String (rateHz[i] / 1000.0, rateHz[i] < 100000.0 ? 1 : 0)
                         + " kHz" + (isSession ? "  (session)" : "");
            rateBox.addItem (label, (int) i + 1);
            if (isSession) selectId = (int) i + 1;
        }
        rateBox.setSelectedId (selectId);
        rateBox.onChange = [this] { refreshAvailability(); };
        addAndMakeVisible (rateBox);

        srNote.setFont (brand::type::caption());
        srNote.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (srNote);

        // Bit depth
        bitsLabel.setText ("Bit depth", juce::dontSendNotification);
        bitsLabel.setColour (juce::Label::textColourId, brand::textPrimary);
        addAndMakeVisible (bitsLabel);

        bitsBox.addItem ("16-bit PCM",   1);
        bitsBox.addItem ("24-bit PCM",   2);
        bitsBox.addItem ("32-bit float", 3);
        bitsBox.setSelectedId (2);
        addAndMakeVisible (bitsBox);

        sessionRateHz = sr;
        refreshAvailability();
    }

    void ExportFormatControls::resized()
    {
        auto r = getLocalBounds();
        const int rowH = 28;
        const int labelW = 110;

        auto row = [&] (juce::Label& label, juce::ComboBox& box)
        {
            auto rr = r.removeFromTop (rowH);
            label.setBounds (rr.removeFromLeft (labelW));
            box  .setBounds (rr);
            r.removeFromTop (brand::space::md);
        };

        row (formatLabel,  formatBox);
        row (rateLabel,    rateBox);
        row (bitsLabel,    bitsBox);

        srNote.setBounds (r.removeFromTop (18));
    }

    void ExportFormatControls::refreshAvailability()
    {
        const bool isFlac = (formatBox.getSelectedId() == 3);

        // FLAC: only 16-bit or 24-bit valid.
        // Disable the 32-bit-float item by rebuilding the combo state,
        // but PRESERVE the engineer's current bit-depth across the
        // rebuild -- clear() wiped it, so flipping WAV->FLAC (both do
        // 16-bit) silently jumped a chosen 16-bit back to 24-bit.
        const int prevId = bitsBox.getSelectedId();
        bitsBox.clear (juce::dontSendNotification);
        bitsBox.addItem ("16-bit PCM",   1);
        bitsBox.addItem ("24-bit PCM",   2);
        if (! isFlac)
            bitsBox.addItem ("32-bit float", 3);
        const bool prevStillValid = prevId == 1 || prevId == 2 || (prevId == 3 && ! isFlac);
        bitsBox.setSelectedId (prevStillValid ? prevId : 2, juce::dontSendNotification);

        const int rateIdx = rateBox.getSelectedId() - 1;
        const double chosen = (rateIdx >= 0 && rateIdx < (int) rateHz.size())
                                  ? rateHz[(size_t) rateIdx] : sessionRateHz;
        const bool converting = std::abs (chosen - sessionRateHz) > 1.0;
        srNote.setText (converting
                          ? "Sample-rate conversion from "
                            + juce::String (sessionRateHz / 1000.0, 1) + " kHz"
                          : "No sample-rate conversion",
                        juce::dontSendNotification);
        srNote.setColour (juce::Label::textColourId,
                          converting ? brand::accentSolo : brand::textTertiary);
    }

    ExportOptions ExportFormatControls::getOptions() const
    {
        ExportOptions opts;

        switch (formatBox.getSelectedId())
        {
            case 1: opts.format = ExportFormat::Wav24;  break;
            case 2: opts.format = ExportFormat::Aiff24; break;
            case 3: opts.format = ExportFormat::Flac24; break;
        }

        const int rateIdx = rateBox.getSelectedId() - 1;
        opts.sampleRate = (rateIdx >= 0 && rateIdx < (int) rateHz.size())
                              ? rateHz[(size_t) rateIdx]
                              : sessionRateHz;

        switch (bitsBox.getSelectedId())
        {
            case 1: opts.bitsPerSample = 16; break;
            case 2: opts.bitsPerSample = 24; break;
            case 3: opts.bitsPerSample = 32; break;
            default: opts.bitsPerSample = 24;
        }

        return opts;
    }

    ExportAudioContent::ExportAudioContent (double sessionRate, const std::vector<juce::String>& names,
                                            ResultCallback onResult)
        : callback (std::move (onResult)), format (sessionRate)
    {
        for (auto* radio : { &rawButton, &editedButton, &mixButton })
        {
            radio->setRadioGroupId (1);
            radio->setColour (juce::ToggleButton::textColourId, brand::textPrimary);
            radio->setColour (juce::ToggleButton::tickColourId, brand::selectFill);
            radio->setColour (juce::ToggleButton::tickDisabledColourId, brand::edge);
            radio->onClick = [this] { refresh(); };
            addAndMakeVisible (*radio);
        }
        rawButton.setToggleState (true, juce::dontSendNotification);

        note.setFont (brand::type::caption());
        note.setColour (juce::Label::textColourId, brand::textSecondary);
        note.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (note);

        // One tick per strip, all on: the common case is the whole session, and
        // un-ticking a few is quicker than ticking many.
        for (const auto& name : names)
        {
            auto tick = std::make_unique<juce::ToggleButton> (name);
            tick->setToggleState (true, juce::dontSendNotification);
            tick->setColour (juce::ToggleButton::textColourId, brand::textPrimary);
            tick->setColour (juce::ToggleButton::tickColourId, brand::selectFill);
            tick->setColour (juce::ToggleButton::tickDisabledColourId, brand::edge);
            tick->onClick = [this] { refresh(); };
            listHolder.addAndMakeVisible (*tick);
            ticks.push_back (std::move (tick));
        }
        viewport.setViewedComponent (&listHolder, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        dialog::styleSecondary (selectAllButton);
        selectAllButton.setButtonText ("Select all");
        selectAllButton.onClick = [this] { setAllTicked (true); };
        addAndMakeVisible (selectAllButton);

        dialog::styleSecondary (clearButton);
        clearButton.setButtonText ("Clear");
        clearButton.onClick = [this] { setAllTicked (false); };
        addAndMakeVisible (clearButton);

        addAndMakeVisible (format);

        dialog::stylePrimary (exportButton);
        exportButton.setButtonText ("Export");
        exportButton.onClick = [this] { finish (true); };
        addAndMakeVisible (exportButton);

        dialog::styleSecondary (cancelButton);
        cancelButton.setButtonText ("Cancel");
        cancelButton.onClick = [this] { finish (false); };
        addAndMakeVisible (cancelButton);

        refresh();

        // Return = Export, Escape = Cancel, matching every other prompt in the
        // app (result code 1 for the primary action).
        exportButton.setWantsKeyboardFocus (true);
        juce::MessageManager::callAsync (
            [safe = juce::Component::SafePointer<ExportAudioContent> (this)]
            { if (safe != nullptr) safe->exportButton.grabKeyboardFocus(); });

        const int listH = juce::jlimit (kRowH, kRowH * kVisibleRows, (int) ticks.size() * kRowH);
        setSize (kWidth, dialog::titleH + kRowH + 20 + 28 + listH + ExportFormatControls::kPreferredHeight + 32
                             + 5 * brand::space::md + 2 * brand::space::lg);
    }

    void ExportAudioContent::resized()
    {
        auto r = getLocalBounds().reduced (brand::space::lg);
        r.removeFromTop (dialog::titleH - brand::space::lg);

        auto choices = r.removeFromTop (kRowH);
        const int third = choices.getWidth() / 3;
        rawButton   .setBounds (choices.removeFromLeft (third));
        editedButton.setBounds (choices.removeFromLeft (third));
        mixButton   .setBounds (choices);
        note.setBounds (r.removeFromTop (20));
        r.removeFromTop (brand::space::md);

        auto top = r.removeFromTop (28);
        selectAllButton.setBounds (top.removeFromLeft (100));
        top.removeFromLeft (brand::space::sm);
        clearButton.setBounds (top.removeFromLeft (80));
        r.removeFromTop (brand::space::md);

        auto buttons = r.removeFromBottom (32);
        exportButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (brand::space::md);
        cancelButton.setBounds (buttons.removeFromRight (110));
        r.removeFromBottom (brand::space::md);

        format.setBounds (r.removeFromBottom (ExportFormatControls::kPreferredHeight));
        r.removeFromBottom (brand::space::md);

        viewport.setBounds (r);
        listHolder.setSize (juce::jmax (0, r.getWidth() - 10), (int) ticks.size() * kRowH);
        int y = 0;
        for (auto& tick : ticks)
        {
            tick->setBounds (4, y, listHolder.getWidth() - 8, kRowH);
            y += kRowH;
        }
    }

    void ExportAudioContent::paint (juce::Graphics& g)
    {
        dialog::paintChrome (g, *this, "EXPORT AUDIO");
    }

    void ExportAudioContent::chooseSource (Source source)
    {
        auto& radio = source == Source::rawTakes ? rawButton
                    : source == Source::editedTracks ? editedButton : mixButton;
        radio.setToggleState (true, juce::dontSendNotification);
        refresh();
    }

    ExportAudioContent::Source ExportAudioContent::getSource() const
    {
        return editedButton.getToggleState() ? Source::editedTracks
             : mixButton.getToggleState()    ? Source::stereoMix
                                             : Source::rawTakes;
    }

    void ExportAudioContent::setAllTicked (bool on)
    {
        for (auto& tick : ticks)
            tick->setToggleState (on, juce::dontSendNotification);
        refresh();
    }

    void ExportAudioContent::refresh()
    {
        const auto source = getSource();
        note.setText (source == Source::rawTakes     ? "The recorded takes, without clip edits, in the format below."
                    : source == Source::editedTracks ? "Each ticked strip with its clip edits. Always 24-bit WAV."
                                                     : "The edited session through the mix, one file. Always 24-bit WAV.",
                      juce::dontSendNotification);
        format.setEnabled (source == Source::rawTakes);

        const bool usesTicks = source != Source::stereoMix;
        viewport       .setEnabled (usesTicks);
        selectAllButton.setEnabled (usesTicks);
        clearButton    .setEnabled (usesTicks);

        bool any = false;
        for (auto& tick : ticks) any = any || tick->getToggleState();
        exportButton.setEnabled (any || ! usesTicks);
    }

    ExportAudioContent::Result ExportAudioContent::getResult() const
    {
        Result result;
        result.source  = getSource();
        result.options = format.getOptions();
        if (result.source != Source::stereoMix)
            for (int i = 0; i < (int) ticks.size(); ++i)
                if (ticks[(size_t) i]->getToggleState())
                    result.rows.push_back (i);
        return result;
    }

    void ExportAudioContent::finish (bool accepted)
    {
        if (callback) callback (accepted ? std::optional<Result> (getResult()) : std::nullopt);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (accepted ? 1 : 0);
    }

    void ExportAudioDialog::launch (double sessionSampleRate,
                                    const std::vector<juce::String>& trackNames,
                                    ExportAudioContent::ResultCallback onResult)
    {
        // We wrap the callback so the dialog can only fire it once.
        auto shared = std::make_shared<ExportAudioContent::ResultCallback> (std::move (onResult));
        auto fireOnce = [shared] (std::optional<ExportAudioContent::Result> result)
        {
            if (shared && *shared) { (*shared) (std::move (result)); *shared = nullptr; }
        };

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned (new ExportAudioContent (sessionSampleRate, trackNames, fireOnce));
        opts.dialogTitle                  = "Export Audio";
        opts.dialogBackgroundColour       = brand::bgPanel;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar            = true;
        opts.resizable                    = false;
        opts.launchAsync();
    }
}
