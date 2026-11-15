// Host replacement for core/thread.cpp: plain pthreads, no Horizon core/priority control.

#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/thread.hpp"

namespace karafin {
namespace {

thread_local char t_name[24] = "?";

struct Start {
    ThreadOptions options;
    std::function<void()> fn;
};

void* trampoline(void* arg) {
    std::unique_ptr<Start> start(static_cast<Start*>(arg));
    setCurrentThreadName(start->options.name);
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
    pthread_attr_setstacksize(&attr, std::max<size_t>(options.stackSize, 256 * 1024));
    auto start = std::make_unique<Start>(Start{std::move(options), std::move(fn)});
    auto impl = std::make_unique<Impl>();
    const int rc = pthread_create(&impl->handle, &attr, &trampoline, start.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) return;
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

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace karafin
