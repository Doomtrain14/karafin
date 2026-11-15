#include "core/thread.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <pthread.h>
#include <switch.h>

#include "core/log.hpp"

namespace karafin {
namespace {

thread_local char t_name[24] = "?";
std::atomic<int> g_coreRoundRobin{0};

constexpr u32 kAppCoreMask = 0b111;     // cores 0-2; core 3 belongs to the system
constexpr u32 kWorkerCoreMask = 0b110;  // cores 1-2; core 0 runs the main/render thread

struct Start {
    ThreadOptions options;
    std::function<void()> fn;
};

void* trampoline(void* arg) {
    std::unique_ptr<Start> start(static_cast<Start*>(arg));
    setCurrentThreadName(start->options.name);

    int core = start->options.core;
    if (core < 0 || core > 2) core = 1 + (g_coreRoundRobin.fetch_add(1) % 2);
    // The kernel migrates runnable threads to idle cores within their mask; with core 0 in it, a
    // decoder or TLS handshake lands on core 0 whenever the main thread waits for vsync and then
    // delays the next frame.
    const u32 mask = core == 0 ? kAppCoreMask : kWorkerCoreMask;
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask))) {
        log::warn("thread", "svcSetThreadCoreMask({}) failed for {}", core, start->options.name);
    }
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(start->options.priority)))) {
        log::warn("thread", "svcSetThreadPriority({:#x}) failed for {}", start->options.priority,
                  start->options.name);
    }

    start->fn();
    return nullptr;
}

}  // namespace

struct Thread::Impl {
    pthread_t handle{};
    bool joinable = false;
};

Thread::Thread() = default;

Thread::Thread(ThreadOptions options, std::function<void()> fn) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, options.stackSize);

    auto start = std::make_unique<Start>(Start{std::move(options), std::move(fn)});
    const std::string name = start->options.name;
    auto impl = std::make_unique<Impl>();
    const int rc = pthread_create(&impl->handle, &attr, &trampoline, start.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        log::error("thread", "pthread_create failed for {} (rc={})", name, rc);
        return;
    }
    start.release();
    impl->joinable = true;
    impl_ = std::move(impl);
}

Thread::~Thread() { join(); }

Thread::Thread(Thread&& other) noexcept = default;

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        join();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool Thread::joinable() const { return impl_ && impl_->joinable; }

void Thread::join() {
    if (!joinable()) return;
    pthread_join(impl_->handle, nullptr);
    impl_->joinable = false;
}

void setCurrentThreadName(std::string_view name) {
    const size_t n = std::min(name.size(), sizeof t_name - 1);
    std::memcpy(t_name, name.data(), n);
    t_name[n] = '\0';
}

std::string_view currentThreadName() { return t_name; }

void sleepMs(int ms) { svcSleepThread(static_cast<s64>(ms) * 1'000'000); }

}  // namespace karafin
