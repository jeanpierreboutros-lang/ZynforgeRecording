#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace zynforge
{
    // One owned background worker for session-file jobs (track export, stem /
    // mix bounce, import, Save As copy, relocation, show handoff, click
    // render) plus the session-I/O reservation that keeps transport, edits and
    // competing transitions off the session while a job owns it.
    //
    // Eight call sites used to hand-write the same sequence: claim the busy
    // flag, join the previous std::thread, launch a new one, and post a
    // completion that cleared the flag. Clearing it was each worker's job on
    // every exit path -- and the track export's cancel path returned before
    // posting, so a quit that cancelled an export left the session busy for
    // good ("quit again when it finishes" could never succeed). Here the
    // release is structural: whatever the work returns, or if it throws, the
    // reservation clears on the message thread before the completion runs.
    //
    // The reservation flag stays a plain atomic owned by the host because
    // metadata saves share it (they track their own pending count and never
    // start while a job holds it -- see enqueueMetadataSnapshot).
    //
    // Threading: every member except the work function is message-thread only.
    // The work function runs on the owned thread; it must touch only values it
    // captured plus the cancel flag, never UI or SafePointers. The destructor
    // cancels and joins, so work may also use objects that outlive the runner
    // (the host declares the runner after the engine).
    class SessionJobRunner
    {
    public:
        // Runs on the message thread after the reservation is released.
        using Completion = std::function<void()>;
        // Runs on the worker. Return the completion (or nothing).
        using Work = std::function<Completion (const std::atomic<bool>& cancelled)>;

        explicit SessionJobRunner (std::atomic<bool>& reservationFlag)
            : shared (std::make_shared<Shared> (reservationFlag)) {}

        ~SessionJobRunner()
        {
            cancelAndJoin();
            shared->alive.store (false);   // a completion still queued becomes a no-op
        }

        // Claim the reservation. False when another session operation holds it.
        bool reserve() noexcept { return ! shared->busy.exchange (true); }

        // Give up a reservation that was claimed but never launched (a chooser
        // was cancelled, validation failed).
        void release() noexcept { shared->busy.store (false); }

        // Start `work` under a reservation the caller already holds. Joins the
        // previous job first; it has already posted its completion.
        void launch (Work work)
        {
            jassert (shared->busy.load());
            join();
            shared->cancel.store (false);
            worker = std::thread ([state = shared, work = std::move (work)]
            {
                Completion done;
                try { done = work (state->cancel); }
                catch (...) { done = nullptr; }   // the reservation still clears
                juce::MessageManager::callAsync ([state, done = std::move (done)]
                {
                    if (! state->alive.load()) return;
                    state->busy.store (false);
                    if (done) done();
                });
            });
        }

        // reserve() + launch(). False, and nothing started, when busy.
        bool start (Work work)
        {
            if (! reserve()) return false;
            launch (std::move (work));
            return true;
        }

        // Ask the running job to stop at its next cancellation point.
        void cancel() noexcept { shared->cancel.store (true); }
        void cancelAndJoin()   { cancel(); join(); }

    private:
        struct Shared
        {
            explicit Shared (std::atomic<bool>& flag) : busy (flag) {}
            std::atomic<bool>& busy;
            std::atomic<bool>  cancel { false };
            std::atomic<bool>  alive  { true };
        };

        void join() { if (worker.joinable()) worker.join(); }

        std::shared_ptr<Shared> shared;
        std::thread             worker;

        JUCE_DECLARE_NON_COPYABLE (SessionJobRunner)
    };
}
