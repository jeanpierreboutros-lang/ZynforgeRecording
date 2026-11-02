// Cue + setlist methods on MainComponent. Extracted from
// MainComponent.cpp as part of the 2026-05-24 god-class split.
//
// Includes: loadSetlistFromActiveSession / saveSetlistToActiveSession
// (.zfproj cue round-trip), jumpToCue / promptCueName /
// addCueAtTransport / renameCurrentCue / updateCueAtTransport (mutate
// the cue list), and printSetlist (browser-printable HTML).

#include "MainComponent.h"
#include "../Audio/AtomicFile.h"
#include "../Theme/DialogChrome.h"
#include "SessionProjPath.h"

using namespace zynforge;

// File-local helper -- moved with the cue methods that exclusively
// use it.
static zynforge::SetlistBar::StripSnapshot snapshotStrip (zynforge::TrackState& t)
{
    zynforge::SetlistBar::StripSnapshot s;
    s.stripId      = t.stripId;
    s.gainDb       = t.gainDb       .load (std::memory_order_relaxed);
    s.pan          = t.pan          .load (std::memory_order_relaxed);
    s.inputRouting = t.inputRouting .load (std::memory_order_relaxed);
    s.outputRouting= t.outputRouting.load (std::memory_order_relaxed);
    s.muted        = t.muted        .load (std::memory_order_relaxed);
    s.soloed       = t.soloed       .load (std::memory_order_relaxed);
    s.monitor      = t.monitor      .load (std::memory_order_relaxed);
    s.armed        = t.armed        .load (std::memory_order_relaxed);
    return s;
}

void MainComponent::loadSetlistFromActiveSession()
{
    cues.clear();
    currentCueIndex = -1;

    // Older sessions saved cue UUIDs but not the corresponding strip UUIDs.
    // Rebind those legacy snapshots by their saved physical order once; newly
    // saved sessions then use portable identities normally. Never apply this
    // fallback to a strip that already has an authoritative session UUID.
    std::vector<bool> portableIds;
    const auto mix = juce::JSON::parse (engine.getActiveSessionDir().getChildFile ("session_mix.json"));
    if (auto* strips = mix["strips"].getArray())
        for (const auto& strip : *strips)
        {
            const int index = (int) strip["index"];
            if (index < 0 || index >= 256) continue;
            if (index >= (int) portableIds.size()) portableIds.resize ((size_t) index + 1, false);
            portableIds[(size_t) index] = strip["uid"].toString().isNotEmpty();
        }

    const auto proj = findSessionProj (engine.getActiveSessionDir());
    if (proj.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (proj);
        if (auto* obj = parsed.getDynamicObject())
        {
            const auto v = obj->getProperty ("setlist");
            if (auto* arr = v.getArray())
            {
                for (const auto& item : *arr)
                {
                    if (auto* c = item.getDynamicObject())
                    {
                        zynforge::SetlistBar::Cue cue;
                        cue.name      = c->getProperty ("name").toString();
                        cue.samplePos = (juce::int64) (double) c->getProperty ("samplePos");

                        // Deserialize per-strip snapshot if present
                        // (cues saved by older builds without snapshots
                        // just leave the strips vector empty -- recall
                        // then skips strip restore).
                        const auto sv = c->getProperty ("strips");
                        if (auto* sa = sv.getArray())
                        {
                            for (const auto& sitem : *sa)
                            {
                                if (auto* so = sitem.getDynamicObject())
                                {
                                    zynforge::SetlistBar::StripSnapshot s;
                                    s.stripId      = so->getProperty ("uid").toString();
                                    const int physical = (int) cue.strips.size();
                                    if (physical < engine.getRecorder().getNumTracks()
                                        && (physical >= (int) portableIds.size() || ! portableIds[(size_t) physical]))
                                        s.stripId = engine.getRecorder().getTrack (physical).stripId;
                                    s.gainDb       = (float) (double) so->getProperty ("gainDb");
                                    s.pan          = (float) (double) so->getProperty ("pan");
                                    s.inputRouting = (int)            so->getProperty ("in");
                                    s.outputRouting= (int)            so->getProperty ("out");
                                    s.muted        = (bool)           so->getProperty ("mute");
                                    s.soloed       = (bool)           so->getProperty ("solo");
                                    s.monitor      = (bool)           so->getProperty ("mon");
                                    s.armed        = (bool)           so->getProperty ("rec");
                                    cue.strips.push_back (s);
                                }
                            }
                        }
                        // Per-cue automation lanes (optional). Kept as the
                        // raw array; jumpToCue feeds it to loadAutomationFromJson.
                        const auto av = c->getProperty ("automation");
                        if (av.isArray()) cue.automation = av;

                        cues.push_back (std::move (cue));
                    }
                }
            }
        }
    }
    setlistBar.setCues (cues, currentCueIndex);

    // Restore playlists (Takes) from the .zfproj if present.
    // seedDefaultClips ran earlier and populated trackPlaylists with
    // Take 1; loadPlaylistsFromJson now overwrites that with whatever
    // the engineer had saved. Re-parse the .zfproj here -- the earlier
    // 'obj' was scoped to the setlist deserialise block above.
    {
        const auto proj = findSessionProj (engine.getActiveSessionDir());
        if (proj != juce::File{})
        {
            const auto parsed = juce::JSON::parse (proj);
            if (auto* root = parsed.getDynamicObject())
            {
                const auto pls = root->getProperty ("playlists");
                if (pls.isArray())
                {
                    engine.loadPlaylistsFromJson (pls);
                    engine.reconcileClipsWithMedia (root->getProperty ("mediaLengths"),
                                                    proj.getSiblingFile ("recording.session").existsAsFile());
                }
                // Automation lanes (formatVersion >= 3). Older saves
                // have no automation key, so the if-check is enough
                // to keep them loadable.
                const auto autom = root->getProperty ("automation");
                if (autom.isArray()) engine.loadAutomationFromJson (autom);
            }
        }
    }
}

