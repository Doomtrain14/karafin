#include "screens/widgets/download_button.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/list_menu.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace dl = karafin::downloads;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kCompact = 44;
constexpr float kPillW = 214;
constexpr float kPillH = 46;
}  // namespace

DownloadButton::DownloadButton(bool compact) : compact_(compact) {
    setFocusable(true);
    setCullMargin(30);
    sub_ = dl::Subscription(guard([this](const dl::Event& e) {
        if (item_.id.empty()) return;
        if (group_ ? (e.info.seriesId == item_.id || e.info.seasonId == item_.id) : e.info.itemId == item_.id) refresh();
    }));
}

void DownloadButton::setItem(const jf::BaseItem& item) {
    group_ = false;
    item_ = item;
    setFocusId(item_.id.empty() ? std::string() : "download:" + serverId() + ":" + item_.id);
    refresh();
}

void DownloadButton::setGroup(const jf::BaseItem& seasonOrSeries) {
    group_ = true;
    item_ = seasonOrSeries;
    setFocusId(item_.id.empty() ? std::string() : "download-group:" + serverId() + ":" + item_.id);
    refresh();
}

std::string DownloadButton::serverId() const {
    if (!item_.serverId.empty()) return item_.serverId;
    auto client = jf::currentClient();
    return client ? client->serverId() : std::string();
}

void DownloadButton::refresh() {
    refreshState();
    // activate() is overridden (no onActivate), so the root adds no "Select" hint by itself.
    setHints({{Key::A, visual_ == Visual::None ? "Download" : "Download options"}});
}

void DownloadButton::refreshState() {
    auto& dm = dl::DownloadManager::instance();
    error_.clear();
    if (group_) {
        const std::string noun = item_.isSeries() ? "series" : "season";
        summary_ = dm.summary(serverId(), item_.id);
        const int known = item_.isSeason() && item_.childCount > 0 ? item_.childCount : 0;
        progress_ = static_cast<float>(summary_.progress());
        if (!summary_.any()) {
            visual_ = Visual::None;
            label_ = "Download " + noun;
        } else if (summary_.active > 0 || summary_.queued > 0) {
            visual_ = summary_.active > 0 ? Visual::Downloading : Visual::Queued;
            label_ = std::format("{} of {} done", summary_.completed, summary_.total);
        } else if (summary_.failed > 0) {
            visual_ = Visual::Failed;
            label_ = std::format("{} failed", summary_.failed);
        } else if (summary_.paused > 0) {
            visual_ = Visual::Paused;
            label_ = std::format("Paused {} of {}", summary_.completed, summary_.total);
        } else if (summary_.allCompleted() && (known == 0 || summary_.completed >= known)) {
            visual_ = Visual::Done;
            progress_ = 1.0f;
            label_ = item_.isSeries() ? "Series downloaded" : "Season downloaded";
        } else {
            visual_ = Visual::Partial;
            progress_ = known > 0 ? static_cast<float>(summary_.completed) / static_cast<float>(known) : 0.5f;
            label_ = known > 0 ? std::format("{} of {} downloaded", summary_.completed, known)
                               : std::format("{} downloaded", summary_.completed);
        }
        return;
    }
    auto info = dm.find(serverId(), item_.id);
    if (!info) {
        key_.clear();
        visual_ = Visual::None;
        progress_ = 0;
        label_ = "Download";
        return;
    }
    key_ = info->key;
    progress_ = static_cast<float>(info->progress());
    error_ = info->error;
    switch (info->state) {
        case dl::State::Queued:
            visual_ = Visual::Queued;
            label_ = "Queued";
            break;
        case dl::State::Probing:
            visual_ = Visual::Preparing;
            label_ = "Preparing...";
            break;
        case dl::State::Downloading:
            visual_ = Visual::Downloading;
            label_ = info->bytesTotal > 0 ? std::format("Downloading {}%", static_cast<int>(progress_ * 100.0f))
                                          : "Downloading " + formatBytes(info->bytesDone);
            break;
        case dl::State::Paused:
            visual_ = Visual::Paused;
            label_ = info->bytesTotal > 0 ? std::format("Paused {}%", static_cast<int>(progress_ * 100.0f)) : "Paused";
            break;
        case dl::State::WaitingForNetwork:
            visual_ = Visual::Waiting;
            label_ = "Waiting for network";
            break;
        case dl::State::Completed:
            visual_ = Visual::Done;
            progress_ = 1.0f;
            label_ = "Downloaded";
            break;
        case dl::State::Failed:
            visual_ = Visual::Failed;
            label_ = "Download failed";
            break;
    }
}

Vec2 DownloadButton::measure(Vec2 available) {
    (void)available;
    const Vec2 pref = preferredSize();
    if (pref.x >= 0 && pref.y >= 0) return pref;
    return compact_ ? Vec2{kCompact, kCompact} : Vec2{kPillW, kPillH};
}

void DownloadButton::onFocus() {
    if (focusGainSilent()) {
        focus_.set(1.0f);
        scale.set(theme::button::focusScale);
        setZ(1);
        return;
    }
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(theme::button::focusScale, theme::motion::focus);
    setZ(1);
}

