#include "screens/splash_screen.hpp"

#include <algorithm>
#include <cmath>

#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/platform.hpp"
#include "core/settings.hpp"
#include "downloads/manager.hpp"
#include "gfx/image.hpp"
#include "gfx/text.hpp"
#include "screens/brand_art.hpp"
#include "screens/common.hpp"
#include "screens/library_lock.hpp"
#include "screens/session.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kMinColdTime = 1.3f;
constexpr float kMinWarmTime = 0.35f;
constexpr float kIcon = 176.0f;
constexpr float kWordmarkHeight = 40.0f;  // Karastream's splash draws its wordmark.png this tall
// Swimming in place, tools/make_brand.py swim() animated: each stroke squeezes the bell (narrower,
// longer) and darts the jellyfish along its lean; it relaxes and drifts back while a wave runs
// down the tentacles.
constexpr float kSwimPeriod = 1.6f;    // seconds per stroke
constexpr float kSwimRise = 8.0f;      // dart along the lean at full contraction (px of a kIcon mark)
constexpr float kSwimSqueeze = 0.22f;  // bell rim narrowing at full contraction
constexpr float kSwimBellStretch = 0.10f;
constexpr float kSwimTailStretch = 0.12f;
constexpr int kSwimBands = 32;  // the sprite is drawn in bands across its axis, each sheared to follow

// Decode at the drawn size: the area-averaged shrink stays smooth, while GL minifying the
// full-size texture (no mipmaps) leaves jagged edges. The art is made at twice the docked size.
gfx::ImagePtr loadArt(const char* name, float width, float height) {
    gfx::ImageRequest req;
    req.url = paths::romfs(name);
    req.width = static_cast<int>(std::ceil(width * ui::displayScale()));
    req.height = static_cast<int>(std::ceil(height * ui::displayScale()));
    req.fit = gfx::ImageFit::Fill;
    return gfx::ImageCache::instance().load(req);
}

// Bell contraction 0..1 over one stroke: a quick squeeze, then a slower relaxation.
float contraction(float phase) {
    constexpr float kSqueeze = 0.35f;
    if (phase < kSqueeze) return 0.5f - 0.5f * std::cos(gfx::kPi * phase / kSqueeze);
    return 0.5f + 0.5f * std::cos(gfx::kPi * (phase - kSqueeze) / (1.0f - kSqueeze));
}

// The jellyfish in the `mark` square at time `t`. Works in romfs/jelly.png's source px (x across,
// y down the body axis): each band of rows is squeezed about the axis, offset by the tentacle wave
// and stretched along the axis, then the whole jellyfish is leaned and placed.
void drawSwimmingJelly(gfx::Renderer& r, const gfx::Texture& tex, const Rect& mark, float t) {
    using namespace brand;
    const float phase = std::fmod(t / kSwimPeriod, 1.0f);
    const float c = contraction(phase);
    const float w = kJellySrcW, h = kJellySrcH, pad = kJellyPad;
    const float rim = kSwimRim * h;
    const float lean = kJellyLeanDeg * gfx::kPi / 180.0f;
    const float k = kJellyScale * mark.w;  // screen px per source px
    const float rise = kSwimRise * c * mark.w / kIcon;
    const gfx::Affine place = gfx::Affine::translate(mark.x + kJellyCX * mark.w + std::sin(lean) * rise,
                                                     mark.y + kJellyCY * mark.h - std::cos(lean) * rise) *
                              gfx::Affine::rotate(lean) * gfx::Affine::scale(k, k) *
                              gfx::Affine::translate(-w * 0.5f, -h * 0.5f);
    auto along = [&](float y) {
        const float bell = 1.0f + kSwimBellStretch * c, tail = 1.0f + kSwimTailStretch * c;
        return (y <= rim ? y * bell : rim * bell + (y - rim) * tail) - 0.012f * h * c;
    };
    auto across = [&](float y) {
        const float v = std::clamp(y / rim, 0.0f, 1.0f);
        return 1.0f - kSwimSqueeze * c * v * std::sqrt(v);
    };
    auto wave = [&](float y) {
        const float s = std::clamp((y - rim) / (h - rim), 0.0f, 1.0f);
        return kSwimWave * w * s * std::sqrt(s) * std::sin(2.0f * gfx::kPi * phase - kSwimWaveTravel * gfx::kPi * s);
    };
    const float top = -pad, bottom = h + pad, step = (bottom - top) / kSwimBands;
    const float cx = w * 0.5f;
    // Neighbouring bands share their edge exactly (same position and wave there), so with hard
    // quad edges they join without seams or double coverage; the sprite's margin and alpha keep the
    // outline smooth.
    for (int i = 0; i < kSwimBands; ++i) {
        const float y0 = top + step * static_cast<float>(i);
        const float y1 = i + 1 == kSwimBands ? bottom : y0 + step;
        const float dy = (along(y1) - along(y0)) / (y1 - y0);
        const float dx = (wave(y1) - wave(y0)) / (y1 - y0);
        const float sx = across((y0 + y1) * 0.5f);
        const gfx::Affine band{sx, 0.0f, dx, dy, cx - cx * sx + wave(y0) - y0 * dx, along(y0) - y0 * dy};
        r.pushTransform(place * band);
        r.drawImage(tex, {-pad, y0, w + 2 * pad, y1 - y0},
                    {.fit = gfx::ImageFit::Fill,
                     .uv = {0.0f, (y0 + pad) / (h + 2 * pad), 1.0f, (y1 - y0) / (h + 2 * pad)},
                     .antialias = false});
        r.popTransform();
    }
}
}  // namespace

