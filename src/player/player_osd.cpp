#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <format>

#include <SDL.h>
#include <switch.h>

#include "core/app.hpp"
#include "core/log.hpp"
#include "gfx/icons.hpp"
#include "gfx/text.hpp"
#include "player/media_tags.hpp"
#include "player/player_screen.hpp"
#include "ui/hint_bar.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"

namespace karafin::player {

namespace theme = ui::theme;
using gfx::Color;
using gfx::Icon;
using gfx::Rect;
using gfx::Vec2;

namespace {

constexpr float kMargin = theme::layout::marginX;
constexpr float kBarY = 646;
constexpr float kControlsY = 690;  // vertical center of the control row

std::string formatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const int total = static_cast<int>(seconds + 0.5);
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
}

// Whole seconds as formatTime() rounds them (cache key for formatted times).
int64_t timeKey(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    return static_cast<int64_t>(seconds + 0.5);
}

// Local wall-clock time `offset` seconds from now, "HH:MM".
std::string clockAt(double offset) {
    u64 now = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &now))) {
        TimeCalendarTime cal{};
        TimeCalendarAdditionalInfo info{};
        if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(now + static_cast<u64>(std::max(0.0, offset)), &cal, &info)))
            return std::format("{:02}:{:02}", cal.hour, cal.minute);
    }
    const std::time_t t = std::time(nullptr) + static_cast<std::time_t>(std::max(0.0, offset));
    std::tm lt{};
    localtime_r(&t, &lt);
    return std::format("{:02}:{:02}", lt.tm_hour, lt.tm_min);
}

std::string formatBitrate(double bps) {
    if (bps <= 0) return "-";
    if (bps >= 1e6) return std::format("{:.1f} Mbps", bps / 1e6);
    return std::format("{:.0f} kbps", bps / 1e3);
}

}  // namespace

const gfx::TextLayout* CachedText::get(std::string_view s, const gfx::TextStyle& style, const gfx::TextOptions& options) {
    if (!layout || s != text || layout->scale != gfx::text::displayScale()) {
        text.assign(s);
        layout = gfx::text::layout(s, style, options);
    }
    return layout.get();
}

// --- OSD state ------------------------------------------------------------------------------

void PlayerScreen::showOsd(double seconds) { osdTimer_ = std::max(osdTimer_, seconds); }

void PlayerScreen::hideOsd() { osdTimer_ = 0; }

void PlayerScreen::updateClock() {
    const double now = ui::now();
    if (now < clockNext_ && !clockText_.empty()) return;
    clockNext_ = now + 1.0;
    clockText_ = clockAt(0);
    const double remaining = (duration_ - displayPosition()) / std::max(0.25, speed_);
    endsText_ = duration_ > 0 && fileLoaded_ ? "Ends at " + clockAt(std::max(0.0, remaining)) : "";
}

Rect PlayerScreen::seekBarRect() const { return {kMargin, kBarY, theme::layout::screenWidth - kMargin * 2, 5}; }

double PlayerScreen::positionFromX(float x) const {
    const Rect bar = seekBarRect();
    const double t = std::clamp((x - bar.x) / bar.w, 0.0f, 1.0f);
    return t * std::max(0.0, duration_);
}

