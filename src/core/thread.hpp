#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace karafin {

// Horizon thread priorities: 0 = highest, 0x3F = lowest. The main thread runs at 0x2C on core 0.
// Horizon does not time-slice threads of equal priority, so nothing that can burn CPU (TLS
// handshakes, decoding) may share the main thread's priority: it would run to completion while
// the frame waits.
inline constexpr int kPriorityMain = 0x2C;
inline constexpr int kPriorityIo = 0x2D;          // HTTP, downloads (mostly blocked on sockets)
inline constexpr int kPriorityWorker = 0x2E;      // thread pool, image decode
inline constexpr int kPriorityBackground = 0x30;  // housekeeping

struct ThreadOptions {
    std::string name = "thread";
    size_t stackSize = 256 * 1024;  // TLS handshakes (mbedtls) want >= 128 KiB
    int priority = kPriorityWorker;
    // Preferred core (0..2). -1 lets Karafin pick core 1 or 2. Threads not explicitly placed on
    // core 0 may only migrate between cores 1 and 2: core 0 belongs to the main/render thread.
    int core = -1;
};

// Joinable native thread (pthread underneath) with an explicit stack size, priority and core
// affinity, which std::thread cannot express. Move-only; the destructor joins.
class Thread {
public:
    Thread();
    Thread(ThreadOptions options, std::function<void()> fn);
    ~Thread();

    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool joinable() const;
    void join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Name used in log lines ("main", "http", "pool-1", ...). Set automatically for Thread.
void setCurrentThreadName(std::string_view name);
std::string_view currentThreadName();

void sleepMs(int ms);

}  // namespace karafin