SplashScreen::SplashScreen(bool coldStart) : coldStart_(coldStart) {
    setShowsHintBar(false);
    spinner_ = add<Spinner>(28, theme::color::accentBright);
    spinner_->setFrame({theme::layout::screenWidth * 0.5f - 14, 470, 28, 28});
    spinner_->alpha.set(0.0f);
    statusText_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    statusText_->setAlign(gfx::TextAlign::Center);
    statusText_->setFrame({240, 512, 800, 24});
    statusText_->alpha.set(0.0f);
    problem_ = add<StatusView>();
    problem_->setFrame({0, 300, theme::layout::screenWidth, 380});
    logo_.springTo(1.0f, SpringSpec{0.9f, 0.78f});
    frame_ = loadArt("frame.png", kIcon, kIcon);
    const float jellyScale = brand::kJellyScale * kIcon;
    jelly_ = loadArt("jelly.png", (brand::kJellySrcW + 2 * brand::kJellyPad) * jellyScale,
                     (brand::kJellySrcH + 2 * brand::kJellyPad) * jellyScale);
    const float ws = kWordmarkHeight / brand::kWordmarkRef;
    wordmark_ = loadArt("wordmark.png", brand::kWordmarkW * ws, brand::kWordmarkH * ws);
}

SplashScreen::~SplashScreen() { token_.cancel(); }

void SplashScreen::onShow() {
    if (started_) return;
    started_ = true;
    timeline().after(coldStart_ ? 0.25f : 0.0f, [this] { restore(); }, lifetime());
}

void SplashScreen::restore() {
    token_.cancel();
    token_.reset();
    outcome_ = Outcome::Pending;
    problem_->hide();
    setShowsHintBar(false);
    lift_.springTo(0.0f, theme::motion::gentle);
    auto last = jf::AccountStore::instance().lastSession(platform::switchProfileId());
    if (!last) {
        outcome_ = Outcome::Servers;
        maybeNavigate();
        return;
    }
    serverId_ = last->server.id;
    userId_ = last->user.id;
    userName_ = last->user.name;
    serverName_ = last->server.name.empty() ? last->server.url : last->server.name;
    statusText_->setText("Connecting to " + serverName_ + "...");
    spinner_->alpha.tweenTo(1.0f, 0.4f, ease::outCubic);
    statusText_->alpha.tweenTo(1.0f, 0.4f, ease::outCubic);
    client_ = jf::Client::fromStored(last->server, &last->user);
    client_->validateSession().then(
        [this](jf::Result<jf::UserDto> r) {
            log::mark("screens", "session check: {}", r ? "ok" : r.error.describe());
            if (r) {
                outcome_ = Outcome::Home;
            } else if (r.error.kind == jf::ErrorKind::Unauthorized) {
                outcome_ = Outcome::Login;
            } else if (r.error.isConnectivity()) {
                outcome_ = Outcome::Offline;
            } else {
                log::warn("screens", "session restore failed: {}", r.error.describe());
                outcome_ = Outcome::Error;
                statusText_->setText(errorMessage(r.error));
            }
            maybeNavigate();
        },
        token_);
}