void DownloadButton::onBlur() {
    focus_.springTo(0.0f, theme::motion::snappy);
    scale.springTo(1.0f, theme::motion::focus);
    setZ(0);
}

bool DownloadButton::activate() {
    if (!enabled() || item_.id.empty()) return false;
    scale.set(focused() ? theme::button::focusScale * 0.94f : 0.94f);
    scale.springTo(focused() ? theme::button::focusScale : 1.0f, theme::motion::bouncy);
    if (visual_ == Visual::None) startDownload();
    else if (group_) openGroupMenu();
    else openItemMenu();
    return true;
}

void DownloadButton::startDownload() {
    auto client = jf::currentClient();
    if (!client) return;
    if (!group_ && !item_.canDownload && !client->user().canDownload()) {
        present(Dialog::alert("Downloads not allowed",
                              "Your Jellyfin account isn't allowed to download media. Ask the server administrator "
                              "to enable \"Allow media downloading\" for your user."));
        return;
    }
    const bool group = group_;
    dl::DownloadManager::instance().enqueue(client, item_, guard([this, group](dl::EnqueueResult res) {
        refresh();
        if (!res.ok()) {
            present(Dialog::alert("Can't download", res.error));
            return;
        }
        if (res.added == 0) {
            toast("Already downloaded or in the queue", ToastKind::Info, gfx::Icon::Download);
            return;
        }
        toast(group ? std::format("Downloading {} episode{}", res.added, res.added == 1 ? "" : "s") : "Added to downloads",
              ToastKind::Success, gfx::Icon::Download);
    }));
}

void DownloadButton::openItemMenu() {
    auto& dm = dl::DownloadManager::instance();
    auto info = dm.find(serverId(), item_.id);
    if (!info) {
        refresh();
        return;
    }
    const std::string key = info->key;
    const std::string sizeText = info->bytesTotal > 0 ? formatBytes(info->bytesTotal) : formatBytes(info->bytesDone);
    std::vector<ListItem> items;
    auto confirmCancel = [key] {
        present(Dialog::confirm("Cancel download?", "The partially downloaded file will be deleted.", "Cancel download",
                                "Keep", [key](bool ok) {
                                    if (ok) dl::DownloadManager::instance().cancel(key);
                                }));
    };
    switch (info->state) {
        case dl::State::Queued:
        case dl::State::Probing:
        case dl::State::Downloading:
        case dl::State::WaitingForNetwork:
            items.push_back({"Pause download", "", "", gfx::Icon::Pause, false, true,
                             [key] { dl::DownloadManager::instance().pause(key); }});
            items.push_back({"Cancel download", "", "", gfx::Icon::Close, false, true, confirmCancel});
            break;
        case dl::State::Paused:
            items.push_back({"Resume download", "", "", gfx::Icon::Play, false, true,
                             [key] { dl::DownloadManager::instance().resume(key); }});
            items.push_back({"Cancel download", "", "", gfx::Icon::Close, false, true, confirmCancel});
            break;
        case dl::State::Failed:
            items.push_back({"Try again", info->error, "", gfx::Icon::Refresh, false, true,
                             [key] { dl::DownloadManager::instance().retry(key); }});
            items.push_back({"Remove", "", "", gfx::Icon::Trash, false, true,
                             [key] { dl::DownloadManager::instance().remove(key); }});
            break;
        case dl::State::Completed: {
            const std::string id = info->itemId, name = info->name;
            const int64_t pos = info->positionTicks, runtime = info->runTimeTicks;
            items.push_back({"Play downloaded copy", "Works without a connection", "", gfx::Icon::Play, false, true,
                             [id, name, pos, runtime] { playOffline(id, name, pos, runtime); }});
            items.push_back({"Delete download", sizeText, "", gfx::Icon::Trash, false, true, [key, sizeText] {
                                 present(Dialog::confirm("Delete download?", "This frees " + sizeText + " on the SD card.",
                                                         "Delete", "Keep", [key](bool ok) {
                                                             if (ok) dl::DownloadManager::instance().remove(key);
                                                         }));
                             }});
            break;
        }
    }
    present(SideSheet::make(item_.name.empty() ? "Download" : item_.name, std::move(items), 0, label_));
}

void DownloadButton::openGroupMenu() {
    const std::string sid = serverId();
    const std::string parent = item_.id;
    std::vector<ListItem> items;
    if (visual_ != Visual::Done)
        items.push_back({"Download remaining episodes", "Episodes already downloaded are skipped", "",
                         gfx::Icon::Download, false, true, guard([this] { startDownload(); })});
    if (summary_.failed > 0)
        items.push_back({"Retry failed downloads", "", "", gfx::Icon::Refresh, false, true,
                         [] { dl::DownloadManager::instance().retryFailed(); }});
    items.push_back({"Delete all downloaded episodes", formatBytes(summary_.bytesDone), "", gfx::Icon::Trash, false, true,
                     [sid, parent] {
                         present(Dialog::confirm("Delete these downloads?",
                                                 "Every downloaded or queued episode here will be removed.",
                                                 "Delete", "Keep", [sid, parent](bool ok) {
                                                     if (ok) dl::DownloadManager::instance().removeGroup(sid, parent);
                                                 }));
                     }});
    present(SideSheet::make(item_.name.empty() ? "Downloads" : item_.name, std::move(items), 0, label_));
}

