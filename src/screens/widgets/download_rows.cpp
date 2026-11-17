#include "screens/widgets/download_rows.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "downloads/manager.hpp"
#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "ui/actions.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace dl = karafin::downloads;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kPosterW = 52;
constexpr float kPosterH = 78;
constexpr float kPad = 14;

std::string displayTitle(const dl::DownloadInfo& i) {
    if (i.isEpisode() && !i.seriesName.empty()) return i.seriesName;
    return i.name;
}

std::string displaySub(const dl::DownloadInfo& i) {
    if (!i.isEpisode()) return i.productionYear > 0 ? std::to_string(i.productionYear) : std::string();
    std::string tag;
    if (i.parentIndexNumber >= 0) tag += std::format("S{}", i.parentIndexNumber);
    if (i.indexNumber >= 0) tag += (tag.empty() ? "" : ":") + std::format("E{}", i.indexNumber);
    return joinDots({tag, i.name});
}

std::string statusLine(const dl::DownloadInfo& i) {
    const std::string size = i.bytesTotal > 0 ? std::format("{} of {}", formatBytes(i.bytesDone), formatBytes(i.bytesTotal))
                                              : (i.bytesDone > 0 ? formatBytes(i.bytesDone) : std::string());
    switch (i.state) {
        case dl::State::Downloading:
            return joinDots({i.bytesTotal > 0 ? std::format("{}%", static_cast<int>(i.progress() * 100.0)) : "",
                             formatSpeed(i.bytesPerSecond), formatEta(i.etaSeconds), size});
        case dl::State::Probing: return "Preparing...";
        case dl::State::Queued: return joinDots({"Queued", size});
        case dl::State::Paused: return joinDots({"Paused", size});
        case dl::State::WaitingForNetwork:
            return joinDots({"Waiting for network",
                             i.retryInSeconds > 0 ? std::format("retrying in {} s", static_cast<int>(std::ceil(i.retryInSeconds)))
                                                  : "",
                             size});
        case dl::State::Failed: return i.error.empty() ? "Download failed" : "Failed: " + i.error;
        case dl::State::Completed: return "Downloaded";
    }
    return {};
}

gfx::ImagePtr loadPoster(const std::string& url, float w, float h) {
    if (url.empty()) return nullptr;
    return gfx::ImageCache::instance().load(imageRequest(url, w, h));
}

void drawPoster(Renderer& r, const gfx::ImagePtr& img, const Rect& rect, const std::string& seed, gfx::Icon icon) {
    if (const gfx::Texture* t = img ? img->texture() : nullptr) {
        r.drawImage(*t, rect, {.fit = gfx::ImageFit::Cover, .radius = theme::radius::sm});
        return;
    }
    r.fillRoundRect(rect, theme::radius::sm, placeholderColor(seed));
    const float s = std::min(rect.w, rect.h) * 0.4f;
    gfx::drawIcon(r, icon, {rect.center().x - s * 0.5f, rect.center().y - s * 0.5f, s, s}, Color::white(0.5f));
}
}  // namespace

// ---- PendingDownloadRow ---------------------------------------------------------------------

