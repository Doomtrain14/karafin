#include "core/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

#include <sys/stat.h>

#include "core/thread.hpp"

namespace karafin::log {
namespace {

constexpr long kMaxLogBytes = 1024 * 1024;
// A runaway session (trace level) rotates in place instead of filling the SD card.
constexpr uint64_t kMaxSessionBytes = 4u * 1024 * 1024;
// Every fwrite/fflush on the SD card is a filesystem IPC round trip that can take milliseconds
// while downloads or the image cache write. Trace..Info lines are queued and written by a
// background thread at most every 250 ms; Warn/Error lines are written (and flushed) by the
// caller at once so a crash keeps them.
constexpr auto kFlushInterval = std::chrono::milliseconds(250);

// Lock order: g_ioMutex, then g_mutex. g_mutex only guards the queue (cheap to take on the main
// thread); g_ioMutex serializes the actual writes and keeps lines in order.
std::mutex g_ioMutex;
std::mutex g_mutex;
std::condition_variable g_cv;
std::string g_pending;
bool g_flushRequested = false;
bool g_stop = false;
// Deliberately not a static-lifetime object: if shutdown() is skipped, exit must not join it.
Thread* g_writer = nullptr;

FILE* g_file = nullptr;  // g_ioMutex
std::string g_path;
uint64_t g_sessionBytes = 0;
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::atomic<bool> g_stdout{false};
std::atomic<double> g_syncUntil{0.0};  // seconds since start; see syncFor()
const auto g_start = std::chrono::steady_clock::now();

char levelChar(Level l) {
    switch (l) {
        case Level::Trace: return 'T';
        case Level::Debug: return 'D';
        case Level::Info: return 'I';
        case Level::Warn: return 'W';
        case Level::Error: return 'E';
    }
    return '?';
}

// g_ioMutex held (or single-threaded init); g_file closed.
void rotate() {
    const std::string old = g_path + ".old";
    std::remove(old.c_str());
    std::rename(g_path.c_str(), old.c_str());
}

double secondsSinceStart() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

// g_ioMutex held. Writes `text` to the file (and stdout when mirrored).
void output(const std::string& text, bool flush) {
    if (text.empty()) return;
    if (g_file) {
        std::fwrite(text.data(), 1, text.size(), g_file);
        g_sessionBytes += text.size();
        if (flush) std::fflush(g_file);
        if (g_sessionBytes > kMaxSessionBytes) {
            std::fclose(g_file);
            rotate();
            g_file = std::fopen(g_path.c_str(), "a");
            g_sessionBytes = 0;
        }
    }
    if (g_stdout.load(std::memory_order_relaxed)) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
    }
}

// g_ioMutex held. Moves the queue out and writes it, optionally followed by `extra`.
void drain(bool flush, std::string_view extra = {}) {
    std::string batch;
    {
        std::lock_guard lock(g_mutex);
        batch.swap(g_pending);
        g_flushRequested = false;
    }
    batch.append(extra);
    output(batch, flush);
}

void writerLoop() {
    std::unique_lock lock(g_mutex);
    while (!g_stop) {
        g_cv.wait_for(lock, kFlushInterval, [] { return g_stop || g_flushRequested; });
        if (g_pending.empty()) {
            g_flushRequested = false;
            continue;
        }
        lock.unlock();
        {
            std::lock_guard io(g_ioMutex);
            drain(true);
        }
        lock.lock();
    }
}

}  // namespace

void init(const std::string& filePath) {
    {
        std::lock_guard io(g_ioMutex);
        if (g_file) return;
        g_path = filePath;

        struct stat st{};
        if (stat(filePath.c_str(), &st) == 0 && st.st_size > kMaxLogBytes) rotate();
        g_file = std::fopen(filePath.c_str(), "a");
        if (g_file) {
            std::time_t now = std::time(nullptr);
            char stamp[32] = {};
            std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
            std::fprintf(g_file, "\n===== Karafin %s started %s =====\n", KARAFIN_VERSION, stamp);
            std::fflush(g_file);
        }
    }
    {
        std::lock_guard lock(g_mutex);
        g_stop = false;
    }
    ThreadOptions opts;
    opts.name = "log";
    opts.stackSize = 64 * 1024;
    opts.priority = kPriorityBackground;
    auto* writer = new Thread(std::move(opts), &writerLoop);
    std::lock_guard lock(g_mutex);
    g_writer = writer;
}

void shutdown() {
    Thread* writer = nullptr;
    {
        std::lock_guard lock(g_mutex);
        g_stop = true;
        writer = g_writer;
        g_writer = nullptr;
    }
    g_cv.notify_all();
    delete writer;  // joins
    std::lock_guard io(g_ioMutex);
    drain(true);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void setLevel(Level level) { g_level.store(static_cast<int>(level)); }
Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

Level parseLevel(std::string_view name, Level fallback) {
    if (name == "trace") return Level::Trace;
    if (name == "debug") return Level::Debug;
    if (name == "info") return Level::Info;
    if (name == "warn") return Level::Warn;
    if (name == "error") return Level::Error;
    return fallback;
}

void setStdoutMirror(bool enabled) { g_stdout.store(enabled); }

void flush() {
    {
        std::lock_guard lock(g_mutex);
        if (g_pending.empty()) return;
        g_flushRequested = true;
    }
    g_cv.notify_one();
}

void syncFor(double seconds) { g_syncUntil.store(secondsSinceStart() + seconds, std::memory_order_relaxed); }

bool syncing() { return secondsSinceStart() < g_syncUntil.load(std::memory_order_relaxed); }

void write(Level level, std::string_view tag, std::string_view message, bool sync) {
    const double t = secondsSinceStart();
    if (t < g_syncUntil.load(std::memory_order_relaxed)) sync = true;
    const std::string_view thread = currentThreadName();
    char prefix[96];
    const int n = std::snprintf(prefix, sizeof prefix, "[%9.3f] %c/%.*s [%.*s] ", t, levelChar(level),
                                static_cast<int>(tag.size()), tag.data(), static_cast<int>(thread.size()),
                                thread.data());
    const size_t prefixLen = n > 0 ? std::min<size_t>(static_cast<size_t>(n), sizeof prefix - 1) : 0;

    bool queued = false;
    if (level < Level::Warn && !sync) {
        std::lock_guard lock(g_mutex);
        if (g_writer && !g_stop) {
            g_pending.append(prefix, prefixLen);
            g_pending.append(message);
            g_pending.push_back('\n');
            queued = true;
        }
    }
    if (queued) return;

    std::string line;
    line.reserve(prefixLen + message.size() + 1);
    line.append(prefix, prefixLen);
    line.append(message);
    line.push_back('\n');
    std::lock_guard io(g_ioMutex);
    drain(true, line);
}

}  // namespace karafin::log
