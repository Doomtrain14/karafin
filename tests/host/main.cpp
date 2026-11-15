// Host-side runner for Jellyfin, downloads and playback-control pure-logic tests.
// Built and run by tools/host-tests.ps1 inside a Linux GCC container; see tests/host/run.sh.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/settings.hpp"
#include "core/thread_pool.hpp"
#include "downloads/downloads.hpp"
#include "downloads/detail/manager_impl.hpp"
#include "jellyfin/jellyfin.hpp"
#include "player/media_tags.hpp"
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
    ok &= check(swipe.value(-1000, 0.2f, 0) == 1, "volume swipe range is capped");
    ok &= check(PlaybackSwipe::volumePercent(1) == 200 && PlaybackSwipe::volumePercent(0.5f) == 100,
                "volume swipe spans 0-200%");
    ok &= check(PlaybackSwipe::volumeFraction(150) == 0.75f && PlaybackSwipe::volumeFraction(500) == 1 &&
                    PlaybackSwipe::volumeFraction(std::nan("")) == 0,
                "player volume maps back onto the swipe range");
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
    swipe.begin(200, 360, 1280, 720, false, false);
    ok &= check(swipe.move(200, 300) == Control::Volume && swipe.left(), "docked left swipe controls volume");
    swipe.begin(1000, 360, 1280, 720, false, false);
    ok &= check(swipe.move(1000, 300) == Control::Volume && !swipe.left(), "docked right swipe controls volume");

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

    ok &= check(player::videoCodecLabel("hevc", "p010") == "HEVC 10-bit", "hardware 10-bit HEVC tag");
    ok &= check(player::videoCodecLabel("h264", "nv12") == "H.264", "nv12 is 8-bit");
    ok &= check(player::videoCodecLabel("vp9", "yuv420p10le") == "VP9 10-bit", "software 10-bit tag");
    ok &= check(player::videoCodecLabel("", "yuv420p").empty(), "no video, no tag");
    ok &= check(player::resolutionLabel(3840, 1600) == "4K", "letterboxed 4K");
    ok &= check(player::resolutionLabel(1920, 800) == "1080p", "letterboxed 1080p");
    ok &= check(player::resolutionLabel(720, 480) == "480p", "SD resolution");
    ok &= check(player::resolutionLabel(0, 0).empty(), "unknown resolution");
    ok &= check(player::audioCodecLabel("eac3", 6) == "EAC3 5.1", "surround audio tag");
    ok &= check(player::audioCodecLabel("aac", 2) == "AAC Stereo", "stereo audio tag");
    ok &= check(player::audioCodecLabel("dts", 8) == "DTS 7.1", "DTS 7.1 tag");
    ok &= check(player::audioCodecLabel("pcm_s16le", 0) == "PCM", "PCM without channel count");
    if (ok) report = "swipe controls, episode countdown and stream tags";
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
        // Threads follow the setting: two at start, more once it is raised, none removed after.
        std::lock_guard lock(manager.mutex);
        settings.set("downloads.concurrency", 2);
        manager.startLocked();
        ok &= manager.slots.size() == 2;
        settings.set("downloads.concurrency", 4);
        manager.ensureSlotsLocked();
        ok &= manager.slots.size() == 4;
        settings.set("downloads.concurrency", 1);
        manager.ensureSlotsLocked();
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

// Must run last: it closes the main queue.
bool mainQueueBoundTest(std::string& report) {
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    auto check = [&](bool ok, const char* message) {
        if (!ok) report += std::string(message) + "; ";
        return ok;
    };
    auto waitFor = [](auto&& done) {
        const auto until = Clock::now() + 5s;
        while (!done() && Clock::now() < until) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(50ms);
    };
    main_thread::runPending(1000);
    bool ok = check(main_thread::pendingCount() == 0, "queue starts empty");

    constexpr int kTasks = 1500;
    std::atomic<int> posted{0};
    int ran = 0;
    std::thread producer([&] {
        for (int i = 0; i < kTasks; ++i) {
            main_thread::post([&ran] { ++ran; });
            posted.fetch_add(1);
        }
    });
    waitFor([&] { return posted.load() >= 1024; });
    ok &= check(posted.load() == 1024 && main_thread::pendingCount() == 1024, "producer blocks at 1024 pending");
    size_t maxPending = 0;
    const auto until = Clock::now() + 5s;
    while (ran < kTasks && Clock::now() < until) {
        maxPending = std::max(maxPending, main_thread::pendingCount());
        main_thread::runPending(1);
    }
    if (ran < kTasks) main_thread::close();
    producer.join();
    ok &= check(ran == kTasks, "every blocked post runs once drained");
    ok &= check(maxPending <= 1024, "queue never exceeds 1024 posts from other threads");

    std::atomic<bool> filled{false};
    std::thread filler([&] {
        for (int i = 0; i < 1025; ++i) main_thread::post([] {});
        filled.store(true);
    });
    waitFor([] { return main_thread::pendingCount() >= 1024; });
    ok &= check(!filled.load() && main_thread::pendingCount() == 1024, "1025th post waits");
    main_thread::post([] {});
    main_thread::postNoWait([] {});
    ok &= check(main_thread::pendingCount() == 1026, "main thread and postNoWait bypass the bound");
    main_thread::close();
    filler.join();
    ok &= check(filled.load() && main_thread::pendingCount() == 1026, "close wakes and drops blocked posts");
    main_thread::post([] {});
    ok &= check(main_thread::pendingCount() == 1026, "posts after close are dropped");
    main_thread::clear();
    if (ok) report = "1024 cap, blocking producers, main-thread bypass and close";
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
    std::string queue;
    const bool queueOk = mainQueueBoundTest(queue);

    pool.shutdown();
    detail::setGlobalWorkers(nullptr);
    log::shutdown();

    std::printf("jellyfin::selfTest  %s  %s\n", jfOk ? "PASS" : "FAIL", jf.c_str());
    std::printf("downloads::selfTest %s  %s\n", dlOk ? "PASS" : "FAIL", dl.c_str());
    std::printf("playback controls   %s  %s\n", playbackOk ? "PASS" : "FAIL", playback.c_str());
    std::printf("download workers    %s  1-4 settings, bounds and four live worker slots\n", concurrencyOk ? "PASS" : "FAIL");
    std::printf("main queue          %s  %s\n", queueOk ? "PASS" : "FAIL", queue.c_str());
    return jfOk && dlOk && playbackOk && concurrencyOk && queueOk ? 0 : 1;
}
