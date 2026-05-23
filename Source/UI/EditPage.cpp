#include "EditPage.h"
#include "AutomationToolbar.h"
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

        // Toolbar / click-overlay context — set by the host EditPage
        // after construction so all rows share the same global view.
        AutomationToolbar* toolbar { nullptr };
        bool   clickOverlay { false };
        int    clickRowIdx  { -1 };
        // Drag tracking — while > -1, mouseDrag moves the indexed
        // point on the active lane.
        int    draggingPointIdx { -1 };

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
            armButton .setToggleState (s.armed  .load(), juce::dontSendNotification);
            monButton .setToggleState (s.monitor.load(), juce::dontSendNotification);
            muteButton.setToggleState (s.muted  .load(), juce::dontSendNotification);
            soloButton.setToggleState (s.soloed .load(), juce::dontSendNotification);
            // Per-button gradient colours match the mixer:
            //   I → green, R → red, M → orange, S → yellow.
            styleBtn (armButton,  brand::accentRecord);
            styleBtn (monButton,  brand::accentPlay);
            styleBtn (muteButton, brand::brandOrange);
            styleBtn (soloButton, brand::accentSolo);
            armButton .onClick = [this]
            {
                engine.getRecorder().getTrack (index).armed.store (armButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).armed.store (armButton.getToggleState());
            };
            monButton.onClick = [this]
            {
                engine.getRecorder().getTrack (index).monitor.store (monButton.getToggleState());
                if (stereo) engine.getRecorder().getTrack (index + 1).monitor.store (monButton.getToggleState());
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
            addAndMakeVisible (monButton);
            addAndMakeVisible (muteButton);
            addAndMakeVisible (soloButton);

            // Per-track 'VIEW' picker — clicking pops the lane-content
            // menu matching the screenshot (blocks/playlists/analysis/
            // warp are reserved for future builds and stay disabled).
            viewButton.setColour (juce::TextButton::buttonColourId,  brand::bgElevated);
            viewButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
            viewButton.setTooltip ("Pick what this row's lane draws — waveform / volume / pan / …");
            viewButton.onClick = [this]
            {
                // Lane-content picker. Reserved Pro Tools-style items
                // (blocks / playlists / analysis / warp / transcript)
                // are dropped from the menu — they were never wired and
                // the greyed entries were just clutter.
                juce::PopupMenu m;
                m.addItem (22, "waveform",    true, laneMode == LaneMode::Waveform);
                m.addItem (20, "markers",     true, laneMode == LaneMode::Markers);
                m.addSeparator();
                m.addItem (30, "volume",      true, laneMode == LaneMode::Volume);
                m.addItem (31, "volume trim", true, laneMode == LaneMode::VolumeTrim);
                m.addItem (32, "mute",        true, laneMode == LaneMode::Mute);
                m.addItem (33, "pan",         true, laneMode == LaneMode::Pan);
                m.addItem (34, "click",       true, laneMode == LaneMode::Click);

                juce::Component::SafePointer<TrackRow> self (this);
                m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton),
                                 [self] (int chosen)
                {
                    if (self == nullptr || chosen == 0) return;
                    switch (chosen)
                    {
                        case 20: self->laneMode = LaneMode::Markers;    break;
                        case 22: self->laneMode = LaneMode::Waveform;   break;
                        case 30: self->laneMode = LaneMode::Volume;     break;
                        case 31: self->laneMode = LaneMode::VolumeTrim; break;
                        case 32: self->laneMode = LaneMode::Mute;       break;
                        case 33: self->laneMode = LaneMode::Pan;        break;
                        case 34: self->laneMode = LaneMode::Click;      break;
                        default: return;
                    }
                    self->repaint();
                });
            };
            addAndMakeVisible (viewButton);

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

            // Automation lane modes — flat horizontal line representing
            // the current value. (Time-varying automation is reserved
            // for a later build; this gives the engineer an at-a-glance
            // read of the current parameter alongside the waveform UI.)
            if (laneMode != LaneMode::Waveform)
            {
                auto& t = engine.getRecorder().getTrack (index);
                g.setColour (brand::edge);
                g.drawRect (inner, 1);

                // Centre line for reference (zero / unity).
                g.setColour (brand::edge.brighter (0.2f).withAlpha (0.6f));
                g.drawHorizontalLine (inner.getCentreY(),
                                      (float) inner.getX(), (float) inner.getRight());

                juce::Colour lineCol = waveColour;
                float yProp = 0.5f;  // 0 = top, 1 = bottom
                juce::String label;

                switch (laneMode)
                {
                    case LaneMode::Volume:
                    case LaneMode::VolumeTrim:
                    {
                        const float dB = t.gainDb.load (std::memory_order_relaxed);
                        // -60..+12 dB mapped to 1..0 (loud → top)
                        yProp = 1.0f - juce::jlimit (0.0f, 1.0f,
                                                     (dB + 60.0f) / 72.0f);
                        lineCol = brand::accentStatus;
                        label   = (laneMode == LaneMode::VolumeTrim ? "trim " : "vol ")
                                + juce::String (dB, 1) + " dB";
                        break;
                    }
                    case LaneMode::Pan:
                    {
                        const float pan = t.pan.load (std::memory_order_relaxed);
                        // -1..+1 mapped to 1..0 (left = top, right = bottom)
                        yProp = (1.0f - pan) * 0.5f;
                        lineCol = brand::accentSolo;
                        const int pct = juce::roundToInt (std::abs (pan) * 100.0f);
                        label = (pct == 0) ? "pan C"
                                            : (pan < 0 ? "pan L" + juce::String (pct)
                                                       : "pan R" + juce::String (pct));
                        break;
                    }
                    case LaneMode::Mute:
                    {
                        const bool muted = t.muted.load (std::memory_order_relaxed);
                        yProp = muted ? 0.05f : 0.95f;
                        lineCol = muted ? brand::brandOrange : brand::textMuted;
                        label   = muted ? "MUTED" : "open";
                        break;
                    }
                    case LaneMode::Click:
                    {
                        // Beat grid for the whole lane — every quarter
                        // note at the current session tempo. The lane
                        // adapts its density: if beats would pack closer
                        // than ~6 px we drop to bar markers, then to
                        // 4-bar markers, etc., so the lane never moires
                        // into a solid stripe at long zoom-outs.
                        const float bpm = engine.getSessionTempoBpm();
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples() : 0;
                        const double sr = player.getSampleRate() > 0.0
                            ? player.getSampleRate()
                            : (engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                               ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
                               : 48000.0);
                        if (totalSamples > 0 && bpm > 0.0f && sr > 0.0)
                        {
                            const double samplesPerBeat = 60.0 * sr / bpm;
                            const double pxPerBeat = samplesPerBeat
                                * (double) inner.getWidth()
                                / (double) totalSamples;

                            // Pick a stride: 1 beat / 1 bar (4) / 4 bars
                            // (16) / 16 bars (64). Keeps adjacent ticks
                            // at least ~6 px apart for visual clarity.
                            int stride = 1;
                            while (pxPerBeat * stride < 6.0 && stride < 4096)
                                stride *= 4;

                            int beat = 0;
                            for (double s = 0.0; s < (double) totalSamples;
                                 s += samplesPerBeat * stride, beat += stride)
                            {
                                const double prop = s / (double) totalSamples;
                                const int x = inner.getX()
                                            + (int) (prop * inner.getWidth());
                                const bool downbeat = (beat % 4) == 0;
                                g.setColour (downbeat ? brand::brandOrange
                                                       : brand::accentStatus.withAlpha (0.55f));
                                g.drawVerticalLine (x,
                                                    (float) inner.getY(),
                                                    (float) inner.getBottom());
                            }
                        }
                        g.setColour (brand::textTertiary);
                        g.setFont (juce::FontOptions().withHeight (11.0f));
                        g.drawText ("click " + juce::String (bpm, 1) + " BPM",
                                    inner.reduced (4, 2),
                                    juce::Justification::topLeft, false);
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (0.85f));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    case LaneMode::Tempo:
                    {
                        // Shared tempo curve — one lane for the whole
                        // session, drawn identically on every row. Each
                        // point in engine.getTempoMap() is rendered as
                        // a handle on a stepped curve (40..240 BPM
                        // mapped to bottom..top of the lane).
                        const auto& tempoMap = engine.getTempoMap();
                        const auto& player   = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded()
                            ? player.getTotalLengthSamples()
                            : (juce::int64) (48000.0 * 60.0);

                        auto bpmToY = [&] (float bpm) -> int
                        {
                            const float clamped = juce::jlimit (40.0f, 240.0f, bpm);
                            const float yp = 1.0f - (clamped - 40.0f) / 200.0f;
                            return inner.getY() + juce::roundToInt (yp * inner.getHeight());
                        };
                        auto sampleToX = [&] (juce::int64 sp) -> int
                        {
                            const double prop = juce::jlimit (0.0, 1.0,
                                (double) sp / juce::jmax<double> (1.0, (double) totalSamples));
                            return inner.getX() + juce::roundToInt (prop * inner.getWidth());
                        };

                        const float sessionBpm = engine.getSessionTempoBpm();
                        g.setColour (brand::brandOrange);

                        if (tempoMap.empty())
                        {
                            const int y = bpmToY (sessionBpm);
                            g.drawHorizontalLine (y, (float) inner.getX(), (float) inner.getRight());
                        }
                        else
                        {
                            juce::Path path;
                            int prevY = bpmToY (sessionBpm);
                            path.startNewSubPath ((float) inner.getX(), (float) prevY);
                            for (const auto& tc : tempoMap)
                            {
                                const int x = sampleToX (tc.samplePos);
                                const int y = bpmToY  (tc.bpm);
                                path.lineTo ((float) x, (float) prevY);
                                path.lineTo ((float) x, (float) y);
                                prevY = y;
                            }
                            path.lineTo ((float) inner.getRight(), (float) prevY);
                            g.strokePath (path, juce::PathStrokeType (1.8f));

                            for (const auto& tc : tempoMap)
                            {
                                const int x = sampleToX (tc.samplePos);
                                const int y = bpmToY  (tc.bpm);
                                g.fillEllipse ((float) x - 4.0f, (float) y - 4.0f, 8.0f, 8.0f);
                            }
                        }

                        g.setColour (brand::textTertiary);
                        g.setFont (juce::FontOptions().withHeight (11.0f));
                        g.drawText ("tempo " + juce::String (sessionBpm, 1) + " BPM",
                                    inner.reduced (6, 2),
                                    juce::Justification::topLeft, false);
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (0.85f));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    case LaneMode::Markers:
                    {
                        // Markers are session-wide; draw vertical ticks
                        // (without per-marker time mapping for now — the
                        // timeline component is authoritative on positions).
                        g.setColour (brand::accentStatus);
                        for (int i = 0; i < 6; ++i)
                        {
                            const float x = inner.getX() + (i + 1) * inner.getWidth() * 0.1f;
                            g.drawVerticalLine ((int) x,
                                                (float) inner.getY(),
                                                (float) inner.getBottom());
                        }
                        g.setColour (brand::textTertiary);
                        g.setFont (juce::FontOptions().withHeight (11.0f));
                        g.drawText ("markers", inner.reduced (4, 2),
                                    juce::Justification::topLeft, false);
                        // Playhead overlay still applies below.
                        if (playheadX >= 0 && playheadX < wavePane.getWidth())
                        {
                            g.setColour (brand::accentPlay.withAlpha (0.85f));
                            g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                              0, 2, getHeight()));
                        }
                        return;
                    }
                    default: break;
                }

                // Resolve the active parameter from the toolbar (falls
                // back to the per-row laneMode chosen via VIEW menu).
                auto chosenParam = AudioEngine::AutomationParam::Volume;
                if (toolbar != nullptr)
                {
                    switch (toolbar->getParam())
                    {
                        case AutomationToolbar::Param::Volume: chosenParam = AudioEngine::AutomationParam::Volume; break;
                        case AutomationToolbar::Param::Pan:    chosenParam = AudioEngine::AutomationParam::Pan;    break;
                        case AutomationToolbar::Param::Mute:   chosenParam = AudioEngine::AutomationParam::Mute;   break;
                    }
                }
                else
                {
                    switch (laneMode)
                    {
                        case LaneMode::Pan:  chosenParam = AudioEngine::AutomationParam::Pan;  break;
                        case LaneMode::Mute: chosenParam = AudioEngine::AutomationParam::Mute; break;
                        default:             chosenParam = AudioEngine::AutomationParam::Volume; break;
                    }
                }

                const auto& points = engine.getAutomation (index, chosenParam);

                // Value-to-y mapping (mirrors laneCoordAt above).
                auto valueToY = [&] (float v) -> int
                {
                    float yp = 0.5f;
                    switch (chosenParam)
                    {
                        case AudioEngine::AutomationParam::Volume:
                            yp = 1.0f - juce::jlimit (0.0f, 1.0f, (v + 60.0f) / 72.0f);
                            break;
                        case AudioEngine::AutomationParam::Pan:
                            yp = (1.0f - juce::jlimit (-1.0f, 1.0f, v)) * 0.5f;
                            break;
                        case AudioEngine::AutomationParam::Mute:
                            yp = v > 0.5f ? 0.05f : 0.95f;
                            break;
                    }
                    return inner.getY() + juce::roundToInt (yp * inner.getHeight());
                };

                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                   : (juce::int64) (48000.0 * 60.0);
                auto sampleToX = [&] (juce::int64 sp) -> int
                {
                    if (totalSamples <= 0) return inner.getX();
                    const double prop = juce::jlimit (0.0, 1.0,
                                                      (double) sp / (double) totalSamples);
                    return inner.getX() + juce::roundToInt (prop * inner.getWidth());
                };

                if (points.empty())
                {
                    // No automation yet — fall back to the flat reference
                    // line for the current parameter value, so the lane
                    // still reads at a glance.
                    const int y = inner.getY()
                                + juce::roundToInt (yProp * inner.getHeight());
                    g.setColour (lineCol);
                    g.drawHorizontalLine (y, (float) inner.getX(), (float) inner.getRight());
                }
                else
                {
                    // Render the point sequence as a stepped polyline +
                    // round handles. Step style (no interpolation) for
                    // now — easier to read for the engineer when there
                    // are only a handful of points.
                    g.setColour (lineCol);
                    juce::Path path;
                    int prevY = valueToY (points.front().value);
                    int prevX = inner.getX();
                    path.startNewSubPath ((float) prevX, (float) prevY);
                    for (const auto& pt : points)
                    {
                        const int x = sampleToX (pt.samplePos);
                        const int y = valueToY  (pt.value);
                        path.lineTo ((float) x, (float) prevY);
                        path.lineTo ((float) x, (float) y);
                        prevY = y;
                        prevX = x;
                    }
                    path.lineTo ((float) inner.getRight(), (float) prevY);
                    g.strokePath (path, juce::PathStrokeType (1.6f));

                    for (const auto& pt : points)
                    {
                        const int x = sampleToX (pt.samplePos);
                        const int y = valueToY  (pt.value);
                        g.setColour (lineCol);
                        g.fillEllipse ((float) x - 3.5f, (float) y - 3.5f, 7.0f, 7.0f);
                        g.setColour (juce::Colours::black.withAlpha (0.6f));
                        g.drawEllipse ((float) x - 3.5f, (float) y - 3.5f, 7.0f, 7.0f, 1.0f);
                    }
                }

                g.setColour (brand::textPrimary);
                g.setFont (juce::FontOptions().withHeight (11.0f).withStyle ("Bold"));
                g.drawText (label,
                            inner.reduced (6, 2),
                            juce::Justification::topRight, false);

                if (playheadX >= 0 && playheadX < wavePane.getWidth())
                {
                    g.setColour (brand::accentPlay.withAlpha (0.85f));
                    g.fillRect (juce::Rectangle<int> (headerW + playheadX,
                                                      0, 2, getHeight()));
                }
                return;
            }

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

            // ─── Click-beat overlay (waveform mode only) ───────────
            // When the engineer has dropped a metronome track, every
            // OTHER row gets faint vertical ticks at every beat so the
            // click pulse is visible against the recorded audio.
            if (clickOverlay && index != clickRowIdx)
            {
                const float bpm = engine.getSessionTempoBpm();
                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                   : 0;
                const double sr = player.getSampleRate() > 0.0
                                    ? player.getSampleRate()
                                    : (engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                                       ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate()
                                       : 48000.0);
                if (totalSamples > 0 && bpm > 0.0f && sr > 0.0)
                {
                    const double samplesPerBeat = 60.0 * sr / bpm;
                    const auto inner2 = wavePane.reduced (4, 6);
                    const int  paneL  = inner2.getX();
                    const int  paneW  = juce::jmax (1, inner2.getWidth());
                    const double pxPerBeat = samplesPerBeat * (double) paneW
                                           / (double) totalSamples;
                    int stride = 1;
                    while (pxPerBeat * stride < 6.0 && stride < 4096)
                        stride *= 4;

                    int beat = 0;
                    for (double s = 0.0; s < (double) totalSamples;
                         s += samplesPerBeat * stride, beat += stride)
                    {
                        const double prop = s / (double) totalSamples;
                        const int x = paneL + (int) (prop * paneW);
                        const bool downbeat = (beat % 4) == 0;
                        g.setColour (downbeat
                            ? brand::brandOrange.withAlpha (0.40f)
                            : brand::accentStatus.withAlpha (0.18f));
                        g.drawVerticalLine (x,
                                            (float) inner2.getY(),
                                            (float) inner2.getBottom());
                    }
                }
            }

            // ─── Clip boundary overlay (waveform mode only) ─────────
            // After Edit ▸ Split / Separate, the track grows a clip list.
            // Paint each clip boundary as a 1 px vertical cut + a small
            // ⌐ marker at the top of the lane so the engineer can see
            // where the split landed.
            if (auto* clips = engine.tryClipsFor (index))
            {
                const auto& player = engine.getPlayer();
                const juce::int64 totalSamples = player.isLoaded()
                    ? player.getTotalLengthSamples() : 0;
                if (totalSamples > 0)
                {
                    const auto inner2 = wavePane.reduced (4, 6);
                    for (const auto& c : *clips)
                    {
                        if (c.timelineStartSamples <= 0) continue;
                        const double prop = (double) c.timelineStartSamples / (double) totalSamples;
                        const int x = inner2.getX()
                                    + (int) (prop * inner2.getWidth());
                        g.setColour (brand::accentSolo.withAlpha (0.85f));
                        g.drawVerticalLine (x, (float) inner2.getY(),
                                            (float) inner2.getBottom());
                        // Tiny corner flag at the top so the cut is
                        // visible against busy audio.
                        g.fillRect (juce::Rectangle<int> (x, inner2.getY(), 6, 3));
                    }
                }
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

            // 2×2 grid matching the mixer: [ I | R ] / [ S | M ].
            // The Click row hides R and I (playback-only track) and shows
            // a single [ S | M ] row instead.
            const bool isClickRow =
                engine.getRecorder().getTrack (index).name == "Click";
            const int btnH = 22;
            if (isClickRow)
            {
                armButton .setVisible (false); armButton .setBounds ({});
                monButton .setVisible (false); monButton .setBounds ({});
                auto row = content.removeFromTop (btnH);
                const int halfW = row.getWidth() / 2;
                soloButton.setBounds (row.removeFromLeft (halfW).reduced (1));
                muteButton.setBounds (row.reduced (1));
            }
            else
            {
                armButton.setVisible (true);
                monButton.setVisible (true);
                auto row1 = content.removeFromTop (btnH);
                content.removeFromTop (3);
                auto row2 = content.removeFromTop (btnH);
                const int halfW = row1.getWidth() / 2;
                monButton .setBounds (row1.removeFromLeft (halfW).reduced (1));
                armButton .setBounds (row1.reduced (1));
                soloButton.setBounds (row2.removeFromLeft (halfW).reduced (1));
                muteButton.setBounds (row2.reduced (1));
            }
            content.removeFromTop (4);

            inputCombo .setBounds (content.removeFromTop (18));
            content.removeFromTop (2);
            outputCombo.setBounds (content.removeFromTop (18));
            content.removeFromTop (3);
            // Hide the VIEW button on the Click row — its lane is the
            // metronome waveform, no automation choices apply.
            const bool clickRow =
                engine.getRecorder().getTrack (index).name == "Click";
            viewButton.setVisible (! clickRow);
            if (! clickRow)
                viewButton.setBounds (content.removeFromTop (18));
            else
                viewButton.setBounds ({});
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

        // Map an (x,y) inside the lane area to (samplePos, value) so
        // mouse interactions can place / move automation points.
        struct LaneCoord { juce::int64 samplePos; float value; };
        LaneCoord laneCoordAt (juce::Point<int> p) const
        {
            const auto inner = getLocalBounds().withTrimmedLeft (headerW)
                                                .reduced (4, 6);
            const auto& player = engine.getPlayer();
            const juce::int64 total = player.isLoaded() ? player.getTotalLengthSamples()
                                                        : (juce::int64) (engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                                                            ? engine.getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate() * 60.0
                                                            : 48000.0 * 60.0);
            const double prop = (double) (p.x - inner.getX()) / juce::jmax (1, inner.getWidth());
            const juce::int64 samplePos = (juce::int64) (juce::jlimit (0.0, 1.0, prop) * (double) total);

            // Y → value depends on the active param.
            float value = 0.0f;
            if (toolbar != nullptr)
            {
                const float yProp = juce::jlimit (0.0f, 1.0f,
                                                  (float) (p.y - inner.getY()) / juce::jmax (1, inner.getHeight()));
                switch (toolbar->getParam())
                {
                    case AutomationToolbar::Param::Volume:
                        value = (1.0f - yProp) * 72.0f - 60.0f; // 1..0 → -60..+12
                        break;
                    case AutomationToolbar::Param::Pan:
                        value = 1.0f - yProp * 2.0f;            // 1..0 → -1..+1 inverted
                        break;
                    case AutomationToolbar::Param::Mute:
                        value = yProp < 0.5f ? 1.0f : 0.0f;
                        break;
                }
            }
            return { samplePos, value };
        }

        static AudioEngine::AutomationParam toEngineParam (AutomationToolbar::Param p)
        {
            switch (p)
            {
                case AutomationToolbar::Param::Volume: return AudioEngine::AutomationParam::Volume;
                case AutomationToolbar::Param::Pan:    return AudioEngine::AutomationParam::Pan;
                case AutomationToolbar::Param::Mute:   return AudioEngine::AutomationParam::Mute;
            }
            return AudioEngine::AutomationParam::Volume;
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
            {
                openColourPicker();
                return;
            }

            // Lane-area interaction — only when the toolbar is wired and
            // this row isn't the Click track itself.
            if (toolbar != nullptr && index != clickRowIdx && e.x >= headerW)
            {
                const auto coord = laneCoordAt (e.getPosition());
                const auto p     = toEngineParam (toolbar->getParam());

                // Special case: Tempo param edits the engine's shared
                // tempo map instead of the per-track point store.
                if (toolbar->getParam() == AutomationToolbar::Param::Tempo)
                {
                    const auto& player = engine.getPlayer();
                    const juce::int64 totalSamples = player.isLoaded()
                        ? player.getTotalLengthSamples()
                        : (juce::int64) (48000.0 * 60.0);
                    const juce::int64 tol = juce::jmax<juce::int64> (1,
                        totalSamples / juce::jmax (1, getWidth() - headerW) * 8);
                    // Y → BPM (40..240 range mirrors the paint mapping).
                    const auto inner2 = getLocalBounds().withTrimmedLeft (headerW).reduced (4, 6);
                    const float yProp = juce::jlimit (0.0f, 1.0f,
                        (float) (e.y - inner2.getY()) / juce::jmax (1, inner2.getHeight()));
                    const float bpm = juce::jlimit (40.0f, 240.0f,
                                                    40.0f + (1.0f - yProp) * 200.0f);

                    switch (toolbar->getTool())
                    {
                        case AutomationToolbar::Tool::AddPoint:
                            engine.addTempoChange (coord.samplePos, bpm);
                            break;
                        case AutomationToolbar::Tool::DeletePoint:
                            engine.removeTempoChangeNear (coord.samplePos, tol);
                            break;
                        case AutomationToolbar::Tool::Select:
                            // Tempo drag isn't wired yet — Select on
                            // the Tempo lane just records nothing.
                            break;
                    }
                    repaint();
                    return;
                }

                switch (toolbar->getTool())
                {
                    case AutomationToolbar::Tool::AddPoint:
                        engine.addAutomationPoint (index, p, coord.samplePos, coord.value);
                        repaint();
                        return;
                    case AutomationToolbar::Tool::DeletePoint:
                    {
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                           : (juce::int64) (48000.0 * 60.0);
                        const juce::int64 tol = juce::jmax<juce::int64> (1, totalSamples / juce::jmax (1, getWidth() - headerW) * 8);
                        engine.removeAutomationPointNear (index, p, coord.samplePos, tol);
                        repaint();
                        return;
                    }
                    case AutomationToolbar::Tool::Select:
                    {
                        // Try to grab the nearest point — drag will move
                        // it if mouseDrag fires after this.
                        const auto& lane = engine.getAutomation (index, p);
                        const auto& player = engine.getPlayer();
                        const juce::int64 totalSamples = player.isLoaded() ? player.getTotalLengthSamples()
                                                                           : (juce::int64) (48000.0 * 60.0);
                        const juce::int64 tol = juce::jmax<juce::int64> (1, totalSamples / juce::jmax (1, getWidth() - headerW) * 6);
                        draggingPointIdx = -1;
                        for (size_t i = 0; i < lane.size(); ++i)
                        {
                            if (std::abs (lane[i].samplePos - coord.samplePos) < tol)
                            {
                                draggingPointIdx = (int) i;
                                break;
                            }
                        }
                        return;
                    }
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Resize drag wins if it's already in flight.
            if (dragging)
            {
                const int target = juce::jlimit (kMinRowH, kMaxRowH,
                                                 dragStartHeight + e.getDistanceFromDragStartY());
                rowSize = Size::Custom;
                customH = target;
                if (onSizeChosen) onSizeChosen (*this, Size::Custom);
                return;
            }

            // Otherwise: drag a held automation point with the Select tool.
            if (toolbar != nullptr
                && toolbar->getTool() == AutomationToolbar::Tool::Select
                && draggingPointIdx >= 0)
            {
                const auto p     = toEngineParam (toolbar->getParam());
                const auto coord = laneCoordAt (e.getPosition());
                // Easiest path: re-add with the new (samplePos, value);
                // addAutomationPoint replaces inside its tolerance, so
                // we just delete the old + add new to allow position to
                // move freely.
                const auto& lane = engine.getAutomation (index, p);
                if (draggingPointIdx < (int) lane.size())
                {
                    const auto oldPos = lane[(size_t) draggingPointIdx].samplePos;
                    engine.removeAutomationPointNear (index, p, oldPos, 1);
                    engine.addAutomationPoint        (index, p, coord.samplePos, coord.value);
                    repaint();
                }
            }
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            dragging         = false;
            draggingPointIdx = -1;
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
        // What this row draws in the lane area (matches the toolbar's
        // Param, or the row's own VIEW choice when no toolbar is wired).
        enum class LaneMode { Waveform, Markers, Volume, VolumeTrim, Mute, Pan, Click, Tempo };
        LaneMode laneMode { LaneMode::Waveform };

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
        // Same single-letter glyphs as the mixer strips: R / I / M / S.
        // LookAndFeel::drawToggleButton paints the pill in the colour
        // pinned via ToggleButton::tickColourId when toggled on.
        juce::ToggleButton        armButton  { "R" };
        juce::ToggleButton        monButton  { "I" };
        juce::ToggleButton        muteButton { "M" };
        juce::ToggleButton        soloButton { "S" };
        juce::ComboBox            inputCombo;
        juce::ComboBox            outputCombo;
        juce::TextButton          viewButton { "VIEW" };
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

        // Push-down configuration: toolbar pointer + click-overlay state.
        // Stored here so newly-created rows pick them up automatically;
        // existing rows are mutated through updateRowContext().
        AutomationToolbar* sharedToolbar       { nullptr };
        bool               sharedClickPresent  { false };
        int                sharedClickRowIdx   { -1 };

        void updateRowContext()
        {
            for (auto& r : rows)
            {
                r->toolbar      = sharedToolbar;
                r->clickOverlay = sharedClickPresent;
                r->clickRowIdx  = sharedClickRowIdx;
                r->repaint();
            }
        }

        void forceLaneMode (TrackRow::LaneMode lm)
        {
            for (auto& r : rows)
            {
                r->laneMode = lm;
                r->repaint();
            }
        }

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
                r->toolbar      = sharedToolbar;
                r->clickOverlay = sharedClickPresent;
                r->clickRowIdx  = sharedClickRowIdx;
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

    void EditPage::setAutomationToolbar (AutomationToolbar* t)
    {
        toolbar = t;
        if (list != nullptr)
        {
            list->sharedToolbar = t;
            list->updateRowContext();
        }
        applyToolbarParamToAllRows();
        repaint();
    }

    void EditPage::applyToolbarParamToAllRows()
    {
        if (toolbar == nullptr || list == nullptr) return;

        TrackRow::LaneMode lm = TrackRow::LaneMode::Volume;
        switch (toolbar->getParam())
        {
            case AutomationToolbar::Param::Volume: lm = TrackRow::LaneMode::Volume; break;
            case AutomationToolbar::Param::Pan:    lm = TrackRow::LaneMode::Pan;    break;
            case AutomationToolbar::Param::Mute:   lm = TrackRow::LaneMode::Mute;   break;
            case AutomationToolbar::Param::Click:  lm = TrackRow::LaneMode::Click;  break;
            case AutomationToolbar::Param::Tempo:  lm = TrackRow::LaneMode::Tempo;  break;
        }
        list->forceLaneMode (lm);
    }

    void EditPage::setClickTrackPresent (bool present, int clickIdx)
    {
        clickPresent  = present;
        clickTrackIdx = clickIdx;
        if (list != nullptr)
        {
            list->sharedClickPresent = present;
            list->sharedClickRowIdx  = clickIdx;
            list->updateRowContext();
        }
        repaint();
    }

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