bool PlayerScreen::onTouchEvent(const SDL_Event& e) {
    if (modalOpen_ || !isTop() || !platform::hasFocus() || exiting_) {
        if (touchDragging_) cancelScrub();
        touchActive_ = touchOnBar_ = touchDragging_ = false;
        swipe_.cancel();
        return true;
    }
    const Vec2 p{e.tfinger.x * theme::layout::screenWidth, e.tfinger.y * theme::layout::screenHeight};
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
        log::warn("player", "ignoring invalid touch coordinates");
        if (touchDragging_) cancelScrub();
        touchActive_ = touchOnBar_ = touchDragging_ = false;
        swipe_.cancel();
        return true;
    }
    if (e.type == SDL_FINGERDOWN) {
        // The Switch SDL driver can synthesize a second DOWN for a motion slot; keep the
        // original contact and follow MOTION coordinates without indexing SDL's finger list.
        if (!touchActive_) {
            touchActive_ = true;
            touchFinger_ = e.tfinger.fingerId;
            touchStartedAt_ = e.tfinger.timestamp;
            touchStart_ = p;
            const Rect bar = seekBarRect();
            const Rect zone{bar.x - 16, bar.y - 26, bar.w + 32, bar.h + 52};
            touchOnBar_ = osdVisible() && fileLoaded_ && duration_ > 0 && zone.contains(p);
            bool onButton = false;
            for (const auto& hit : hits_)
                if (hit.rect.contains(p)) onButton = true;
            if (!swipe_.begin(p.x, p.y, theme::layout::screenWidth, theme::layout::screenHeight,
                              touchOnBar_ || onButton || phase_ != Phase::Playing || !fileLoaded_))
                log::warn("player", "cannot start swipe with invalid touch geometry");
        }
    } else if (e.type == SDL_FINGERMOTION && touchActive_) {
        if (touchOnBar_) {
            if (!touchDragging_ && std::fabs(p.x - touchStart_.x) > 8.0f) {
                touchDragging_ = true;
                beginScrub();
            }
            if (touchDragging_) {
                scrubTarget_ = positionFromX(p.x);
                showOsd(3.0);
            }
        } else {
            const auto before = swipe_.control();
            const auto control = swipe_.move(p.x, p.y);
            if (control != PlaybackSwipe::Control::None) {
                const bool brightness = control == PlaybackSwipe::Control::Brightness;
                if (before == PlaybackSwipe::Control::None) {
                    if (brightness) {
                        if (platform::isDocked()) {
                            ui::toast("Brightness is only available in handheld mode", ui::ToastKind::Warning);
                            swipe_.cancel();
                            return true;
                        }
                        const auto value = brightness_.current();
                        if (!value) {
                            ui::toast("Unable to adjust screen brightness", ui::ToastKind::Warning);
                            swipe_.cancel();
                            return true;
                        }
                        swipeInitial_ = *value;
                    } else {
                        const auto value = mpv_ ? mpv_->getDouble("volume") : std::nullopt;
                        if (!value || !std::isfinite(*value)) {
                            log::warn("player", "cannot read playback volume");
                            ui::toast("Unable to adjust playback volume", ui::ToastKind::Warning);
                            swipe_.cancel();
                            return true;
                        }
                        swipeInitial_ = PlaybackSwipe::volumeFraction(*value);
                    }
                }
                const float value = swipe_.value(p.y, swipeInitial_, brightness ? 0.05f : 0.0f);
                const int percent = brightness ? static_cast<int>(std::round(value * 100))
                                               : PlaybackSwipe::volumePercent(value);
                const std::string label = std::format("{}%", percent);
                if (before == PlaybackSwipe::Control::None || label != adjustmentLabel_) {
                    bool ok = false;
                    if (brightness) {
                        ok = brightness_.set(value);
                    } else if (mpv_) {
                        int rc = mpv_->setDouble("volume", percent);
                        if (rc >= 0 && percent > 0) rc = mpv_->setFlag("mute", false);
                        ok = rc >= 0;
                        if (!ok) log::warn("player", "cannot adjust volume: {}", mpv_error_string(rc));
                    }
                    if (!ok) {
                        ui::toast(brightness ? "Unable to adjust screen brightness" : "Unable to adjust playback volume",
                                  ui::ToastKind::Warning);
                        swipe_.cancel();
                        return true;
                    }
                    adjustmentValue_ = value;
                    adjustingBrightness_ = brightness;
                    adjustmentLabel_ = label;
                }
                adjustmentTimer_ = 1.5;
            }
        }
    } else if (e.type == SDL_FINGERUP && touchActive_ &&
               (e.tfinger.fingerId == touchFinger_ || SDL_GetNumTouchFingers(e.tfinger.touchId) <= 0)) {
        // The Switch driver may report a different id on release; the last lift always ends the gesture.
        const bool dragged = touchDragging_ || swipe_.control() != PlaybackSwipe::Control::None;
        touchActive_ = false;
        touchOnBar_ = false;
        swipe_.cancel();
        if (touchDragging_) {
            touchDragging_ = false;
            commitScrub();
        }
        if (!dragged && (p - touchStart_).length() < 14.0f && e.tfinger.timestamp - touchStartedAt_ < 800)
            onTap(p);
    }
    return true;
}

void PlayerScreen::runHit(HitAction action) {
    switch (action) {
        case HitAction::Back:
        case HitAction::ErrorBack: exitPlayer(); break;
        case HitAction::SeekBack: seekRelative(-seekStep()); break;
        case HitAction::PlayPause:
            if (phase_ == Phase::Ended) seekAbsolute(0);
            else togglePause();
            break;
        case HitAction::SeekForward: seekRelative(seekStep()); break;
        case HitAction::Next: playNext(upNextVisible_); break;
        case HitAction::Tracks: openTrackMenu(); break;
        case HitAction::Options: openMoreMenu(); break;
        case HitAction::Skip: skipActiveSegment(); break;
        case HitAction::UpNext: playNext(true); break;
        case HitAction::DismissUpNext: dismissUpNext(); break;
    }
}

bool PlayerScreen::onTap(Vec2 local) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (it->rect.contains(local)) {
            runHit(it->action);
            return true;
        }
    }
    if (phase_ == Phase::Error) return true;
    if (osdVisible()) {
        const Rect bar = seekBarRect();
        const Rect zone{bar.x - 16, bar.y - 26, bar.w + 32, bar.h + 52};
        if (zone.contains(local) && fileLoaded_ && duration_ > 0) {
            seekAbsolute(positionFromX(local.x));
            showOsd(3.0);
        } else if (phase_ == Phase::Playing) {
            hideOsd();
        }
    } else {
        showOsd(4.0);
    }
    return true;
}

// --- Drawing --------------------------------------------------------------------------------

void PlayerScreen::draw(ui::Renderer& r) {
    const Rect full = bounds();
    if (!mpv_) r.fillRect(full, Color::black());  // otherwise mpv paints the whole target
    drawVideo(r);
    drawBackdrop(r);
    hits_.clear();

    const float a = std::clamp(osd_.value(), 0.0f, 1.0f);
    if (a > 0.004f) {
        r.fillRect({0, 0, full.w, 170}, gfx::Gradient::vertical(Color::black(0.78f * a), Color::black(0)));
        r.fillRect({0, full.h - 260, full.w, 260}, gfx::Gradient::vertical(Color::black(0), Color::black(0.88f * a)));
        drawTopBar(r, a);
        drawBottomBar(r, a);
    }
    drawCenter(r);
    drawSkipButton(r);
    drawUpNext(r);
    drawSeekPreview(r, a);
    drawError(r);
    if (showInfo_) drawInfoOverlay(r);
    drawAdjustment(r);
}

