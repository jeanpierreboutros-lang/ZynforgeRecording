#include "EditPage.h"
#include "AutomationToolbar.h"
#include "EditToolsBar.h"
#include "EditTimeRuler.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "LedMeter.h"
#include "StripColourPicker.h"
#include "RenameLabel.h"
#include "EditTrackRow.h"

#include <algorithm>

namespace zynforge
{

    // Owner of the TrackRow vertical list. EditPage drops this into the
    // viewport; this lets the rows scroll while the page header stays put.
    class EditPage::TrackList final : public juce::Component
    {
    public:
        TrackList (AudioEngine& eng,
                   juce::AudioFormatManager& fm,
                   std::vector<std::unique_ptr<juce::AudioThumbnailCache>>& caches)
            : engine (eng), formats (fm), thumbCaches (caches) {}

        // Zoom gestures (DAW-standard): Cmd/Ctrl + wheel zooms the timeline
        // horizontally; add Shift for vertical (amplitude) zoom. A plain
        // wheel is forwarded to the viewport so normal scrolling is intact.
        void mouseWheelMove (const juce::MouseEvent& e,
                             const juce::MouseWheelDetails& w) override
        {
            const bool cmd = e.mods.isCommandDown() || e.mods.isCtrlDown();
            if (cmd)
            {
                if (auto* page = findParentComponentOfClass<EditPage>())
                {
                    if (e.mods.isShiftDown()) page->wheelZoomVertical   (w.deltaY);
                    else                      page->wheelZoomHorizontal (w.deltaY);
                    return;
                }
            }
            if (auto* vp = findParentComponentOfClass<juce::Viewport>())
                vp->mouseWheelMove (e.getEventRelativeTo (vp), w);
        }

        // Push-down configuration: toolbar pointer + click-overlay state.
        // Stored here so newly-created rows pick them up automatically;
        // existing rows are mutated through updateRowContext().
        AutomationToolbar* sharedToolbar       { nullptr };
        EditToolsBar*      sharedToolsBar      { nullptr };
        int                sharedClickRowIdx   { -1 };
        // Forwarded from EditPage -> MainComponent. When set, every
        // per-point automation edit a TrackRow performs goes through
        // this wrapper so Cmd+Z reverts it.
        std::function<void (const juce::String& label,
                            std::function<void()> mutate)> sharedAutomationEditWrapper;
        // Drag begin / end -- coalesces a multi-tick drag into one
        // undo step.
        std::function<void()>                       sharedAutomationDragBegin;
        std::function<void (const juce::String&)>   sharedAutomationDragEnd;

        void updateRowContext()
        {
            for (auto& r : rows)
            {
                r->toolbar                = sharedToolbar;
                r->toolsBar               = sharedToolsBar;
                r->clickRowIdx            = sharedClickRowIdx;
                r->automationEditWrapper  = sharedAutomationEditWrapper;
                r->automationDragBegin    = sharedAutomationDragBegin;
                r->automationDragEnd      = sharedAutomationDragEnd;
                r->repaint();
            }
        }

        // Apply one height preset (or a custom pixel height) to every row
        // at once -- the destination of Option-drag + "Set all tracks to".
        void applySizeToAll (TrackRow::Size s, int customPx)
        {
            for (auto& r : rows)
            {
                r->setRowSize (s);
                if (s == TrackRow::Size::Custom && customPx > 0)
                    r->setCustomHeight (customPx);
            }
            resized();
        }

        // Tab / Shift+Tab from a row's name field hops to the next /
        // previous row (wrapping at the ends) and opens its editor.
        void tabRename (TrackRow& from, bool shift)
        {
            const int n = (int) rows.size();
            if (n == 0) return;
            int pos = -1;
            for (int i = 0; i < n; ++i)
                if (rows[(size_t) i].get() == &from) { pos = i; break; }
            if (pos < 0) return;
            const int target = ((pos + (shift ? -1 : 1)) % n + n) % n;
            rows[(size_t) target]->beginRename();
        }

        void forceLaneMode (TrackRow::LaneMode lm)
        {
            for (auto& r : rows)
            {
                r->laneMode = lm;
                r->repaint();
            }
        }

        // The viewport's visible height -- needed to resolve "fit to window".
        // EditPage sets this on every resize.
        void setViewportHeight (int h) { viewportHeight = juce::jmax (60, h); }

