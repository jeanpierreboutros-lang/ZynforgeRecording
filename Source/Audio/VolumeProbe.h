#pragma once

#include <juce_core/juce_core.h>

#include <sys/mount.h>
#include <sys/param.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace zynforge
{
    // One destination's volume, read on a thread of its own. A stat on a dead
    // mount can block for minutes or for good, so nothing that matters may wait
    // on one: readers get the last published numbers and how old they are.
    // The thread owns only `Shared`, so a read that never returns strands that
    // thread and nothing else, and the probe can still be destroyed.
    class VolumeProbe
    {
    public:
        struct Reading
        {
            juce::int64  freeBytes { 0 };
            juce::int64  totalBytes { 0 };
            juce::uint64 volumeId { 0 };   // the same for two paths on one volume
            double       atMs { 0.0 };     // when this was read; 0 = nothing yet
        };
        using Reader = std::function<Reading (const juce::File&)>;

        // One statfs. juce::File::getVolumeSerialNumber is always 0 on macOS,
        // and without an id two copies on one drive would each look like they
        // had it to themselves.
        static Reading readVolume (const juce::File& root)
        {
            auto target = root;
            for (int i = 5; --i >= 0 && ! target.exists();)
                target = target.getParentDirectory();

            Reading reading;
            struct statfs info {};
            if (::statfs (target.getFullPathName().toRawUTF8(), &info) != 0)
                return reading;
            const auto blockSize = (juce::int64) info.f_bsize;
            reading.freeBytes  = blockSize * (juce::int64) info.f_bavail;
            reading.totalBytes = blockSize * (juce::int64) info.f_blocks;
            reading.volumeId   = ((juce::uint64) (juce::uint32) info.f_fsid.val[0] << 32)
                               | (juce::uint32) info.f_fsid.val[1];
            return reading;
        }

        // readVolume, with a folder that has gone reading as no volume at all.
        static Reading readFolder (const juce::File& folder)
        {
            auto reading = readVolume (folder);
            if (! folder.isDirectory()) reading.totalBytes = 0;
            return reading;
        }

        // Whether the volume answers in time. The read runs on a thread of its
        // own, so a mount that never answers costs the caller timeoutMs at most.
        static bool answersWithin (const juce::File& root, int timeoutMs, Reader reader = readVolume)
        {
            auto answered = std::make_shared<juce::WaitableEvent> (true);
            std::thread ([answered, root, read = std::move (reader)]
            {
                read (root);
                answered->signal();
            }).detach();
            return answered->wait (timeoutMs);
        }

        VolumeProbe (const juce::File& root, Reader reader, int periodMs)
        {
            std::thread ([state = shared, root, read = std::move (reader), periodMs]
            {
                juce::Thread::setCurrentThreadName ("Volume probe");
                for (;;)
                {
                    const auto reading = read (root);
                    state->freeBytes .store (reading.freeBytes,  std::memory_order_relaxed);
                    state->totalBytes.store (reading.totalBytes, std::memory_order_relaxed);
                    state->volumeId  .store (reading.volumeId,   std::memory_order_relaxed);
                    state->atMs.store (juce::Time::getMillisecondCounterHiRes(),
                                       std::memory_order_release);

                    std::unique_lock<std::mutex> lock (state->lock);
                    if (state->wake.wait_for (lock, std::chrono::milliseconds (periodMs),
                                              [&state] { return state->leaving; }))
                        return;
                }
            }).detach();
        }

        ~VolumeProbe()
        {
            {
                const std::lock_guard<std::mutex> guard (shared->lock);
                shared->leaving = true;
            }
            shared->wake.notify_all();
        }

        Reading latest() const noexcept
        {
            Reading reading;
            reading.atMs       = shared->atMs.load (std::memory_order_acquire);
            reading.freeBytes  = shared->freeBytes .load (std::memory_order_relaxed);
            reading.totalBytes = shared->totalBytes.load (std::memory_order_relaxed);
            reading.volumeId   = shared->volumeId  .load (std::memory_order_relaxed);
            return reading;
        }

    private:
        struct Shared
        {
            std::atomic<juce::int64>  freeBytes { 0 };
            std::atomic<juce::int64>  totalBytes { 0 };
            std::atomic<juce::uint64> volumeId { 0 };
            std::atomic<double>       atMs { 0.0 };
            std::mutex                lock;
            std::condition_variable   wake;
            bool                      leaving { false };
        };
        std::shared_ptr<Shared> shared { std::make_shared<Shared>() };
    };
}