void SplashScreen::maybeNavigate() {
    if (navigated_ || outcome_ == Outcome::Pending || dialogOpen_) return;
    if (t_ < (coldStart_ ? kMinColdTime : kMinWarmTime)) return;
    if (coldStart_ && !warningChecked_ && platform::isAppletMode()) return;  // warning first
    switch (outcome_) {
        case Outcome::Home:
            navigated_ = true;
            if (LibraryLock::profileNeedsCode(serverId_, userId_)) {
                spinner_->alpha.tweenTo(0.0f, 0.2f);
                statusText_->alpha.tweenTo(0.0f, 0.2f);
                LibraryLock::instance().askProfileCode(serverId_, userId_, userName_, [client = client_] { enterApp(client); },
                                                       [serverId = serverId_] { goToLogin(serverId); });
            } else {
                enterApp(client_);
            }
            break;
        case Outcome::Login:
            navigated_ = true;
            goToLogin(serverId_);
            break;
        case Outcome::Servers:
            navigated_ = true;
            goToServerSelect();
            break;
        case Outcome::Offline:
            showProblem("Can't reach " + serverName_,
                        "Check that the console is online and the server is running. Your downloads are still "
                        "available offline.",
                        true);
            break;
        case Outcome::Error:
            showProblem("Couldn't sign in", statusText_->text(), false);
            break;
        case Outcome::Pending: break;
    }
}

void SplashScreen::showProblem(const std::string& title, const std::string& message, bool offline) {
    if (problem_->showing()) return;
    spinner_->alpha.tweenTo(0.0f, 0.2f);
    statusText_->alpha.tweenTo(0.0f, 0.2f);
    lift_.springTo(1.0f, theme::motion::gentle);
    setShowsHintBar(true);
    std::vector<StatusAction> actions;
    actions.push_back({"Try again", gfx::Icon::Refresh, [this] { restore(); }, !offline});
    // A server error (not just "unreachable") still leaves the downloads playable.
    if (offline || downloads::DownloadManager::instance().storage().completedCount > 0) {
        actions.push_back({"Offline downloads", gfx::Icon::Download, [this] {
                               navigated_ = true;
                               enterOffline(client_);
                           },
                           offline});
    }
    actions.push_back({"Change server", gfx::Icon::Server, [this] {
                           navigated_ = true;
                           goToServerSelect();
                       }});
    problem_->showMessage(offline ? gfx::Icon::Wifi : gfx::Icon::Warning, title, message, std::move(actions),
                          offline ? theme::color::warning : theme::color::danger);
    problem_->focusAction();
}

void SplashScreen::showAppletWarning() {
    warningChecked_ = true;
    if (Settings::instance().get<bool>("ui.applet_warning_hidden", false)) return;
    DialogSpec spec;
    spec.title = "Running in applet mode";
    spec.message =
        "Karafin was opened from the Album, which leaves much less memory for video. Playback may stutter or "
        "fail. For the best experience, hold R while launching any game to start Karafin with full memory "
        "(title override).";
    spec.icon = gfx::Icon::Warning;
    spec.iconColor = theme::color::warning;
    spec.buttons = {{"Don't show again", ButtonStyle::Secondary,
                     [] { Settings::instance().set("ui.applet_warning_hidden", true); }},
                    {"Continue", ButtonStyle::Primary, {}}};
    spec.cancelIndex = 1;
    spec.defaultIndex = 1;
    auto dialog = std::make_unique<Dialog>(std::move(spec));
    dialog->onDismissed = guard([this] {
        dialogOpen_ = false;
        maybeNavigate();
    });
    dialogOpen_ = true;
    present(std::move(dialog));
}