juce::String MainComponent::captureProjectPatchJson() const
{
    juce::DynamicObject::Ptr obj (new juce::DynamicObject());
    juce::Array<juce::var> arr;
    for (const auto& c : cues)
    {
        juce::DynamicObject::Ptr entry (new juce::DynamicObject());
        entry->setProperty ("name",      c.name);
        entry->setProperty ("samplePos", (double) c.samplePos);

        juce::Array<juce::var> stripArr;
        for (const auto& s : c.strips)
        {
            juce::DynamicObject::Ptr st (new juce::DynamicObject());
            st->setProperty ("uid",    s.stripId);
            st->setProperty ("gainDb", (double) s.gainDb);
            st->setProperty ("pan",    (double) s.pan);
            st->setProperty ("in",     s.inputRouting);
            st->setProperty ("out",    s.outputRouting);
            st->setProperty ("mute",   s.muted);
            st->setProperty ("solo",   s.soloed);
            st->setProperty ("mon",    s.monitor);
            st->setProperty ("rec",    s.armed);
            stripArr.add (juce::var (st.get()));
        }
        entry->setProperty ("strips", juce::var (stripArr));

        // Per-cue automation snapshot (volume/pan/mute lanes). Stored as the
        // same array automationToJson() produces, so the recall path can feed
        // it straight back into loadAutomationFromJson.
        if (c.automation.isArray())
            entry->setProperty ("automation", c.automation);

        arr.add (juce::var (entry.get()));
    }
    obj->setProperty ("setlist",   juce::var (arr));
    // Persist playlists (Takes) -- RAM-only before this fix
    // meant every alternate take was lost on app quit.
    obj->setProperty ("playlists",  engine.playlistsToJson());
    obj->setProperty ("mediaLengths", engine.mediaLengthsToJson());
    // Persist per-track automation lanes (volume/pan/mute points +
    // curve types). Without this, every point the engineer drew was
    // lost on app quit. Same data-loss class as the Takes bug fix.
    obj->setProperty ("automation", engine.automationToJson());
    // .zfproj schema version: 2 introduced playlists + stable strip
    // IDs. v3 adds automation lanes. Older saves read transparently.
    obj->setProperty ("formatVersion", 3);
    obj->setProperty ("updatedAt",     juce::Time::getCurrentTime().toISO8601 (true));

    return juce::JSON::toString (juce::var (obj.get()));
}

