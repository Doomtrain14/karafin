#include "screens/settings_section.hpp"

#include <algorithm>
#include <format>
#include <memory>

#include "core/http.hpp"
#include "core/libinfo.hpp"
#include "core/log.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "downloads/manager.hpp"
#include "gfx/image.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/library_lock.hpp"
#include "screens/session.hpp"
#include "ui/root.hpp"
#include "ui/sound.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;
namespace dl = karafin::downloads;
using Json = nlohmann::json;

namespace {

constexpr float kLeftX = theme::layout::marginX - 8;
constexpr float kLeftW = 300;
constexpr float kRightX = kLeftX + kLeftW + 34;
constexpr float kTop = 110;

const char* categoryName(int c) {
    static const char* names[] = {"Account", "Playback", "Subtitles", "Downloads", "Interface", "About"};
    return c >= 0 && c < 6 ? names[c] : "";
}

const char* categoryBlurb(int c) {
    static const char* text[] = {
        "Server, user and sign-in.",
        "Streaming quality and player behaviour. Changes apply from the next video.",
        "Default subtitle language, mode and style. Changes apply from the next video.",
        "Offline downloads and SD card storage.",
        "Image cache, network and diagnostics.",
        "Version and system information.",
    };
    return c >= 0 && c < 6 ? text[c] : "";
}

gfx::Icon categoryIcon(int c) {
    static const gfx::Icon icons[] = {gfx::Icon::User,     gfx::Icon::Play,     gfx::Icon::Subtitles,
                                      gfx::Icon::Download, gfx::Icon::Settings, gfx::Icon::Info};
    return c >= 0 && c < 6 ? icons[c] : gfx::Icon::None;
}

std::vector<std::pair<std::string, std::string>> languages() {
    return {{"eng", "English"},  {"jpn", "Japanese"}, {"fre", "French"},   {"ger", "German"},     {"spa", "Spanish"},
            {"ita", "Italian"},  {"por", "Portuguese"}, {"rus", "Russian"}, {"chi", "Chinese"},   {"kor", "Korean"},
            {"dut", "Dutch"},    {"swe", "Swedish"},  {"nor", "Norwegian"}, {"dan", "Danish"},     {"fin", "Finnish"},
            {"pol", "Polish"},   {"tur", "Turkish"},  {"ara", "Arabic"},   {"hin", "Hindi"},      {"tha", "Thai"},
            {"vie", "Vietnamese"}, {"ind", "Indonesian"}, {"ukr", "Ukrainian"}, {"cze", "Czech"}, {"hun", "Hungarian"},
            {"gre", "Greek"},    {"heb", "Hebrew"}};
}

std::string bitrateLabel(int64_t bps) {
    if (bps >= 1000000) {
        const double m = static_cast<double>(bps) / 1e6;
        return m == static_cast<double>(static_cast<int64_t>(m)) ? std::format("{} Mbps", static_cast<int64_t>(m))
                                                                  : std::format("{:.1f} Mbps", m);
    }
    return std::format("{} kbps", bps / 1000);
}

}  // namespace

SettingsSection::SettingsSection(MainMode mode) : mode_(mode) {
    categories_ = add<ListMenu>();
    categories_->setFrame({kLeftX, kTop, kLeftW, theme::layout::screenHeight - kTop - 56});
    std::vector<ListItem> cats;
    for (int c = 0; c < static_cast<int>(Category::Count); ++c)
        cats.push_back({categoryName(c), "", "", categoryIcon(c), false, true,
                        [this] { focus().setFocus(items_->defaultFocus()); }});
    categories_->setItems(std::move(cats), 0);

    paneTitle_ = add<Label>("", theme::type::title1);
    paneTitle_->setFrame({kRightX + 10, kTop + 2, 700, 36});
    paneSubtitle_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    paneSubtitle_->setFrame({kRightX + 10, kTop + 40, 760, 22});
    items_ = add<ListMenu>();
    items_->setFrame({kRightX, kTop + 74, theme::layout::screenWidth - kRightX - theme::layout::marginX + 8,
                      theme::layout::screenHeight - kTop - 74 - 50});
    items_->onSelect = [this](size_t index) {
        lastIndex_ = index;
        restoreFocus_ = true;
    };
    setRememberedFocus(categories_);
    showCategory(Category::Account);
}

void SettingsSection::onSectionShown(bool first) {
    (void)first;
    restoreFocus_ = false;
    refresh();
}