void SplashScreen::update(float dt) {
    t_ += dt;
    logo_.update(dt);
    lift_.update(dt);
    if (coldStart_ && !warningChecked_ && t_ > 0.7f) {
        if (platform::isAppletMode()) showAppletWarning();
        else warningChecked_ = true;
    }
    maybeNavigate();
    Screen::update(dt);
}

void SplashScreen::draw(Renderer& r) {
    Screen::draw(r);
    using namespace theme;
    const Rect b = bounds();
    // Slowly drifting ambient glows.
    const float t = t_;
    const float a = std::clamp(logo_.value(), 0.0f, 1.0f);
    const Vec2 g1{b.w * 0.32f + std::sin(t * 0.35f) * 60.0f, b.h * 0.38f + std::cos(t * 0.27f) * 30.0f};
    const Vec2 g2{b.w * 0.70f + std::cos(t * 0.31f) * 70.0f, b.h * 0.58f + std::sin(t * 0.22f) * 40.0f};
    r.drawShadow({g1.x - 120, g1.y - 120, 240, 240}, 120, 260, color::accentDeep.withAlpha(0.35f * a));
    r.drawShadow({g2.x - 110, g2.y - 110, 220, 220}, 110, 240, color::accent.withAlpha(0.22f * a));

    const float lift = std::clamp(lift_.value(), -0.2f, 1.2f);
    const float s = 0.86f + 0.14f * logo_.value();
    const Vec2 center{b.w * 0.5f, 300.0f - 120.0f * lift};
    r.pushTransform(gfx::Affine::scaleAround(s, center));
    r.pushAlpha(a);
    const Rect icon{center.x - kIcon * 0.5f, center.y - 180.0f, kIcon, kIcon};
    // The glow brightens with each stroke of the swimming jellyfish.
    const float c = contraction(std::fmod(t / kSwimPeriod, 1.0f));
    const Rect glow = icon.inset(18);
    r.drawShadow(glow, glow.w * 0.5f, 70 + 20 * c, color::accent.withAlpha(0.18f + 0.14f * c));
    if (const gfx::Texture* tex = frame_ ? frame_->texture() : nullptr)
        r.drawImage(*tex, icon, {.fit = gfx::ImageFit::Fill});
    if (const gfx::Texture* tex = jelly_ ? jelly_->texture() : nullptr) {
        if (!artLogged_) {
            artLogged_ = true;
            log::mark("screens", "splash art drawn ({}x{})", tex->width(), tex->height());
        }
        drawSwimmingJelly(r, *tex, icon, t);
    }
    // Same glyph size and position as Karastream's wordmark; the jellyfish on the i may rise above it.
    const float ws = kWordmarkHeight / brand::kWordmarkRef;
    const float wordTop = icon.bottom() + 30;
    if (const gfx::Texture* tex = wordmark_ ? wordmark_->texture() : nullptr) {
        const Rect word{center.x - brand::kWordmarkW * ws * 0.5f, wordTop + brand::kWordmarkOffsetY * ws,
                        brand::kWordmarkW * ws, brand::kWordmarkH * ws};
        r.drawImage(*tex, word, {.fit = gfx::ImageFit::Fill});
    }
    const float tagA = std::clamp((t_ - 0.45f) / 0.5f, 0.0f, 1.0f);
    auto tag = gfx::text::layout("Jellyfin for Nintendo Switch", type::headline);
    r.drawText(*tag, {center.x - tag->width * 0.5f, wordTop + kWordmarkHeight + 18},
               color::textSecondary.mulAlpha(tagA));
    r.popAlpha();
    r.popTransform();
}

}  // namespace karafin::screens
