#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Per-frame performance counters (main thread). Subsystems add to perf::frame() while the frame
// runs; App closes the frame after the swap, keeps a short history for the stats overlay and
// logs slow frames (> 25 ms) at debug level with what happened in them.
namespace karafin::perf {

using Clock = std::chrono::steady_clock;

inline double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

struct Frame {
    // App loop phases (ms).
    double eventsMs = 0;
    double tasksMs = 0;  // main_thread::post tasks
    double updateMs = 0;
    double renderMs = 0;
    double swapMs = 0;   // blocked in SDL_GL_SwapWindow (vsync / GPU backpressure)
    double cpuMs = 0;    // everything but the swap
    double frameMs = 0;  // swap-to-swap interval
    int tasks = 0;

    // Texture uploads (image cache).
    int uploads = 0;           // images completed this frame
    size_t uploadBytes = 0;
    double uploadMs = 0;

    // Text.
    int layouts = 0;           // shaped layouts (cache misses)
    double layoutMs = 0;
    int glyphs = 0;            // glyphs rasterized
    int atlasUploads = 0;      // glyph atlas sub-image uploads
    size_t atlasBytes = 0;

    // Renderer.
    int drawCalls = 0;
    int quads = 0;
    int targets = 0;           // offscreen render target passes
};

struct Sample {
    float frameMs = 0;
    float cpuMs = 0;
    float gpuMs = -1;
};

inline constexpr size_t kHistory = 120;

// Counters of the frame in progress.
Frame& frame();
// Counters of the last completed frame.
const Frame& last();
// Ring buffer of recent frames; history()[(historyHead() + i) % kHistory] is oldest-first.
const std::array<Sample, kHistory>& history();
size_t historyHead();

// GPU time of the most recent frame whose timer query resolved (ms), or < 0 when unavailable.
void setGpuMs(double ms);
double gpuMs();

// Called by App after the swap: records the sample, logs slow frames, resets frame().
void endFrame();

}  // namespace karafin::perf