void PlayerScreen::drawAdjustment(ui::Renderer& r) {
    if (adjustmentTimer_ <= 0 || modalOpen_) return;
    // Vertical pill on the side being swiped, so the fill follows the finger's direction.
    constexpr float w = 64, h = 300;
    const float x = adjustingBrightness_ ? 72.0f : theme::layout::screenWidth - 72.0f - w;
    const Rect box{x, (theme::layout::screenHeight - h) * 0.5f, w, h};
    const float fade = std::clamp(static_cast<float>(adjustmentTimer_) / 0.25f, 0.0f, 1.0f);
    r.pushAlpha(fade);
    r.fillRoundRect(box, w * 0.5f, Color::black(0.72f));
    if (const auto* l = adjustmentText_.get(adjustmentLabel_, theme::type::subhead))
        r.drawText(*l, {box.center().x - l->width * 0.5f, box.y + 18}, theme::color::textPrimary);
    const Rect track{box.center().x - 4, box.y + 54, 8, box.h - 54 - 66};
    const float value = std::clamp(adjustmentValue_, 0.0f, 1.0f);
    r.fillRoundRect(track, 4, theme::color::progressTrack);
    r.fillRoundRect({track.x, track.bottom() - track.h * value, track.w, track.h * value}, 4, theme::accentGradient);
    if (!adjustingBrightness_) {
        // Marks 100%; above it mpv amplifies the audio.
        const float unity = track.bottom() - track.h * PlaybackSwipe::volumeFraction(100);
        r.fillRoundRect({track.x - 6, unity - 1, track.w + 12, 2}, 1, Color::white(0.7f));
    }
    const Rect icon{box.center().x - 14, box.bottom() - 48, 28, 28};
    if (adjustingBrightness_) {
        const Vec2 c = icon.center();
        r.fillCircle(c, 6, theme::color::textPrimary);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.785398f;
            r.fillCircle({c.x + std::cos(a) * 12, c.y + std::sin(a) * 12}, 2, theme::color::textPrimary);
        }
    } else {
        gfx::drawIcon(r, value <= 0.001f ? Icon::Close : Icon::Audio, icon, theme::color::textPrimary, 1.1f);
    }
    r.popAlpha();
}

void PlayerScreen::drawVideo(ui::Renderer& r) {
    if (!mpv_) return;
    const auto target = r.beginExternal();
    mpv_->render(target);
    r.endExternal();
}

void PlayerScreen::drawBackdrop(ui::Renderer& r) {
    const float hide = std::clamp(videoReveal_.value(), 0.0f, 1.0f);
    if (hide >= 0.999f || phase_ == Phase::Ended) return;
    const Rect full = bounds();
    r.fillRect(full, Color::black(1.0f - hide));
    if (backdrop_) {
        if (const gfx::Texture* t = backdrop_->texture()) {
            const float fade = backdrop_->revealProgress(ui::now(), theme::motion::imageFade);
            r.drawImage(*t, full, {.fit = gfx::ImageFit::Cover, .tint = Color::white(0.42f * fade * (1.0f - hide))});
        }
    }
    r.fillRect(full, gfx::Gradient::vertical(Color::black(0.1f * (1 - hide)), Color::black(0.65f * (1 - hide))));
}

void PlayerScreen::drawTopBar(ui::Renderer& r, float a) {
    const float dy = -18.0f * (1.0f - a);
    r.pushAlpha(a);
    r.pushTranslate(0, dy);

    const Rect back{kMargin - 14, 30, 40, 40};
    gfx::drawIcon(r, Icon::ChevronLeft, back.inset(6), theme::color::textPrimary, 1.2f);
    hits_.push_back({back.expanded(8), HitAction::Back});

    const float textX = back.right() + 10;
    const float maxW = 760;
    const gfx::Texture* logo = logo_ ? logo_->texture() : nullptr;
    if (logo) {
        const float h = 64;
        const float w = std::min(300.0f, h * logo_->aspect());
        r.drawImage(*logo, {textX, 24, w, h}, {.fit = gfx::ImageFit::Contain});
    } else if (const auto* l = titleText_.get(title_.empty() ? std::string_view("Loading") : std::string_view(title_),
                                              theme::type::title2, {.maxWidth = maxW})) {
        r.drawText(*l, {textX, 32}, theme::color::textPrimary);
    }
    float lineBottom = logo ? 24.0f + 64.0f : 62.0f;
    if (!subtitleLine_.empty())
        if (const auto* l = subtitleText_.get(subtitleLine_, theme::type::subhead, {.maxWidth = maxW})) {
            const float y = logo ? 96.0f : 62.0f;
            r.drawText(*l, {textX, y}, theme::color::textSecondary);
            lineBottom = y + l->height;
        }
    drawTags(r, {textX, lineBottom + 10});

    const float right = theme::layout::screenWidth - kMargin;
    if (const auto* l = clockLayout_.get(clockText_, theme::type::title3)) {
        r.drawText(*l, {right - l->width, 34}, theme::color::textPrimary);
        battery_.draw(r, right - l->width - 18, 34 + l->height * 0.5f, theme::color::textSecondary);
    }
    if (!endsText_.empty())
        if (const auto* l = endsLayout_.get(endsText_, theme::type::caption))
            r.drawText(*l, {right - l->width, 62}, theme::color::textSecondary);

    r.popTransform();
    r.popAlpha();
}

