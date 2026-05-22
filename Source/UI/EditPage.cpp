#include "EditPage.h"
#include "../Theme/BrandColors.h"
#include "LedMeter.h"
#include "StripColourPicker.h"

namespace zynforge
{
    // Per-track row: header on the left (colour wash + name + REC/MUTE/SOLO),
    // waveform on the right. The TrackList builds one of these per track and
    // stacks them vertically inside the EditPage's viewport.
    class EditPage::TrackRow final : public juce::Component
    {
    public:
        TrackRow (int trackIdx,
                  AudioEngine& eng,
                  juce::AudioFormatManager& formats,
                  juce::AudioThumbnailCache& cache)
            : index (trackIdx), engine (eng),
              thumbnail (1024, formats, cache),
              meter (engine.getRecorder().getTrack (index))
        {
            auto& s = engine.getRecorder().getTrack (index);

            nameLabel.setText (s.name, juce::dontSendNotification);
            nameLabel.setJustificationType (juce::Justification::centredLeft);
            nameLabel.setColour (juce::Label::textColourId, brand::textPrimary);
            nameLabel.setFont (juce::Font (juce::FontOptions().withHeight (13.0f).withStyle ("Bold")));
            nameLabel.setEditable (false, true, false);
            nameLabel.onTextChange = [this]
            {
                const auto newName = nameLabel.getText().trim();
                auto& st = engine.getRecorder().getTrack (index);
                st.name = newName.isEmpty() ? juce::String ("In " + juce::String (index + 1))
                                            : newName;
                nameLabel.setText (st.name, juce::dontSendNotification);
                engine.setTrackName (index, st.name);
            };
            addAndMakeVisible (nameLabel);

            auto styleBtn = [] (juce::ToggleButton& b, juce::Colour onCol)
            {
                b.setColour (juce::ToggleButton::textColourId, brand::textPrimary);
                b.setColour (juce::ToggleButton::tickColourId, onCol);
            };
            armButton .setToggleState (s.armed .load(), juce::dontSendNotification);
            muteButton.setToggleState (s.muted .load(), juce::dontSendNotification);
            soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
            styleBtn (armButton,  brand::accentRecord);
            styleBtn (muteButton, brand::accentRecord);
            styleBtn (soloButton, juce::Colour::fromRGB (0xff, 0xd6, 0x4d));
            armButton .onClick = [this] { engine.getRecorder().getTrack (index).armed .store (armButton .getToggleState()); };
            muteButton.onClick = [this] { engine.getRecorder().getTrack (index).muted .store (muteButton.getToggleState()); };
            soloButton.onClick = [this] { engine.getRecorder().getTrack (index).soloed.store (soloButton.getToggleState()); };
            addAndMakeVisible (armButton);
            addAndMakeVisible (muteButton);
            addAndMakeVisible (soloButton);

            // Input + output routing combos — same wiring as the mixer.
            auto styleCombo = [] (juce::ComboBox& c)
            {
                c.setColour (juce::ComboBox::backgroundColourId, brand::bgDeep.withAlpha (0.55f));
                c.setColour (juce::ComboBox::outlineColourId,    brand::edge);
                c.setColour (juce::ComboBox::textColourId,       brand::textPrimary);
                c.setColour (juce::ComboBox::arrowColourId,      brand::textMuted);
            };
            styleCombo (inputCombo);
            styleCombo (outputCombo);

            inputCombo.onChange = [this]
            {
                const int id = inputCombo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackLinkedRouting (index, dev);
            };
            outputCombo.onChange = [this]
            {
                const int id = outputCombo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackLinkedRouting (index, dev);
            };
            addAndMakeVisible (inputCombo);
            addAndMakeVisible (outputCombo);

            rebuildRoutingCombos();
            refreshRoutingSelection();

            // Live signal meter on the right edge of the header.
            addAndMakeVisible (meter);
            meter.setTooltip ("Live signal level — click to clear clip.");

            updatePollState();
        }

