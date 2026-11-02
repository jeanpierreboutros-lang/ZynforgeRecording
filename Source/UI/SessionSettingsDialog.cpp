// Session ▸ Session Settings: capture container / bit depth, session sample
// rate, recording location and the optional stereo stream-mix capture.
// Moved out of MainComponent::menuItemSelected, where it was declared inline
// inside the menu dispatcher; it now reaches the host only through callbacks.

#include "SessionSettingsDialog.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"

namespace zynforge
{
namespace
{
    struct SessionFormatContent final : public juce::Component
    {
        SessionFormatContent (AudioEngine& e, std::function<void()> relocate, std::function<void (double)> applyRate)
            : eng (e), onRelocate (std::move (relocate)), onApplySampleRate (std::move (applyRate)) { rebuild(); setSize (460, 378); }

        void rebuild()
        {
            using F = zynforge::CaptureFormat;
            const auto cur = eng.getRecorder().getCaptureFormat();
            // Show the SESSION's intended rate (what the session was
            // created/opened at), NOT the live device rate -- a Dante
            // clock-slave often runs a different rate than the session.
            const double sr = eng.getSessionSampleRate() > 0.0
                                ? eng.getSessionSampleRate()
                                : eng.getDeviceManager().getAudioDeviceSetup().sampleRate;

            fmtL.setText ("Audio Format", juce::dontSendNotification);
            rateL.setText ("Sample Rate", juce::dontSendNotification);
            bitsL.setText ("Bit Depth", juce::dontSendNotification);
            preRollL.setText ("Pre-roll", juce::dontSendNotification);
            for (auto* l : { &fmtL, &rateL, &bitsL, &preRollL })
                { l->setColour (juce::Label::textColourId, brand::textPrimary); addAndMakeVisible (*l); }

            fmtBox.addItem ("WAV",  1);
            fmtBox.addItem ("AIFF", 2);
            fmtBox.addItem ("FLAC", 3);
            int container = (cur == F::Wav16 || cur == F::Wav24 || cur == F::Wav32Float) ? 1
                          : (cur == F::Aiff16 || cur == F::Aiff24 || cur == F::Aiff32Float) ? 2 : 3;
            fmtBox.setSelectedId (container, juce::dontSendNotification);
            fmtBox.onChange = [this] { refreshBits(); };
            for (auto* box : { &fmtBox, &rateBox, &bitsBox, &preRollBox }) dialog::styleCombo (*box);
            addAndMakeVisible (fmtBox);

            rateBox.addItem ("44.1 kHz",  1);
            rateBox.addItem ("48 kHz",    2);
            rateBox.addItem ("88.2 kHz",  3);
            rateBox.addItem ("96 kHz",    4);
            rateBox.addItem ("176.4 kHz", 5);
            rateBox.addItem ("192 kHz",   6);
            rateBox.setSelectedId (rateIdFromHz (sr), juce::dontSendNotification);
            addAndMakeVisible (rateBox);

            preRollBox.setTitle ("Pre-roll");
            preRollBox.setTooltip ("Audio held before RECORD and written to the start of a fresh take");
            for (int seconds : { 0, 5, 10, 30 })
                preRollBox.addItem (seconds == 0 ? juce::String ("Off") : juce::String (seconds) + " s", seconds + 1);
            preRollBox.setSelectedId (eng.getRecorder().getPreRollSeconds() + 1, juce::dontSendNotification);
            addAndMakeVisible (preRollBox);

            addAndMakeVisible (bitsBox);
            refreshBits();
            bitsBox.setSelectedId ((cur == F::Wav16 || cur == F::Aiff16 || cur == F::Flac16) ? 1
                                 : (cur == F::Wav32Float || cur == F::Aiff32Float) ? 3 : 2,
                                 juce::dontSendNotification);

            // Recording location -- the active session folder. "Change..."
            // moves the whole session (recorded audio included) elsewhere.
            pathL.setText ("Recording Path", juce::dontSendNotification);
            pathL.setColour (juce::Label::textColourId, brand::textPrimary);
            addAndMakeVisible (pathL);

            const auto dir = eng.getActiveSessionDir();
            pathVal.setText (dir.isDirectory() ? dir.getFullPathName()
                                               : juce::String ("(no active session)"),
                             juce::dontSendNotification);
            pathVal.setColour (juce::Label::textColourId, brand::textSecondary);
            pathVal.setFont (brand::type::caption());
            pathVal.setTooltip (pathVal.getText());
            addAndMakeVisible (pathVal);

            changePathB.setButtonText ("Change...");
            changePathB.setEnabled (dir.isDirectory() && ! eng.isRecording());
            changePathB.onClick = [this]
            {
                auto relocate = onRelocate;   // copy first: close() retires this dialog
                close();                      // dismiss settings first
                if (relocate) relocate();     // then run the move flow
            };
            addAndMakeVisible (changePathB);

            stereoMixB.setButtonText ("Record optional stereo stream mix");
            dialog::styleCheckRow (stereoMixB);
            stereoMixB.setTooltip ("Also records the stream-send bus to Export Files; multitrack capture is unchanged.");
            stereoMixB.setToggleState (eng.getRecordStereoMix(), juce::dontSendNotification);
            addAndMakeVisible (stereoMixB);

            applyB.setButtonText ("Apply");
            cancelB.setButtonText ("Cancel");
            dialog::stylePrimary (applyB);
            dialog::styleSecondary (cancelB);
            applyB.onClick  = [this] { apply(); };
            cancelB.onClick = [this] { close(); };
            addAndMakeVisible (applyB);
            addAndMakeVisible (cancelB);
        }