void PlayerScreen::drawTags(ui::Renderer& r, Vec2 at) {
    constexpr float h = 24, padX = 9, gap = 6;
    float x = at.x;
    for (const auto& tag : osdTags_) {
        if (!tag.layout) continue;
        const Rect chip{x, at.y, tag.layout->width + padX * 2, h};
        Color fill = Color::white(0.14f), text = theme::color::textPrimary;
        if (tag.playMethod) {
            const Color tint = tag.transcoding ? theme::color::warning : theme::color::success;
            fill = tint.withAlpha(0.2f);
            text = tint;
        }
        r.fillRoundRect(chip, theme::radius::pill, fill);
        r.drawText(*tag.layout, {chip.x + padX, chip.center().y - tag.layout->height * 0.5f}, text);
        x = chip.right() + gap;
    }
}

void PlayerScreen::loadLogo() {
    logo_.reset();
    if (!client_ || request_.offline) return;
    const std::string url = client_->imageUrl(item_, jellyfin::ImageType::Logo, {.maxWidth = 640});
    if (url.empty()) return;
    gfx::ImageRequest req;
    req.url = url;
    req.width = static_cast<int>(300 * ui::displayScale());
    req.height = static_cast<int>(64 * ui::displayScale());
    req.fit = gfx::ImageFit::Contain;
    logo_ = gfx::ImageCache::instance().load(req);
}

void PlayerScreen::drawBottomBar(ui::Renderer& r, float a) {
    const float dy = 22.0f * (1.0f - a);
    r.pushAlpha(a);
    r.pushTranslate(0, dy);

    const Rect bar = seekBarRect();
    const double pos = displayPosition();
    const double dur = std::max(0.0, duration_);
    const float grow = std::clamp(scrubGrow_.value(), 0.0f, 1.0f);

    // Chapter name and times above the bar.
    if (!chapters_.empty()) {
        const std::string* chapter = nullptr;
        for (const auto& c : chapters_)
            if (c.time <= pos + 0.25) chapter = &c.title;
        if (chapter && !chapter->empty())
            if (const auto* l = chapterText_.get(*chapter, theme::type::caption, {.maxWidth = 600}))
                r.drawText(*l, {bar.x, bar.y - 50}, theme::color::textSecondary);
    }
    if (const int64_t k = timeKey(pos); k != elapsedKey_) {
        elapsedKey_ = k;
        elapsedStr_ = formatTime(pos);
    }
    if (const auto* l = elapsedText_.get(elapsedStr_, theme::type::callout))
        r.drawText(*l, {bar.x, bar.y - 28}, theme::color::textPrimary);
    if (dur > 0) {
        if (const int64_t k = timeKey(dur - pos); k != remainingKey_) {
            remainingKey_ = k;
            remainingStr_ = "-" + formatTime(std::max(0.0, dur - pos));
        }
        if (const auto* l = remainingText_.get(remainingStr_, theme::type::callout))
            r.drawText(*l, {bar.right() - l->width, bar.y - 28}, theme::color::textSecondary);
    }

    // Seek bar: track, buffered range, played part, chapter gaps, thumb.
    const float h = 5.0f + 4.0f * grow;
    const Rect track{bar.x, bar.y + (bar.h - h) * 0.5f, bar.w, h};
    r.fillRoundRect(track, h * 0.5f, theme::color::progressTrack);
    if (dur > 0) {
        const float played = static_cast<float>(std::clamp(pos / dur, 0.0, 1.0));
        const float realPlayed = static_cast<float>(std::clamp(position_ / dur, 0.0, 1.0));
        if (cacheEnd_ > position_) {
            const float buffered = static_cast<float>(std::clamp(cacheEnd_ / dur, 0.0, 1.0));
            r.fillRoundRect({track.x + track.w * realPlayed, track.y, track.w * (buffered - realPlayed), h}, h * 0.5f,
                            Color::white(0.32f));
        }
        if (scrubbing_ && std::fabs(played - realPlayed) > 0.0005f) {
            r.fillRoundRect({track.x, track.y, track.w * realPlayed, h}, h * 0.5f, Color::white(0.55f));
            r.fillRoundRect({track.x, track.y, track.w * played, h}, h * 0.5f, theme::accentGradient);
        } else {
            r.fillRoundRect({track.x, track.y, track.w * played, h}, h * 0.5f, theme::accentGradient);
        }
        for (const auto& c : chapters_) {
            if (c.time <= 0.5 || c.time >= dur - 0.5) continue;
            const float x = track.x + track.w * static_cast<float>(c.time / dur);
            r.fillRect({x - 1.0f, track.y, 2.0f, h}, Color::black(0.6f));
        }
        const Vec2 thumb{track.x + track.w * played, track.y + h * 0.5f};
        const float radius = 7.0f + 3.0f * grow;
        r.drawShadow({thumb.x - radius, thumb.y - radius, radius * 2, radius * 2}, radius, 10, Color::black(0.5f));
        r.fillCircle(thumb, radius, Color::white());
    }

    // Control row.
    const float cx = theme::layout::screenWidth * 0.5f;
    auto iconButton = [&](Icon icon, float x, float size, HitAction action, bool enabled = true) {
        const Rect box{x - size * 0.5f, kControlsY - size * 0.5f, size, size};
        gfx::drawIcon(r, icon, box, enabled ? theme::color::textPrimary : theme::color::textTertiary, 1.1f);
        if (enabled) hits_.push_back({box.expanded(10), action});
    };
    iconButton(Icon::SeekBack, cx - 84, 30, HitAction::SeekBack, fileLoaded_);
    {
        const float rad = 24;
        const Vec2 c{cx, kControlsY};
        r.fillCircle(c, rad, Color::white(0.95f));
        const bool showPlay = paused_ || !fileLoaded_ || phase_ == Phase::Ended;
        gfx::drawIcon(r, showPlay ? Icon::Play : Icon::Pause, {c.x - 12, c.y - 12, 24, 24}, theme::color::textOnLight);
        hits_.push_back({Rect{c.x - rad, c.y - rad, rad * 2, rad * 2}.expanded(8), HitAction::PlayPause});
    }
    iconButton(Icon::SeekForward, cx + 84, 30, HitAction::SeekForward, fileLoaded_);
    if (next_) iconButton(Icon::SkipNext, cx + 152, 26, HitAction::Next);

    // Labelled shortcuts: Y (left) and X (right), also tappable.
    auto shortcut = [&](ui::Key key, Icon icon, CachedText& text, std::string_view label, float x, bool alignRight,
                        HitAction action) {
        const gfx::TextLayout* l = text.get(label, theme::type::callout);
        const float glyph = 22;
        const float w = glyph + 8 + 22 + 8 + (l ? l->width : 0.0f);
        const float left = alignRight ? x - w : x;
        float px = left;
        ui::drawButtonGlyph(r, key, {px + glyph * 0.5f, kControlsY}, glyph, Color::white(0.92f), Color::black());
        px += glyph + 8;
        gfx::drawIcon(r, icon, {px, kControlsY - 11, 22, 22}, theme::color::textPrimary);
        px += 22 + 8;
        if (l) r.drawText(*l, {px, kControlsY - l->height * 0.5f}, theme::color::textPrimary);
        hits_.push_back({Rect{left, kControlsY - 20, w, 40}.expanded(6), action});
    };
    shortcut(ui::Key::Y, Icon::Subtitles, tracksLabel_, "Audio & Subtitles", kMargin, false, HitAction::Tracks);
    shortcut(ui::Key::X, Icon::Settings, optionsLabel_, "Options", theme::layout::screenWidth - kMargin, true,
             HitAction::Options);

    r.popTransform();
    r.popAlpha();
}