        // Cheap poll — called by EditPage::timerCallback so mixer-side
        // changes (mute/solo via mixer, rename, etc.) show up here.
        void updatePollState()
        {
            auto& s = engine.getRecorder().getTrack (index);
            if (armButton .getToggleState() != s.armed .load()) armButton .setToggleState (s.armed .load(), juce::dontSendNotification);
            if (muteButton.getToggleState() != s.muted .load()) muteButton.setToggleState (s.muted .load(), juce::dontSendNotification);
            if (soloButton.getToggleState() != s.soloed.load()) soloButton.setToggleState (s.soloed.load(), juce::dontSendNotification);
            const auto curName = nameLabel.getText();
            if (curName != s.name) nameLabel.setText (s.name, juce::dontSendNotification);

            // Reflect device topology + patch-page changes.
            const int curIn  = engine.getCurrentDeviceInputCount();
            const int curOut = engine.getCurrentDeviceOutputCount();
            if (curIn != lastInputDeviceCount || curOut != lastOutputDeviceCount)
            {
                lastInputDeviceCount  = curIn;
                lastOutputDeviceCount = curOut;
                rebuildRoutingCombos();
            }
            refreshRoutingSelection();

            // Stripe colour might have changed (mixer right-click → colour
            // picker). Repaint the wash on the next paint cycle.
            const auto argb = s.colourARGB.load();
            if (argb != lastColourArgb)
            {
                lastColourArgb = argb;
                repaint();
            }
        }

        void rebuildRoutingCombos()
        {
            const int numIns  = engine.getCurrentDeviceInputCount();
            const int numOuts = engine.getCurrentDeviceOutputCount();
            const int visibleIn  = juce::jmax (numIns,  index + 1, 8);
            const int visibleOut = juce::jmax (numOuts, index + 1, 8);

            inputCombo.clear (juce::dontSendNotification);
            inputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleIn; ++i)
                inputCombo.addItem (i < numIns ? ("In "  + juce::String (i + 1))
                                               : ("In "  + juce::String (i + 1) + " (off)"),
                                    i + 2);

