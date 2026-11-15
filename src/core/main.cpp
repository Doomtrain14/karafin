// Karafin entry point: platform + logging + settings bring-up, then the App loop driven by the
// UI host (ui::Root) starting at the splash screen. The download manager lives alongside the UI
// host: started once workers/HTTP are up, shut down before the HTTP client stops. On quit the
// player gets a bounded chance to deliver its final playback report first.

#include <cstdlib>
#include <exception>
#include <memory>

#include "core/app.hpp"
#include "core/fs.hpp"
#include "core/libinfo.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "core/thread_pool.hpp"
#include "downloads/manager.hpp"
#include "player/player.hpp"
#include "screens/splash_screen.hpp"
#include "ui/root.hpp"

using namespace karafin;

namespace {

class AppHost final : public AppDelegate {
public:
    explicit AppHost(ui::Root::ScreenFactory initial) : root_(std::make_unique<ui::Root>(std::move(initial))) {}

    void onStart() override {
        root_->onStart();
        downloads::DownloadManager::instance().start();
    }
    bool onEvent(const SDL_Event& event) override { return root_->onEvent(event); }
    void onUpdate(double dt) override {
        // clkrst IPC costs about 1 ms; keep it off the frame.
        clockTimer_ += dt;
        if (clockTimer_ >= 1.0) {
            clockTimer_ = 0;
            workers().post([] { platform::tickPerformanceClocks(); });
        }
        root_->onUpdate(dt);
    }
    void onRender() override { root_->onRender(); }
    void onDisplayChanged(const DisplayInfo& display) override {
        workers().post([] { platform::refreshPerformanceClocks(); });
        root_->onDisplayChanged(display);
    }
    void onFocusChanged(bool focused) override { root_->onFocusChanged(focused); }
    void onResume() override {
        workers().post([] { platform::refreshPerformanceClocks(); });
    }
    void onStop() override {
        // Order matters: the player's final Stopped report needs the HTTP client (stopped by App
        // after onStop) and the UI; downloads must stop before the HTTP client; the UI goes last.
        player::shutdown(2000);
        downloads::DownloadManager::instance().shutdown();
        root_->onStop();
    }

private:
    std::unique_ptr<ui::Root> root_;
    double clockTimer_ = 0;
};

}  // namespace

int main(int, char**) {
    platform::init();

    const bool sdOk = paths::ensureAppDirs();
    log::init(paths::logFile());
    if (!sdOk) log::error("main", "cannot create {}", paths::dataDir());
    // An uncaught exception otherwise closes the app with nothing in log.txt.
    std::set_terminate([] {
        try {
            if (auto e = std::current_exception()) std::rethrow_exception(e);
            log::error("main", "std::terminate without an active exception");
        } catch (const std::exception& e) {
            log::error("main", "uncaught exception: {}", e.what());
        } catch (...) {
            log::error("main", "uncaught non-std exception");
        }
        log::flush();
        std::abort();
    });

    Settings::instance().load(paths::settingsFile());
    log::setLevel(log::parseLevel(Settings::instance().get("app.log_level", "info")));

    platform::setPerformanceClocks(Settings::instance().get<int>("performance.cpu_level", 2),
                                   Settings::instance().get<int>("performance.gpu_level", 2));

    log::info("main", "Karafin {} | {}", KARAFIN_VERSION, platform::describe());
    log::info("main", "libs: {}", libinfo::versions());
    log::info("main", "ffmpeg hwdevices: {}", libinfo::hwdecTypes());
    if (auto marker = fs::readFile(paths::romfs("karafin.txt"))) log::info("main", "romfs ok ({} bytes marker)", marker->size());
    if (platform::isAppletMode()) log::warn("main", "running in applet mode; launch via title override for playback");

    int rc = 0;
    {
        App app;
        // The factory runs inside App::run (workers, HTTP and GL are up by then).
        rc = app.run(std::make_unique<AppHost>([] {
            // A throwaway mpv core costs threads and memory at startup; only for diagnostics.
            if (log::enabled(log::Level::Debug)) {
                workers().post([] {
                    const bool ok = libinfo::probeMpv();
                    main_thread::post([ok] { log::info("main", "mpv probe {}", ok ? "ok" : "FAILED"); });
                });
            }
            return std::make_unique<screens::SplashScreen>(true);
        }));
    }

    log::info("main", "exit code {}", rc);
    log::shutdown();
    platform::shutdown();
    return rc;
}
