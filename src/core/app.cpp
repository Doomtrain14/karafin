#include "core/app.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include <GLES3/gl3.h>
#include <switch.h>

#include "core/http.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/perf.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "core/thread.hpp"
#include "core/thread_pool.hpp"

namespace karafin {
namespace {
App* g_app = nullptr;

constexpr double kRefreshPeriod = 1.0 / 60.0;
// A long stall (shader compile, sleep) must not make every animation jump; past this they slow
// down instead.
constexpr double kMaxDt = 0.05;

// Measured frame intervals jitter by a millisecond or two around vsync; feeding that jitter into
// springs and scroll positions shows up as uneven motion. Intervals close to a whole number of
// refresh periods are snapped to it.
double animationDt(double raw) {
    const double periods = std::round(raw / kRefreshPeriod);
    double dt = raw;
    if (periods >= 1.0 && std::fabs(raw - periods * kRefreshPeriod) < kRefreshPeriod * 0.2)
        dt = periods * kRefreshPeriod;
    return std::clamp(dt, 0.0, kMaxDt);
}
}

// libnx applet hooks run inside appletMainLoop(), i.e. inside SDL_PollEvent on the main thread.
// SDL pumps once per SDL_PollEvent call, so a focus-lost/focus-gained pair can both be processed
// before the loop polls appletGetFocusState(); counting the hook calls catches that.
struct App::AppletHook {
    AppletHookCookie cookie{};
    std::atomic<int> focusEvents{0};
    std::atomic<int> resumeEvents{0};

    static void callback(AppletHookType hook, void* param) {
        auto* self = static_cast<AppletHook*>(param);
        if (hook == AppletHookType_OnFocusState) self->focusEvents.fetch_add(1);
        else if (hook == AppletHookType_OnResume) self->resumeEvents.fetch_add(1);
    }
};

App::App(AppConfig config) : config_(config) { g_app = this; }

App::~App() {
    if (appletHook_) appletUnhook(&appletHook_->cookie);
    if (g_app == this) g_app = nullptr;
}

App& App::instance() { return *g_app; }

void* App::glProcAddress(void*, const char* name) { return SDL_GL_GetProcAddress(name); }

void App::requestQuit() { running_ = false; }

bool App::initVideo() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        log::error("app", "SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const bool docked = platform::isDocked();
    const int w = docked ? 1920 : 1280;
    const int h = docked ? 1080 : 720;
    window_ = SDL_CreateWindow("Karafin", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_OPENGL);
    if (!window_) {
        log::error("app", "SDL_CreateWindow failed: {}", SDL_GetError());
        return false;
    }
    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        log::error("app", "SDL_GL_CreateContext failed: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, glContext_);
    if (SDL_GL_SetSwapInterval(config_.vsync ? 1 : 0) != 0) log::warn("app", "vsync unavailable: {}", SDL_GetError());

    log::info("app", "GL_VENDOR={} GL_RENDERER={} GL_VERSION={}",
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    for (int i = 0; i < SDL_NumJoysticks(); ++i)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);

    updateDisplay(true);
    return true;
}

void App::shutdownVideo() {
    if (glContext_) SDL_GL_DeleteContext(glContext_);
    if (window_) SDL_DestroyWindow(window_);
    glContext_ = nullptr;
    window_ = nullptr;
    SDL_Quit();
}

void App::updateDisplay(bool force) {
    const bool docked = platform::isDocked();
    if (!force && docked == display_.docked) return;

    const int wantW = docked ? 1920 : 1280;
    const int wantH = docked ? 1080 : 720;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (w != wantW || h != wantH) SDL_SetWindowSize(window_, wantW, wantH);

    SDL_GL_GetDrawableSize(window_, &w, &h);
    if (w <= 0 || h <= 0) {
        w = wantW;
        h = wantH;
    }
    // SDL_SetWindowSize above also queues SDL_WINDOWEVENT_SIZE_CHANGED, which forces another
    // call; only a real change reaches the delegate.
    const bool changed = w != display_.pixelWidth || h != display_.pixelHeight || docked != display_.docked;
    display_.pixelWidth = w;
    display_.pixelHeight = h;
    display_.scale = static_cast<float>(h) / display_.logicalHeight;
    display_.docked = docked;
    if (!changed && frame_ > 0) return;
    log::info("app", "display {}x{} scale {:.2f} ({})", w, h, display_.scale, docked ? "docked" : "handheld");
    if (delegate_) delegate_->onDisplayChanged(display_);
}

void App::pumpEvents() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT: running_ = false; break;
            case SDL_CONTROLLERDEVICEADDED:
                if (SDL_IsGameController(ev.cdevice.which)) SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) updateDisplay(true);
                break;
            default: break;
        }
        if (delegate_) delegate_->onEvent(ev);
    }
}

