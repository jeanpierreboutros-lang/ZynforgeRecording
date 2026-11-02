#include "EditPage.h"
#include "EditToolsBar.h"
#include "EditTimeRuler.h"
#include "../Theme/BrandColors.h"
#include "../Theme/BrandTokens.h"
#include "../Theme/DialogChrome.h"
#include "LedMeter.h"
#include "StripColourPicker.h"
#include "RenameLabel.h"
#include "EditTrackRow.h"
#include "../Audio/TrackOrder.h"

#include <algorithm>
#include <map>

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

        // Push-down configuration: the tools palette.
        // Stored here so newly-created rows pick it up automatically;
        // existing rows are mutated through updateRowContext().
        EditToolsBar*      sharedToolsBar      { nullptr };

        void updateRowContext()
        {
            for (auto& r : rows)
            {
                r->toolsBar               = sharedToolsBar;
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
                noteRowView (*r);
            }
            resized();
        }

        // A row's height and lane belong to its strip, not to its position, so
        // they outlive a rebuild and go where a move takes the strip.
        std::function<void()> onRowViewChanged;

        void noteRowView (const TrackRow& r)
        {
            if (r.getStripKey().isNotEmpty())
                rowViews[r.getStripKey()] = { r.getRowSize(), r.getCustomHeight(), r.laneMode };
        }

        void applyRowView (TrackRow& r) const
        {
            const auto saved = rowViews.find (r.getStripKey());
            if (saved == rowViews.end()) return;
            r.setRowSize (saved->second.size);
            r.setCustomHeight (saved->second.height);
            r.laneMode = saved->second.lane;
        }

        // By the recorder's strips, not the rows: a save can land between a
        // session swap and the rebuild, and the rows would name the old session.
        juce::var rowViewsJson() const
        {
            juce::Array<juce::var> out;
            auto& rec = engine.getRecorder();
            for (int i = 0; i < rec.getNumTracks(); ++i)
            {
                const auto saved = rowViews.find (rec.getTrack (i).stripId);
                if (saved == rowViews.end()
                    || (saved->second.size == TrackRow::Size::Small && saved->second.lane == TrackRow::LaneMode::Waveform))
                    continue;
                juce::DynamicObject::Ptr view (new juce::DynamicObject());
                view->setProperty ("uid",    saved->first);
                view->setProperty ("size",   (int) saved->second.size);
                view->setProperty ("height", saved->second.height);
                view->setProperty ("lane",   saved->second.lane == TrackRow::LaneMode::Markers ? "markers" : "waveform");
                out.add (juce::var (view.get()));
            }
            return out;
        }

        void restoreRowViews (const juce::var& saved)
        {
            rowViews.clear();
            if (const auto* views = saved.getArray())
                for (const auto& v : *views)
                    if (const auto uid = v.getProperty ("uid", {}).toString(); uid.isNotEmpty())
                        rowViews[uid] = { (TrackRow::Size) juce::jlimit (0, (int) TrackRow::Size::Custom, (int) v.getProperty ("size", 2)),
                                          juce::jlimit (TrackRow::kMinRowH, TrackRow::kMaxRowH, (int) v.getProperty ("height", 80)),
                                          v.getProperty ("lane", {}).toString() == "markers" ? TrackRow::LaneMode::Markers
                                                                                             : TrackRow::LaneMode::Waveform };
            for (auto& r : rows) applyRowView (*r);
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
                r->onViewChanged = [this] (TrackRow& row)
                {
                    noteRowView (row);
                    if (onRowViewChanged) onRowViewChanged();
                };
                r->toolsBar               = sharedToolsBar;
                applyRowView (*r);
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
            if (auto* page = findParentComponentOfClass<EditPage>();
                ! isEnabled() || engine.isRecording() || (page != nullptr && ! page->canEditTimeline()))
                return;
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
            waveformSession = sessionDir;
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
                if (r->isStereoPair())
                {
                    r->setWaveformFiles (findTrackFile (trackIdx), findTrackFile (trackIdx + 1));
                    continue;
                }
                // Resolved the way playback resolves it: after an unlink, a
                // mono row can be the right channel of the file on its left.
                int channel = 0;
                const auto file = engine.getTrackAudioFile (trackIdx, &channel);
                r->setWaveformFiles (file.existsAsFile() ? file : findTrackFile (trackIdx), {}, channel);
            }
        }

        // The same outcome for a take on disk, with no disk: resolving a file
        // means stat and directory scans on the message thread, and the take's
        // primary may be the mount that has stopped answering. Rows already show
        // what they should; a new session folder holds nothing to show yet.
        void holdWaveformsForTake (const juce::File& sessionDir, bool skipArmedRows)
        {
            const bool sameSession = sessionDir == waveformSession;
            waveformSession = sessionDir;
            for (auto& r : rows)
                if (! sameSession || (skipArmedRows && rowIsArmed (*r)))
                    r->setWaveformFiles ({}, {});
        }

        // While recording, the WAV files grow on disk but the file path
        // doesn't change -- so setWaveformFiles' "if path changed"
        // shortcut skips the refresh. Call this from the EditPage timer
        // to force every row to re-scan its current file each tick.
        void forceRefreshWaveforms()
        {
            for (auto& r : rows) r->reloadCurrentWaveformFiles();
        }

        // The rows a take is writing, by physical track.
        std::set<int> armedRows() const
        {
            std::set<int> armed;
            for (auto& r : rows)
                if (rowIsArmed (*r)) armed.insert (r->getTrackIndex());
            return armed;
        }

        // After a take only the rows it wrote change on disk. The others keep
        // their peaks: rescanning them blanks lanes that are already right,
        // for longer the bigger the session.
        void refreshWaveformsOf (const std::set<int>& tracks)
        {
            for (auto& r : rows)
                if (tracks.count (r->getTrackIndex()) > 0) r->reloadCurrentWaveformFiles();
        }

        // Live capture envelope: arm / disarm every row's live draw, and
        // (each tick) feed every armed row its current input peak so the
        // lane grows a red waveform during the take. Reads only the live
        // meter atomics -- no disk I/O, so capture integrity is untouched.
        void setLiveRecording (bool on, juce::int64 prefillSilentPoints = 0)
        {
            for (auto& r : rows) r->setLiveRecording (on, prefillSilentPoints);
        }
        // Take stopped: hold each row's live envelope as a provisional waveform
        // until its real thumbnail scans in, so the lane never blanks.
        void holdLiveEnvelopesUntilScanned()
        {
            for (auto& r : rows) r->holdLiveEnvelopeUntilScanned();
        }

        // Stop every row's meter timer before the caller frees the TrackStates
        // they reference. The rows themselves are rebuilt on EditPage's next
        // 24 Hz tick, which is far too late -- see TrackRow::invalidate().
        void condemnAllRows()
        {
            for (auto& r : rows) r->invalidate();
        }
        void pushRecLevels()
        {
            auto& rec = engine.getRecorder();
            const int nTracks = rec.getNumTracks();
            const auto remote = engine.captureStatus();
            const bool daemon = remote.source == "daemon";
            if (engine.isRecording() && ! rec.isRecording() && ! daemon) return;
            // Where the new audio anchors: a CONTINUE seeds the live envelope
            // with the existing take's length (in overview columns) so the
            // capture draws AFTER the take, not from the left edge. Stable for
            // the whole pass (recordBaseSamples doesn't move mid-take).
            const auto base    = daemon ? remote.positionSamples - remote.elapsedSamples
                                        : rec.getRecordBaseSamples();
            const juce::int64 prefill = base > 0 ? base / TrackState::kLiveBinSamples : 0;
            for (auto& r : rows)
            {
                const int idx = r->getTrackIndex();
                if (idx < 0 || idx >= nTracks) continue;
                if (daemon)
                {
                    if (idx >= (int) remote.tracks.size() || ! remote.tracks[(size_t) idx].armed)
                        continue;
                    r->setLiveRecording (true, prefill);
                    const auto bins = remote.positionSamples / TrackState::kLiveBinSamples;
                    r->appendRemotePeakL (remote.tracks[(size_t) idx].peak, bins);
                    if (r->isStereoPair() && idx + 1 < (int) remote.tracks.size())
                        r->appendRemotePeakR (remote.tracks[(size_t) idx + 1].peak, bins);
                    r->repaint();
                    continue;
                }
                if (! rec.getTrack (idx).armed.load (std::memory_order_relaxed)) continue;
                // SELF-ARM the live draw from the current recording+armed state,
                // every tick -- never a one-shot "record just started" event.
                // setLiveRecording only resets on the off->on edge, so this is
                // idempotent after the first tick; but if that first edge was
                // ever missed or clobbered (e.g. a refresh between ticks), the
                // armed lane still enters live mode here instead of silently
                // discarding the drained peaks for the rest of the take. This
                // is what makes the waveform build live on a CONTINUE, matching
                // a fresh take.
                r->setLiveRecording (true, prefill);
                // Drain the recorder's fine live-wave overview (min/max pairs
                // captured on the audio thread) -- one peak column per pair, so
                // the lane builds a DETAILED waveform as it records, not a
                // 1-per-tick coarse envelope.
                bool grew = false;
                rec.getTrack (idx).liveWaveDrain ([&] (float mn, float mx)
                    { r->appendLivePeakL (juce::jmax (-mn, mx)); grew = true; });
                if (r->isStereoPair() && idx + 1 < nTracks)
                    rec.getTrack (idx + 1).liveWaveDrain ([&] (float mn, float mx)
                        { r->appendLivePeakR (juce::jmax (-mn, mx)); });
                if (grew) r->repaint();   // one repaint per tick after the batch
            }
        }

        void setPlayheadX (int px)
        {
            for (auto& r : rows) r->setPlayheadX (px);
        }

        // Repaint every row unconditionally -- used while a background waveform
        // scan is running so the freshly-recorded take draws in AS it loads,
        // without needing a mouse move to trigger a repaint.
        void repaintRows()
        {
            for (auto& r : rows) r->repaint();
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

        // True when the rows were built for exactly this logical layout, strip
        // for strip. A count check alone misses a block move that keeps both the
        // track and row counts ([mono, stereo] -> [stereo, mono]) and would leave
        // rows drawing the wrong strip as a pair.
        bool matchesLayout (const trackorder::Blocks& blocks) const
        {
            if (blocks.size() != rows.size()) return false;
            auto& rec = engine.getRecorder();
            for (size_t k = 0; k < rows.size(); ++k)
                if (rows[k]->getTrackIndex() != blocks[k].front()
                    || rows[k]->isStereoPair() != (blocks[k].size() == 2)
                    || rows[k]->getStripKey() != rec.getTrack (blocks[k].front()).stripId)
                    return false;
            return true;
        }

        // Swatch-drag reorder preview: the physical index the dragged strip
        // would be inserted before (numTracks = after the last row), with a
        // drop line drawn at that gap. Rows are logical, so a row's index is
        // always a strip's L track. Hidden rows (Memory-Location recall) are
        // skipped; dropping below the last visible row lands in front of the
        // next row, visible or not.
        int updateReorderPreview (juce::Point<int> p, int numTracks)
        {
            int before = -1, lineY = -1, lastVisible = -1;
            for (size_t k = 0; k < rows.size(); ++k)
            {
                const auto& r = *rows[k];
                if (! r.isVisible()) continue;
                lastVisible = (int) k;
                if (p.y < r.getBounds().getCentreY())
                {
                    before = r.getTrackIndex();
                    lineY  = r.getY() - kRowGap / 2;
                    break;
                }
            }
            if (before < 0 && lastVisible >= 0)
            {
                const auto next = (size_t) lastVisible + 1;
                before = next < rows.size() ? rows[next]->getTrackIndex() : numTracks;
                lineY  = rows[(size_t) lastVisible]->getBottom() + kRowGap / 2;
            }
            setReorderLineY (lineY);
            return before;
        }

        void setReorderLineY (int y)
        {
            if (y == reorderLineY) return;
            reorderLineY = y;
            repaint();
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            if (reorderLineY < 0) return;
            g.setColour (brand::accentEdit);
            g.fillRect (0, reorderLineY - 1, getWidth(), 2);
        }

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
        struct RowView { TrackRow::Size size; int height; TrackRow::LaneMode lane; };
        std::map<juce::String, RowView>           rowViews;   // by TrackState::stripId
        juce::File waveformSession;   // the folder the rows last resolved against
        int                                       viewportHeight { 480 };
        int                                       reorderLineY { -1 };   // swatch-drag drop line; -1 = none
    };

    EditPage::EditPage (AudioEngine& eng)
        : engine (eng)
    {
        formatManager.registerBasicFormats();

        // Owned by EditPage so the lifetime tracks the page, but laid
        // out by MainComponent -- the host re-parents it via
        // getEditToolsBar() + addAndMakeVisible().
        toolsBar = std::make_unique<EditToolsBar>();

        // Waveform scans must yield disk access to recording, playback and
        // metadata saves. Keep the existing shard/cache layout, but never run
        // whole-take scans at user-initiated priority on an external volume.
        for (int s = 0; s < kNumScanShards; ++s)
        {
            auto cache = std::make_unique<juce::AudioThumbnailCache> (256);
            auto& scanThread = cache->getTimeSliceThread();
            scanThread.stopThread (-1);
            scanThread.startThread (juce::Thread::Priority::background);
            thumbnailCaches.push_back (std::move (cache));
        }

        list = std::make_unique<TrackList> (engine, formatManager, thumbnailCaches);
        list->sharedToolsBar = toolsBar.get();
        list->onRowViewChanged = [this] { if (onRowViewChanged) onRowViewChanged(); };
        viewport.setViewedComponent (list.get(), false);
        // Vertical scrollbar visible; horizontal scrollbar hidden BUT
        // horizontal scrolling still enabled (4th arg = allow horizontal
        // scrolling without a scrollbar) so the trackpad / wheel still pan
        // the timeline left-right. The redundant horizontal scrollbar (a
        // second blue bar duplicating the TimelineMinimap overview navigator)
        // is gone, but the scroll gesture is not.
        viewport.setScrollBarsShown (true, false, /*allowVertWithoutBar*/ false,
                                     /*allowHorizWithoutBar*/ true);
        // While the row list is click-through during capture, the viewport
        // receives wheel events directly. Keep the same Cmd-wheel zoom paths.
        viewport.onZoomWheel = [this] (float delta, bool vertical)
        {
            if (vertical) wheelZoomVertical (delta);
            else          wheelZoomHorizontal (delta);
        };
        // Re-pin every row's header column to the left edge on horizontal
        // scroll so the meter / routing / R-I-S-M controls never slide away.
        viewport.onScroll = [this]
        {
            const int viewX = viewport.getViewPositionX();
            const bool wasFollowing = followState.isFollowing();
            followState.horizontalScroll (lastViewportX, viewX,
                                          programmaticViewportChange);
            lastViewportX = viewX;
            if (wasFollowing != followState.isFollowing()) syncFollowButton();
            if (list != nullptr) list->relayoutHeaders();
            // Keep the fixed-width ruler aligned with the scrolling wave lanes.
            // visibleAreaChanged fires for manual AND programmatic scrolls
            // (auto-scroll while playing, zoom-to-selection), so this one hook
            // covers them all -- without it the ruler ticks/playhead drift right
            // of the audio by the scroll amount when zoomed in.
            if (ruler != nullptr) ruler->setScrollOffsetX (viewX);
        };
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
        // While rolling, manual horizontal navigation switches to BROWSE.
        // The engineer can return to the live edge explicitly with FOLLOW;
        // vertical scrolling and amplitude zoom do not change this state.
        followButton.setClickingTogglesState (true);
        followButton.setTitle ("Follow playhead");
        followButton.setHelpText ("Follow the playhead while recording or playing; horizontal scroll or zoom pauses follow.");
        followButton.setColour (juce::TextButton::buttonColourId, brand::controlBg);
        followButton.setColour (juce::TextButton::buttonOnColourId, brand::toolActive());
        followButton.setColour (juce::TextButton::textColourOffId, brand::textPrimary);
        followButton.setColour (juce::TextButton::textColourOnId, brand::onSignal (brand::toolActive()));
        followButton.onClick = [this]
        {
            followState.setFollowing (followButton.getToggleState());
            syncFollowButton();
        };
        addAndMakeVisible (followButton);
        syncFollowButton();

        zoomHIn .onClick = [this] { pauseFollowForZoom(); setZoom (steppedTimelineZoom (zoom, true)); };
        zoomHOut.onClick = [this] { pauseFollowForZoom(); setZoom (steppedTimelineZoom (zoom, false)); };

        // Pro Tools-style Min:Secs time ruler perched above the track
        // list. Reads session length + sample rate from the engine via
        // its own 4 Hz timer.
        ruler = std::make_unique<EditTimeRuler> (engine);
        addAndMakeVisible (*ruler);
        recordingEditLabel.setFont (brand::type::ui (11.0f, true));
        recordingEditLabel.setJustificationType (juce::Justification::centred);
        recordingEditLabel.setColour (juce::Label::textColourId, brand::accentRecord);
        recordingEditLabel.setColour (juce::Label::backgroundColourId, brand::bgDeep);
        recordingEditLabel.setTooltip ("Timeline and marker edits are locked while recording; zoom, scroll and FOLLOW remain available");
        recordingEditLabel.setInterceptsMouseClicks (false, false);
        addChildComponent (recordingEditLabel);

        // Reusable loading/empty/error surface, overlaid on the wave area.
        // It manages its own visibility -- shown when no session is loaded,
        // cleared once waveforms are present (see refresh()).
        addChildComponent (placeholder);

        refresh();
        startTimerHz (24);
    }

    EditPage::~EditPage()
    {
        stopTimer();
        cancelWaveCacheWork();
        waveCacheThread.dropAndWait();
        // ~TimeSliceThread would kill a scan after two seconds. A scan on a
        // slow drive can easily run that long, and it holds locks.
        for (auto& cache : thumbnailCaches)
            cache->getTimeSliceThread().stopThread (-1);
        // Flush the waveform cache to WaveCache.wfm in whichever session is
        // active. Best-effort: failure here only means the next launch
        // re-scans waveforms, no data loss -- so SKIP it when the timer
        // already wrote this session's cache (the common case), rather than
        // paying a cache-sized synchronous write on every quit.
        const auto sessionDir = engine.getActiveSessionDir();
        if (! waveCacheSaved && ! engine.isTakeOnDisk() && sessionDir.isDirectory())
            wavecache::save (sessionDir, thumbnailCaches, [] { return false; });
    }

    void EditPage::cancelWaveCacheWork()
    {
        ++waveCacheWork;
        waveCacheLoading = false;
    }

    void EditPage::beginWaveCacheLoad (const juce::File& sessionDir)
    {
        cancelWaveCacheWork();
        const auto job = ++waveCacheWork;
        waveCacheLoading = true;
        waveCacheThread.add ([this, sessionDir, job]
        {
            wavecache::load (sessionDir, thumbnailCaches, [this, job] { return waveCacheWork.load() != job; });
            if (waveCacheWork.load() == job) waveCacheLoaded.store (job);
        });
    }

    void EditPage::beginWaveCacheSave (const juce::File& sessionDir)
    {
        cancelWaveCacheWork();
        const auto job = ++waveCacheWork;
        waveCacheSaving = true;
        waveCacheThread.add ([this, sessionDir, job]
        {
            wavecache::save (sessionDir, thumbnailCaches, [this, job] { return waveCacheWork.load() != job; });
            waveCacheSaving = false;
        });
    }

    int EditPage::selectedClipIndexOn (const SelectedClip& s) const
    {
        const auto* list = engine.tryClipsFor (s.track);
        if (list == nullptr || s.id == 0) return -1;
        int found = -1;
        for (int i = 0; i < (int) list->size(); ++i)
            if ((*list)[(size_t) i].selectionId == s.id)
            {
                if (found >= 0) return -1;   // split since: neither piece is "the" clip
                found = i;
            }
        return found;
    }

    void EditPage::followSelectedClips()
    {
        if (selection.empty() && selClips.empty()) return;

        std::set<ClipRef> found;
        std::vector<SelectedClip> kept;
        for (const auto& s : selection)
            if (const int i = selectedClipIndexOn (s); i >= 0)
            {
                found.insert ({ s.track, i });
                kept.push_back (s);
            }
        selection = std::move (kept);

        // The primary hands on when it leaves the selection.
        const bool primaryKept = std::any_of (selection.begin(), selection.end(), [this] (const SelectedClip& s)
                                              { return s.id == primarySelection.id; });
        if (! primaryKept)
            primarySelection = selection.empty() ? SelectedClip {} : selection.front();
        selectedClipIndex = selectedClipIndexOn (primarySelection);
        selectedClipTrack = selectedClipIndex >= 0 ? primarySelection.track : -1;

        if (found != selClips)
        {
            selClips = std::move (found);
            repaintLanes();
        }
    }

    // A fresh id on every selection: a clip that shares an older id with the
    // other piece of a split still gets one of its own.
    juce::uint32 EditPage::stampSelectionId (ClipRef ref)
    {
        const auto* list = engine.tryClipsFor (ref.first);
        if (list == nullptr || ref.second < 0 || ref.second >= (int) list->size()) return 0;
        return engine.clipsFor (ref.first)[(size_t) ref.second].selectionId = ++lastSelectionId;
    }

    void EditPage::setSelectedClips (const std::vector<ClipRef>& refs, ClipRef primary)
    {
        selection.clear();
        primarySelection = {};
        for (const auto& ref : refs)
            if (const auto id = stampSelectionId (ref); id != 0)
            {
                selection.push_back ({ ref.first, id });
                if (ref == primary) primarySelection = selection.back();
            }
        if (primarySelection.id == 0)
            if (const auto id = stampSelectionId (primary); id != 0)
            {
                primarySelection = { primary.first, id };
                selection.push_back (primarySelection);
            }
        followSelectedClips();
        repaintLanes();
    }

    void EditPage::toggleSelectedClips (const std::vector<ClipRef>& refs, ClipRef clicked)
    {
        followSelectedClips();
        if (selClips.count (clicked) == 0)
        {
            for (const auto& ref : refs)
                if (selClips.count (ref) == 0)
                    if (const auto id = stampSelectionId (ref); id != 0)
                    {
                        selection.push_back ({ ref.first, id });
                        if (ref == clicked) primarySelection = selection.back();   // the new primary
                    }
        }
        else
        {
            const auto isRemoved = [&] (const SelectedClip& s)
            {
                const ClipRef at { s.track, selectedClipIndexOn (s) };
                return std::find (refs.begin(), refs.end(), at) != refs.end();
            };
            selection.erase (std::remove_if (selection.begin(), selection.end(), isRemoved),
                             selection.end());
        }
        followSelectedClips();
        repaintLanes();
    }

    void EditPage::clearSelectedClip()
    {
        if (selection.empty() && selClips.empty()) return;
        selection.clear();
        primarySelection = {};
        selClips.clear();
        selectedClipTrack = selectedClipIndex = -1;
        repaintLanes();
    }

    void EditPage::refresh()
    {
        const int n = engine.getRecorder().getNumTracks();

        // Rebuild whenever the rows no longer match the logical (stereo-aware)
        // layout. Counts are not enough: a stereo link/unlink changes the row
        // count, but a strip moved past a stereo pair changes WHERE the pair
        // sits while keeping both counts.
        const auto layout = trackorder::logicalBlocks (trackorder::stereoFlags (engine.getRecorder()));
        if (rowsCondemned || n != lastTrackCount || ! list->matchesLayout (layout))
        {
            rowsCondemned = false;
            list->rebuild (n);
            lastTrackCount = n;
            resized();
        }

        // Use the engine-wide 'active' session (recorder takes priority
        // over player) so waveforms render the file being WRITTEN, not
        // just the file being read back.
        const auto sessionDir = engine.getActiveSessionDir();

        // A clip selection is (track, clip index). Once strips move, are
        // deleted or relinked (the generation bumps) or another session
        // opens, it names whichever clip now sits there -- and Delete /
        // Nudge / Duplicate would act on that clip.
        const int gen = engine.getRecorder().getTrackGeneration();
        if (gen != selectionTrackGen || sessionDir != selectionSessionDir)
        {
            clearSelectedClip();
            setActiveRowTrackIndex (-1);   // a position, like the selection
            selectionTrackGen   = gen;
            selectionSessionDir = sessionDir;
        }

        // When the session changes, pull the on-disk WaveCache.wfm
        // into the thumbnail cache BEFORE the new TrackRows ask
        // their thumbnails for sources -- otherwise the thumbnails
        // re-scan the WAV files even though cached peaks exist.
        // Gate on our OWN marker, not lastSessionDir. timerCallback assigns
        // lastSessionDir before calling us, so this test was already false by
        // the time we ran -- and reopening a session with the same strip count
        // as the current one (the only path that reaches refresh() through
        // that branch) silently skipped the cache and re-scanned every WAV.
        // A dedicated marker makes the load immune to the caller's ordering
        // and also stops the double-load when both branches fire on one tick.
        if (engine.isTakeOnDisk())
        {
            if (sessionDir != waveCacheLoadedFor)
            {
                waveCacheLoadedFor = sessionDir;
                cancelWaveCacheWork();
            }
        }
        else if (sessionDir.isDirectory() && sessionDir != waveCacheLoadedFor)
        {
            waveCacheLoadedFor = sessionDir;
            if (wavecache::fileIn (sessionDir).existsAsFile())
                beginWaveCacheLoad (sessionDir);
            else
                cancelWaveCacheWork();
        }

        // While recording, only the ARMED rows defer to the live red capture
        // envelope (their growing WAVs would scan as blocky garbage and the
        // disk read would contend with the recorder's own writes). Every OTHER
        // row keeps showing its already-recorded waveform -- arming one track
        // and rolling no longer blanks the tracks you captured earlier. The
        // armed rows' real waveforms scan in once, cleanly, on stop.
        if (engine.isCaptureFinalizing())
        {
            // An async STOP is still draining, closing and (for a punch)
            // splicing the take files. Leave every row on what it showed during
            // the take; timerCallback refreshes and rescans once it settles.
        }
        else if (engine.isRecording())
        {
            // A normal take is being WRITTEN to its file -> skip the armed rows
            // (scanning a growing file paints garbage; the live envelope owns
            // them). A CONTINUE writes a separate new part, so the armed track's
            // existing file is stable -- keep showing its waveform.
            const bool isContinue = engine.getRecorder().getRecordBaseSamples() > 0;
            list->holdWaveformsForTake (sessionDir, /*skipArmedRows*/ ! isContinue);
        }
        else
            // Rows blank until the saved peaks are in: the previous session's
            // waveforms must not sit under this session's names.
            list->setWaveformsFromSession (waveCacheLoading ? juce::File() : sessionDir);

        updatePlaceholder();
        lastLoaded = engine.getPlayer().isLoaded();
    }

    void EditPage::repaintLanes()
    {
        // Explicit per-row repaint -- the selection highlight is
        // drawn inside each row's paint(), and repainting only the parent list
        // doesn't reliably redraw the (header-pinned) children. Used after a
        // selection change (Cmd+A / clear).
        if (list != nullptr) list->repaintRows();
        repaint();
    }

    juce::var EditPage::captureRowViews() const { return list->rowViewsJson(); }
    void      EditPage::restoreRowViews (const juce::var& saved) { list->restoreRowViews (saved); }

    void EditPage::condemnAllRows()
    {
        if (list != nullptr) list->condemnAllRows();
        rowsCondemned = true;
    }

    int EditPage::updateReorderPreview (juce::Point<int> pointInPage)
    {
        if (list == nullptr || readOnlyWhileRecording) return -1;
        // Scroll the row list when the pointer nears its top/bottom edge, so a
        // strip can travel the whole session in one drag. The row drives this
        // with beginDragAutoRepeat, so it keeps scrolling while held still.
        const auto inViewport = viewport.getLocalPoint (this, pointInPage);
        viewport.autoScroll (inViewport.x, inViewport.y, 24, 16);
        return list->updateReorderPreview (list->getLocalPoint (this, pointInPage),
                                           engine.getRecorder().getNumTracks());
    }

    void EditPage::clearReorderPreview()
    {
        if (list != nullptr) list->setReorderLineY (-1);
    }

    bool EditPage::commitReorder (int fromPhysical, int insertBeforePhysical)
    {
        clearReorderPreview();
        auto report = [this] (const juce::String& message) { if (onStatus) onStatus (message); };
        // reorderTracks refuses during capture and its finalization; say so
        // rather than report its failure as a disk-permission problem.
        if (readOnlyWhileRecording || engine.isRecording() || engine.isSessionTransitionActive()
            || (canMutateTimeline && ! canMutateTimeline()))
        {
            report ("Strip move refused: wait for the take to finish recording and saving");
            return false;
        }
        auto& recorder = engine.getRecorder();
        const auto blocks = trackorder::logicalBlocks (trackorder::stereoFlags (recorder));
        const int n = recorder.getNumTracks();
        const int from = trackorder::blockStartingAt (blocks, fromPhysical);
        const int before = insertBeforePhysical >= n ? (int) blocks.size()
                                                     : trackorder::blockStartingAt (blocks, insertBeforePhysical);
        // The layout changed under the drag (a delete or stereo link from
        // another surface): refuse rather than move the wrong strip.
        if (from < 0 || before < 0)
        {
            report ("Strip move cancelled: the track layout changed during the drag");
            return false;
        }
        const auto order = trackorder::moveBlock (blocks, from, before);
        if (order.empty()) return true;   // dropped where it started

        // reorderTracks stops the player and reloads it at 0; keep the playhead.
        const auto playhead = engine.getPlayer().getPositionSamples();
        if (! engine.reorderTracks (order))
        {
            report ("Strip move failed; check disk permissions and Session File Backups before retrying");
            return false;
        }
        engine.getPlayer().setPositionSamples (playhead);
        // Row rebuild + waveform rescan follow from the host's
        // onTracksReordered handler, shared with every other reorder path.
        report ("Moved strip");
        return true;
    }

    void EditPage::rescanWaveforms()
    {
        // A strip reorder renames Track_NN.wav on disk but each row's file PATH
        // is unchanged, so the path-keyed thumbnail cache would keep the old
        // peaks (row shows the swapped name/colour over the previous track's
        // waveform). forceRefreshWaveforms drops each row's cached hash and
        // re-scans, so the lanes pick up the swapped content.
        if (list != nullptr) list->forceRefreshWaveforms();

        // The saved peaks are keyed by path too, and a rename keeps each file's
        // modification time, so nothing else would notice them go stale. On the
        // cache's own thread, so it lands after any save already in flight.
        cancelWaveCacheWork();
        waveCacheSaved = false;
        if (const auto sessionDir = engine.getActiveSessionDir(); sessionDir.isDirectory())
            waveCacheThread.add ([sessionDir] { wavecache::fileIn (sessionDir).deleteFile(); });
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
        followSelectedClips();

        if (waveCacheLoading && waveCacheLoaded.load() == waveCacheWork.load())
        {
            waveCacheLoading = false;
            refresh();
        }

        // Session-change detection + waveform attach run REGARDLESS of EDIT
        // visibility, so the AudioThumbnail background scan (and WaveCache.wfm
        // build) kick off the moment audio is imported / recorded -- not when
        // the engineer first switches to EDIT. Rows built while hidden are
        // just invisible components; the actual file read happens on the
        // thumbnail cache's background thread. Only the per-tick visual work
        // further down is gated on visibility.
        const int n   = engine.getRecorder().getNumTracks();
        const int gen = engine.getRecorder().getTrackGeneration();
        // Rebuild on a track-count change OR a generation bump. A stereo
        // link/unlink (e.g. from the PATCH page) changes no count but bumps the
        // generation, and EDIT must re-collapse the pair like MIXER + PATCH do.
        // A condemned row never comes back, and nothing else here notices: a
        // session of the same size changes neither number, and a delete the
        // engine refused changes nothing at all.
        if (n != lastTrackCount || gen != lastTrackGen || rowsCondemned)
        {
            lastTrackGen = gen;
            refresh();
        }

        // Pick up session swaps -- recording starts, recording stops,
        // session loaded, session changed, ...
        const bool loaded = engine.getPlayer().isLoaded();
        const bool rec    = engine.isRecording();
        // A local STOP clears `recording` at once, but the take files are
        // drained, closed and (for a punch) spliced on a worker afterwards.
        const bool finalizing = engine.isCaptureFinalizing();
        const auto& player = engine.getPlayer();
        const double liveRate = engine.getDeviceSampleRate() > 0.0
            ? engine.getDeviceSampleRate()
            : (player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0);
        if (rec && ! lastRecording)
        {
            recordingBaseSpanSamples = recordingTimelineSpanSamples (
                0, player.getTotalLengthSamples(), liveRate);
            capturedRows = list != nullptr ? list->armedRows() : std::set<int> {};   // arms freeze with the take
            waveCacheSaved = false; // this take will change the cached media
            cancelWaveCacheWork();  // and nothing writes to the take volume now
            // A deep post-STOP zoom would be multiplied by the growing live
            // timeline width. Return to the capture-safe range for this pass.
            if (zoom > kMaxRecordingTimelineZoom)
            {
                zoom = kMaxRecordingTimelineZoom;
                resized();
                if (onZoomChanged) onZoomChanged (zoom);
            }
        }
        // OR, never overwrite: the host also locks while an async STOP
        // finalizes and metadata saves are pending, after `recording` has
        // cleared. Writing `rec` alone here at 24 Hz fought the host's 10 Hz
        // write, so the lock flickered and mouse edits got through.
        applyReadOnly (hostReadOnly || rec);
        // The lock is wider than a take (a metadata save holds it too); this
        // red label says a take is rolling, so it follows the take alone.
        recordingEditLabel.setVisible (rec || finalizing);
        const bool transportActive = rec || engine.getPlayer().isPlaying();
        const bool wasFollowing = followState.isFollowing();
        const bool wasTransportActive = followState.isTransportActive();
        followState.transportChanged (transportActive);
        if (wasFollowing != followState.isFollowing()
            || wasTransportActive != followState.isTransportActive())
            syncFollowButton();
        const bool recJustStopped = (! rec && lastRecording);
        // The take files are final: recording over AND finalization done. A
        // synchronous stop settles on the same tick `recording` clears.
        const bool captureJustSettled = ! rec && ! finalizing && (lastRecording || lastFinalizing);
        lastFinalizing = finalizing;

        // Live capture envelope: arming is now SELF-DRIVEN per row inside
        // pushRecLevels (below), every tick, from the live recording+armed
        // state -- not a one-shot "record just started" event that could be
        // missed or clobbered by a refresh between ticks. Crucially it arms
        // ONLY the ARMED rows: a blanket arm here put every row into live mode,
        // so non-armed rows (which never capture) drew an empty envelope and
        // their existing waveform vanished while another track recorded. The
        // armed lane seeds its envelope with the existing take's length on a
        // CONTINUE so the new audio draws AFTER it (see pushRecLevels).
        // On stop, HOLD the live envelope (don't clear it) until the real disk
        // thumbnail scans in -- the lane shows the just-captured waveform
        // instantly instead of blanking for the duration of the scan.
        if (recJustStopped && list != nullptr) list->holdLiveEnvelopesUntilScanned();
        if (loaded != lastLoaded || rec != lastRecording || captureJustSettled
            || engine.getActiveSessionDir() != lastSessionDir)
        {
            if (engine.getActiveSessionDir() != lastSessionDir)
                waveCacheSaved = false;   // new session -> its cache needs (re)writing
            lastSessionDir = engine.getActiveSessionDir();
            lastRecording  = rec;
            refresh();
        }
        if (recJustStopped)
        {
            recordingBaseSpanSamples = 0;
            resized(); // restore fit-to-take width after live scrolling
        }

        // Drop pre-recording peaks before testing whether the scan is complete.
        // Saving first could mark an old cache as final for this session.
        // Wait for the capture to SETTLE: rescanning at the STOP edge read
        // files an async finalization still had open (the last ~5 s missing,
        // a punch not yet spliced) and nothing rescanned once it completed.
        if (captureJustSettled && list != nullptr)
            list->refreshWaveformsOf (capturedRows);

        // Persist WaveCache.wfm as soon as the background scan finishes for the
        // whole session -- even while EDIT is hidden -- so reopening the show
        // (or relaunching) paints waveforms instantly instead of re-scanning
        // every WAV. Without this the cache was only flushed on app quit, so a
        // freshly-imported session that wasn't quit cleanly re-scanned on the
        // next open. Done once per session; skipped while recording (files
        // still growing).
        if (waveCacheSaving.load() && engine.isSessionTransitionActive())
        {
            cancelWaveCacheWork();   // a session job owns these files now
            waveCacheSaved = false;
        }
        if (! waveCacheSaved && ! waveCacheSaving.load() && ! waveCacheLoading
            && ! rec && ! engine.isSessionTransitionActive()
            && list != nullptr && list->rowCount() > 0
            && engine.hasSessionFolder()
            && list->allWaveformsLoaded())
        {
            beginWaveCacheSave (engine.getActiveSessionDir());
            waveCacheSaved = true;
        }

        // Waveforms are NOT re-scanned from disk while recording: 48 channels
        // re-read at 24 Hz would contend with the recorder's own capture
        // writes and risk dropouts (capture integrity wins). The meters show
        // live signal during the take. The post-stop re-scan above drops any
        // partial cached peaks before the new cache can be saved.

        // Everything below is purely visual (playhead + repaints) and only
        // matters when EDIT is on screen. While recording we keep going even
        // when hidden so peaks stay current for the flip back to EDIT.
        if (! isVisible() && ! rec)
            return;

        // While a background waveform scan is running -- notably the post-stop
        // re-scan -- repaint the rows each tick so the freshly-recorded take
        // draws in AS it loads. Without this nothing triggers a repaint once
        // recording stops (the playhead is parked, pushRecLevels isn't running),
        // so the lane sat on the held envelope until the engineer moved the
        // mouse. Bounded: stops one tick after every thumbnail is fully loaded.
        const bool scanning = ! rec && list != nullptr && list->rowCount() > 0
                           && engine.hasSessionFolder() && ! list->allWaveformsLoaded();
        if (scanRepaint.tick (scanning) && list != nullptr)
            list->repaintRows();

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
        // Timeline position: on a CONTINUE this is the take end + samples so far,
        // so the playhead carries on past the existing take instead of jumping
        // back to 0.
        const auto  recPos  = rec
            ? (engine.isCaptureWindowActive() && player.isPlaying()
                   ? player.getPositionSamples()
                   : engine.getRecordTimelineSamples()) : 0;
        const auto  total   = rec ? recordingTimelineSpanSamples (
                                      recPos, player.getTotalLengthSamples(), liveRate)
                                  : player.getTotalLengthSamples();
        const auto  pos     = rec ? recPos : player.getPositionSamples();
        int playheadX = -1;
        if (list == nullptr) return;   // every other use in this function guards; these two didn't
        if (rec && recordingBaseSpanSamples > 0)
        {
            const int desiredW = recordingContentWidth (viewport.getWidth(), zoom,
                                                        total, recordingBaseSpanSamples);
            if (desiredW > list->getWidth() + 8)
                resized();
        }
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
        if (followState.isFollowing() && transportActive && total > 0 && list != nullptr)
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
                    {
                        juce::ScopedValueSetter<bool> scrolling (programmaticViewportChange, true);
                        viewport.setViewPosition (newX, viewport.getViewPositionY());
                    }
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
        juce::ScopedValueSetter<bool> scrolling (programmaticViewportChange, true);
        auto bounds = getLocalBounds();

        // Time ruler perches across the top, 46 px tall:
        //   20 px marker strip + 26 px Min:Secs scale.
        // Spans the full width so the header label column aligns with
        // each TrackRow's header column.
        const int rulerH = 46;
        if (ruler != nullptr)
            ruler->setBounds (bounds.removeFromTop (rulerH));
        recordingEditLabel.setBounds (juce::jmax (0, getWidth() - 177), 5, 170, 20);

        // Overview navigator strip along the bottom -- only when zoomed in
        // (content wider than the view), leaving room for the H/V zoom
        // clusters at the bottom-right.
        const bool rec = engine.isRecording();
        const auto& player = engine.getPlayer();
        const double liveRate = engine.getDeviceSampleRate() > 0.0
            ? engine.getDeviceSampleRate()
            : (player.getSampleRate() > 0.0 ? player.getSampleRate() : 48000.0);
        const auto liveSpan = recordingTimelineSpanSamples (
            engine.getRecordTimelineSamples(),
            player.getTotalLengthSamples(), liveRate);
        const auto baseSpan = recordingBaseSpanSamples > 0
            ? recordingBaseSpanSamples
            : recordingTimelineSpanSamples (0, player.getTotalLengthSamples(), liveRate);
        const bool showMinimap = zoom > 1.001f || (rec && liveSpan > baseSpan);
        if (showMinimap)
        {
            auto mmRow = bounds.removeFromBottom (18);
            mmRow.removeFromRight (185);  // clear FOLLOW + wider zoom controls
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
        const int contentW = rec
            ? recordingContentWidth (viewport.getWidth(), zoom, liveSpan, baseSpan)
            : juce::jmax (viewport.getWidth(),
                          (int) ((float) viewport.getWidth() * zoom));
        list->setSize (contentW, list->getHeight());
        list->resized();

        // Push the same content width into the ruler so its
        // pixels-per-second matches the wave pane below it, and the current
        // horizontal scroll so its ticks line up with the (possibly scrolled)
        // lanes after a relayout / zoom.
        if (ruler != nullptr)
        {
            ruler->setContentWidth (contentW);
            ruler->setScrollOffsetX (viewport.getViewPositionX());
        }

        // Edge zoom clusters, overlaid in the bottom-right corner:
        //   V+        (vertical / amplitude, stacked)
        //   V-
        //   H- H+     (horizontal / timeline, side by side)
        // The default JUCE button font needs this much room for "V+"/"H+";
        // narrower buttons draw those labels as "..." on macOS.
        const int zoomW = 44, zoomH = 26, followW = 72, gap = 4, pad = 8;
        const int rightX = bounds.getRight()  - zoomW - pad;
        const int botY   = bounds.getBottom() - zoomH - pad;
        zoomVIn .setBounds (rightX,                 botY - 2 * (zoomH + gap), zoomW, zoomH);
        zoomVOut.setBounds (rightX,                 botY -     (zoomH + gap), zoomW, zoomH);
        zoomHOut.setBounds (rightX - zoomW - gap,  botY,                      zoomW, zoomH);
        zoomHIn .setBounds (rightX,                 botY,                      zoomW, zoomH);
        followButton.setBounds (rightX - zoomW - 2 * gap - followW,
                                botY, followW, zoomH);
        for (auto* b : { &zoomVIn, &zoomVOut, &zoomHIn, &zoomHOut })
            b->toFront (false);
        followButton.toFront (false);
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
        // Centre the sample in the VISIBLE lane, right of the pinned header,
        // using the lane's own mapping (header + 4 px insets each side).
        constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
        const double prop = (double) sample / (double) totalSamples;
        const int    targetX = kHeaderW + 4 + (int) (prop * (double) juce::jmax (1, contentW - kHeaderW - 8));
        const int    viewW   = viewport.getViewWidth();
        const int    centreX = kHeaderW + juce::jmax (0, viewW - kHeaderW) / 2;
        // A marker jump or desk locate is not the engineer browsing: FOLLOW stays.
        juce::ScopedValueSetter<bool> scrolling (programmaticViewportChange, true);
        viewport.setViewPosition (juce::jmax (0, targetX - centreX),
                                  viewport.getViewPositionY());
    }

    void EditPage::zoomToSamples (juce::int64 a, juce::int64 b)
    {
        if (list == nullptr) return;
        const auto& player = engine.getPlayer();
        const auto total = player.isLoaded() ? player.getTotalLengthSamples() : 0;
        if (total <= 0 || b <= a) return;
        pauseFollowForZoom();
        // Zoom so the selection ~fills the VISIBLE lane (a little headroom),
        // then centre on it. The pinned header covers the first editHeaderW
        // px of the view and the lane is inset 4 px each side of the content
        // width, so solve contentW = viewport width * zoom for that. setZoom
        // applies the finalized/live limits.
        constexpr int kHeaderW = brand::space::editHeaderW;   // == TrackRow::headerW (same token)
        const double frac = (double) (b - a) / (double) total;
        const double visibleLane = (double) juce::jmax (1, viewport.getViewWidth() - kHeaderW);
        if (frac > 0.0)
            setZoom ((float) juce::jlimit (1.0, (double) kMaxTimelineZoom,
                                           (0.9 * visibleLane / frac + kHeaderW + 8)
                                               / (double) juce::jmax (1, viewport.getWidth())));
        scrollToSample ((a + b) / 2);
    }

    void EditPage::setZoom (float z)
    {
        z = juce::jlimit (1.0f,
                          engine.isRecording() ? kMaxRecordingTimelineZoom
                                               : kMaxTimelineZoom,
                          z);
        // A deadband here can strand the view just above 1x, leaving the
        // minimap visible even after another H- click. Exact equality only
        // rejects a clamped no-op at the zoom limits.
        if (z == zoom) return;
        zoom = z;
        resized();
        if (onZoomChanged) onZoomChanged (zoom);
    }

    void EditPage::userSetZoom (float z)
    {
        pauseFollowForZoom();
        setZoom (z);
    }

    void EditPage::setVerticalZoom (float z)
    {
        z = juce::jlimit (0.25f, 32.0f, z);
        if (std::abs (z - vZoom) < 0.001f) return;
        vZoom = z;
        if (list != nullptr) list->repaint();
    }

    void EditPage::setReadOnlyWhileRecording (bool readOnly)
    {
        hostReadOnly = readOnly;
        applyReadOnly (readOnly || engine.isRecording());
    }

    void EditPage::applyReadOnly (bool readOnly)
    {
        if (readOnlyWhileRecording == readOnly) return;
        readOnlyWhileRecording = readOnly;
        // Passing mouse events through the rows keeps the viewport's pan and
        // scroll gestures available. The ruler's marker/range edits stay inert.
        // No component is disabled, so waveform paint and zoom chrome retain
        // their normal appearance and hit targets.
        if (list != nullptr) list->setInterceptsMouseClicks (! readOnly, ! readOnly);
        if (ruler != nullptr) ruler->setInterceptsMouseClicks (! readOnly, ! readOnly);
    }

    void EditPage::wheelZoomHorizontal (float delta)
    {
        if (list == nullptr) return;
        pauseFollowForZoom();
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

    void EditPage::pauseFollowForZoom()
    {
        const bool wasFollowing = followState.isFollowing();
        followState.userZoom();
        if (wasFollowing != followState.isFollowing()) syncFollowButton();
    }

    void EditPage::syncFollowButton()
    {
        followButton.setToggleState (followState.isFollowing(), juce::dontSendNotification);
        followButton.setVisible (followState.isTransportActive());
        followButton.setTooltip (followState.isFollowing()
            ? "Following the live playhead; scroll left/right or zoom in to browse"
            : "Browsing the timeline; click to return to the live playhead");
    }
}
