#include "core/perf.hpp"

#include "core/log.hpp"

namespace karafin::perf {
namespace {

constexpr double kSlowFrameMs = 25.0;
constexpr int kMaxSlowLogsPerSecond = 20;

Frame g_frame;
Frame g_last;
std::array<Sample, kHistory> g_history{};
size_t g_head = 0;
double g_gpuMs = -1;
uint64_t g_frames = 0;
Clock::time_point g_logWindow{};
int g_logsInWindow = 0;

void logSlow(const Frame& f) {
    const auto now = Clock::now();
    if (now - g_logWindow > std::chrono::seconds(1)) {
        g_logWindow = now;
        g_logsInWindow = 0;
    }
    if (++g_logsInWindow > kMaxSlowLogsPerSecond) return;
    log::debug("perf",
               "slow frame {:.1f} ms: cpu {:.1f} (events {:.1f}, tasks {} {:.1f}, update {:.1f}, render {:.1f}) swap "
               "{:.1f} gpu {:.1f} | uploads {} {} KiB {:.1f} ms | layouts {} {:.1f} ms, glyphs {}, atlas {} {} KiB | "
               "draws {} quads {} rt {}",
               f.frameMs, f.cpuMs, f.eventsMs, f.tasks, f.tasksMs, f.updateMs, f.renderMs, f.swapMs, g_gpuMs,
               f.uploads, f.uploadBytes / 1024, f.uploadMs, f.layouts, f.layoutMs, f.glyphs, f.atlasUploads,
               f.atlasBytes / 1024, f.drawCalls, f.quads, f.targets);
}

}  // namespace

Frame& frame() { return g_frame; }
const Frame& last() { return g_last; }
const std::array<Sample, kHistory>& history() { return g_history; }
size_t historyHead() { return g_head; }

void setGpuMs(double ms) { g_gpuMs = ms; }
double gpuMs() { return g_gpuMs; }

void endFrame() {
    g_history[g_head] = {static_cast<float>(g_frame.frameMs), static_cast<float>(g_frame.cpuMs),
                         static_cast<float>(g_gpuMs)};
    g_head = (g_head + 1) % kHistory;
    // The first frames build the initial screen and compile shaders; not interesting.
    if (++g_frames > 30 && g_frame.frameMs > kSlowFrameMs && log::enabled(log::Level::Debug)) logSlow(g_frame);
    g_last = g_frame;
    g_frame = {};
}

}  // namespace karafin::perf
