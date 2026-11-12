#include "Markers.h"
#include "AtomicFile.h"
#include "SessionRepair.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <mutex>
#include <optional>
#include <thread>

namespace zynforge
{
    static juce::File markersFileFor (const juce::File& dir)
    {
        return dir.getChildFile ("markers.json");
    }

    static bool writeMarkers (const juce::File& dir, const juce::String& json, juce::File& kept)
    {
        if (! dir.isDirectory()) return false;
        kept = sessionrepair::keepIfUnreadable (markersFileFor (dir));
        return atomicfile::writeText (markersFileFor (dir), json);
    }

    // The newest list waiting to reach the disk. Only the newest matters, each
    // one is the whole list, and the thread exits once nothing is waiting. It
    // owns this and nothing of the manager, so a write that never returns
    // strands only that thread.
    struct MarkersManager::SaveQueue
    {
        struct Save { juce::File dir; juce::String json; };

        std::mutex lock;
        std::optional<Save> waiting;
        // The newest list handed to the queue, until a write of it lands. A
        // reload of that folder reads this, not a file the write has not
        // reached, and a failed write leaves it here for the next save.
        std::optional<Save> unlanded;
        bool writing { false };
        std::function<void()> beforeWriteForTests;
        MarkersManager* owner { nullptr };   // message thread only

        // Later saves must queue behind a write in progress, or a slow old list
        // could land on top of a newer one.
        bool isBusy()
        {
            const std::lock_guard<std::mutex> guard (lock);
            return writing;
        }

        std::optional<juce::String> unlandedFor (const juce::File& dir)
        {
            const std::lock_guard<std::mutex> guard (lock);
            if (unlanded.has_value() && unlanded->dir == dir) return unlanded->json;
            return std::nullopt;
        }

        // A newer list reached this folder some other way.
        void landed (const juce::File& dir)
        {
            const std::lock_guard<std::mutex> guard (lock);
            if (unlanded.has_value() && unlanded->dir == dir) unlanded.reset();
        }

        static void add (const std::shared_ptr<SaveQueue>& queue, Save save)
        {
            {
                const std::lock_guard<std::mutex> guard (queue->lock);
                queue->unlanded = save;
                queue->waiting = std::move (save);
                if (queue->writing) return;
                queue->writing = true;
            }
            std::thread ([queue]
            {
                juce::Thread::setCurrentThreadName ("Marker writer");
                for (;;)
                {
                    Save next;
                    std::function<void()> hook;
                    {
                        const std::lock_guard<std::mutex> guard (queue->lock);
                        if (! queue->waiting.has_value()) { queue->writing = false; return; }
                        next = std::move (*queue->waiting);
                        queue->waiting.reset();
                        hook = queue->beforeWriteForTests;
                    }
                    if (hook != nullptr) hook();
                    juce::File kept;
                    const bool written = writeMarkers (next.dir, next.json, kept);
                    {
                        const std::lock_guard<std::mutex> guard (queue->lock);
                        if (written && queue->unlanded.has_value() && queue->unlanded->dir == next.dir
                            && queue->unlanded->json == next.json)
                            queue->unlanded.reset();
                    }
                    if (kept != juce::File())
                        juce::MessageManager::callAsync ([queue, kept]
                        {
                            if (queue->owner != nullptr && queue->owner->onDamagedFileKept != nullptr)
                                queue->owner->onDamagedFileKept (kept);
                        });
                }
            }).detach();
        }
    };

    MarkersManager::MarkersManager() : saveQueue (std::make_shared<SaveQueue>())
    {
        saveQueue->owner = this;
    }

    MarkersManager::~MarkersManager()
    {
        saveQueue->owner = nullptr;
    }

    void MarkersManager::setBeforeDeferredWriteForTests (std::function<void()> hook)
    {
        const std::lock_guard<std::mutex> guard (saveQueue->lock);
        saveQueue->beforeWriteForTests = std::move (hook);
    }

    void MarkersManager::setContext (const juce::File& dir, double sr)
    {
        // The folder already here: the list in memory is the newest there is.
        // Every change went through save(), and a deferred write of it may not
        // have landed yet (a take's STOP and the next RECORD both land here).
        if (dir == sessionDir && dir != juce::File())
        {
            sampleRate = sr > 0 ? sr : sampleRate;
            return;
        }
        sessionDir = dir;
        sampleRate = sr > 0 ? sr : sampleRate;
        markers.clear();
        // Default names count within this session: the next "Marker N" follows
        // its own markers, not the last session's.
        markerCounter = 0;
        load();
    }

    void MarkersManager::clearContext()
    {
        sessionDir = juce::File();
        markers.clear();
        markerCounter = 0;
    }

    void MarkersManager::sortByPosition()
    {
        std::stable_sort (markers.begin(), markers.end(),
                          [] (const Marker& a, const Marker& b)
                          { return a.sampleOffset < b.sampleOffset; });
    }

