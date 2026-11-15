#include "core/main_queue.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/log.hpp"

namespace karafin::main_thread {
namespace {

using Clock = std::chrono::steady_clock;

// Hard bound on tasks posted from other threads. A poster that finds the queue full blocks until
// the main thread drains it (or close() is called). Tasks are never dropped while the app loop
// runs: many carry completion callbacks. Only the main thread itself and postNoWait() bypass the
// bound; see main_queue.hpp.
constexpr size_t kMaxPending = 1024;

struct Delayed {
    Clock::time_point due;
    std::function<void()> task;
};

std::mutex g_mutex;
std::condition_variable g_space;
std::deque<std::function<void()>> g_queue;
size_t g_bounded = 0;  // queued tasks that count against kMaxPending
std::deque<bool> g_counted;  // per queued task: did it count against kMaxPending
bool g_closed = false;
bool g_full = false;  // warned about this overflow; cleared once the queue drains
std::vector<Delayed> g_delayed;
std::thread::id g_mainId;

void runGuarded(std::function<void()>& task) {
    try {
        task();
    } catch (const std::exception& e) {
        log::error("main", "posted task threw: {}", e.what());
    } catch (...) {
        log::error("main", "posted task threw a non-std exception");
    }
}

void push(std::function<void()> task, bool counted) {
    g_queue.push_back(std::move(task));
    g_counted.push_back(counted);
    if (counted) ++g_bounded;
}

}  // namespace

void post(std::function<void()> task) {
    std::unique_lock lock(g_mutex);
    if (g_closed) return;
    if (isCurrent()) {
        push(std::move(task), false);
        return;
    }
    if (g_bounded >= kMaxPending) {
        if (!g_full) {
            g_full = true;
            log::warn("main", "task queue full ({} pending); posting threads wait for the main thread", g_bounded);
        }
        g_space.wait(lock, [] { return g_closed || g_bounded < kMaxPending; });
        if (g_closed) return;
    }
    push(std::move(task), true);
}

void postNoWait(std::function<void()> task) {
    std::lock_guard lock(g_mutex);
    if (g_closed) return;
    push(std::move(task), false);
}

void postDelayed(double seconds, std::function<void()> task) {
    const auto due = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    std::lock_guard lock(g_mutex);
    if (g_closed) return;
    g_delayed.push_back({due, std::move(task)});
}

bool isCurrent() { return std::this_thread::get_id() == g_mainId; }

void assertCurrent(const char* where) {
    if (isCurrent()) return;
    static std::mutex m;
    static std::set<std::string> reported;
    std::lock_guard lock(m);
    if (reported.insert(where).second) log::error("main", "{} called off the main thread", where);
}

void bindCurrentThread() { g_mainId = std::this_thread::get_id(); }

size_t runPending(double budgetMs) {
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(budgetMs));

    // Due delayed tasks run first and never enter the bounded queue.
    std::vector<std::function<void()>> due;
    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_delayed.begin(); it != g_delayed.end();) {
            if (it->due <= start) {
                due.push_back(std::move(it->task));
                it = g_delayed.erase(it);
            } else {
                ++it;
            }
        }
    }
    size_t ran = 0;
    for (auto& task : due) {
        runGuarded(task);
        ++ran;
    }

    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lock(g_mutex);
            if (g_queue.empty()) {
                g_full = false;
                break;
            }
            task = std::move(g_queue.front());
            g_queue.pop_front();
            if (g_counted.front()) {
                --g_bounded;
                g_space.notify_one();
            }
            g_counted.pop_front();
        }
        runGuarded(task);
        ++ran;
        if (Clock::now() >= deadline) break;
    }
    return ran;
}

size_t pendingCount() {
    std::lock_guard lock(g_mutex);
    return g_queue.size();
}

void close() {
    std::lock_guard lock(g_mutex);
    g_closed = true;
    g_space.notify_all();
}

void clear() {
    std::lock_guard lock(g_mutex);
    g_queue.clear();
    g_counted.clear();
    g_bounded = 0;
    g_delayed.clear();
    g_full = false;
    g_space.notify_all();
}

}  // namespace karafin::main_thread