        static int rateIdFromHz (double hz)
        {
            if (juce::approximatelyEqual (hz, 44100.0))  return 1;
            if (juce::approximatelyEqual (hz, 88200.0))  return 3;
            if (juce::approximatelyEqual (hz, 96000.0))  return 4;
            if (juce::approximatelyEqual (hz, 176400.0)) return 5;
            if (juce::approximatelyEqual (hz, 192000.0)) return 6;
            return 2;   // 48 kHz default
        }
        static double hzFromRateId (int id)
        {
            switch (id) { case 1: return 44100.0;  case 3: return 88200.0;
                          case 4: return 96000.0;  case 5: return 176400.0;
                          case 6: return 192000.0; }
            return 48000.0;
        }

        void refreshBits()
        {
            const int prev = bitsBox.getSelectedId();
            bitsBox.clear (juce::dontSendNotification);
            bitsBox.addItem ("16-bit PCM", 1);
            bitsBox.addItem ("24-bit PCM", 2);
            if (fmtBox.getSelectedId() != 3) bitsBox.addItem ("32-bit float", 3);
            if (prev > 0) bitsBox.setSelectedId (prev, juce::dontSendNotification);
            if (bitsBox.getSelectedId() == 0)
                bitsBox.setSelectedId (2, juce::dontSendNotification);
        }

        void apply()
        {
            if (eng.isRecording()) { close(); return; }
            using F = zynforge::CaptureFormat;
            F f = F::Wav24;
            const int c = fmtBox.getSelectedId(), b = bitsBox.getSelectedId();
            if      (c == 1) f = b == 1 ? F::Wav16 : b == 2 ? F::Wav24 : F::Wav32Float;
            else if (c == 2) f = b == 1 ? F::Aiff16 : b == 2 ? F::Aiff24 : F::Aiff32Float;
            else             f = b == 1 ? F::Flac16 : F::Flac24;
            eng.getRecorder().setCaptureFormat (f);
            if (preRollBox.getSelectedId() > 0)
                eng.getRecorder().setPreRollSeconds (preRollBox.getSelectedId() - 1);
            eng.setRecordStereoMix (stereoMixB.getToggleState());

            // Change the SESSION rate (updates pendingSampleRate + the
            // record-guard, and asks the device to follow) via the host,
            // so it actually sticks instead of only nudging the device.
            if (onApplySampleRate) onApplySampleRate (hzFromRateId (rateBox.getSelectedId()));
            close();
        }

        void close()
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        }

        void resized() override
        {
            auto r = dialog::bodyBounds (*this).reduced (brand::space::xl, brand::space::md);
            const int rowH = 30, labelW = 120;
            auto row = [&] (juce::Label& l, juce::ComboBox& b)
            {
                auto rr = r.removeFromTop (rowH);
                l.setBounds (rr.removeFromLeft (labelW));
                b.setBounds (rr);
                r.removeFromTop (brand::space::sm);
            };
            row (fmtL, fmtBox); row (rateL, rateBox); row (bitsL, bitsBox); row (preRollL, preRollBox);

            // Recording-path row: label on top, path + Change... below.
            r.removeFromTop (brand::space::sm);
            pathL.setBounds (r.removeFromTop (20));
            auto pr = r.removeFromTop (rowH);
            changePathB.setBounds (pr.removeFromRight (110));
            pr.removeFromRight (brand::space::sm);
            pathVal.setBounds (pr);

            r.removeFromTop (brand::space::md);
            stereoMixB.setBounds (r.removeFromTop (28));

            auto br = dialog::footerBounds (*this).reduced (0, brand::space::xs);
            applyB .setBounds (br.removeFromRight (dialog::btnPrimary));
            br.removeFromRight (brand::space::sm);
            cancelB.setBounds (br.removeFromRight (dialog::btnSecond));
        }

        void paint (juce::Graphics& g) override
        {
            dialog::paintChrome (g, *this, "SESSION SETTINGS");
        }

        AudioEngine& eng;
        std::function<void()>         onRelocate;
        std::function<void (double)>  onApplySampleRate;
        juce::Label fmtL, rateL, bitsL, preRollL, pathL, pathVal;
        juce::ComboBox fmtBox, rateBox, bitsBox, preRollBox;
        juce::TextButton applyB, cancelB, changePathB;
        juce::ToggleButton stereoMixB;
    };
}

void SessionSettingsDialog::launch (AudioEngine& engine,
                                    std::function<void()> onRelocate,
                                    std::function<void (double)> onApplySampleRate)
{
    auto* content = new SessionFormatContent (engine, std::move (onRelocate), std::move (onApplySampleRate));

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (content);
    opts.dialogTitle                  = "Session Settings";
    opts.dialogBackgroundColour       = brand::bgElevated;
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar            = true;
    opts.resizable                    = false;
    if (auto* dw = opts.launchAsync())
    {
        dw->setAlwaysOnTop (true);
        dw->toFront (true);
    }
}
}