    void MarkersManager::drop (juce::int64 sampleOffset, const juce::String& name,
                               const juce::String& type)
    {
        Marker m;
        m.sampleOffset = sampleOffset;
        m.type = type;

        if (name.isNotEmpty())
        {
            m.name = name;
        }
        else
        {
            // Monotonic counter, then bump past any collision (e.g. after a
            // removeMarker left an existing "Marker N") so auto-names stay unique.
            auto nameInUse = [this] (const juce::String& n)
            {
                for (const auto& existing : markers)
                    if (existing.name == n) return true;
                return false;
            };
            juce::String candidate;
            do { candidate = "Marker " + juce::String (++markerCounter); }
            while (nameInUse (candidate));
            m.name = candidate;
        }

        lastDroppedId = m.runtimeId;
        markers.push_back (std::move (m));
        sortByPosition();
        save();
    }

    void MarkersManager::clear()
    {
        markers.clear();
        save();
    }

    Marker MarkersManager::getLast() const
    {
        if (markers.empty()) return {};
        return markers.back();
    }

    Marker MarkersManager::getMarker (int i) const
    {
        if (i < 0 || i >= (int) markers.size()) return {};
        return markers[(std::size_t) i];
    }

    int MarkersManager::indexOfId (const juce::String& runtimeId) const
    {
        for (int i = 0; i < (int) markers.size(); ++i)
            if (markers[(std::size_t) i].runtimeId == runtimeId) return i;
        return -1;
    }

    void MarkersManager::removeMarker (int i)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers.erase (markers.begin() + i);
        save();
    }

    void MarkersManager::renameMarker (int i, const juce::String& newName)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers[(std::size_t) i].name = newName;
        save();
    }

    void MarkersManager::setMarkerSample (int i, juce::int64 sample)
    {
        if (i < 0 || i >= (int) markers.size()) return;
        markers[(std::size_t) i].sampleOffset = sample;
        sortByPosition();
        save();
    }

    void MarkersManager::replaceAll (std::vector<Marker> newMarkers)
    {
        markers = std::move (newMarkers);
        sortByPosition();
        save();
    }

    void MarkersManager::insert (Marker marker)
    {
        markers.push_back (std::move (marker));
        sortByPosition();
        save();
    }

    void MarkersManager::removeById (const juce::String& runtimeId)
    {
        markers.erase (std::remove_if (markers.begin(), markers.end(),
                                       [&runtimeId] (const Marker& m) { return m.runtimeId == runtimeId; }),
                       markers.end());
        save();
    }

    bool MarkersManager::save() const
    {
        const bool deferred = diskDeferred() || saveQueue->isBusy();
        if (! deferred && ! sessionDir.isDirectory()) return false;

        juce::DynamicObject::Ptr root (new juce::DynamicObject());
        root->setProperty ("sampleRate", sampleRate);

        juce::Array<juce::var> arr;
        for (const auto& m : markers)
        {
            juce::DynamicObject::Ptr o (new juce::DynamicObject());
            o->setProperty ("sample", (juce::int64) m.sampleOffset);
            o->setProperty ("name",   m.name);
            o->setProperty ("type",   m.type);
            if (m.zoom > 0.0f)
                o->setProperty ("zoom", (double) m.zoom);
            if (! m.visibleTracks.empty())
            {
                juce::Array<juce::var> vis;
                for (int v : m.visibleTracks) vis.add (v);
                o->setProperty ("visible", juce::var (vis));
            }
            arr.add (juce::var (o.get()));
        }
        root->setProperty ("markers", arr);

        const auto json = juce::JSON::toString (juce::var (root.get()), true);
        if (deferred)
        {
            SaveQueue::add (saveQueue, { sessionDir, json });
            return true;
        }
        juce::File kept;
        const bool written = writeMarkers (sessionDir, json, kept);
        if (written) saveQueue->landed (sessionDir);
        if (kept != juce::File() && onDamagedFileKept)
            onDamagedFileKept (kept);
        return written;
    }

    bool MarkersManager::load()
    {
        markers.clear();
        if (sessionDir == juce::File()) return false;
        juce::String text;
        if (const auto owed = saveQueue->unlandedFor (sessionDir))
        {
            text = *owed;   // newer than the file: its write has not landed
        }
        else
        {
            if (! sessionDir.isDirectory()) return false;
            const auto file = markersFileFor (sessionDir);
            if (! file.existsAsFile()) return false;
            text = file.loadFileAsString();
        }

        juce::var parsed;
        if (juce::JSON::parse (text, parsed).failed())
            return false;

        if (auto* obj = parsed.getDynamicObject())
        {
            if (auto sr = obj->getProperty ("sampleRate"); sr.isDouble() || sr.isInt())
                sampleRate = (double) sr;

            if (auto* arr = obj->getProperty ("markers").getArray())
            {
                for (const auto& v : *arr)
                {
                    if (auto* mo = v.getDynamicObject())
                    {
                        Marker m;
                        m.sampleOffset = (juce::int64) mo->getProperty ("sample");
                        m.name         = mo->getProperty ("name").toString();
                        m.type         = mo->getProperty ("type").toString();
                        if (m.type.isEmpty()) m.type = "user";
                        if (mo->hasProperty ("zoom"))
                            m.zoom = (float) (double) mo->getProperty ("zoom");
                        if (mo->hasProperty ("visible"))
                            if (auto* va = mo->getProperty ("visible").getArray())
                                for (const auto& vv : *va)
                                    m.visibleTracks.push_back ((int) vv);
                        markers.push_back (std::move (m));
                    }
                }
            }
        }
        sortByPosition();
        return true;
    }
}