PendingDownloadRow::PendingDownloadRow(const dl::DownloadInfo& info) : info_(info) {
    setRememberFocus(true);
    setCullMargin(16);
    toggle_ = add<ActionButton>("", gfx::Icon::Pause, ButtonStyle::Icon, "Pause");
    cancel_ = add<ActionButton>("", gfx::Icon::Close, ButtonStyle::Icon, "Cancel");
    setRememberedFocus(toggle_);
    toggle_->setFocusId("download-toggle:" + info_.key);
    cancel_->setFocusId("download-cancel:" + info_.key);
    toggle_->onActivate = [this] {
        auto& dm = dl::DownloadManager::instance();
        switch (info_.state) {
            case dl::State::Paused: dm.resume(info_.key); break;
            case dl::State::Failed: dm.retry(info_.key); break;
            case dl::State::Completed: break;
            default: dm.pause(info_.key); break;
        }
    };
    cancel_->onActivate = [this] {
        const std::string key = info_.key;
        present(Dialog::confirm("Cancel download?", "\"" + displayTitle(info_) + "\" and its partial file will be removed.",
                                "Cancel download", "Keep", [key](bool ok) {
                                    if (ok) dl::DownloadManager::instance().cancel(key);
                                }));
    };
    if (auto client = jf::currentClient(); client && client->serverId() == info_.serverId) {
        const std::string id = info_.isEpisode() && !info_.seriesId.empty() ? info_.seriesId : info_.itemId;
        poster_ = loadPoster(client->itemImageUrl(id, jf::ImageType::Primary, "", {.maxHeight = 200}), kPosterW, kPosterH);
    }
    progress_.set(static_cast<float>(info_.progress()));
    title_ = displayTitle(info_);
    subtitle_ = displaySub(info_);
    status_ = statusLine(info_);
    applyButtons();
}

void PendingDownloadRow::setInfo(const dl::DownloadInfo& info) {
    const bool stateChanged = info.state != info_.state;
    info_ = info;
    progress_.springTo(static_cast<float>(info_.progress()), theme::motion::standard);
    status_ = statusLine(info_);
    if (stateChanged) {
        title_ = displayTitle(info_);
        subtitle_ = displaySub(info_);
        applyButtons();
    }
}

void PendingDownloadRow::applyButtons() {
    switch (info_.state) {
        case dl::State::Paused:
            toggle_->setIcon(gfx::Icon::Play);
            toggle_->setTooltip("Resume");
            break;
        case dl::State::Failed:
            toggle_->setIcon(gfx::Icon::Refresh);
            toggle_->setTooltip("Retry");
            break;
        default:
            toggle_->setIcon(gfx::Icon::Pause);
            toggle_->setTooltip("Pause");
            break;
    }
}

Vec2 PendingDownloadRow::measure(Vec2 available) { return {available.x, kHeight}; }

void PendingDownloadRow::layout() {
    const float x1 = width() - theme::layout::marginX - kPad;
    const float y = (height() - 48) * 0.5f;
    cancel_->setFrame({x1 - 48, y, 48, 48});
    toggle_->setFrame({x1 - 48 - 12 - 48, y, 48, 48});
}

void PendingDownloadRow::update(float dt) {
    progress_.update(dt);
    focus_.springTo(hasFocusWithin() ? 1.0f : 0.0f, theme::motion::snappy);
    focus_.update(dt);
    phase_ = std::fmod(phase_ + dt, 1.0f);
    View::update(dt);
}