const gfx::Texture* PlayerScreen::trickplaySheet(int sheet, Rect* uv) {
    if (!trickplay_ || !client_ || offline_) return nullptr;
    const auto& info = *trickplay_;
    auto it = trickplaySheets_.find(sheet);
    if (it == trickplaySheets_.end()) {
        // Keep the sheets around the one being scrubbed; drop the farthest.
        if (trickplaySheets_.size() > 6) {
            auto farthest = std::max_element(trickplaySheets_.begin(), trickplaySheets_.end(),
                                             [sheet](const auto& x, const auto& y) {
                                                 return std::abs(x.first - sheet) < std::abs(y.first - sheet);
                                             });
            trickplaySheets_.erase(farthest);
        }
        const float thumbW = 256.0f * ui::displayScale();
        const float f = std::min(1.0f, thumbW / static_cast<float>(info.width));
        gfx::ImageRequest req;
        req.url = client_->trickplayTileUrl(item_.id, trickplaySource_, info.width, sheet);
        req.width = static_cast<int>(info.width * info.tileWidth * f);
        req.height = static_cast<int>(info.height * info.tileHeight * f);
        req.fit = gfx::ImageFit::Contain;
        it = trickplaySheets_.emplace(sheet, gfx::ImageCache::instance().load(req)).first;
    }
    const gfx::Texture* tex = it->second->texture();
    if (!tex || it->second->width() <= 0 || it->second->height() <= 0) return nullptr;
    // The decoded sheet keeps the column count; the last sheet may have fewer rows.
    const float scale = static_cast<float>(it->second->width()) / static_cast<float>(info.width * info.tileWidth);
    const auto tile = info.tileAt(static_cast<int64_t>(scrubTarget_ * 1000.0));
    *uv = {tile.x * scale / it->second->width(), tile.y * scale / it->second->height(),
           tile.w * scale / it->second->width(), tile.h * scale / it->second->height()};
    return tex;
}

void PlayerScreen::drawSeekPreview(ui::Renderer& r, float a) {
    const float g = std::clamp(scrubGrow_.value(), 0.0f, 1.0f) * a;
    if (g <= 0.01f || duration_ <= 0) return;
    const Rect bar = seekBarRect();
    const float x = bar.x + bar.w * static_cast<float>(std::clamp(scrubTarget_ / duration_, 0.0, 1.0));
    if (const int64_t k = timeKey(scrubTarget_); k != previewKey_) {
        previewKey_ = k;
        previewStr_ = formatTime(scrubTarget_);
    }
    const double delta = scrubTarget_ - position_;
    if (const int64_t k = timeKey(std::fabs(delta)) * 2 + (delta < 0 ? 1 : 0); k != previewDeltaKey_) {
        previewDeltaKey_ = k;
        previewDeltaStr_ = std::format("{}{}", delta >= 0 ? "+" : "-", formatTime(std::fabs(delta)));
    }

    r.pushAlpha(g);
    if (trickplay_) {
        const float w = 256;
        const float h = w * static_cast<float>(trickplay_->height) / static_cast<float>(std::max(1, trickplay_->width));
        const float left = std::clamp(x - w * 0.5f, kMargin, theme::layout::screenWidth - kMargin - w);
        const Rect box{left, bar.y - 56 - h, w, h};
        r.drawShadow(box, theme::radius::md, 24, Color::black(0.6f), {0, 8});
        Rect uv;
        if (const gfx::Texture* tex = trickplaySheet(trickplay_->tileAt(static_cast<int64_t>(scrubTarget_ * 1000.0)).sheetIndex, &uv)) {
            r.drawImage(*tex, box, {.fit = gfx::ImageFit::Fill, .radius = theme::radius::md, .uv = uv});
        } else {
            r.fillShimmer(box, theme::radius::md, theme::color::skeleton, static_cast<float>(std::fmod(spinnerPhase_ * 0.8, 1.0)));
        }
        r.strokeRoundRect(box, theme::radius::md, 2, Color::white(0.9f));
        if (const auto* tl = previewTime_.get(previewStr_, theme::type::headline)) {
            const Rect pill{box.center().x - tl->width * 0.5f - 12, box.bottom() - tl->height - 16, tl->width + 24,
                            tl->height + 8};
            r.fillRoundRect(pill, theme::radius::pill, Color::black(0.72f));
            r.drawText(*tl, {pill.x + 12, pill.y + 4}, theme::color::textPrimary);
        }
        if (const auto* dl = previewDelta_.get(previewDeltaStr_, theme::type::caption))
            r.drawText(*dl, {box.x, box.y - 22}, theme::color::textSecondary);
    } else {
        const auto* tl = previewTimeLarge_.get(previewStr_, theme::type::title3);
        const auto* dl = previewDeltaLarge_.get(previewDeltaStr_, theme::type::caption);
        if (tl && dl) {
            const float w = std::max(tl->width, dl->width) + 28;
            const float left = std::clamp(x - w * 0.5f, kMargin, theme::layout::screenWidth - kMargin - w);
            const Rect pill{left, bar.y - 98, w, tl->height + dl->height + 14};
            r.drawShadow(pill, theme::radius::md, 18, Color::black(0.5f), {0, 6});
            r.fillRoundRect(pill, theme::radius::md, theme::color::glassFill);
            r.drawText(*tl, {pill.center().x - tl->width * 0.5f, pill.y + 6}, theme::color::textPrimary);
            r.drawText(*dl, {pill.center().x - dl->width * 0.5f, pill.y + 6 + tl->height}, theme::color::textSecondary);
        }
    }
    r.popAlpha();
}

