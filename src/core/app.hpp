#pragma once

#include <cstdint>
#include <memory>

#include <SDL.h>

namespace karafin {

class ThreadPool;

// Render target geometry. UI code works in a fixed 1280x720 logical space; `scale` maps it to
// the framebuffer (1.0 handheld, 1.5 docked at 1080p).
struct DisplayInfo {
    int pixelWidth = 1280;
    int pixelHeight = 720;
    float logicalWidth = 1280.0f;
    float logicalHeight = 720.0f;
    float scale = 1.0f;
    bool docked = false;
};

// Receives the app loop callbacks. The UI engine (src/ui) implements this; core ships a trivial
// one in main.cpp until then. All methods run on the main thread with the GL context current.
class AppDelegate {
public:
    virtual ~AppDelegate() = default;

    virtual void onStart() {}
    // Return true if the event was consumed.
    virtual bool onEvent(const SDL_Event& event) { (void)event; return false; }
    // dt in seconds: snapped to whole vsync periods when close to one, clamped to [0, 0.05].
    virtual void onUpdate(double dt) { (void)dt; }
    // Viewport is set to the full framebuffer; the delegate clears and draws.
    virtual void onRender() {}
    virtual void onDisplayChanged(const DisplayInfo& display) { (void)display; }
    // Also delivered as false->true when focus was lost and regained between two frames.
    virtual void onFocusChanged(bool focused) { (void)focused; }
    // The console woke from sleep. Sockets opened before sleep are dead and the network may need
    // a few seconds to come back.
    virtual void onResume() {}
    virtual void onStop() {}
};

struct AppConfig {
    int workerThreads = 3;
    bool vsync = true;
};

// Owns the SDL window, GL ES 3 context, the shared worker pool and the HTTP client lifetime.
// Single instance; access with App::instance() once run() has started.
class App {
public:
    explicit App(AppConfig config = {});
    ~App();

    static App& instance();

    // Initializes SDL/GL, runs the loop until quit, tears everything down. Returns exit code.
    int run(std::unique_ptr<AppDelegate> delegate);
    void requestQuit();

    const DisplayInfo& display() const { return display_; }
    SDL_Window* window() const { return window_; }
    SDL_GLContext glContext() const { return glContext_; }
    // For mpv_opengl_init_params::get_proc_address and GL loaders.
    static void* glProcAddress(void* ctx, const char* name);

    double time() const { return time_; }  // seconds since loop start
    uint64_t frameIndex() const { return frame_; }
    double fps() const { return fps_; }

private:
    bool initVideo();
    void shutdownVideo();
    void updateDisplay(bool force);
    void pumpEvents();
    void handleAppletEvents();

    AppConfig config_;
    struct AppletHook;
    std::unique_ptr<AppletHook> appletHook_;
    std::unique_ptr<AppDelegate> delegate_;
    std::unique_ptr<ThreadPool> workers_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    DisplayInfo display_;
    bool running_ = false;
    bool focused_ = true;
    double time_ = 0.0;
    double fps_ = 0.0;
    uint64_t frame_ = 0;
};

}  // namespace karafin