void DownloadButton::update(float dt) {
    focus_.update(dt);
    ring_.springTo(progress_, theme::motion::standard);
    ring_.update(dt);
    spin_ = std::fmod(spin_ + dt * 1.2f, 1.0f);
    pulse_ = std::fmod(pulse_ + dt * 0.8f, 1.0f);
    View::update(dt);
}

void DownloadButton::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const Rect b = bounds();
    const float rad = b.h * 0.5f;
    if (f > 0.01f) r.drawShadow(b, rad, 12 + 16 * f, Color::black(0.45f * f), {0, 6 + 6 * f});
    Color fill = Color::lerp(color::buttonSecondary, color::buttonSecondaryFocused, f);
    if (visual_ == Visual::Done && f < 0.5f) fill = Color::lerp(color::success.withAlpha(0.18f), fill, f * 2);
    r.fillRoundRect(b, compact_ ? rad : radius::md, fill);
    const Color ink = Color::lerp(color::textPrimary, color::textOnLight, f);

    // Icon disc (whole button when compact, left square of the pill otherwise).
    const Rect disc = compact_ ? b : Rect{0, 0, b.h, b.h};
    const Vec2 c = disc.center();
    const float ringR = disc.h * 0.5f - 7;
    const Color accent = Color::lerp(color::accentBright, color::accentDeep, f);
    const float p = std::clamp(ring_.value(), 0.0f, 1.0f);
    auto ring = [&](float amount, Color col) {
        r.strokeCircle(c, ringR, 2.5f, ink.mulAlpha(0.18f));
        if (amount > 0.002f) r.drawArc(c, ringR, 2.5f, -gfx::kPi * 0.5f, 2 * gfx::kPi * amount, col);
    };
    const float is = compact_ ? 18.0f : 20.0f;
    const Rect icon{c.x - is * 0.5f, c.y - is * 0.5f, is, is};
    const Rect small{c.x - 7, c.y - 7, 14, 14};
    switch (visual_) {
        case Visual::None: gfx::drawIcon(r, gfx::Icon::Download, icon, ink); break;
        case Visual::Queued:
            ring(p, accent);
            gfx::drawIcon(r, gfx::Icon::Clock, small, ink);
            break;
        case Visual::Preparing:
            r.strokeCircle(c, ringR, 2.5f, ink.mulAlpha(0.18f));
            r.drawArc(c, ringR, 2.5f, spin_ * 2 * gfx::kPi, gfx::kPi * 0.6f, accent);
            break;
        case Visual::Downloading: {
            ring(p, accent);
            if (compact_) {
                const std::string pct = std::to_string(static_cast<int>(p * 100.0f));
                const gfx::TextStyle st{11, gfx::FontWeight::SemiBold, 1.0f, 0.0f};
                auto l = gfx::text::layout(pct, st);
                const float capH = gfx::text::capHeight(st);
                r.drawText(*l, {c.x - l->width * 0.5f, c.y - capH * 0.5f - (l->firstBaseline - capH)}, ink);
            } else {
                gfx::drawIcon(r, gfx::Icon::Download, small, ink);
            }
            break;
        }
        case Visual::Paused:
            ring(p, ink.mulAlpha(0.6f));
            gfx::drawIcon(r, gfx::Icon::Pause, small, ink);
            break;
        case Visual::Waiting: {
            const float a = 0.55f + 0.45f * std::sin(pulse_ * 2 * gfx::kPi);
            ring(p, color::warning.mulAlpha(a));
            gfx::drawIcon(r, gfx::Icon::Wifi, small, ink);
            break;
        }
        case Visual::Done: {
            const Color ok = f > 0.5f ? Color::hex(0x1E9E62) : color::success;
            r.fillCircle(c, ringR + 1, ok);
            gfx::drawIcon(r, gfx::Icon::Check, {c.x - 8, c.y - 8, 16, 16}, Color::white(), 1.4f);
            break;
        }
        case Visual::Failed:
            r.strokeCircle(c, ringR, 2.5f, color::danger);
            gfx::drawIcon(r, gfx::Icon::Warning, small, color::danger);
            break;
        case Visual::Partial:
            ring(p, accent.mulAlpha(0.7f));
            gfx::drawIcon(r, gfx::Icon::Download, small, ink);
            break;
    }
    if (compact_) return;
    auto l = gfx::text::layout(label_, type::button, {.maxWidth = b.w - b.h - 14});
    const float capH = gfx::text::capHeight(type::button);
    const float y = (b.h - capH) * 0.5f - (l->firstBaseline - capH);
    r.drawText(*l, {b.h - 2, y}, ink);
}

}  // namespace karafin::screens