void PlayerScreen::drawCenter(ui::Renderer& r) {
    const Vec2 c = bounds().center();
    const bool busy = busyFor_ > 0.25 && phase_ != Phase::Error;
    if (busy) {
        const float t = static_cast<float>(spinnerPhase_);
        const float sweep = 1.2f + 0.9f * std::sin(t * 2.4f);
        r.fillCircle(c, 40, Color::black(0.45f));
        r.drawArc(c, 22, 4, t * 5.2f, sweep, theme::color::accentBright);
        std::string_view label;
        if (recovering_ || reloading_) {
            label = "Reconnecting";
        } else if (phase_ == Phase::Loading) {
            label = "Loading";
        } else if (pausedForCache_ && bufferingPct_ > 0) {
            if (bufferingPct_ != bufferingKey_) {
                bufferingKey_ = bufferingPct_;
                bufferingStr_ = std::format("Buffering {}%", bufferingPct_);
            }
            label = bufferingStr_;
        } else if (pausedForCache_) {
            label = "Buffering";
        }
        if (!label.empty())
            if (const auto* l = centerLabel_.get(label, theme::type::callout))
                r.drawText(*l, {c.x - l->width * 0.5f, c.y + 52}, theme::color::textSecondary);
    }
    const float f = std::clamp(flash_.value(), 0.0f, 1.0f);
    if (f > 0.01f && !busy) {
        const float rad = 44.0f * (1.25f - 0.25f * f);
        r.fillCircle(c, rad, Color::black(0.5f * f));
        gfx::drawIcon(r, flashIcon_, {c.x - rad * 0.5f, c.y - rad * 0.5f, rad, rad}, Color::white(f));
    }
}

void PlayerScreen::drawSkipButton(ui::Renderer& r) {
    const float a = std::clamp(skipAppear_.value(), 0.0f, 1.0f);
    if (a <= 0.01f || skipLabel_.empty()) return;
    const gfx::TextLayout* l = skipText_.get(skipLabel_, theme::type::button);
    if (!l) return;
    const float w = 22 + 10 + l->width + 36;
    const float h = 46;
    const float y = (osdVisible() ? 560.0f : 600.0f) - std::clamp(upNextAppear_.value(), 0.0f, 1.0f) * 140.0f;
    const Rect box{theme::layout::screenWidth - kMargin - w + (1 - a) * 40, y, w, h};
    r.pushAlpha(a);
    r.drawShadow(box, theme::radius::pill, 24, Color::black(0.5f), {0, 8});
    r.fillRoundRect(box, theme::radius::pill, Color::white(0.95f));
    ui::drawButtonGlyph(r, ui::Key::A, {box.x + 18 + 11, box.center().y}, 22, Color::black(0.85f), Color::white());
    r.drawText(*l, {box.x + 18 + 22 + 10, box.center().y - l->height * 0.5f}, theme::color::textOnLight);
    const double span = activeSegment_ ? activeSegment_->endSeconds() - activeSegment_->startSeconds() : 0.0;
    if (span > 0) {
        const float p = static_cast<float>(std::clamp((position_ - activeSegment_->startSeconds()) / span, 0.0, 1.0));
        r.fillRoundRect({box.x + 20, box.bottom() - 6, (box.w - 40) * (1 - p), 2.5f}, 1.25f, theme::color::accent);
    }
    r.popAlpha();
    if (activeSegment_) hits_.push_back({box, HitAction::Skip});
}

