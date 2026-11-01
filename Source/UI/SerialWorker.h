#pragma once

#include <pthread/qos.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace zynforge
{
    // Runs jobs one at a time on one low-priority thread, and always joins it.
    // A juce::ThreadPool would not do: its destructor gives each worker 500 ms
    // and then kills it, which on a busy machine takes a lock down with it.
    class SerialWorker
    {
    public:
        ~SerialWorker()
        {
            {
                const std::lock_guard<std::mutex> hold (lock);
                stopping = true;
                jobs.clear();
            }
            changed.notify_all();
            if (worker.joinable()) worker.join();
        }

        void add (std::function<void()> job)
        {
            {
                const std::lock_guard<std::mutex> hold (lock);
                jobs.push_back (std::move (job));
                if (! worker.joinable()) worker = std::thread ([this] { run(); });
            }
            changed.notify_all();
        }

        // Drops what has not started and waits out what has.
        void dropAndWait()
        {
            std::unique_lock<std::mutex> hold (lock);
            jobs.clear();
            changed.wait (hold, [this] { return ! running; });
        }

    private:
        void run()
        {
            pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
            std::unique_lock<std::mutex> hold (lock);
            for (;;)
            {
                changed.wait (hold, [this] { return stopping || ! jobs.empty(); });
                if (stopping) return;
                auto job = std::move (jobs.front());
                jobs.pop_front();
                running = true;
                hold.unlock();
                job();
                job = nullptr;
                hold.lock();
                running = false;
                changed.notify_all();
            }
        }

        std::mutex lock;
        std::condition_variable changed;
        std::deque<std::function<void()>> jobs;
        std::thread worker;
        bool running { false }, stopping { false };
    };
}