bool MainComponent::saveSetlistToActiveSession (bool writeBackup) const
{
    if (pendingMetadataSaves != 0) return false;
    zynforge::SessionMetadataSnapshot snapshot;
    snapshot.directory = engine.getActiveSessionDir();
    snapshot.projectPatchJson = captureProjectPatchJson();
    snapshot.mixerOwner = engine.getMixerSession();
    snapshot.backup = writeBackup;
    return zynforge::writeSessionMetadata (snapshot).ok;
}

void MainComponent::jumpToCue (int index)
{
    if (index < 0 || index >= (int) cues.size()) return;
    currentCueIndex = index;
    setlistBar.setCues (cues, currentCueIndex);

    const auto& cue = cues[(size_t) index];

    auto& player = engine.getPlayer();
    if (! playheadLockedForPunch())   // the mixer recall below still runs
        player.setPositionSamples (cue.samplePos);

    // Reinstall this cue's automation lanes so the song plays back with its
    // own volume / pan / mute moves. An empty array clears the lanes; a void
    // var (older cues) leaves the current lanes untouched.
    int autoPts = 0;
    if (cue.automation.isArray())
    {
        // Clear EVERY lane first, then load the cue's snapshot. A cue only
        // stores automation for tracks that had any WHEN IT WAS CAPTURED, so
        // loadAutomationFromJson alone would leave a track that another cue
        // automated still showing that other cue's curve. Clearing first makes
        // recall fully authoritative -- the view + playback show exactly this
        // song's moves and nothing leaks across cues.
        engine.clearAutomation (AudioEngine::AutomationParam::Volume);
        engine.clearAutomation (AudioEngine::AutomationParam::Pan);
        engine.clearAutomation (AudioEngine::AutomationParam::Mute);
        engine.loadAutomationFromJson (cue.automation);
        if (auto* arr = cue.automation.getArray())
            for (const auto& trk : *arr)
                if (auto* o = trk.getDynamicObject())
                    for (const char* lane : { "volume", "pan", "mute" })
                        if (auto* la = o->getProperty (lane).getArray())
                            autoPts += la->size();
        if (editPage != nullptr) editPage->repaintLanes();   // redraw the curves now
    }

    // Recall the mixer state captured with this cue: gain / pan /
    // input + output routing / mute / solo / mon / arm -- for every
    // strip the cue knows about. Strips beyond the snapshot length
    // (added after the cue was saved) are left untouched.
    auto& rec = engine.getRecorder();
    const int total = rec.getNumTracks();
    // 250 ms soft-takeover -- short enough to feel snappy on a cue
    // jump, long enough to avoid an audible zipper / click when
    // gain or pan jumps by 6+ dB.
    constexpr double kCueRecallSeconds = 0.25;

    // Stable-ID lookup: pre-v2 cues (no stripId) fall back to array
    // index. v2+ cues match by UUID so reordering strips no longer
    // silently scrambles the cue.
    auto resolveTarget = [&] (const zynforge::SetlistBar::StripSnapshot& s,
                               int fallbackIdx) -> int
    {
        if (s.stripId.isNotEmpty())
        {
            for (int j = 0; j < total; ++j)
                if (rec.getTrack (j).stripId == s.stripId)
                    return j;
            return -1;   // strip was deleted since the cue was saved
        }
        return fallbackIdx < total ? fallbackIdx : -1;
    };

    // Arm state + input routing decide what is being CAPTURED. A cue recall
    // must never change them mid-record -- a cue saved with a track disarmed
    // (or pointed at a different input) would silently stop / re-source capture
    // on that track for the rest of the take. Mix state (gain/pan/mute/solo/
    // monitor/output) is always safe to move.
    const bool recording = engine.isRecording();
    const int n = (int) cue.strips.size();
    for (int i = 0; i < n; ++i)
    {
        const auto& s = cue.strips[(size_t) i];
        const int target = resolveTarget (s, i);
        if (target < 0) continue;

        engine.setTrackGainDbRamped (target, s.gainDb, kCueRecallSeconds);
        engine.setTrackPanRamped    (target, s.pan,    kCueRecallSeconds);
        engine.setTrackOutputRouting(target, s.outputRouting);
        if (! recording)
            engine.setTrackInputRouting (target, s.inputRouting);

        auto& t = rec.getTrack (target);
        t.muted  .store (s.muted,   std::memory_order_relaxed);
        t.soloed .store (s.soloed,  std::memory_order_relaxed);
        t.monitor.store (s.monitor, std::memory_order_relaxed);
        if (! recording)
            t.armed.store (s.armed, std::memory_order_relaxed);
    }
    lastTrackCount = -1;   // force strip refresh so combos + buttons redraw

    updateTransportLabels();
    showStatus ("Cue " + juce::String (index + 1) + " '" + cue.name + "' recalled -- "
                + juce::String (n) + " strips, " + juce::String (autoPts) + " automation pts"
                + (autoPts > 0 ? " (plays during transport)" : "")
                + (recording ? " (arm + input held -- recording)" : ""));
}