void PlayerScreen::drawUpNext(ui::Renderer& r) {
    const float a = std::clamp(upNextAppear_.value(), 0.0f, 1.0f);
    if (a <= 0.01f || !next_) return;
    // Tall enough that a two-line title clears the countdown line and the button hints.
    const float w = 420, h = 132;
    const float y = (osdVisible() ? 470.0f : 560.0f) + (1 - a) * 30;
    const Rect card{theme::layout::screenWidth - kMargin - w, y, w, h};
    r.pushAlpha(a);
    r.drawShadow(card, theme::radius::lg, 36, Color::black(0.65f), {0, 12});
    r.fillRoundRect(card, theme::radius::lg, Color::hex(0x15151E, 0.96f));
    r.strokeRoundRect(card, theme::radius::lg, 1, theme::color::stroke);

    const Rect thumb{card.x + 12, card.y + (h - 99) * 0.5f, 176, 99};
    const gfx::Texture* tex = nextImage_ ? nextImage_->texture() : nullptr;
    if (tex) {
        r.drawImage(*tex, thumb, {.fit = gfx::ImageFit::Cover, .radius = theme::radius::sm});
    } else {
        r.fillRoundRect(thumb, theme::radius::sm, theme::color::surfaceHigh);
        gfx::drawIcon(r, Icon::Tv, {thumb.center().x - 16, thumb.center().y - 16, 32, 32}, theme::color::textTertiary);
    }

    const float tx = thumb.right() + 14;
    const float tw = card.right() - 14 - tx;
    if (const auto* l = upNextTitle_.get("UP NEXT", theme::type::overline))
        r.drawText(*l, {tx, card.y + 14}, theme::color::accentBright);
    const jellyfin::BaseItem& n = next_->item;
    if (upNextFor_ != n.id || upNextNameStr_.empty()) {
        upNextFor_ = n.id;
        upNextNameStr_ = n.parentIndexNumber >= 0 && n.indexNumber >= 0
                             ? std::format("S{:02}E{:02}  ·  {}", n.parentIndexNumber, n.indexNumber, n.name)
                             : n.name;
    }
    if (const auto* l = upNextName_.get(upNextNameStr_, theme::type::headline, {.maxWidth = tw, .maxLines = 2}))
        r.drawText(*l, {tx, card.y + 32}, theme::color::textPrimary);

    const bool autoplay = autoplayNext_ && upNextTimer_.remaining() > 0;
    const float by = card.bottom() - 30;
    if (autoplay) {
        if (const int64_t k = static_cast<int64_t>(std::ceil(upNextTimer_.remaining())); k != countdownKey_) {
            countdownKey_ = k;
            countdownStr_ = std::format("Playing in {}", k);
        }
        if (const auto* l = upNextCountdown_.get(countdownStr_, theme::type::caption))
            r.drawText(*l, {tx, by - 22}, theme::color::textSecondary);
        const float p = static_cast<float>(upNextTimer_.progress());
        r.fillRoundRect({tx, by - 4, tw, 3}, 1.5f, theme::color::progressTrack);
        r.fillRoundRect({tx, by - 4, tw * p, 3}, 1.5f, theme::accentGradient);
    }
    float hx = tx;
    auto hint = [&](ui::Key key, CachedText& text, std::string_view label) {
        ui::drawButtonGlyph(r, key, {hx + 9, by + 12}, 18, Color::white(0.9f), Color::black());
        hx += 18 + 6;
        if (const auto* l = text.get(label, theme::type::footnote)) {
            r.drawText(*l, {hx, by + 12 - l->height * 0.5f}, theme::color::textSecondary);
            hx += l->width + 16;
        }
    };
    hint(ui::Key::A, upNextPlay_, "Play now");
    const float hideX = hx;
    hint(ui::Key::B, upNextHide_, "Hide");
    r.popAlpha();
    if (upNextVisible_) {
        hits_.push_back({card, HitAction::UpNext});
        hits_.push_back({{hideX - 4, by - 3, card.right() - hideX, 34}, HitAction::DismissUpNext});
    }
}

void PlayerScreen::drawError(ui::Renderer& r) {
    // Behind the error dialog nothing is shown; once it is gone this keeps a clear way out.
    if (phase_ != Phase::Error || modalOpen_) return;
    const Vec2 c = bounds().center();
    const float w = 560;
    const gfx::TextLayout* title = errorTitleText_.get(errorTitle_.empty() ? std::string_view("Playback stopped") : errorTitle_,
                                                       theme::type::title2, {.maxWidth = w, .align = gfx::TextAlign::Center});
    const gfx::TextLayout* body = errorBodyText_.get(
        errorMessage_, theme::type::body, {.maxWidth = w, .maxLines = 4, .align = gfx::TextAlign::Center});
    const gfx::TextLayout* hint = errorHint_.get("Back", theme::type::callout);
    float y = c.y - 90;
    gfx::drawIcon(r, Icon::Warning, {c.x - 22, y, 44, 44}, theme::color::danger);
    y += 60;
    if (title) {
        r.drawText(*title, {c.x - title->width * 0.5f, y}, theme::color::textPrimary);
        y += title->height + 10;
    }
    if (body && !errorMessage_.empty()) {
        r.drawText(*body, {c.x - body->width * 0.5f, y}, theme::color::textSecondary);
        y += body->height + 22;
    }
    if (hint) {
        const float glyph = 22;
        const float bw = glyph + 8 + hint->width;
        const Rect box{c.x - bw * 0.5f - 18, y, bw + 36, 40};
        r.fillRoundRect(box, theme::radius::pill, theme::color::buttonSecondary);
        ui::drawButtonGlyph(r, ui::Key::B, {box.x + 18 + glyph * 0.5f, box.center().y}, glyph, Color::white(0.92f),
                            Color::black());
        r.drawText(*hint, {box.x + 18 + glyph + 8, box.center().y - hint->height * 0.5f}, theme::color::textPrimary);
        hits_.push_back({box, HitAction::ErrorBack});
    }
}

