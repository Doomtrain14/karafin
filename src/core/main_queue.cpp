#include "core/main_queue.hpp"

#include <chrono>
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

struct Delayed {
    Clock::time_point due;
    std::function<void()> task;
};

std::mutex g_mutex;
std::deque<std::function<void()>> g_queue;
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

}  // namespace

void post(std::function<void()> task) {
    std::lock_guard lock(g_mutex);
    g_queue.push_back(std::move(task));
}

void postDelayed(double seconds, std::function<void()> task) {
    const auto due = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
    std::lock_guard lock(g_mutex);
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

    {
        std::lock_guard lock(g_mutex);
        for (auto it = g_delayed.begin(); it != g_delayed.end();) {
            if (it->due <= start) {
                g_queue.push_back(std::move(it->task));
                it = g_delayed.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t ran = 0;
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lock(g_mutex);
            if (g_queue.empty()) break;
            task = std::move(g_queue.front());
            g_queue.pop_front();
        }
        runGuarded(task);
        ++ran;
        if (Clock::now() >= deadline) break;
    }
    return ran;
}

void clear() {
    std::lock_guard lock(g_mutex);
    g_queue.clear();
    g_delayed.clear();
}

}  // namespace karafin::main_thread