        void rebuild (int numTracks)
        {
            rows.clear();
            rows.reserve ((size_t) numTracks);

            // Logical iteration -- stereo L track owns the next R partner.
            int i = 0;
            while (i < numTracks)
            {
                auto& tL = engine.getRecorder().getTrack (i);
                const bool stereo = tL.isStereo.load() && (i + 1 < numTracks);
                // Shard by physical track index so each WAV always lands in the
                // same cache (stable cache hits) and adjacent tracks spread
                // across the parallel scan threads.
                auto& shard = *thumbCaches[(size_t) (i % (int) thumbCaches.size())];
                auto r = std::make_unique<TrackRow> (i, stereo, engine, formats, shard);
                r->onSizeChosen = [this] (TrackRow&, TrackRow::Size) { resized(); };
                r->onTabRename  = [this] (TrackRow& from, bool shift) { tabRename (from, shift); };
                r->onApplyToAllRows = [this] (TrackRow::Size s, int customPx) { applySizeToAll (s, customPx); };
                r->toolbar                = sharedToolbar;
                r->toolsBar               = sharedToolsBar;
                r->clickRowIdx            = sharedClickRowIdx;
                r->automationEditWrapper  = sharedAutomationEditWrapper;
                r->automationDragBegin    = sharedAutomationDragBegin;
                r->automationDragEnd      = sharedAutomationDragEnd;
                addAndMakeVisible (*r);
                rows.push_back (std::move (r));
                i += stereo ? 2 : 1;
            }
            resized();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            // DOUBLE-click in the empty area below the last row -> create a
            // new mono audio track. (Single click no longer adds a track --
            // it was too easy to spawn empty tracks by accident.) The
            // TrackList only sees the event when no TrackRow consumes it,
            // which means the engineer hit empty space. Right-click is left
            // alone for a potential future "Add multiple..." menu.
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown()) return;
            if (engine.getRecorder().isRecording()) return;   // safety
            engine.addOneStrip();
        }

        // Dark gap left between adjacent rows so each track reads as its
        // own block (rows = light bgStrip, gaps + empty area = dark bgDeep).
        static constexpr int kRowGap = brand::space::sm;