void MainComponent::promptCueName (const juce::String& title,
                                   const juce::String& initial,
                                   std::function<void (const juce::String&)> onAccept)
{
    auto* aw = new juce::AlertWindow (title,
                                      "Cue name:",
                                      juce::MessageBoxIconType::NoIcon);
    aw->setLookAndFeel (&laf);   // grey ZynForge chrome, not JUCE default
    aw->addTextEditor ("cueName", initial, juce::String{});
    dialog::primeNameEditor (*aw, "cueName");
    aw->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<MainComponent> self (this);
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create ([self, aw, accept = std::move (onAccept)] (int result)
        {
            std::unique_ptr<juce::AlertWindow> dispose (aw);
            if (result != 1 || self == nullptr) return;
            const auto name = aw->getTextEditorContents ("cueName").trim();
            if (accept) accept (name);
        }),
        false);
}

void MainComponent::addCueAtTransport()
{
    if (! engine.hasSessionFolder())
    {
        showStatus ("Open or create a session before adding cues");
        return;
    }
    auto& player = engine.getPlayer();
    const auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
    const auto suggested = "Song " + juce::String ((int) cues.size() + 1);

    promptCueName ("Add Cue", suggested, [this, pos, &player] (const juce::String& name)
    {
        zynforge::SetlistBar::Cue c;
        c.name      = name.isEmpty()
                         ? juce::String ("Song " + juce::String ((int) cues.size() + 1))
                         : name;
        c.samplePos = pos;

        // Snapshot the current mixer state so recalling this cue later
        // restores fader, pan, mute/solo/mon/arm, and routing for every
        // strip.
        auto& rec = engine.getRecorder();
        const int total = rec.getNumTracks();
        c.strips.reserve ((size_t) total);
        for (int i = 0; i < total; ++i)
            c.strips.push_back (snapshotStrip (rec.getTrack (i)));

        // Snapshot the automation lanes too, so this song recalls its own
        // volume / pan / mute moves -- not whatever was last edited globally.
        c.automation = engine.automationToJson();

        cues.push_back (std::move (c));
        currentCueIndex = (int) cues.size() - 1;
        setlistBar.setCues (cues, currentCueIndex);
        saveCueChanges ("Added cue '" + cues.back().name + "' at "
                    + juce::String ((double) pos / juce::jmax (1.0, player.getSampleRate()), 2)
                    + " s (mix snapshot captured)");
    });
}

void MainComponent::renameCurrentCue()
{
    if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size())
    {
        showStatus ("Pick a cue first, then right-click to rename");
        return;
    }
    const auto idx     = currentCueIndex;
    const auto current = cues[(size_t) idx].name;

    promptCueName ("Rename Cue", current, [this, idx] (const juce::String& name)
    {
        if (name.isEmpty()) return;
        if (idx < 0 || idx >= (int) cues.size()) return;
        cues[(size_t) idx].name = name;
        setlistBar.setCues (cues, currentCueIndex);
        saveCueChanges ("Renamed cue -> '" + name + "'");
    });
}

