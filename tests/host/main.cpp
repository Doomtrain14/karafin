// Host-side runner for Jellyfin, downloads and playback-control pure-logic tests.
// Built and run by tools/host-tests.ps1 inside a Linux GCC container; see tests/host/run.sh.

#include <cstdio>
#include <cmath>
#include <limits>
#include <string>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/settings.hpp"
#include "core/thread_pool.hpp"
#include "downloads/downloads.hpp"
#include "downloads/detail/manager_impl.hpp"
#include "jellyfin/jellyfin.hpp"
#include "player/swipe.hpp"
#include "player/up_next.hpp"

using namespace karafin;

namespace {

bool playbackControlsTest(std::string& report) {
    using player::PlaybackSwipe;
    using Control = PlaybackSwipe::Control;
    auto check = [&](bool ok, const char* message) {
        if (!ok) report += std::string(message) + "; ";
        return ok;
    };
    bool ok = true;
    PlaybackSwipe swipe;
    swipe.begin(200, 360, 1280, 720, false);
    ok &= check(swipe.move(201, 350) == Control::None, "touch slop");
    ok &= check(swipe.move(203, 300) == Control::Brightness, "left vertical swipe");
    ok &= check(std::fabs(swipe.value(144, 0.3f, 0.05f) - 0.8f) < 0.001f, "swipe up increases");
    ok &= check(swipe.value(1000, 0.3f, 0.05f) == 0.05f, "brightness minimum");
    ok &= check(swipe.value(-1000, 0.3f, 0.05f) == 1, "brightness maximum");
    ok &= check(swipe.move(900, 200) == Control::Brightness, "gesture side stays locked");
    swipe.cancel();
    ok &= check(swipe.move(200, 100) == Control::None, "cancelled gesture");
    swipe.begin(1000, 360, 1280, 720, false);
    ok &= check(swipe.move(1000, 500) == Control::Volume, "right vertical swipe");
    ok &= check(swipe.value(1000, 0.2f, 0) == 0, "volume can reach mute");
    ok &= check(swipe.value(-1000, 0.2f, 0) == 1, "volume cannot amplify above 100%");
    swipe.begin(200, 360, 1280, 720, true);
    ok &= check(swipe.move(200, 100) == Control::None, "seek bar and buttons exclude swipes");
    swipe.begin(200, 360, 1280, 720, false);
    ok &= check(swipe.move(300, 350) == Control::None, "horizontal drag excluded");
    ok &= check(swipe.move(300, 100) == Control::None, "horizontal drag cannot become adjustment");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    ok &= check(!swipe.begin(nan, 360, 1280, 720, false), "invalid start rejected");
    ok &= check(!swipe.begin(200, 360, 1280, 0, false), "zero height rejected");
    swipe.begin(1000, 360, 1280, 720, false);
    swipe.move(1000, 300);
    ok &= check(swipe.move(1000, nan) == Control::None, "invalid motion cancels active gesture");
    ok &= check(swipe.move(1000, 200) == Control::None, "invalid motion cannot resume gesture");
    swipe.begin(200, 360, 1280, 720, false);
    ok &= check(swipe.move(200, 300) == Control::Brightness, "new gesture after cancellation");

    player::UpNextTimer timer;
    timer.start(true);
    ok &= check(!timer.tick(3, true) && timer.remaining() == 7, "countdown advances");
    ok &= check(!timer.tick(60, false) && timer.remaining() == 7, "pause buffer focus and menus freeze countdown");
    ok &= check(!timer.tick(-1, true) && timer.remaining() == 7, "negative delta ignored");
    // EOF does not restart the timer: the same instance continues after playback stops.
    ok &= check(timer.tick(7, true) && timer.progress() == 1, "countdown expires after ten active seconds");
    ok &= check(!timer.tick(1, true), "autoplay only fires once");
    timer.start(true);
    timer.cancel();
    ok &= check(!timer.tick(20, true), "dismissal prevents autoplay");
    timer.start(false);
    ok &= check(timer.remaining() == 0 && !timer.tick(20, true), "disabled autoplay has no countdown");
    timer.start(true);
    ok &= check(timer.remaining() == 10 && !timer.tick(9, true), "next episode gets a fresh countdown");
    if (ok) report = "swipe controls and episode countdown";
    return ok;
}

bool downloadConcurrencyTest() {
    auto& settings = Settings::instance();
    downloads::DownloadManager::Impl manager;
    manager.loaded = true;  // Empty isolated queue; do not restore files from another self-test.
    bool ok = true;
    for (int n : {1, 2, 3, 4}) {
        settings.set("downloads.concurrency", n);
        ok &= manager.concurrencySetting() == n;
    }
    settings.set("downloads.concurrency", 9);
    ok &= manager.concurrencySetting() == 4;
    settings.set("downloads.concurrency", 0);
    ok &= manager.concurrencySetting() == 1;
    downloads::DownloadManager::instance().setConcurrency(4);
    ok &= downloads::DownloadManager::instance().concurrency() == 4;
    downloads::DownloadManager::instance().setConcurrency(5);
    ok &= downloads::DownloadManager::instance().concurrency() == 4;
    {
        std::lock_guard lock(manager.mutex);
        manager.startLocked();
        ok &= manager.slots.size() == 4;
        manager.stopping = true;
        manager.cv.notify_all();
    }
    for (auto& slot : manager.slots) slot.join();
    {
        std::lock_guard lock(manager.ioMutex);
        manager.ioStop = true;
        manager.ioCv.notify_all();
    }
    manager.ioThread.join();
    settings.set("downloads.concurrency", 1);
    return ok;
}

}  // namespace

int main() {
    log::init("host-test.log");
    log::setStdoutMirror(true);
    log::setLevel(log::Level::Info);
    main_thread::bindCurrentThread();
    Settings::instance().load("host-settings.json");

    ThreadPool pool(2, "pool");
    detail::setGlobalWorkers(&pool);

    std::string jf, dl, playback;
    const bool jfOk = jellyfin::selfTest(&jf);
    const bool dlOk = downloads::selfTest(&dl);
    const bool playbackOk = playbackControlsTest(playback);
    const bool concurrencyOk = downloadConcurrencyTest();
    main_thread::runPending(1000);

    pool.shutdown();
    detail::setGlobalWorkers(nullptr);
    log::shutdown();

    std::printf("jellyfin::selfTest  %s  %s\n", jfOk ? "PASS" : "FAIL", jf.c_str());
    std::printf("downloads::selfTest %s  %s\n", dlOk ? "PASS" : "FAIL", dl.c_str());
    std::printf("playback controls   %s  %s\n", playbackOk ? "PASS" : "FAIL", playback.c_str());
    std::printf("download workers    %s  1-4 settings, bounds and four live worker slots\n", concurrencyOk ? "PASS" : "FAIL");
    return jfOk && dlOk && playbackOk && concurrencyOk ? 0 : 1;
}