        void resized() override
        {
            // Fit-to-window fallback distributes the viewport height across
            // the rows that opted into Size::FitToWindow -- minus the gaps
            // between rows, so a fit-to-window row still lands flush in view.
            int fixedTotal = 0;
            int fitRows    = 0;
            for (auto& r : rows)
            {
                if (r->getRowSize() == TrackRow::Size::FitToWindow) ++fitRows;
                else fixedTotal += TrackRow::pixelsFor (r->getRowSize());
            }
            const int gapTotal = (int) rows.size() > 1 ? ((int) rows.size() - 1) * kRowGap : 0;
            const int fitFallback = fitRows > 0
                ? juce::jmax (28, (viewportHeight - fixedTotal - gapTotal) / fitRows)
                : 80;

            auto bounds = getLocalBounds();
            int totalH = 0;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const int h = rows[i]->getRowPixelHeight (fitFallback);
                rows[i]->setBounds (bounds.removeFromTop (h));
                totalH += h;
                if (i + 1 < rows.size())
                {
                    bounds.removeFromTop (kRowGap);   // dark separator
                    totalH += kRowGap;
                }
            }
            // Resize ourselves so the viewport scrolls when content > view.
            setSize (getWidth(), juce::jmax (viewportHeight, totalH));
        }

        // Paint the dark substrate so the gaps between rows (and the area
        // below the last row) read as bgDeep, separating the light rows.
        void paint (juce::Graphics& g) override { g.fillAll (brand::bgDeep); }

        // True when this row's track (or its stereo partner) is armed for
        // capture -- such a row defers its lane to the live capture envelope.
        bool rowIsArmed (const TrackRow& r) const
        {
            auto& rec = engine.getRecorder();
            const int idx = r.getTrackIndex();
            if (idx < 0 || idx >= rec.getNumTracks()) return false;
            if (rec.getTrack (idx).armed.load (std::memory_order_relaxed)) return true;
            return r.isStereoPair() && idx + 1 < rec.getNumTracks()
                && rec.getTrack (idx + 1).armed.load (std::memory_order_relaxed);
        }

        // skipArmedRows: while recording, the ARMED rows show the live capture
        // envelope (their growing files would scan as garbage + contend with
        // capture writes), but every OTHER row keeps showing its existing
        // waveform -- so arming one track and rolling no longer blanks the
        // already-recorded tracks.
        void setWaveformsFromSession (const juce::File& sessionDir, bool skipArmedRows = false)
        {
            if (! sessionDir.isDirectory())
            {
                for (auto& r : rows) r->setWaveformFiles ({}, {});
                return;
            }
            // Pro Tools-style: tracks live under "Audio Files/". Legacy
            // sessions kept them at the root, so fall back to the root
            // when the subfolder is absent.
            const auto audioFiles = sessionDir.getChildFile ("Audio Files");
            const auto base = audioFiles.isDirectory() ? audioFiles : sessionDir;
            // Try .wav first, then .flac/.aif so backup-format sessions
            // still draw thumbnails.
            auto findTrackFile = [&] (int trackIdx) -> juce::File
            {
                auto pad = [] (int n) { return juce::String (n).paddedLeft ('0', 2); };
                const juce::String stem = "Track_" + pad (trackIdx + 1);
                const char* exts[] = { ".wav", ".flac", ".aif", ".aiff" };
                for (auto* ext : exts)
                {
                    auto f = base.getChildFile (stem + ext);
                    if (f.existsAsFile()) return f;
                }
                return base.getChildFile (stem + ".wav");
            };
            for (auto& r : rows)
            {
                if (skipArmedRows && rowIsArmed (*r))
                {
                    r->setWaveformFiles ({}, {});   // armed -> live envelope owns the lane
                    continue;
                }
                const int trackIdx = r->getTrackIndex();
                const auto fL = findTrackFile (trackIdx);
                const auto fR = r->isStereoPair() ? findTrackFile (trackIdx + 1) : juce::File();
                r->setWaveformFiles (fL, fR);
            }
        }

        // While recording, the WAV files grow on disk but the file path
        // doesn't change -- so setWaveformFiles' "if path changed"
        // shortcut skips the refresh. Call this from the EditPage timer
        // to force every row to re-scan its current file each tick.
        void forceRefreshWaveforms()
        {
            for (auto& r : rows) r->reloadCurrentWaveformFiles();
        }

        // Live capture envelope: arm / disarm every row's live draw, and
        // (each tick) feed every armed row its current input peak so the
        // lane grows a red waveform during the take. Reads only the live
        // meter atomics -- no disk I/O, so capture integrity is untouched.
        void setLiveRecording (bool on, int prefillSilentPoints = 0)
        {
            for (auto& r : rows) r->setLiveRecording (on, prefillSilentPoints);
        }
        // Take stopped: hold each row's live envelope as a provisional waveform
        // until its real thumbnail scans in, so the lane never blanks.
        void holdLiveEnvelopesUntilScanned()
        {
            for (auto& r : rows) r->holdLiveEnvelopeUntilScanned();
        }
        void pushRecLevels()
        {
            auto& rec = engine.getRecorder();
            const int nTracks = rec.getNumTracks();
            for (auto& r : rows)
            {
                const int idx = r->getTrackIndex();
                if (idx < 0 || idx >= nTracks) continue;
                if (! rec.getTrack (idx).armed.load (std::memory_order_relaxed)) continue;
                const float l = rec.getTrack (idx).peak.load (std::memory_order_relaxed);
                float rr = 0.0f;
                if (r->isStereoPair() && idx + 1 < nTracks)
                    rr = rec.getTrack (idx + 1).peak.load (std::memory_order_relaxed);
                r->pushRecLevel (l, rr);
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

        // Re-pin every row's header column to the (new) horizontal scroll
        // position: re-lay the header child components and repaint so the
        // painted header panel follows. Called from the viewport's scroll
        // callback whenever the timeline scrolls horizontally.
        void relayoutHeaders()
        {
            for (auto& r : rows) { r->resized(); r->repaint(); }
        }

        int rowCount() const { return (int) rows.size(); }

        // True when every row's thumbnail has finished its background scan.
        bool allWaveformsLoaded() const
        {
            for (auto& r : rows)
                if (r != nullptr && ! r->waveformsLoaded())
                    return false;
            return true;
        }

        // Used by EditPage::setLogicalRowsVisible for Memory-Location
        // recall. visibleRows is a list of logical row indices to
        // show; empty list = show all.
        void setRowsVisibility (const std::vector<int>& visibleRows)
        {
            const bool showAll = visibleRows.empty();
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i] != nullptr)
                {
                    const bool wanted = showAll
                        || std::find (visibleRows.begin(), visibleRows.end(), (int) i)
                               != visibleRows.end();
                    rows[i]->setVisible (wanted);
                }
            resized();
        }

    private:
        AudioEngine&                              engine;
        juce::AudioFormatManager&                 formats;
        std::vector<std::unique_ptr<juce::AudioThumbnailCache>>& thumbCaches;
        std::vector<std::unique_ptr<TrackRow>>    rows;
        int                                       viewportHeight { 480 };
    };

    EditPage::EditPage (AudioEngine& eng)
        : engine (eng)
    {
        formatManager.registerBasicFormats();

        // Owned by EditPage so the lifetime tracks the page, but laid
        // out by MainComponent on the same 28 px row as the automation
        // toolbar -- the host re-parents it via getEditToolsBar() +
        // addAndMakeVisible().
        toolsBar = std::make_unique<EditToolsBar>();
        // Likewise owned here, re-parented by the host via
        // getAutomationToolbar() + addAndMakeVisible().
        autoToolbar = std::make_unique<AutomationToolbar>();

        // Gig-one field report: waveform builds felt slow after record/load.
        // Two levers: (1) each scan thread runs at high priority so it paints
        // as fast as the disk allows; (2) the caches are SHARDED so several
        // WAVs scan in PARALLEL instead of serially on one thread -- the cost
        // that dominated first-opening an un-cached multitrack session.
        for (int s = 0; s < kNumScanShards; ++s)
        {
            auto cache = std::make_unique<juce::AudioThumbnailCache> (256);
            auto& scanThread = cache->getTimeSliceThread();
            scanThread.stopThread (2000);
            scanThread.startThread (juce::Thread::Priority::high);
            thumbnailCaches.push_back (std::move (cache));
        }

        list = std::make_unique<TrackList> (engine, formatManager, thumbnailCaches);
        list->sharedToolsBar = toolsBar.get();
        viewport.setViewedComponent (list.get(), false);
        // Vertical scrollbar visible; horizontal scrollbar hidden BUT
        // horizontal scrolling still enabled (4th arg = allow horizontal
        // scrolling without a scrollbar) so the trackpad / wheel still pan
        // the timeline left-right. The redundant horizontal scrollbar (a
        // second blue bar duplicating the TimelineMinimap overview navigator)
        // is gone, but the scroll gesture is not.
        viewport.setScrollBarsShown (true, false, /*allowVertWithoutBar*/ false,
                                     /*allowHorizWithoutBar*/ true);
        // Re-pin every row's header column to the left edge on horizontal
        // scroll so the meter / routing / R-I-S-M controls never slide away.
        viewport.onScroll = [this] { if (list != nullptr) list->relayoutHeaders(); };
        addAndMakeVisible (viewport);

        // Overview navigator -- hidden until zoomed in (resized() shows it).
        addChildComponent (minimap);
        minimap.onScrollToContentX = [this] (int x)
        {
            const int maxX = juce::jmax (0, list->getWidth() - viewport.getViewWidth());
            viewport.setViewPosition (juce::jlimit (0, maxX, x), viewport.getViewPositionY());
        };

        // DAW-style edge zoom clusters, overlaid on top of the viewport.
        // V (amplitude) stacked at the right edge; H (timeline) at the
        // bottom-right. Step a fixed ratio per click.
        for (auto* b : { &zoomVIn, &zoomVOut, &zoomHIn, &zoomHOut })
        {
            b->setColour (juce::TextButton::buttonColourId, brand::controlBg);
            b->setColour (juce::TextButton::textColourOffId, brand::textPrimary);
            addAndMakeVisible (*b);
        }
        zoomVIn .setTooltip ("Taller waveforms (vertical zoom in)");
        zoomVOut.setTooltip ("Shorter waveforms (vertical zoom out)");
        zoomHIn .setTooltip ("Zoom in on the timeline");
        zoomHOut.setTooltip ("Zoom out (1x = whole take)");
        // Accessibility: the on-screen labels are "V+/V-/H+/H-", which a
        // screen reader would read as glyphs. Give each a spoken name (and
        // surface the tooltip as help text). They are TextButtons, so the
        // button role + Return/Space activation already work.
        zoomVIn .setTitle ("Vertical zoom in");    zoomVIn .setHelpText (zoomVIn .getTooltip());
        zoomVOut.setTitle ("Vertical zoom out");   zoomVOut.setHelpText (zoomVOut.getTooltip());
        zoomHIn .setTitle ("Horizontal zoom in");  zoomHIn .setHelpText (zoomHIn .getTooltip());
        zoomHOut.setTitle ("Horizontal zoom out"); zoomHOut.setHelpText (zoomHOut.getTooltip());
        zoomVIn .onClick = [this] { setVerticalZoom (vZoom * 1.41f); };
        zoomVOut.onClick = [this] { setVerticalZoom (vZoom * 0.71f); };
        zoomHIn .onClick = [this] { setZoom (zoom * 1.41f); };
        zoomHOut.onClick = [this] { setZoom (zoom * 0.71f); };

        // Pro Tools-style Min:Secs time ruler perched above the track
        // list. Reads session length + sample rate from the engine via
        // its own 4 Hz timer.
        ruler = std::make_unique<EditTimeRuler> (engine);
        addAndMakeVisible (*ruler);

        // Reusable loading/empty/error surface, overlaid on the wave area.
        // It manages its own visibility -- shown when no session is loaded,
        // cleared once waveforms are present (see refresh()).
        addChildComponent (placeholder);

        // Now that the rows exist, point the EDIT view at its own
        // automation toolbar (wires list->sharedToolbar + row lanes).
        setAutomationToolbar (autoToolbar.get());

        refresh();
        startTimerHz (24);
    }

    EditPage::~EditPage()
    {
        stopTimer();
        // Flush the waveform cache to WaveCache.wfm in whichever
        // session is active. Best-effort: failure here only means the
        // next launch re-scans waveforms, no data loss.
        const auto sessionDir = engine.getActiveSessionDir();
        if (sessionDir.isDirectory())
            saveCacheToSession (sessionDir);
    }

    void EditPage::loadCacheFromSession (const juce::File& sessionDir)
    {
        const auto cacheFile = sessionDir.getChildFile ("WaveCache.wfm");
        if (! cacheFile.existsAsFile() || cacheFile.getSize() < 16) return;

        // Versioned header: magic + (resolution<<8|rev). A cache baked at a
        // different thumbnail resolution would re-paint as coarse stair-step
        // blocks (its min/max points are too sparse for the new draw), so a
        // mismatch -- including pre-header caches whose first int isn't our
        // magic -- means delete the stale file and let the thumbnails
        // re-scan the Track_NN.wav files at the current resolution. The
        // stream is scoped so its file handle is released before we delete.
        bool stale = false;
        {
            juce::FileInputStream in (cacheFile);
            if (! in.openedOk()) return;

            const int magic  = in.readInt();
            const int ver    = in.readInt();
            const int shards = in.readInt();
            if (magic != kWaveCacheMagic || ver != kWaveCacheVersion
                || shards != (int) thumbnailCaches.size())
                stale = true;
            else
                // One length-prefixed section per shard cache. Best-effort:
                // readFromStream returns false on a corrupt / wrong-JUCE-version
                // body, in which case those thumbnails just re-scan -- worst
                // case 'slow first paint', never wrong audio.
                for (auto& cache : thumbnailCaches)
                {
                    const int len = in.readInt();
                    if (len <= 0) continue;
                    juce::MemoryBlock mb;
                    in.readIntoMemoryBlock (mb, len);
                    juce::MemoryInputStream mis (mb, false);
                    cache->readFromStream (mis);
                }
        }
        if (stale)
            cacheFile.deleteFile();
    }

    void EditPage::saveCacheToSession (const juce::File& sessionDir)
    {
        const auto cacheFile = sessionDir.getChildFile ("WaveCache.wfm");
        // Overwrite atomically -- write to a temp file then rename so
        // a crash mid-write leaves the previous cache intact.
        const auto tmpFile = sessionDir.getChildFile ("WaveCache.wfm.tmp");
        tmpFile.deleteFile();
        {
            juce::FileOutputStream out (tmpFile);
            if (! out.openedOk()) return;
            out.writeInt (kWaveCacheMagic);              // header: tag the resolution
            out.writeInt (kWaveCacheVersion);            // so a later res change drops it
            out.writeInt ((int) thumbnailCaches.size()); // shard count
            // One length-prefixed section per shard cache so each loads back
            // into the same shard on reopen.
            for (auto& cache : thumbnailCaches)
            {
                juce::MemoryOutputStream mos;
                cache->writeToStream (mos);
                out.writeInt ((int) mos.getDataSize());
                out.write (mos.getData(), mos.getDataSize());
            }
            out.flush();
        }
        if (tmpFile.getSize() > 0)
        {
            cacheFile.deleteFile();
            tmpFile.moveFileTo (cacheFile);
        }
        else
        {
            tmpFile.deleteFile();
        }
    }

    void EditPage::setAutomationEditWrapper (AutoEditWrapper fn)
    {
        automationEditWrapper = std::move (fn);
        if (list != nullptr)
        {
            list->sharedAutomationEditWrapper = automationEditWrapper;
            list->sharedAutomationDragBegin   = automationDragBegin;
            list->sharedAutomationDragEnd     = automationDragEnd;
            list->updateRowContext();
        }
    }

    void EditPage::setAutomationToolbar (AutomationToolbar* t)
    {
        toolbar = t;
        if (list != nullptr)
        {
            list->sharedToolbar = t;
            list->sharedAutomationEditWrapper = automationEditWrapper;
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
            list->sharedClickRowIdx  = present ? clickIdx : -1;
            list->updateRowContext();
        }
        repaint();
    }

    void EditPage::refresh()
    {
        const int n = engine.getRecorder().getNumTracks();

        // Compute logical (stereo-aware) row count and rebuild when it
        // differs -- physical track count alone doesn't detect isStereo
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

        // Use the engine-wide 'active' session (recorder takes priority
        // over player) so waveforms render the file being WRITTEN, not
        // just the file being read back.
        const auto sessionDir = engine.getActiveSessionDir();

        // When the session changes, pull the on-disk WaveCache.wfm
        // into the thumbnail cache BEFORE the new TrackRows ask
        // their thumbnails for sources -- otherwise the thumbnails
        // re-scan the WAV files even though cached peaks exist.
        if (sessionDir != lastSessionDir && sessionDir.isDirectory())
            loadCacheFromSession (sessionDir);

        // While recording, only the ARMED rows defer to the live red capture
        // envelope (their growing WAVs would scan as blocky garbage and the
        // disk read would contend with the recorder's own writes). Every OTHER
        // row keeps showing its already-recorded waveform -- arming one track
        // and rolling no longer blanks the tracks you captured earlier. The
        // armed rows' real waveforms scan in once, cleanly, on stop.
        if (engine.isRecording())
            list->setWaveformsFromSession (sessionDir, /*skipArmedRows*/ true);
        else
            list->setWaveformsFromSession (sessionDir);

        updatePlaceholder();
        lastLoaded = engine.getPlayer().isLoaded();
    }

    void EditPage::repaintLanes()
    {
        if (list != nullptr) list->repaint();   // rows read engine automation in paint()
        repaint();
    }

    void EditPage::updatePlaceholder()
    {
        // Show the empty-state placeholder only when there are NO channels.
        // Once channels exist (recorded or not) the rows own the view, so the
        // overlay never sits on top of them. getState() guard = no re-announce.
        const bool hasChannels = engine.getRecorder().getNumTracks() > 0;
        const auto want = hasChannels ? PlaceholderView::State::Hidden
                                      : PlaceholderView::State::Empty;
        if (placeholder.getState() == want) return;
        if (hasChannels) placeholder.clear();
        else             placeholder.showEmpty ("No session loaded",
                             "Add channels (+CH), or open / record a session.");
    }

    void EditPage::timerCallback()
    {
        // Session-change detection + waveform attach run REGARDLESS of EDIT
        // visibility, so the AudioThumbnail background scan (and WaveCache.wfm
        // build) kick off the moment audio is imported / recorded -- not when
        // the engineer first switches to EDIT. Rows built while hidden are
        // just invisible components; the actual file read happens on the
        // thumbnail cache's background thread. Only the per-tick visual work
        // further down is gated on visibility.
        const int n = engine.getRecorder().getNumTracks();
        if (n != lastTrackCount)
            refresh();

        // Pick up session swaps -- recording starts, recording stops,
        // session loaded, session changed, ...
        const bool loaded = engine.getPlayer().isLoaded();
        const bool rec    = engine.isRecording();
        const bool recJustStopped = (! rec && lastRecording);
        const bool recJustStarted = (rec && ! lastRecording);

        // Live capture envelope: clear + arm on take start, disarm on stop
        // (before the post-stop disk re-scan swaps in the real waveform).
        if (recJustStarted && list != nullptr)
        {
            // Continue take: seed the envelope with silent columns for the
            // existing take so the live waveform draws AFTER it (at the base),
            // matching the playhead. The EDIT timer ticks at 24 Hz, so the
            // envelope grows ~24 columns/sec -- prefill the same density.
            const auto base = engine.getRecorder().getRecordBaseSamples();
            const double sr = engine.getPlayer().getSampleRate() > 0.0
                                  ? engine.getPlayer().getSampleRate() : 48000.0;
            const int prefill = base > 0 ? (int) ((double) base / sr * 24.0) : 0;
            list->setLiveRecording (true, prefill);
        }
        // On stop, HOLD the live envelope (don't clear it) until the real disk
        // thumbnail scans in -- the lane shows the just-captured waveform
        // instantly instead of blanking for the duration of the scan.
        if (recJustStopped && list != nullptr) list->holdLiveEnvelopesUntilScanned();
        if (loaded != lastLoaded || rec != lastRecording || engine.getActiveSessionDir() != lastSessionDir)
        {
            if (engine.getActiveSessionDir() != lastSessionDir)
                waveCacheSaved = false;   // new session -> its cache needs (re)writing
            lastSessionDir = engine.getActiveSessionDir();
            lastRecording  = rec;
            refresh();
        }

        // Persist WaveCache.wfm as soon as the background scan finishes for the
        // whole session -- even while EDIT is hidden -- so reopening the show
        // (or relaunching) paints waveforms instantly instead of re-scanning
        // every WAV. Without this the cache was only flushed on app quit, so a
        // freshly-imported session that wasn't quit cleanly re-scanned on the
        // next open. Done once per session; skipped while recording (files
        // still growing).
        if (! waveCacheSaved && ! rec && list != nullptr && list->rowCount() > 0
            && engine.getActiveSessionDir().isDirectory()
            && list->allWaveformsLoaded())
        {
            saveCacheToSession (engine.getActiveSessionDir());
            waveCacheSaved = true;
        }

        // Waveforms are NOT re-scanned from disk while recording: 48 channels
        // re-read at 24 Hz would contend with the recorder's own capture
        // writes and risk dropouts (capture integrity wins). The meters show
        // live signal during the take. The moment recording stops, the files
        // are final -- do one clean full re-scan (which drops any partial
        // cached mid-capture) so the waveform paints at full resolution.
        if (recJustStopped && list != nullptr)
            list->forceRefreshWaveforms();

        // Everything below is purely visual (playhead + repaints) and only
        // matters when EDIT is on screen. While recording we keep going even
        // when hidden so peaks stay current for the flip back to EDIT.
        if (! isVisible() && ! rec)
            return;

        // Append this tick's input peaks to the live capture envelope so
        // every armed lane grows a red waveform during the take. Kept
        // running even while EDIT is hidden so flipping back mid-take
        // shows the envelope already filled in.
        if (rec && list != nullptr)
            list->pushRecLevels();

        // Playhead. While RECORDING the live head is the recorder's elapsed
        // count on a grow-to-fit timeline (same timebase as the ruler + the
        // live capture envelope) -- gig-one field report: the engineer must
        // see the take rolling in EDIT, not a parked view.
        const auto& player = engine.getPlayer();
        // Timeline position: on a CONTINUE this is the take end + samples so far,
        // so the playhead carries on past the existing take instead of jumping
        // back to 0.
        const auto  recPos  = rec ? engine.getRecorder().getRecordTimelineSamples() : 0;
        const auto  total   = rec ? juce::jmax (player.getTotalLengthSamples(), recPos)
                                  : player.getTotalLengthSamples();
        const auto  pos     = rec ? recPos : player.getPositionSamples();
        int playheadX = -1;
        if (total > 0 && list->rowCount() > 0)
        {
            constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
            // Map across the ROWS' width (list->getWidth() == contentW), not
            // the EditPage's visible width -- when zoomed in the two differ,
            // and the playhead lives inside the contentW-wide TrackRow. The
            // -8/+4 matches the lane's clip inset (brand::space::xs each side)
            // AND the ruler's mapping, so both playheads land on the same x.
            const auto wavePaneWidth = juce::jmax (1, list->getWidth() - kHeaderW);
            const double frac = (double) pos / (double) total;
            playheadX = (int) (frac * (wavePaneWidth - 8)) + 4;
        }
        list->setPlayheadX (playheadX);
        list->pollMixerState();

        // ── Auto-scroll: keep the playhead on screen while playing ──────────
        // On a long take zoomed in, the playhead would otherwise roll off the
        // right edge and you'd lose it. Page-scroll (not continuous follow, so
        // it doesn't jitter): when the playhead passes ~85% of the visible
        // width, jump so it lands just right of the pinned header, showing the
        // audio that's coming up. Only while playing and only when zoomed in
        // (content wider than the view) -- so a stopped engineer can scroll
        // freely.
        if ((player.isPlaying() || rec) && total > 0 && list != nullptr)
        {
            constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
            const int viewW = viewport.getViewWidth();
            const int contentW = list->getWidth();
            if (contentW > viewW + 1)   // zoomed in
            {
                const int contentWaveW = juce::jmax (1, contentW - kHeaderW);
                const double frac = (double) pos / (double) total;
                const int playheadContentX = kHeaderW + (int) (frac * contentWaveW);
                const int viewX = viewport.getViewPositionX();
                const int rightTrigger = viewX + (int) (viewW * 0.85);
                if (playheadContentX > rightTrigger || playheadContentX < viewX + kHeaderW)
                {
                    const int maxX = juce::jmax (0, contentW - viewW);
                    int newX = playheadContentX - kHeaderW - (int) (viewW * 0.10);
                    newX = juce::jlimit (0, maxX, newX);
                    if (newX != viewX)
                        viewport.setViewPosition (newX, viewport.getViewPositionY());
                }
            }
        }

        // Keep the overview navigator in sync with the live scroll/zoom.
        if (minimap.isVisible())
            minimap.setView (list->getWidth(), 380,
                             viewport.getViewPositionX(), viewport.getViewWidth());
    }

    void EditPage::paint (juce::Graphics& g)
    {
        g.fillAll (brand::bgDeep);
    }

    void EditPage::resized()
    {
        auto bounds = getLocalBounds();

        // Time ruler perches across the top, 46 px tall:
        //   20 px marker strip + 26 px Min:Secs scale.
        // Spans the full width so the header label column aligns with
        // each TrackRow's header column.
        const int rulerH = 46;
        if (ruler != nullptr)
            ruler->setBounds (bounds.removeFromTop (rulerH));

        // Overview navigator strip along the bottom -- only when zoomed in
        // (content wider than the view), leaving room for the H/V zoom
        // clusters at the bottom-right.
        const bool showMinimap = (zoom > 1.001f);
        if (showMinimap)
        {
            auto mmRow = bounds.removeFromBottom (18);
            mmRow.removeFromRight (70);   // clear the zoom clusters
            minimap.setBounds (mmRow.reduced (4, 1));
            minimap.setVisible (true);
        }
        else
        {
            minimap.setVisible (false);
        }

        viewport.setBounds (bounds);
        placeholder.setBounds (bounds);   // overlays the wave area when shown
        list->setViewportHeight (viewport.getHeight());
        // Apply the zoom factor -- content widens past the viewport when
        // zoom > 1; the horizontal scrollbar lights up to navigate.
        const int contentW = juce::jmax (viewport.getWidth(),
                                         (int) (viewport.getWidth() * zoom));
        list->setSize (contentW, list->getHeight());
        list->resized();

        // Push the same content width into the ruler so its
        // pixels-per-second matches the wave pane below it.
        if (ruler != nullptr)
            ruler->setContentWidth (contentW);

        // Edge zoom clusters, overlaid in the bottom-right corner:
        //   V+        (vertical / amplitude, stacked)
        //   V-
        //   H- H+     (horizontal / timeline, side by side)
        const int zb = 26, pad = 8;
        const int rightX = bounds.getRight()  - zb - pad;
        const int botY   = bounds.getBottom() - zb - pad;
        zoomVIn .setBounds (rightX,            botY - 2 * (zb + 4), zb, zb);
        zoomVOut.setBounds (rightX,            botY -     (zb + 4), zb, zb);
        zoomHOut.setBounds (rightX - zb - 4,   botY,                zb, zb);
        zoomHIn .setBounds (rightX,            botY,                zb, zb);
        for (auto* b : { &zoomVIn, &zoomVOut, &zoomHIn, &zoomHOut })
            b->toFront (false);
    }

    void EditPage::setLogicalRowsVisible (const std::vector<int>& visibleRows)
    {
        if (list == nullptr) return;
        list->setRowsVisibility (visibleRows);
    }

    void EditPage::scrollToSample (juce::int64 sample)
    {
        if (list == nullptr || sample < 0) return;
        const auto& player = engine.getPlayer();
        const auto totalSamples = player.isLoaded() ? player.getTotalLengthSamples() : 0;
        if (totalSamples <= 0) return;
        const int contentW = list->getWidth();
        if (contentW <= 0) return;
        const double prop = (double) sample / (double) totalSamples;
        const int    targetX = (int) (prop * (double) contentW);
        const int    halfV   = viewport.getViewWidth() / 2;
        viewport.setViewPosition (juce::jmax (0, targetX - halfV),
                                  viewport.getViewPositionY());
    }

    void EditPage::zoomToSamples (juce::int64 a, juce::int64 b)
    {
        if (list == nullptr) return;
        const auto& player = engine.getPlayer();
        const auto total = player.isLoaded() ? player.getTotalLengthSamples() : 0;
        if (total <= 0 || b <= a) return;
        // Zoom so the selection ~fills the viewport (a little headroom), then
        // centre on it. setZoom clamps to [1, 16].
        const double frac = (double) (b - a) / (double) total;
        if (frac > 0.0) setZoom ((float) juce::jlimit (1.0, 16.0, 0.9 / frac));
        scrollToSample ((a + b) / 2);
    }

    void EditPage::setZoom (float z)
    {
        z = juce::jlimit (1.0f, 16.0f, z);
        if (std::abs (z - zoom) < 0.01f) return;
        zoom = z;
        resized();
        if (onZoomChanged) onZoomChanged (zoom);
    }

    void EditPage::setVerticalZoom (float z)
    {
        z = juce::jlimit (0.25f, 32.0f, z);
        if (std::abs (z - vZoom) < 0.001f) return;
        vZoom = z;
        if (list != nullptr) list->repaint();
    }

    void EditPage::wheelZoomHorizontal (float delta)
    {
        // Keep the time under the viewport centre stable across the zoom.
        const double centreFrac = (viewport.getViewPositionX()
                                   + viewport.getWidth() * 0.5)
                                  / (double) juce::jmax (1, list->getWidth());
        setZoom (zoom * (delta > 0.0f ? 1.18f : 1.0f / 1.18f));
        const int newW = list->getWidth();
        viewport.setViewPosition (
            juce::jmax (0, (int) (centreFrac * newW - viewport.getWidth() * 0.5)),
            viewport.getViewPositionY());
    }

    void EditPage::wheelZoomVertical (float delta)
    {
        setVerticalZoom (vZoom * (delta > 0.0f ? 1.18f : 1.0f / 1.18f));
    }
}
