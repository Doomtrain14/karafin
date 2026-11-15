#include "core/thread_pool.hpp"

#include <cstdlib>
#include <exception>

#include "core/log.hpp"

namespace karafin {
namespace {
ThreadPool* g_workers = nullptr;
}

ThreadPool::ThreadPool(size_t threads, std::string name, size_t stackSize, int priority) {
    threads_.reserve(threads);
    for (size_t i = 0; i < threads; ++i) {
        ThreadOptions opts;
        opts.name = name + "-" + std::to_string(i);
        opts.stackSize = stackSize;
        opts.priority = priority;
        threads_.emplace_back(std::move(opts), [this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

void ThreadPool::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::shutdown() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ && threads_.empty()) return;
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
    threads_.clear();
}

size_t ThreadPool::pending() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task();
        } catch (const std::exception& e) {
            log::error("pool", "task threw: {}", e.what());
        } catch (...) {
            log::error("pool", "task threw a non-std exception");
        }
    }
}

ThreadPool& workers() {
    if (!g_workers) {
        log::error("pool", "workers() used before App started");
        std::abort();
    }
    return *g_workers;
}

namespace detail {
void setGlobalWorkers(ThreadPool* pool) { g_workers = pool; }
}  // namespace detail

}  // namespace karafin