void App::handleAppletEvents() {
    const bool focused = platform::hasFocus();
    const bool focusEvents = appletHook_ && appletHook_->focusEvents.exchange(0) > 0;
    if (focused != focused_) {
        focused_ = focused;
        log::info("app", "focus {}", focused ? "gained" : "lost");
        delegate_->onFocusChanged(focused);
    } else if (focused && focusEvents) {
        log::info("app", "focus lost and regained");
        delegate_->onFocusChanged(false);
        delegate_->onFocusChanged(true);
    }

    if (appletHook_ && appletHook_->resumeEvents.exchange(0) > 0) {
        log::info("app", "resumed from sleep (network {})", platform::networkConnected() ? "up" : "down");
        http::Client::instance().resetConnections();
        delegate_->onResume();
    }
}

int App::run(std::unique_ptr<AppDelegate> delegate) {
    delegate_ = std::move(delegate);
    main_thread::bindCurrentThread();
    setCurrentThreadName("main");

    workers_ = std::make_unique<ThreadPool>(static_cast<size_t>(std::max(1, config_.workerThreads)), "pool");
    detail::setGlobalWorkers(workers_.get());
    http::Client::instance().start();

    if (!initVideo()) {
        http::Client::instance().stop();
        detail::setGlobalWorkers(nullptr);
        workers_.reset();
        shutdownVideo();
        return 1;
    }

    appletHook_ = std::make_unique<AppletHook>();
    appletHook(&appletHook_->cookie, &AppletHook::callback, appletHook_.get());
    // AppletMessage_Resume (wake from sleep) is opt-in; only applications may enable it.
    if (R_FAILED(appletSetRestartMessageEnabled(true))) log::debug("app", "resume notification unavailable");

    running_ = true;
    {
        s32 prio = 0;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        log::info("app", "main thread on core {} priority {:#x}", svcGetCurrentProcessorNumber(), prio);
    }
    delegate_->onStart();

    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    uint64_t last = SDL_GetPerformanceCounter();
    double fpsAccum = 0.0;
    int fpsFrames = 0;
    auto lastSwap = perf::Clock::now();
    auto settingsSaving = std::make_shared<std::atomic<bool>>(false);

    while (running_) {
        const uint64_t now = SDL_GetPerformanceCounter();
        const double raw = static_cast<double>(now - last) / freq;
        last = now;
        const double dt = animationDt(raw);
        time_ += dt;

        fpsAccum += raw;
        if (++fpsFrames >= 30) {
            fps_ = fpsFrames / fpsAccum;
            fpsAccum = 0.0;
            fpsFrames = 0;
        }

        perf::Frame& pf = perf::frame();
        const auto frameStart = perf::Clock::now();
        pumpEvents();
        if (!running_) break;

        handleAppletEvents();
        updateDisplay(false);
        pf.eventsMs = perf::msSince(frameStart);

        auto t = perf::Clock::now();
        pf.tasks = static_cast<int>(main_thread::runPending());
        pf.tasksMs = perf::msSince(t);

        t = perf::Clock::now();
        delegate_->onUpdate(dt);
        pf.updateMs = perf::msSince(t);

        t = perf::Clock::now();
        glViewport(0, 0, display_.pixelWidth, display_.pixelHeight);
        delegate_->onRender();
        pf.renderMs = perf::msSince(t);

        if (Settings::instance().saveDue() && !settingsSaving->exchange(true)) {
            workers_->post([settingsSaving] {
                Settings::instance().save();
                settingsSaving->store(false);
            });
        }
        if (++frame_ % 60 == 0) log::flush();
        pf.cpuMs = perf::msSince(frameStart);

        t = perf::Clock::now();
        SDL_GL_SwapWindow(window_);
        const auto swapped = perf::Clock::now();
        pf.swapMs = std::chrono::duration<double, std::milli>(swapped - t).count();
        pf.frameMs = std::chrono::duration<double, std::milli>(swapped - lastSwap).count();
        lastSwap = swapped;
        perf::endFrame();
    }

    log::info("app", "leaving main loop after {} frames", frame_);
    // Threads joined during shutdown may be blocked posting to a full queue.
    main_thread::close();
    delegate_->onStop();
    delegate_.reset();
    appletUnhook(&appletHook_->cookie);
    appletHook_.reset();

    http::Client::instance().stop();
    main_thread::clear();
    workers_->shutdown();
    detail::setGlobalWorkers(nullptr);
    workers_.reset();
    Settings::instance().save();
    shutdownVideo();
    return 0;
}

}  // namespace karafin