void MainComponent::updateCueAtTransport()
{
    if (currentCueIndex < 0 || currentCueIndex >= (int) cues.size())
    {
        showStatus ("Pick a cue first, then Update to move it");
        return;
    }
    auto& player = engine.getPlayer();
    const auto pos = player.isLoaded() ? player.getPositionSamples() : juce::int64 (0);
    auto& cue = cues[(size_t) currentCueIndex];
    cue.samplePos = pos;

    // Re-capture every strip's state. Update means 'whatever the mixer
    // looks like NOW is what this cue should recall to in the future'.
    auto& rec = engine.getRecorder();
    const int total = rec.getNumTracks();
    cue.strips.clear();
    cue.strips.reserve ((size_t) total);
    for (int i = 0; i < total; ++i)
        cue.strips.push_back (snapshotStrip (rec.getTrack (i)));

    // Re-capture the automation lanes too -- Update means 'this is the
    // mix AND automation this song should recall to'.
    cue.automation = engine.automationToJson();

    setlistBar.setCues (cues, currentCueIndex);
    saveCueChanges ("Cue '" + cue.name + "' updated -- "
                + juce::String ((double) pos / juce::jmax (1.0, player.getSampleRate()), 2) + " s, "
                + juce::String (total) + " strip mix snapshotted");
}

void MainComponent::printSetlist()
{
    if (engine.isRecording() || captureMetadataPending)
    { showStatus ("Printing the setlist waits until the take has stopped"); return; }
    if (cues.empty()) { showStatus ("No cues to print"); return; }

    const auto dir = engine.getActiveSessionDir();
    const auto target = dir.isDirectory()
        ? dir.getChildFile ("setlist.html")
        : juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
              .getChildFile ("zynforge_setlist.html");

    const auto sr = engine.getPlayer().getSampleRate();
    const auto sessNameRaw = dir.isDirectory() ? dir.getFileName() : juce::String ("Session");
    // Escape like the cue names below -- a folder named "Fest <Main> & Co"
    // otherwise broke/truncated the <title>/<h1>.
    const auto sessName = sessNameRaw.replace ("&", "&amp;")
                                     .replace ("<", "&lt;").replace (">", "&gt;");

    juce::String html;
    html << "<!doctype html><html><head><meta charset=\"utf-8\"><title>"
         << sessName << " setlist</title>"
         << "<style>"
         << "body{font:14pt -apple-system,Helvetica;background:#fff;color:#000;margin:24pt}"
         << "h1{font-size:22pt;margin:0 0 4pt 0}"
         << "h2{font-size:11pt;color:#888;font-weight:normal;margin:0 0 18pt 0}"
         << "table{border-collapse:collapse;width:100%}"
         << "th{text-align:left;padding:6pt 8pt;background:#eee;border-bottom:1pt solid #888;font-size:10pt}"
         << "td{padding:6pt 8pt;border-bottom:1pt solid #ccc}"
         << ".n{text-align:right;color:#555;font-variant-numeric:tabular-nums}"
         << ".cue{font-weight:600}"
         << "@media print{body{margin:12pt}}"
         << "</style></head><body>"
         << "<h1>" << sessName << "</h1>"
         << "<h2>Setlist - " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M")
         << " * " << (int) cues.size() << " cue" << (cues.size() == 1 ? "" : "s")
         << "</h2>"
         << "<table><thead><tr><th class=\"n\">#</th><th>Cue</th><th>Time</th><th class=\"n\">Strips</th></tr></thead><tbody>";

    for (size_t i = 0; i < cues.size(); ++i)
    {
        const auto& c = cues[i];
        juce::String timeStr;
        if (sr > 0.0)
        {
            const double secs = (double) c.samplePos / sr;
            const int mins = (int) secs / 60;
            const double sec = secs - mins * 60;
            timeStr = juce::String::formatted ("%02d:%06.3f", mins, sec);
        }
        html << "<tr><td class=\"n\">" << (int) (i + 1) << "</td>"
             << "<td class=\"cue\">" << c.name.replace ("&", "&amp;").replace ("<", "&lt;") << "</td>"
             << "<td>" << timeStr << "</td>"
             << "<td class=\"n\">" << (int) c.strips.size() << "</td></tr>";
    }
    html << "</tbody></table></body></html>";

    if (zynforge::atomicfile::writeText (target, html))
    {
        target.startAsProcess();    // open in default browser
        showStatus ("Setlist -> " + target.getFileName() + " (printable)");
    }
    else
        showStatus ("Setlist export failed -- previous file was preserved");
}