void PendingDownloadRow::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const float x0 = layout::marginX;
    const Rect plate{x0, 4, width() - 2 * x0, height() - 8};
    r.fillRoundRect(plate, radius::lg, Color::lerp(Color::white(0.035f), Color::hex(0x262632), f));
    if (f > 0.01f) r.strokeRoundRect(plate, radius::lg, 1.5f, color::stroke.mulAlpha(1.0f + f));

    const Rect poster{x0 + kPad, (height() - kPosterH) * 0.5f, kPosterW, kPosterH};
    drawPoster(r, poster_, poster, info_.itemId, info_.isEpisode() ? gfx::Icon::Tv : gfx::Icon::Movie);

    const float tx = poster.right() + 18;
    const float tw = std::max(60.0f, toggle_->frame().x - 24 - tx);
    auto t = gfx::text::layout(title_, type::headline, {.maxWidth = tw});
    auto s = gfx::text::layout(subtitle_, type::caption, {.maxWidth = tw});
    auto st = gfx::text::layout(status_, type::caption, {.maxWidth = tw});
    gfx::TextLayoutPtr o;
    if (!info_.overview.empty()) o = gfx::text::layout(info_.overview, type::footnote, {.maxWidth = tw});
    // Title, subtitle, summary, progress track and status line, centered as one block.
    const float trackGap = 6, statusGap = 6;
    const float summaryH = o ? o->height + 2 : 0.0f;
    const float blockH = t->height + 1 + s->height + summaryH + trackGap + 6 + statusGap + st->height;
    const float y0 = std::round((height() - blockH) * 0.5f);
    r.drawText(*t, {tx, y0}, color::textPrimary);
    r.drawText(*s, {tx, y0 + t->height + 1}, color::textTertiary);
    if (o) r.drawText(*o, {tx, y0 + t->height + 1 + s->height + 2}, color::textTertiary);

    const Rect track{tx, y0 + t->height + 1 + s->height + summaryH + trackGap, tw, 6};
    r.fillRoundRect(track, 3, color::progressTrack);
    const float p = std::clamp(progress_.value(), 0.0f, 1.0f);
    Color fillColor = color::accent;
    if (info_.state == dl::State::Paused) fillColor = Color::white(0.5f);
    if (info_.state == dl::State::Failed) fillColor = color::danger;
    if (info_.state == dl::State::WaitingForNetwork) fillColor = color::warning;
    if (info_.state == dl::State::Probing || (info_.state == dl::State::Downloading && info_.bytesTotal == 0)) {
        const float seg = track.w * 0.25f;
        const float x = track.x + (track.w + seg) * phase_ - seg;
        r.pushClip(track);
        r.fillRoundRect({x, track.y, seg, track.h}, 3, color::accent);
        r.popClip();
    } else if (p > 0) {
        if (info_.state == dl::State::Downloading) r.fillRoundRect({track.x, track.y, std::max(6.0f, track.w * p), track.h}, 3, accentGradient);
        else r.fillRoundRect({track.x, track.y, std::max(6.0f, track.w * p), track.h}, 3, fillColor);
    }
    const Color statusColor = info_.state == dl::State::Failed ? color::danger
                              : info_.state == dl::State::WaitingForNetwork ? color::warning
                                                                            : color::textSecondary;
    r.drawText(*st, {tx, track.bottom() + statusGap}, statusColor);
}

// ---- OfflineItemRow -------------------------------------------------------------------------

OfflineItemRow::OfflineItemRow(std::string title, std::string subtitle, std::string detail, const std::string& posterPath,
                               bool wideArt)
    : title_(std::move(title)), subtitle_(std::move(subtitle)), detail_(std::move(detail)), wide_(wideArt) {
    setFocusable(true);
    setCullMargin(16);
    poster_ = loadPoster(posterPath, wide_ ? 124 : kPosterW, kPosterH * (wide_ ? 0.9f : 1.0f));
    actions = [this](ActionMenu& m) {
        m.setTitle(title_, subtitle_);
        if (onActivate) {
            m.section(chevron_ ? "Show" : "Playback");
            if (chevron_) m.add("Open", gfx::Icon::ChevronRight, guard([this] { onActivate(); }));
            else m.add("Play", gfx::Icon::Play, guard([this] { onActivate(); }));
        }
        m.section("Download");
        if (onToggleWatched)
            m.add(watched_ ? "Mark as unwatched" : "Mark as watched", watched_ ? gfx::Icon::Close : gfx::Icon::Check,
                  guard([this] { onToggleWatched(); }));
        if (onDelete) {
            Action a;
            a.title = chevron_ ? "Delete all episodes" : "Delete download";
            a.icon = gfx::Icon::Trash;
            a.destructive = true;
            a.run = guard([this] { onDelete(); });
            m.add(std::move(a));
        }
    };
}

Vec2 OfflineItemRow::measure(Vec2 available) { return {available.x, summary_.empty() ? kHeight : kHeightWithSummary}; }

void OfflineItemRow::onFocus() {
    if (focusGainSilent()) {
        focus_.set(1.0f);
        scale.set(1.012f);
        setZ(1);
        return;
    }
    focus_.springTo(1.0f, theme::motion::snappy);
    scale.springTo(1.012f, theme::motion::focus);
    setZ(1);
}

