#include "EditPage.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
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
        // Track-height presets — Pro Tools-style 7-step scale plus a
        // dynamic "fit to window" computed from the viewport height.
        // Size::Custom is engaged whenever the user drags the row's
        // bottom edge — the dragged pixel height is stored in customH.
        enum class Size { Micro, Mini, Small, Medium, Large, Jumbo, Extreme, FitToWindow, Custom };

        static int pixelsFor (Size s, int fitFallback = 80, int customPixels = 80)
        {
            switch (s)
            {
                case Size::Micro:       return 28;
                case Size::Mini:        return 50;
                case Size::Small:       return 80;
                case Size::Medium:      return 110;
                case Size::Large:       return 160;
                case Size::Jumbo:       return 220;
                case Size::Extreme:     return 320;
                case Size::FitToWindow: return fitFallback;
                case Size::Custom:      return customPixels;
            }
            return 80;
        }

        static constexpr int kResizeZoneH = 6;   // bottom-edge grab zone
        static constexpr int kMinRowH     = 20;
        static constexpr int kMaxRowH     = 800;

        // Callback fired by the right-click menu so the owning TrackList
        // can recompute layout (and resolve "fit to window").
        std::function<void(TrackRow&, Size)> onSizeChosen;

        TrackRow (int trackIdx,
                  bool isStereoPair,
                  AudioEngine& eng,
                  juce::AudioFormatManager& formats,
                  juce::AudioThumbnailCache& cache)
            : index (trackIdx), stereo (isStereoPair), engine (eng),
              thumbnailL (1024, formats, cache),
              thumbnailR (1024, formats, cache),
              meter (engine.getRecorder().getTrack (index))
        {
            if (stereo)
                meter.setStereoPartner (&engine.getRecorder().getTrack (index + 1));

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
                st.name = newName.isEmpty() ? juce::String (index + 1)
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
            styleBtn (soloButton, brand::accentSolo);
            armButton .onClick = [this]
            {
                engine.getRecorder().getTrack (index).armed.store (armButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).armed.store (armButton.getToggleState());
            };
            muteButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).muted.store (muteButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).muted.store (muteButton.getToggleState());
            };
            soloButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).soloed.store (soloButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).soloed.store (soloButton.getToggleState());
            };
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
                if (stereo)
                    engine.setTrackLinkedRouting (index + 1, (dev < 0) ? -1 : dev + 1);
            };
            outputCombo.onChange = [this]
            {
                const int id = outputCombo.getSelectedId();
                const int dev = (id <= 1) ? -1 : id - 2;
                engine.setTrackLinkedRouting (index, dev);
                if (stereo)
                    engine.setTrackLinkedRouting (index + 1, (dev < 0) ? -1 : dev + 1);
            };
            addAndMakeVisible (inputCombo);
            addAndMakeVisible (outputCombo);

            rebuildRoutingCombos();
            refreshRoutingSelection();

            // Live signal meter on the right edge of the header. The
            // strip header is narrow (16 px reserved), so disable the
            // dB-label gutter — the bar gets the full widget width.
            meter.setShowDbLabels (false);
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
            const int visibleIn  = juce::jmax (numIns,  index + 1, stereo ? 16 : 8);
            const int visibleOut = juce::jmax (numOuts, index + 1, stereo ? 16 : 8);
            const int step = stereo ? 2 : 1;

            inputCombo.clear (juce::dontSendNotification);
            inputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleIn; i += step)
            {
                const bool live = stereo ? (i + 1 < numIns) : (i < numIns);
                const auto label = stereo
                    ? juce::String ("In ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                    : juce::String ("In ") + juce::String (i + 1);
                inputCombo.addItem (live ? label : (label + " (off)"), i + 2);
            }

            outputCombo.clear (juce::dontSendNotification);
            outputCombo.addItem ("(unrouted)", 1);
            for (int i = 0; i < visibleOut; i += step)
            {
                const bool live = stereo ? (i + 1 < numOuts) : (i < numOuts);
                const auto label = stereo
                    ? juce::String ("Out ") + juce::String (i + 1) + "-" + juce::String (i + 2)
                    : juce::String ("Out ") + juce::String (i + 1);
                outputCombo.addItem (live ? label : (label + " (off)"), i + 2);
            }
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

        void setWaveformFiles (const juce::File& fL, const juce::File& fR)
        {
            if (fL != currentFileL)
            {
                currentFileL = fL;
                thumbnailL.setSource (fL.existsAsFile() ? new juce::FileInputSource (fL) : nullptr);
            }
            if (fR != currentFileR)
            {
                currentFileR = fR;
                thumbnailR.setSource (fR.existsAsFile() ? new juce::FileInputSource (fR) : nullptr);
            }
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
            g.setGradientFill (brand::verticalGradient (fillColour, swatchArea.toFloat(), 0.18f, 0.28f));
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

            // Paint the waveform in the strip's own colour so it tracks
            // any colour changes the user makes in the mixer or EDIT view.
            const auto waveColour = getStripColour().brighter (0.25f);
            const auto inner = wavePane.reduced (4, 6);

            if (stereo)
            {
                // L on top, R on bottom — Pro-Tools-style stereo lanes.
                const int laneH = inner.getHeight() / 2;
                auto laneL = inner.withHeight (laneH);
                auto laneR = inner.withTrimmedTop (laneH);

                if (thumbnailL.getTotalLength() > 0.0)
                {
                    g.setColour (waveColour);
                    thumbnailL.drawChannels (g, laneL, 0.0, thumbnailL.getTotalLength(), 1.0f);
                }
                if (thumbnailR.getTotalLength() > 0.0)
                {
                    g.setColour (waveColour);
                    thumbnailR.drawChannels (g, laneR, 0.0, thumbnailR.getTotalLength(), 1.0f);
                }
                // Thin divider between lanes
                g.setColour (brand::edge);
                g.drawHorizontalLine (inner.getY() + laneH, (float) inner.getX(),
                                       (float) inner.getRight());

                if (thumbnailL.getTotalLength() <= 0.0 && thumbnailR.getTotalLength() <= 0.0)
                {
                    g.setColour (brand::textTertiary);
                    g.setFont (juce::Font (juce::FontOptions().withHeight (12.0f)));
                    g.drawText ("(no recording yet — start a session and record to see waveforms)",
                                wavePane, juce::Justification::centred, false);
                }
            }
            else if (thumbnailL.getTotalLength() > 0.0)
            {
                g.setColour (waveColour);
                thumbnailL.drawChannels (g, inner, 0.0, thumbnailL.getTotalLength(), 1.0f);
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

        bool isInResizeZone (juce::Point<int> p) const noexcept
        {
            return p.y >= getHeight() - kResizeZoneH;
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            // Avoid spamming setMouseCursor on every pixel of mouse motion —
            // only flip when the resize-zone hit state changes.
            const bool inZone = isInResizeZone (e.getPosition());
            if (inZone != cursorIsResize)
            {
                cursorIsResize = inZone;
                setMouseCursor (inZone ? juce::MouseCursor::UpDownResizeCursor
                                       : juce::MouseCursor::NormalCursor);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // Right-click anywhere on the row → size menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                showSizeMenu (e.getScreenPosition());
                return;
            }
            // Left-click in the bottom resize zone → start a drag-resize.
            if (isInResizeZone (e.getPosition()))
            {
                dragStartHeight = getHeight();
                dragging        = true;
                return;
            }
            // Left-click on the coloured swatch column → colour picker.
            if (e.x < swatchW)
                openColourPicker();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging) return;
            const int target = juce::jlimit (kMinRowH, kMaxRowH,
                                             dragStartHeight + e.getDistanceFromDragStartY());
            rowSize = Size::Custom;
            customH = target;
            if (onSizeChosen) onSizeChosen (*this, Size::Custom);
        }

        void mouseUp (const juce::MouseEvent&) override { dragging = false; }

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

        int  getHeaderWidth() const noexcept { return headerW; }
        int  getTrackIndex()  const noexcept { return index; }
        bool isStereoPair()   const noexcept { return stereo; }

        Size getRowSize() const noexcept { return rowSize; }
        void setRowSize (Size s) noexcept { rowSize = s; }
        int  getCustomHeight() const noexcept { return customH; }
        int  getRowPixelHeight (int fitFallback) const noexcept
        {
            return pixelsFor (rowSize, fitFallback, customH);
        }

    private:
        void showSizeMenu (juce::Point<int> screenPos)
        {
            if (menuOpen) return;   // re-entrancy guard

            juce::PopupMenu menu;
            auto add = [this, &menu] (int id, const juce::String& name, Size s)
            {
                menu.addItem (id, name, true /*enabled*/, s == rowSize);
            };
            add (1, "micro",        Size::Micro);
            add (2, "mini",         Size::Mini);
            add (3, "small",        Size::Small);
            add (4, "medium",       Size::Medium);
            add (5, "large",        Size::Large);
            add (6, "jumbo",        Size::Jumbo);
            add (7, "extreme",      Size::Extreme);
            menu.addSeparator();
            add (8, "fit to window", Size::FitToWindow);

            // Use a screen-area target (1x1 at the click point) so the menu's
            // anchor doesn't depend on the row component still being alive
            // when the callback fires. SafePointer wraps the row body so a
            // rebuild between open and dismiss can't crash the callback.
            menuOpen = true;
            juce::Component::SafePointer<TrackRow> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options()
                                    .withTargetScreenArea ({ screenPos.x, screenPos.y, 1, 1 }),
                                [safe] (int chosen)
            {
                if (auto* row = safe.getComponent())
                {
                    row->menuOpen = false;
                    static const std::array<Size, 8> map { Size::Micro, Size::Mini, Size::Small,
                                                           Size::Medium, Size::Large, Size::Jumbo,
                                                           Size::Extreme, Size::FitToWindow };
                    if (chosen < 1 || chosen > 8) return;
                    const auto next = map[(std::size_t) (chosen - 1)];
                    row->rowSize = next;
                    if (row->onSizeChosen) row->onSizeChosen (*row, next);
                }
            });
        }

    public:

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
        juce::AudioThumbnail      thumbnailL;
        juce::AudioThumbnail      thumbnailR;
        juce::File                currentFileL;
        juce::File                currentFileR;
        bool                      stereo;
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
        Size                      rowSize               { Size::Small };
        int                       customH               { 80 };
        int                       dragStartHeight       { 0 };
        bool                      dragging              { false };
        bool                      menuOpen              { false };
        bool                      cursorIsResize        { false };
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

        // The viewport's visible height — needed to resolve "fit to window".
        // EditPage sets this on every resize.
        void setViewportHeight (int h) { viewportHeight = juce::jmax (60, h); }

        void rebuild (int numTracks)
        {
            rows.clear();
            rows.reserve ((size_t) numTracks);

            // Logical iteration — stereo L track owns the next R partner.
            int i = 0;
            while (i < numTracks)
            {
                auto& tL = engine.getRecorder().getTrack (i);
                const bool stereo = tL.isStereo.load() && (i + 1 < numTracks);
                auto r = std::make_unique<TrackRow> (i, stereo, engine, formats, thumbCache);
                r->onSizeChosen = [this] (TrackRow&, TrackRow::Size) { resized(); };
                addAndMakeVisible (*r);
                rows.push_back (std::move (r));
                i += stereo ? 2 : 1;
            }
            resized();
        }

        void resized() override
        {
            // Fit-to-window fallback distributes the viewport height across
            // the rows that opted into Size::FitToWindow.
            int fixedTotal = 0;
            int fitRows    = 0;
            for (auto& r : rows)
            {
                if (r->getRowSize() == TrackRow::Size::FitToWindow) ++fitRows;
                else fixedTotal += TrackRow::pixelsFor (r->getRowSize());
            }
            const int fitFallback = fitRows > 0
                ? juce::jmax (28, (viewportHeight - fixedTotal) / fitRows)
                : 80;

            auto bounds = getLocalBounds();
            int totalH = 0;
            for (auto& r : rows)
            {
                const int h = r->getRowPixelHeight (fitFallback);
                r->setBounds (bounds.removeFromTop (h));
                totalH += h;
            }
            // Resize ourselves so the viewport scrolls when content > view.
            setSize (getWidth(), juce::jmax (viewportHeight, totalH));
        }

        void setWaveformsFromSession (const juce::File& sessionDir)
        {
            if (! sessionDir.isDirectory())
            {
                for (auto& r : rows) r->setWaveformFiles ({}, {});
                return;
            }
            for (auto& r : rows)
            {
                const int trackIdx = r->getTrackIndex();
                auto pad = [] (int n) { return juce::String (n).paddedLeft ('0', 2); };
                const auto fL = sessionDir.getChildFile ("Track_" + pad (trackIdx + 1) + ".wav");
                const auto fR = r->isStereoPair()
                                ? sessionDir.getChildFile ("Track_" + pad (trackIdx + 2) + ".wav")
                                : juce::File();
                r->setWaveformFiles (fL, fR);
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
        int                                       viewportHeight { 480 };
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

        // Compute logical (stereo-aware) row count and rebuild when it
        // differs — physical track count alone doesn't detect isStereo
        // toggles, which collapse two rows into one.
        int logicalRows = 0;
        for (int i = 0; i < n; )
        {
            const bool s = engine.getRecorder().getTrack (i).isStereo.load() && (i + 1 < n);
            ++logicalRows;
            i += s ? 2 : 1;
        }
        if (n != lastTrackCount || logicalRows != (int) list->rowCount())
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
        // Tell the list how tall the visible area is so "fit to window"
        // sizing can resolve a sensible per-row pixel height.
        list->setViewportHeight (viewport.getHeight());
        list->setSize (viewport.getWidth(), list->getHeight());
        list->resized();
    }
}