void SettingsSection::onFocusWithin(View* descendant) {
    SectionView::onFocusWithin(descendant);
    // Moving through the category list switches the right pane.
    Box* rows = categories_->content();
    for (View* v = descendant; v && v != this; v = v->parent()) {
        if (v->parent() == rows) {
            const size_t idx = rows->indexOf(v);
            if (idx < static_cast<size_t>(Category::Count) && static_cast<Category>(idx) != current_)
                showCategory(static_cast<Category>(idx));
            return;
        }
    }
}

void SettingsSection::showCategory(Category c, bool keepFocus) {
    current_ = c;
    paneTitle_->setText(categoryName(static_cast<int>(c)));
    paneSubtitle_->setText(categoryBlurb(static_cast<int>(c)));
    size_t focusIdx = SIZE_MAX;
    if (keepFocus && restoreFocus_) {
        focusIdx = lastIndex_;
    } else if (keepFocus && items_->hasFocusWithin()) {
        Box* rows = items_->content();
        for (size_t i = 0; i < rows->childCount(); ++i)
            if (rows->childAt(i)->hasFocusWithin()) focusIdx = i;
    }
    restoreFocus_ = false;
    items_->setItems(buildItems(c), focusIdx == SIZE_MAX ? 0 : static_cast<int>(focusIdx));
    if (focusIdx != SIZE_MAX && items_->itemCount() > 0) {
        const size_t idx = std::min(focusIdx, items_->itemCount() - 1);
        // After a side sheet closes the focus scope switches back at the end of the frame.
        timeline().after(0.05f, [this, idx] {
            if (idx < items_->itemCount()) items_->focusItem(idx);
        }, lifetime());
    }
    if (!keepFocus) {
        items_->alpha.set(0.0f);
        items_->alpha.tweenTo(1.0f, 0.25f, ease::outCubic);
        items_->offset.set({16, 0});
        items_->offset.springTo({0, 0}, theme::motion::standard);
        items_->scrollTo(0, false);
    }
}

void SettingsSection::refresh() {
    // Deferred: list rows must not be destroyed while one of them is still activating.
    timeline().after(0.0f, [this] { showCategory(current_, true); }, lifetime());
}

ListItem SettingsSection::choice(const std::string& title, const std::string& key, std::vector<Option> options,
                                 const Json& fallback, std::function<void(const Json&)> onChanged, std::string subtitle) {
    Json current = Settings::instance().getJson(key);
    if (current.is_null()) current = fallback;
    std::string detail = current.is_string() ? current.get<std::string>() : current.dump();
    int checked = -1;
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].value == current) {
            detail = options[i].label;
            checked = static_cast<int>(i);
        }
    auto opts = std::make_shared<std::vector<Option>>(std::move(options));
    return {title, std::move(subtitle), detail, gfx::Icon::None, false, true,
            [this, title, key, opts, checked, onChanged] {
                std::vector<ListItem> items;
                for (const auto& o : *opts) {
                    const Json value = o.value;
                    items.push_back({o.label, o.subtitle, "", gfx::Icon::None, false, true, [this, key, value, onChanged] {
                                         Settings::instance().setJson(key, value);
                                         if (onChanged) onChanged(value);
                                         refresh();
                                     }});
                }
                if (checked >= 0) items[static_cast<size_t>(checked)].checked = true;
                present(SideSheet::make(title, std::move(items), checked));
            }};
}

ListItem SettingsSection::toggle(const std::string& title, const std::string& key, bool fallback,
                                 std::function<void(bool)> onChanged, std::string subtitle) {
    const bool on = Settings::instance().get<bool>(key, fallback);
    return {title, std::move(subtitle), on ? "On" : "Off", gfx::Icon::None, on, true, [this, key, on, onChanged] {
                Settings::instance().set(key, !on);
                if (onChanged) onChanged(!on);
                refresh();
            }};
}