void PlayerScreen::refreshTags() {
    std::vector<OsdTag> tags;
    if (offline_) {
        tags.push_back({.text = "Downloaded", .playMethod = true});
    } else if (plan_) {
        const char* method = plan_->method == jellyfin::PlayMethod::DirectPlay     ? "Direct Play"
                             : plan_->method == jellyfin::PlayMethod::DirectStream ? "Direct Stream"
                                                                                   : "Transcoding";
        tags.push_back({.text = method, .playMethod = true, .transcoding = plan_->isTranscoding()});
    }
    if (mpv_ && fileLoaded_) {
        auto add = [&tags](std::string text) {
            if (!text.empty()) tags.push_back({.text = std::move(text)});
        };
        add(resolutionLabel(mpv_->getInt("width").value_or(0), mpv_->getInt("height").value_or(0)));
        std::string pixelFormat = mpv_->getString("video-params/hw-pixelformat");
        if (pixelFormat.empty()) pixelFormat = mpv_->getString("video-params/pixelformat");
        add(videoCodecLabel(mpv_->getString("video-format"), pixelFormat));
        add(audioCodecLabel(mpv_->getString("audio-codec-name"), mpv_->getInt("audio-params/channel-count").value_or(0)));
    }
    const bool same = std::equal(tags.begin(), tags.end(), osdTags_.begin(), osdTags_.end(),
                                 [](const OsdTag& a, const OsdTag& b) {
                                     return a.text == b.text && a.transcoding == b.transcoding && b.layout &&
                                            b.layout->scale == gfx::text::displayScale();
                                 });
    if (same) return;
    for (auto& t : tags) t.layout = gfx::text::layout(t.text, theme::type::caption);
    osdTags_ = std::move(tags);
}

void PlayerScreen::refreshInfo() {
    infoLines_.clear();
    if (!mpv_) return;
    const float valueW = 470 - 16 * 2 - 130;
    auto add = [this, valueW](std::string_view k, const std::string& v) {
        if (v.empty()) return;
        infoLines_.push_back({gfx::text::layout(k, theme::type::footnote),
                              gfx::text::layout(v, theme::type::footnote, {.maxWidth = valueW})});
    };
    add("Play method", offline_ ? "Local file" : plan_ ? std::string(jellyfin::playMethodName(plan_->method)) : "-");
    add("Container", mpv_->getString("file-format"));
    const auto w = mpv_->getInt("width"), h = mpv_->getInt("height");
    const auto fps = mpv_->getDouble("container-fps");
    std::string video = mpv_->getString("video-format");
    if (w && h) video += std::format("  {}x{}", *w, *h);
    if (fps && *fps > 0) video += std::format(" @ {:.3g} fps", *fps);
    add("Video", video);
    add("Decoder", hwdec_.empty() || hwdec_ == "no" ? std::string("Software") : "Hardware (" + hwdec_ + ")");
    add("Video bitrate", formatBitrate(mpv_->getDouble("video-bitrate").value_or(0)));
    std::string audio = mpv_->getString("audio-codec-name");
    if (auto ch = mpv_->getInt("audio-params/channel-count")) audio += std::format("  {} ch", *ch);
    if (auto sr = mpv_->getInt("audio-params/samplerate")) audio += std::format("  {:.1f} kHz", *sr / 1000.0);
    add("Audio", audio);
    add("Audio bitrate", formatBitrate(mpv_->getDouble("audio-bitrate").value_or(0)));
    add("Dropped frames", std::format("{} output / {} decoder", mpv_->getInt("frame-drop-count").value_or(0),
                                      mpv_->getInt("decoder-frame-drop-count").value_or(0)));
    const double cacheSecs = mpv_->getDouble("demuxer-cache-duration").value_or(0);
    const double cacheSpeed = static_cast<double>(mpv_->getInt("cache-speed").value_or(0));
    add("Cache", std::format("{:.0f} s  ({:.0f} KiB/s)", cacheSecs, cacheSpeed / 1024.0));
    if (const Track* s = tracks_.selectedSubtitle()) add("Subtitles", TrackManager::describe(*s));
    const auto& d = App::instance().display();
    add("Output", std::format("{}x{} {}  ·  {:.0f} fps UI", d.pixelWidth, d.pixelHeight, d.docked ? "docked" : "handheld",
                              App::instance().fps()));
    if (std::fabs(speed_ - 1.0) > 0.01) add("Speed", std::format("{:.2f}x", speed_));
}

void PlayerScreen::drawInfoOverlay(ui::Renderer& r) {
    if (infoLines_.empty()) return;
    const float pad = 16, lineH = 21, keyW = 130;
    const float w = 470;
    const float h = pad * 2 + 24 + lineH * static_cast<float>(infoLines_.size());
    const Rect panel{kMargin, 100, w, h};
    r.fillRoundRect(panel, theme::radius::md, Color::black(0.72f));
    r.strokeRoundRect(panel, theme::radius::md, 1, theme::color::stroke);
    r.drawText("PLAYBACK INFO", {panel.x + pad, panel.y + pad}, theme::type::overline, theme::color::accentBright);
    float y = panel.y + pad + 24;
    for (const auto& line : infoLines_) {
        if (line.key) r.drawText(*line.key, {panel.x + pad, y}, theme::color::textSecondary);
        if (line.value) r.drawText(*line.value, {panel.x + pad + keyW, y}, theme::color::textPrimary);
        y += lineH;
    }
}

}  // namespace karafin::player
