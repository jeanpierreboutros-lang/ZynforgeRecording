#pragma once

#include "../Audio/AtomicFile.h"
#include "../Audio/SessionBackup.h"
#include "SessionProjPath.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace zynforge
{
    struct SessionMetadataSnapshot
    {
        juce::File directory;
        juce::String settingsJson, mixJson, projectPatchJson;
        bool backup = true;
    };

    struct SessionMetadataResult
    {
        bool ok = false;
        juce::String error;
        bool superseded = false; // only an unstarted layout-only request can retire this way
    };

    // The worker receives serialized values, never engine/UI objects or shared
    // mutable JSON. All project read/merge/write and snapshot I/O happens here.
    inline SessionMetadataResult writeSessionMetadata (const SessionMetadataSnapshot& snapshot)
    {
        if (! snapshot.directory.isDirectory()) return { false, "Session folder is unavailable" };
        const auto project = findSessionProj (snapshot.directory);
        if (project == juce::File()) return { false, "Session project is unavailable" };
        const auto patch = juce::JSON::parse (snapshot.projectPatchJson);
        if (! patch.isObject()) return { false, "Invalid session metadata snapshot" };
        auto existing = juce::JSON::parse (project);
        // A failed read/parse is not a new session. Refuse before touching any
        // sibling metadata so the original project remains recoverable.
        if (project.exists() && ! existing.isObject())
            return { false, "Existing session project could not be read; original metadata was preserved" };
        juce::DynamicObject::Ptr merged = existing.isObject() ? existing.getDynamicObject()
                                                            : new juce::DynamicObject();
        for (const auto& property : patch.getDynamicObject()->getProperties())
            merged->setProperty (property.name, property.value);
        for (const auto& file : { std::make_pair ("session_settings.json", snapshot.settingsJson),
                                 std::make_pair ("session_mix.json", snapshot.mixJson) })
            if (file.second.isNotEmpty()
                && ! atomicfile::writeText (snapshot.directory.getChildFile (file.first), file.second))
                return { false, "Could not save " + juce::String (file.first) };
        if (! atomicfile::writeText (project, juce::JSON::toString (juce::var (merged.get()))))
            return { false, "Could not save session project" };
        if (snapshot.backup && ! sessionbackup::writeSnapshot (snapshot.directory).isDirectory())
            return { false, "Could not save session backup snapshot" };
        return { true, {} };
    }

    class SessionMetadataWriter
    {
    public:
        using Completion = std::function<void (SessionMetadataResult)>;
        SessionMetadataWriter() : worker ([this] { run(); }) {}
        ~SessionMetadataWriter() { finish(); }

        // Layout patches may replace an unstarted layout patch for the same
        // session. Full/explicit saves retain their completion obligations.
        // Retiring the old layout callback releases accounting, never reports
        // durable success. Each queued job stores exactly one callback.
        void enqueue (SessionMetadataSnapshot snapshot, bool automatic, Completion completion)
        {
            Completion retired;
            bool replaced = false, rejected = false;
            {
                const std::lock_guard<std::mutex> guard (mutex);
                const bool layoutOnly = automatic && ! snapshot.backup
                    && snapshot.settingsJson.isEmpty() && snapshot.mixJson.isEmpty();
                if (layoutOnly && ! jobs.empty())
                {
                    auto& last = jobs.back();
                    if (last.automatic && ! last.snapshot.backup
                        && last.snapshot.settingsJson.isEmpty() && last.snapshot.mixJson.isEmpty()
                        && last.snapshot.directory == snapshot.directory)
                    {
                        retired = std::move (last.completion);
                        last = { std::move (snapshot), true, std::move (completion) };
                        replaced = true;
                    }
                }
                if (! replaced)
                {
                    rejected = stopping || jobs.size() >= 16;
                    if (! rejected) jobs.push_back ({ std::move (snapshot), automatic, std::move (completion) });
                }
            }
            if (retired) retired ({ false, {}, true });
            if (rejected && completion) completion ({ false, "Too many pending saves; retry after saving completes" });
            wake.notify_one();
        }

        void finish()
        {
            { const std::lock_guard<std::mutex> guard (mutex); stopping = true; }
            wake.notify_all();
            if (worker.joinable()) worker.join();
        }

    private:
        struct Job { SessionMetadataSnapshot snapshot; bool automatic; Completion completion; };
        void run()
        {
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> guard (mutex);
                    wake.wait (guard, [&] { return stopping || ! jobs.empty(); });
                    if (jobs.empty()) return;
                    job = std::move (jobs.front());
                    jobs.pop_front();
                }
                auto result = writeSessionMetadata (job.snapshot);
                if (job.completion) job.completion (std::move (result));
            }
        }
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<Job> jobs;
        bool stopping = false;
        std::thread worker;
    };
}