std::vector<ListItem> SettingsSection::buildItems(Category c) {
    std::vector<ListItem> out;
    auto client = jf::currentClient();
    switch (c) {
        case Category::Account: {
            if (mode_ == MainMode::Offline) {
                out.push_back({"Offline mode", "Only downloaded titles are available until the server is reachable", "",
                               gfx::Icon::Wifi, false, true, {}});
                out.push_back({"Reconnect", "Try to reach the server again", "", gfx::Icon::Refresh, false, true,
                               [] { restartSession(); }});
                out.push_back({"Change server", "", "", gfx::Icon::Server, false, true, [] { goToServerSelect(); }});
                break;
            }
            if (client) {
                const auto info = client->serverInfo();
                out.push_back({info.name.empty() ? "Server" : info.name, client->baseUrl(),
                               info.version.empty() ? "" : "Jellyfin " + info.version, gfx::Icon::Server, false, true, {}});
                const auto user = client->user();
                out.push_back({"Signed in as " + user.name, user.policy.isAdministrator ? "Administrator" : "",
                               user.canDownload() ? "" : "Downloads disabled", gfx::Icon::User, false, true, {}});
                const std::string sid = client->serverId();
                out.push_back({"Switch user", "Pick another account on this server", "", gfx::Icon::User, false, true,
                               [sid] { goToLogin(sid); }});
                if (user.policy.isAdministrator)
                    out.push_back({"Scan all libraries", "Ask the server to look for new and changed files", "",
                                   gfx::Icon::Refresh, false, true, [] { scanLibrary({}, {}); }});
                LibraryLock& lock = LibraryLock::instance();
                const bool hasLock = lock.hasPassword();
                out.push_back({hasLock ? "Change lock password" : "Library lock",
                               hasLock ? "Lock or unlock a library with X in the Libraries tab"
                                       : "Set a password to hide libraries (X on a library in the Libraries tab)",
                               hasLock ? "On" : "Off", gfx::Icon::Lock, hasLock, true,
                               [this] { LibraryLock::instance().changePassword(guard([this] { refresh(); })); }});
                if (hasLock) {
                    const bool open = lock.unlockedThisSession();
                    out.push_back({"Lock now", open ? "Hide locked libraries again" : "Locked libraries are hidden", "",
                                   gfx::Icon::Lock, false, open, [this] {
                                       LibraryLock::instance().lockNow();
                                       refresh();
                                   }});
                    out.push_back({"Turn off library lock", "Removes the password and every lock", "", gfx::Icon::Close,
                                   false, true,
                                   [this] { LibraryLock::instance().removePassword(guard([this] { refresh(); })); }});
                }
            }
            out.push_back({"Change server", "Add or pick another Jellyfin server", "", gfx::Icon::Server, false, true,
                           [] { goToServerSelect(); }});
            out.push_back({"Sign out", "Forget this device's sign-in for the current user", "", gfx::Icon::Exit, false,
                           true, [] {
                               present(Dialog::confirm("Sign out?", "You'll need your password or Quick Connect to sign in again.",
                                                       "Sign out", "Cancel", [](bool ok) {
                                                           if (ok) signOut();
                                                       }));
                           }});
            break;
        }
        case Category::Playback: {
            std::vector<Option> rates;
            for (int64_t bps : {120000000LL, 80000000LL, 60000000LL, 40000000LL, 30000000LL, 20000000LL, 15000000LL,
                                10000000LL, 8000000LL, 6000000LL, 4000000LL, 3000000LL, 2000000LL, 1500000LL, 720000LL})
                rates.push_back({bitrateLabel(bps), bps, bps == 120000000LL ? "Maximum - direct play whenever possible" : ""});
            out.push_back(choice("Max streaming bitrate", "jellyfin.max_streaming_bitrate", std::move(rates), 120000000LL,
                                 {}, "Lower it on slow Wi-Fi; the server transcodes above this"));
            out.push_back(choice("Max audio channels", "jellyfin.max_audio_channels",
                                 {{"Stereo", 2, ""}, {"5.1 surround", 6, ""}, {"7.1 surround", 8, ""}}, 8));
            out.push_back(toggle("Allow AV1", "jellyfin.enable_av1", true, {}, "Software decoded; up to 1080p"));
            const auto applyClocks = [](const Json&) {
                platform::setPerformanceClocks(Settings::instance().get<int>("performance.cpu_level", 2),
                                               Settings::instance().get<int>("performance.gpu_level", 2));
            };
            out.push_back(choice("CPU clock", "performance.cpu_level",
                                 {{"System", 0, "Whatever the console or sys-clk sets"},
                                  {"1428 MHz", 1, ""},
                                  {"1785 MHz", 2, "Default - smoothest, uses more battery"}},
                                 2, applyClocks, "Higher keeps menus and software decoding smooth"));
            out.push_back(choice("GPU clock", "performance.gpu_level",
                                 {{"System", 0, "307 MHz handheld, 768 MHz docked"},
                                  {"460 MHz", 1, ""},
                                  {"768 MHz", 2, "Default - smoothest, uses more battery"}},
                                 2, applyClocks, "Higher keeps animations and video drawing smooth"));
            out.push_back(toggle("Hardware decoding", "player.hwdec", true, {}, "NVDEC via nvtegra (recommended)"));
            std::vector<Option> audio{{"File default", "", "Use the track marked as default"}};
            for (const auto& [code, name] : languages()) audio.push_back({name, code, ""});
            out.push_back(choice("Preferred audio language", "player.audio_lang", std::move(audio), ""));
            out.push_back(toggle("Autoplay next episode", "player.autoplay_next", true));
            out.push_back(choice("Skip intro and credits", "player.skip_segments",
                                 {{"Show a Skip button", "ask", ""}, {"Skip automatically", "auto", ""}, {"Off", "off", ""}},
                                 "ask", {}, "Uses media segments from the server (10.10+)"));
            out.push_back(choice("Seek step", "player.seek_step",
                                 {{"5 seconds", 5, ""}, {"10 seconds", 10, ""}, {"15 seconds", 15, ""}, {"30 seconds", 30, ""}},
                                 10, {}, "D-pad left / right"));
            out.push_back(choice("Skip amount", "player.skip_seconds",
                                 {{"5 seconds", 5, ""},
                                  {"10 seconds", 10, ""},
                                  {"15 seconds", 15, ""},
                                  {"30 seconds", 30, ""},
                                  {"60 seconds", 60, ""}},
                                 10, {}, "How far each skip jumps"));
            out.push_back(choice("Skip with", "player.skip_input",
                                 {{"Double-tap only", "tap", "Default - double-tap the left or right side of the screen"},
                                  {"Buttons only", "buttons", "L / R, and ZL / ZR when a video has no chapters"},
                                  {"Buttons and double-tap", "both", "Both ways skip"}},
                                 "tap", {}, "What skips back or forward during playback"));
            break;
        }
        case Category::Subtitles: {
            out.push_back(choice("Subtitle mode", "player.sub_mode",
                                 {{"Default", "default", "Your language when the audio is in another one, else forced"},
                                  {"Always on", "always", "Pick your language whenever available"},
                                  {"Forced only", "forced", "Signs and foreign dialogue"},
                                  {"Off", "off", ""}},
                                 "default"));
            std::vector<Option> langs;
            for (const auto& [code, name] : languages()) langs.push_back({name, code, ""});
            out.push_back(choice("Preferred language", "player.sub_lang", std::move(langs), "eng"));
            out.push_back(choice("Text size", "player.sub_scale",
                                 {{"Small", 0.75, ""}, {"Medium small", 0.875, ""}, {"Normal", 1.0, ""},
                                  {"Large", 1.15, ""}, {"Larger", 1.3, ""}, {"Huge", 1.5, ""}},
                                 1.0));
            out.push_back(choice("Position", "player.sub_pos",
                                 {{"Bottom", 100, ""}, {"Slightly raised", 95, ""}, {"Raised", 90, ""}, {"High", 80, ""},
                                  {"Very high", 70, ""}},
                                 100));
            out.push_back(toggle("Background box", "player.sub_bg", false, {}, "Dark box behind text subtitles"));
            break;
        }
        case Category::Downloads: {
            auto& dm = dl::DownloadManager::instance();
            out.push_back(choice("Simultaneous downloads", "downloads.concurrency",
                                 {{"1", 1, "Recommended"}, {"2", 2, ""}, {"3", 3, ""}, {"4", 4, ""}}, 1,
                                 [](const Json& v) { dl::DownloadManager::instance().setConcurrency(v.get<int>()); }));
            out.push_back(toggle("Resume downloads on launch", "downloads.auto_resume", false,
                                 [](bool v) { dl::DownloadManager::instance().setAutoResume(v); },
                                 "Otherwise unfinished downloads come back paused"));
            out.push_back(toggle("Keep screen on while downloading", "downloads.keep_screen_on", true,
                                 [](bool v) { dl::DownloadManager::instance().setKeepScreenOn(v); },
                                 "The console never sleeps mid-download; off lets the screen dim"));
            out.push_back(choice("Speed while streaming", "downloads.throttle_kbps",
                                 {{"128 KB/s", 128, "Keeps streaming smooth on slow Wi-Fi"},
                                  {"512 KB/s", 512, ""},
                                  {"1 MB/s", 1024, "Default"},
                                  {"2 MB/s", 2048, ""},
                                  {"4 MB/s", 4096, ""},
                                  {"8 MB/s", 8192, ""}},
                                 1024, {}, "Download speed limit while a video streams from the server"));
            const dl::StorageStats st = dm.storage();
            out.push_back({"Storage", st.totalBytes > 0 ? std::format("{} free of {}", formatBytes(st.freeBytes), formatBytes(st.totalBytes)) : "",
                           std::format("{} in {} download{}", formatBytes(st.downloadedBytes), st.completedCount,
                                       st.completedCount == 1 ? "" : "s"),
                           gfx::Icon::Download, false, true, {}});
            out.push_back({"Delete watched downloads", "Frees space taken by titles you've finished", "", gfx::Icon::Trash,
                           false, st.completedCount > 0, [this] {
                               present(Dialog::confirm("Delete watched downloads?", "Finished movies and episodes will be removed.",
                                                       "Delete", "Cancel", [this](bool ok) {
                                                           if (!ok) return;
                                                           const int n = dl::DownloadManager::instance().removeWatched();
                                                           toast(std::format("Deleted {} download{}", n, n == 1 ? "" : "s"),
                                                                 ToastKind::Info, gfx::Icon::Trash);
                                                           refresh();
                                                       }));
                           }});
            break;
        }
        case Category::Interface: {
            out.push_back(toggle("Show battery", "ui.show_battery", true, {},
                                 "Battery percentage in the top bar and player controls"));
            out.push_back(toggle("Navigation sounds", "ui.navigation_sounds", true,
                                 [](bool enabled) { if (!enabled) ui::sound::stop(); },
                                 "Subtle sounds when moving, selecting and going back"));
            const int fallbackMb = platform::isAppletMode() ? 64 : 192;
            out.push_back(choice("Image memory", "ui.image_cache_mb",
                                 {{"64 MB", 64, "Best for applet mode"}, {"128 MB", 128, ""}, {"192 MB", 192, "Default"},
                                  {"256 MB", 256, ""}, {"384 MB", 384, ""}},
                                 fallbackMb,
                                 [](const Json& v) {
                                     gfx::ImageCache::instance().setMemoryBudget(static_cast<size_t>(v.get<int>()) << 20);
                                 },
                                 "Posters kept in memory for instant scrolling"));
            out.push_back({"Clear image cache", "Deletes cached posters and backdrops from the SD card", "",
                           gfx::Icon::Trash, false, true, [] {
                               gfx::ImageCache::instance().clearDiskCache();
                               toast("Image cache cleared", ToastKind::Success, gfx::Icon::Check);
                           }});
            out.push_back(toggle("Verify server certificates", "http.verify_tls", true,
                                 [](bool v) { http::Client::instance().setVerifyTls(v); },
                                 "Turn off only for a self-signed HTTPS server"));
            out.push_back(toggle("Performance overlay", "ui.show_stats", false, [](bool v) { root().setShowStats(v); },
                                 "Frame rate, draw calls and image memory"));
            out.push_back(choice("Log level", "app.log_level",
                                 {{"Errors only", "error", ""},
                                  {"Warnings", "warn", ""},
                                  {"Info", "info", "Default"},
                                  {"Debug", "debug", "For bug reports"},
                                  {"Trace", "trace", "Very verbose, slows the app down"}},
                                 "info",
                                 [](const Json& v) {
                                     if (v.is_string()) log::setLevel(log::parseLevel(v.get<std::string>()));
                                 },
                                 "Written to sdmc:/switch/karafin/log.txt"));
            out.push_back(toggle("Hide applet mode warning", "ui.applet_warning_hidden", false, {},
                                 "The launch warning about limited memory in applet mode"));
            break;
        }
        case Category::About: {
            out.push_back({"Karafin", "Jellyfin for Nintendo Switch", "Version " KARAFIN_VERSION, gfx::Icon::Star, false,
                           true, {}});
            if (client) {
                const auto info = client->serverInfo();
                out.push_back({"Server", info.name, info.version.empty() ? "" : "Jellyfin " + info.version,
                               gfx::Icon::Server, false, true, {}});
            }
            const std::string fw = platform::firmwareVersion();
            out.push_back({"System", fw.empty() ? "" : "Firmware " + fw,
                           platform::isAppletMode() ? "Applet mode" : (platform::isDocked() ? "Docked" : "Handheld"),
                           gfx::Icon::Info, false, true, {}});
            out.push_back({"Memory", "", std::format("{} of {}", formatBytes(platform::usedMemory()), formatBytes(platform::totalMemory())),
                           gfx::Icon::Speed, false, true, {}});
            out.push_back({"Libraries", "FFmpeg, mpv, curl, FreeType and more", "", gfx::Icon::List, false, true, [] {
                               present(Dialog::alert("Libraries", libinfo::versions() + "\n\nHardware decoders: " +
                                                                      libinfo::hwdecTypes()));
                           }});
            out.push_back({"Device ID", jf::AccountStore::instance().deviceId(), "", gfx::Icon::Lock, false, true, {}});
            break;
        }
        case Category::Count: break;
    }
    return out;
}

}  // namespace karafin::screens