void OfflineItemRow::onBlur() {
    focus_.springTo(0.0f, theme::motion::snappy);
    scale.springTo(1.0f, theme::motion::focus);
    setZ(0);
}

bool OfflineItemRow::activate() {
    scale.set(0.985f);
    scale.springTo(focused() ? 1.012f : 1.0f, theme::motion::bouncy);
    return View::activate();
}

void OfflineItemRow::update(float dt) {
    focus_.update(dt);
    View::update(dt);
}

void OfflineItemRow::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const float x0 = layout::marginX;
    const Rect plate{x0, 4, width() - 2 * x0, height() - 8};
    if (f > 0.01f) {
        r.drawShadow(plate, radius::lg, 20, Color::black(0.35f * f), {0, 6});
        r.fillRoundRect(plate, radius::lg, Color::white(0.95f * f));
    }
    if (f < 0.99f) r.fillRoundRect(plate, radius::lg, Color::white(0.035f * (1.0f - f)));
    const Color ink = Color::lerp(color::textPrimary, color::textOnLight, f);
    const Color sub = Color::lerp(color::textSecondary, Color(0.04f, 0.04f, 0.07f, 0.62f), f);

    const float pw = wide_ ? 124.0f : kPosterW;
    const float ph = wide_ ? 70.0f : kPosterH;
    const Rect poster{x0 + kPad, (height() - ph) * 0.5f, pw, ph};
    drawPoster(r, poster_, poster, title_, wide_ ? gfx::Icon::Tv : gfx::Icon::Movie);
    if (progress_ >= 0 && !watched_) {
        const Rect track{poster.x + 5, poster.bottom() - 9, poster.w - 10, 4};
        r.fillRoundRect(track, 2, color::progressTrack);
        r.fillRoundRect({track.x, track.y, std::max(4.0f, track.w * std::clamp(progress_, 0.0f, 1.0f)), 4}, 2, accentGradient);
    }
    if (watched_) {
        const Vec2 c{poster.right() - 9, poster.y + 9};
        r.fillCircle(c, 8, color::accent);
        gfx::drawIcon(r, gfx::Icon::Check, {c.x - 5, c.y - 5, 10, 10}, Color::white(), 1.3f);
    }

    float right = plate.right() - kPad;
    if (chevron_) {
        gfx::drawIcon(r, gfx::Icon::ChevronRight, {right - 18, (height() - 18) * 0.5f, 18, 18}, sub);
        right -= 30;
    }
    if (!detail_.empty()) {
        const float tx0 = x0 + kPad + pw + 18;
        auto d = gfx::text::layout(detail_, type::subhead, {.maxWidth = std::max(40.0f, (right - tx0) * 0.4f)});
        right -= d->width;
        r.drawText(*d, {right, (height() - d->height) * 0.5f}, sub);
        right -= 18;
    }
    const float tx = poster.right() + 18;
    const float tw = std::max(60.0f, right - tx);
    auto t = gfx::text::layout(title_, type::headline, {.maxWidth = tw});
    auto s = gfx::text::layout(subtitle_, type::caption, {.maxWidth = tw});
    gfx::TextLayoutPtr o;
    if (!summary_.empty()) o = gfx::text::layout(summary_, type::footnote, {.maxWidth = tw, .maxLines = 2});
    const float total = t->height + (subtitle_.empty() ? 0.0f : s->height + 2) + (o ? o->height + 4 : 0.0f);
    float y = std::round((height() - total) * 0.5f);
    r.drawText(*t, {tx, y}, ink);
    y += t->height;
    if (!subtitle_.empty()) {
        r.drawText(*s, {tx, y + 2}, sub);
        y += s->height + 2;
    }
    if (o) r.drawText(*o, {tx, y + 4}, sub);
}

}  // namespace karafin::screens
