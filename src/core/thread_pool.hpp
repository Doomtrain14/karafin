#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "core/thread.hpp"

namespace karafin {

// Fixed-size FIFO worker pool for CPU work (JSON parsing, image decode, blur, disk I/O).
// Never block a pool thread on network I/O: use http::Client::send (async) instead, or a
// dedicated karafin::Thread for long-running loops such as downloads.
class ThreadPool {
public:
    ThreadPool(size_t threads, std::string name, size_t stackSize = 512 * 1024, int priority = kPriorityWorker);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Fire-and-forget. Tasks queued after shutdown() are dropped.
    void post(std::function<void()> task);

    // Returns a future for the task's result (exceptions propagate through the future).
    template <class F>
    auto submit(F&& f) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        auto fut = task->get_future();
        post([task] { (*task)(); });
        return fut;
    }

    // Stops accepting work, drops queued tasks, waits for running tasks to finish.
    void shutdown();

    size_t size() const { return threads_.size(); }
    size_t pending() const;

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<Thread> threads_;
    bool stopping_ = false;
};

// Shared pool owned by the App (3 threads on cores 1-2). Valid between App start and shutdown.
ThreadPool& workers();

namespace detail {
void setGlobalWorkers(ThreadPool* pool);
}

}  // namespace karafin
