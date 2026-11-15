#include "screens/widgets/episode_row.hpp"

#include <algorithm>
#include <format>
#include <memory>

#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/detail_screen.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kThumbW = 192;
constexpr float kThumbH = 108;
constexpr float kPad = 12;
}  // namespace

EpisodeRow::EpisodeRow(const jf::BaseItem& episode, bool showDownload) {
    setFocusable(true);
    setCullMargin(16);
    thumb_ = add<ImageView>();
    thumb_->setRadius(theme::radius::sm);
    if (showDownload) download_ = add<DownloadButton>(true);
    setHints({{Key::X, "Details"}, {Key::Y, "Watched"}});
    onActivate = [this] {
        if (item_.isVirtual()) toast("This episode isn't in your library", ToastKind::Info, gfx::Icon::Info);
        else playItem(item_);
    };
    setItem(episode);
}

EpisodeRow::~EpisodeRow() { token_.cancel(); }

void EpisodeRow::setItem(const jf::BaseItem& episode) {
    item_ = episode;
    setFocusId(item_.id.empty() ? std::string() : "episode:" + item_.id);
    thumb_->setPlaceholderColor(placeholderColor(item_.id));
    thumb_->setFallbackIcon(gfx::Icon::Tv);
    const std::string url = wideUrl(item_);
    if (url != thumbUrl_ || url.empty()) {
        thumbUrl_ = url;
        if (url.empty()) thumb_->clear();
        else thumb_->setUrl(url);
    }
    if (download_) {
        download_->setItem(item_);
        download_->setVisible(!item_.isVirtual());
    }
    updateText();
}

void EpisodeRow::updateText() {
    title_ = item_.indexNumber >= 0 ? std::format("{}. {}", item_.indexNumber, item_.name) : item_.name;
    const std::string premiere = item_.premiereDate.size() >= 10 ? item_.premiereDate.substr(0, 10) : std::string();
    const std::string left = item_.userData.played ? std::string() : remainingText(item_);
    meta_ = joinDots({formatRuntime(item_.runTimeTicks),
                      item_.communityRating > 0 ? "\xE2\x98\x85 " + formatRating(item_.communityRating) : "", premiere,
                      left, item_.isVirtual() ? "Missing" : ""});
    setHints({{Key::X, "Details"}, {Key::Y, item_.userData.played ? "Unwatched" : "Watched"}});
}

Vec2 EpisodeRow::measure(Vec2 available) {
    const Vec2 pref = preferredSize();
    return {pref.x >= 0 ? pref.x : available.x, kHeight};
}

void EpisodeRow::layout() {
    const float x = theme::layout::marginX;
    thumb_->setFrame({x + kPad, (height() - kThumbH) * 0.5f, kThumbW, kThumbH});
    if (download_) download_->setFrame({width() - x - 44 - kPad, (height() - 44) * 0.5f, 44, 44});
}

void EpisodeRow::onFocus() {
    if (focusGainSilent()) focus_.set(1.0f);
    else focus_.springTo(1.0f, theme::motion::snappy);
}
void EpisodeRow::onBlur() { focus_.springTo(0.0f, theme::motion::snappy); }

bool EpisodeRow::onInput(const InputEvent& e) {
    if (e.pressed() && focused()) {
        if (e.button == Key::X) {
            nav().push(std::make_unique<DetailScreen>(item_.id), Transition::Zoom);
            return true;
        }
        if (e.button == Key::Y) {
            toggleWatched();
            return true;
        }
    }
    return View::onInput(e);
}

void EpisodeRow::toggleWatched() {
    auto client = jf::currentClient();
    if (!client || watchedPending_) return;
    const bool played = !item_.userData.played;
    watchedPending_ = true;
    client->setPlayed(item_.id, played).then(
        [this, played](jf::Result<jf::UserData> r) {
            watchedPending_ = false;
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            item_.userData.played = r->played;
            item_.userData.playbackPositionTicks = r->playbackPositionTicks;
            item_.userData.playedPercentage = r->playedPercentage;
            updateText();
            toast(played ? "Marked as watched" : "Marked as unwatched", ToastKind::Success, gfx::Icon::Check, 1.6f);
            if (onWatchedChanged) onWatchedChanged(item_);
        },
        token_);
}

void EpisodeRow::update(float dt) {
    focus_.update(dt);
    View::update(dt);
}

void EpisodeRow::draw(Renderer& r) {
    using namespace theme;
    const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
    const float x0 = layout::marginX;
    const Rect plate{x0, 4, width() - 2 * x0, height() - 8};
    if (f > 0.01f) {
        r.drawShadow(plate, radius::lg, 24, Color::black(0.4f * f), {0, 8});
        r.fillRoundRect(plate, radius::lg, Color::lerp(Color::white(0.04f), Color::hex(0x2B2B38), f));
        r.strokeRoundRect(plate.expanded(3), radius::lg + 3, 2.5f, color::focusRing.mulAlpha(f));
    } else {
        r.fillRoundRect(plate, radius::lg, Color::white(0.035f));
    }
    const float tx = x0 + kPad + kThumbW + 20;
    const float right = width() - x0 - (download_ && download_->visible() ? 44 + kPad * 2 + 8 : kPad + 8);
    const float tw = std::max(60.0f, right - tx);
    float y = 18;
    auto t = gfx::text::layout(title_, type::headline, {.maxWidth = tw});
    r.drawText(*t, {tx, y}, color::textPrimary);
    y += t->height + 3;
    auto m = gfx::text::layout(meta_, type::caption, {.maxWidth = tw});
    r.drawText(*m, {tx, y}, color::textTertiary);
    y += m->height + 6;
    if (!item_.overview.empty()) {
        auto o = gfx::text::layout(item_.overview, type::subhead, {.maxWidth = tw, .maxLines = 2});
        r.drawText(*o, {tx, y}, color::textSecondary);
    }
}

void EpisodeRow::drawOverlay(Renderer& r) {
    using namespace theme;
    const Rect art = thumb_->frame();
    const double p = item_.progress();
    if (!item_.userData.played && item_.userData.playbackPositionTicks > 0 && p > 0) {
        const Rect track{art.x + 8, art.bottom() - 11, art.w - 16, 4};
        r.fillRoundRect(track, 2, color::progressTrack);
        r.fillRoundRect({track.x, track.y, std::max(4.0f, track.w * static_cast<float>(p)), track.h}, 2, accentGradient);
    }
    if (item_.userData.played) {
        const Vec2 c{art.right() - 14, art.y + 14};
        r.fillCircle(c, 10, color::accent);
        gfx::drawIcon(r, gfx::Icon::Check, {c.x - 6.5f, c.y - 6.5f, 13, 13}, Color::white(), 1.3f);
    }
    if (focused()) {
        // Play glyph over the still while the row is focused.
        const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
        const Vec2 c = art.center();
        r.fillCircle(c, 20, Color::black(0.5f * f));
        r.fillTriangle({c.x + 2, c.y}, {16, 16}, gfx::kPi * 0.5f, Color::white(f), 2);
    }
}

}  // namespace karafin::screens