            outputCombo.clear (juce::dontSendNotification);
            outputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleOut; ++i)
                outputCombo.addItem (i < numOuts ? ("Out " + juce::String (i + 1))
                                                 : ("Out " + juce::String (i + 1) + " (off)"),
                                     i + 2);
        }

        void refreshRoutingSelection()
        {
            auto& s = engine.getRecorder().getTrack (index);
            int inR  = s.inputRouting .load();
            int outR = s.outputRouting.load();
            if (inR  == -2) inR  = index;
            if (outR == -2) outR = index;
            const int wantIn  = (inR  < 0) ? 1 : inR  + 2;
            const int wantOut = (outR < 0) ? 1 : outR + 2;
            if (inputCombo .getSelectedId() != wantIn ) inputCombo .setSelectedId (wantIn,  juce::dontSendNotification);
            if (outputCombo.getSelectedId() != wantOut) outputCombo.setSelectedId (wantOut, juce::dontSendNotification);
        }

        void setWaveformFile (const juce::File& f)
        {
            if (f == currentFile) return;
            currentFile = f;
            if (f.existsAsFile())
                thumbnail.setSource (new juce::FileInputSource (f));
            else
                thumbnail.setSource (nullptr);
            repaint();
        }

        // Drawn position of the playhead within this row, in pixels from
        // the start of the waveform pane. -1 = playhead not visible / no
        // session loaded.
        void setPlayheadX (int px) { playheadX = px; repaint(); }

        void paint (juce::Graphics& g) override
        {
            const auto fillColour = getStripColour();

            // ─── Colour swatch column (click to change track colour)
            auto header = getLocalBounds().withWidth (headerW);
            auto swatchArea = header.removeFromLeft (swatchW);
            g.setColour (fillColour);
            g.fillRect (swatchArea);
            g.setColour (fillColour.darker (0.40f));
            g.drawVerticalLine (swatchArea.getRight() - 1, 0.0f, (float) getHeight());

            // ─── Header background (panel)
            g.setColour (brand::bgPanel);
            g.fillRect (header);
            g.setColour (brand::edge);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
            g.drawVerticalLine (headerW - 1, 0.0f, (float) getHeight());

            // ─── Waveform pane
            auto wavePane = getLocalBounds().withTrimmedLeft (headerW);
            g.setColour (brand::bgPanel);
            g.fillRect (wavePane);

            if (thumbnail.getTotalLength() > 0.0)
            {
                // Pro-Tools-style blue waveform on a slightly darker pane.
                g.setColour (juce::Colour::fromRGB (0x4c, 0x82, 0xc8));
                thumbnail.drawChannels (g,
                                        wavePane.reduced (4, 6),
                                        0.0, thumbnail.getTotalLength(),
                                        1.0f);
            }
            else
            {
                g.setColour (brand::textTertiary);
                g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
                g.drawText ("(no recording yet — start a session and record to see waveforms)",
                            wavePane, juce::Justification::centred, false);
            }

            // ─── Playhead overlay
            if (playheadX >= 0 && playheadX < wavePane.getWidth())
            {
                g.setColour (brand::accentPlay.withAlpha (0.85f));
                g.fillRect (juce::Rectangle<int> (headerW + playheadX, 0, 2, getHeight()));
            }
        }

        void resized() override
        {
            auto header = getLocalBounds().withWidth (headerW);
            header.removeFromLeft (swatchW);              // colour swatch column
            auto content = header.reduced (6, 6);

            // Right edge = small live signal meter
            meter.setBounds (content.removeFromRight (meterW));
            content.removeFromRight (4);

            nameLabel.setBounds (content.removeFromTop (18));
            content.removeFromTop (2);

            auto btnRow = content.removeFromTop (20);
            const int bw = btnRow.getWidth() / 3;
            armButton .setBounds (btnRow.removeFromLeft (bw).reduced (1));
            muteButton.setBounds (btnRow.removeFromLeft (bw).reduced (1));
            soloButton.setBounds (btnRow.reduced (1));
            content.removeFromTop (4);

            inputCombo .setBounds (content.removeFromTop (18));
            content.removeFromTop (2);
            outputCombo.setBounds (content.removeFromTop (18));
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Click the coloured swatch column to open the colour picker.
            if (e.x < swatchW)
                openColourPicker();
        }

        void openColourPicker()
        {
            auto& s = engine.getRecorder().getTrack (index);
            auto current = (s.colourARGB.load() != 0)
                            ? juce::Colour ((juce::uint32) s.colourARGB.load())
                            : brand::stripColour (index);

            auto picker = std::make_unique<StripColourPicker> (
                current,
                [this] (juce::Colour chosen) { engine.setTrackColour (index, chosen); repaint(); });

            const auto screenArea = getScreenBounds().withWidth (swatchW);
            juce::CallOutBox::launchAsynchronously (std::move (picker), screenArea, nullptr);
        }

        int getHeaderWidth() const noexcept { return headerW; }

    private:
        juce::Colour getStripColour() const
        {
            auto& s = engine.getRecorder().getTrack (index);
            const auto argb = s.colourARGB.load();
            if (argb != 0) return juce::Colour ((juce::uint32) argb);
            return brand::stripColour (index);
        }

        static constexpr int headerW = 240;
        static constexpr int swatchW = 14;
        static constexpr int meterW  = 16;

        int                       index;
        AudioEngine&              engine;
        juce::AudioThumbnail      thumbnail;
        juce::File                currentFile;
        juce::Label               nameLabel;
        juce::ToggleButton        armButton  { "REC"  };
        juce::ToggleButton        muteButton { "MUTE" };
        juce::ToggleButton        soloButton { "SOLO" };
        juce::ComboBox            inputCombo;
        juce::ComboBox            outputCombo;
        LedMeter                  meter;
        int                       playheadX             { -1 };
        int                       lastInputDeviceCount  { -1 };
        int                       lastOutputDeviceCount { -1 };
        unsigned int              lastColourArgb        { 0 };
    };

    // Owner of the TrackRow vertical list. EditPage drops this into the
    // viewport; this lets the rows scroll while the page header stays put.
    class EditPage::TrackList final : public juce::Component
    {
    public:
        TrackList (AudioEngine& eng,
                   juce::AudioFormatManager& fm,
                   juce::AudioThumbnailCache& cache)
            : engine (eng), formats (fm), thumbCache (cache) {}

        void rebuild (int numTracks)
        {
            rows.clear();
            rows.reserve ((size_t) numTracks);
            for (int i = 0; i < numTracks; ++i)
            {
                auto r = std::make_unique<TrackRow> (i, engine, formats, thumbCache);
                addAndMakeVisible (*r);
                rows.push_back (std::move (r));
            }
            resized();
        }

        void resized() override
        {
            const int rowH = 110;
            auto bounds = getLocalBounds();
            for (auto& r : rows)
                r->setBounds (bounds.removeFromTop (rowH));
            // Resize ourselves so the viewport scrolls.
            setSize (getWidth(), (int) rows.size() * rowH);
        }

        void setWaveformsFromSession (const juce::File& sessionDir)
        {
            if (! sessionDir.isDirectory())
            {
                for (auto& r : rows) r->setWaveformFile ({});
                return;
            }
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto idx = (int) i + 1;
                const auto file = sessionDir.getChildFile (
                    "Track_" + juce::String (idx).paddedLeft ('0', 2) + ".wav");
                rows[i]->setWaveformFile (file);
            }
        }

        void setPlayheadX (int px)
        {
            for (auto& r : rows) r->setPlayheadX (px);
        }

        void pollMixerState()
        {
            for (auto& r : rows) r->updatePollState();
        }

        int rowCount() const { return (int) rows.size(); }

    private:
        AudioEngine&                              engine;
        juce::AudioFormatManager&                 formats;
        juce::AudioThumbnailCache&                thumbCache;
        std::vector<std::unique_ptr<TrackRow>>    rows;
    };

    EditPage::EditPage (AudioEngine& eng)
        : engine (eng)
    {
        formatManager.registerBasicFormats();

        list = std::make_unique<TrackList> (engine, formatManager, thumbnailCache);
        viewport.setViewedComponent (list.get(), false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        emptyLabel.setText ("No session loaded — load or record a session to see waveforms here.",
                            juce::dontSendNotification);
        emptyLabel.setJustificationType (juce::Justification::centred);
        emptyLabel.setColour (juce::Label::textColourId, brand::textTertiary);
        addChildComponent (emptyLabel);

        refresh();
        startTimerHz (24);
    }

    EditPage::~EditPage() { stopTimer(); }

    void EditPage::refresh()
    {
        const int n = engine.getRecorder().getNumTracks();
        if (n != lastTrackCount)
        {
            list->rebuild (n);
            lastTrackCount = n;
            resized();
        }

        const auto& player = engine.getPlayer();
        const auto sessionDir = player.getSessionDir();
        list->setWaveformsFromSession (sessionDir);

        const bool loaded = player.isLoaded();
        emptyLabel.setVisible (! loaded && lastTrackCount > 0 ? false : false);
        // (Empty-state hint is now drawn inside each row.)
        lastLoaded = loaded;
    }

    void EditPage::timerCallback()
    {
        const int n = engine.getRecorder().getNumTracks();
        if (n != lastTrackCount)
            refresh();

        // Pick up session swaps (loaded a session, recorded a new one…)
        const bool loaded = engine.getPlayer().isLoaded();
        const auto sessionDir = engine.getPlayer().getSessionDir();
        if (loaded != lastLoaded)
            refresh();

        // Playhead
        const auto& player = engine.getPlayer();
        const auto total = player.getTotalLengthSamples();
        const auto pos   = player.getPositionSamples();
        int playheadX = -1;
        if (total > 0 && list->rowCount() > 0)
        {
            const auto wavePaneWidth = juce::jmax (1, getWidth() - 240);
            // -8 px to account for waveform reduction in paint.
            const double frac = (double) pos / (double) total;
            playheadX = (int) (frac * (wavePaneWidth - 8)) + 4;
        }
        list->setPlayheadX (playheadX);
        list->pollMixerState();
    }

    void EditPage::paint (juce::Graphics& g)
    {
        g.fillAll (brand::bgDeep);
    }

    void EditPage::resized()
    {
        viewport.setBounds (getLocalBounds());
        // Set list width = viewport width minus scrollbar; height is set in
        // TrackList::resized() based on row count.
        list->setSize (viewport.getWidth(), list->getHeight());
        list->resized();
    }
}
