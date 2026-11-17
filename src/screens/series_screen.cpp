#include "screens/series_screen.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/item_meta.hpp"
#include "screens/widgets/episode_row.hpp"
#include "screens/widgets/item_card.hpp"
#include "screens/main_screen.hpp"
#include "ui/root.hpp"
#include "ui/widgets/list_menu.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kTextWidth = 720;
constexpr float kLogoMaxW = 460;
constexpr float kLogoMaxH = 130;

std::string seasonTitle(const jf::BaseItem& s, bool shortForm) {
    if (!shortForm) return s.name.empty() ? std::format("Season {}", std::max(0, s.indexNumber)) : s.name;
    if (s.indexNumber == 0) return "SP";
    return s.indexNumber > 0 ? std::format("S{}", s.indexNumber) : s.name;
}
}  // namespace

SeriesScreen::SeriesScreen(std::string seriesId, std::string seasonId)
    : seriesId_(std::move(seriesId)), preferredSeason_(std::move(seasonId)) {
    onAppMenu = [] { showAppMenu(); };
    backdrop_ = add<Backdrop>();
    backdrop_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    backdrop_->setIntensity(1.0f);

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    page_->setRevealWholeChild(true);
    page_->setRevealInsets(80, 64);
    page_->setShowsIndicator(true);
    page_->content()->setSpacing(14);
    page_->content()->setPadding({0, 0, 0, 70});

    header_ = page_->content()->add<Box>(Axis::Vertical);
    header_->setPadding({theme::layout::marginX, 150, theme::layout::marginX, 20}).setSpacing(12).setAlign(Align::Start);
    header_->setRememberFocus(true);
    logo_ = header_->add<ImageView>();
    logo_->setFit(gfx::ImageFit::Contain);
    logo_->setShimmer(false);
    logo_->setPlaceholderColor(Color::transparent());
    logo_->setVisible(false);
    title_ = header_->add<Label>("", theme::type::hero);
    title_->setMaxLines(2);
    title_->setPreferredSize(kTextWidth, -1);
    chips_ = header_->add<ChipRow>();
    genres_ = header_->add<Label>("", theme::type::callout, theme::color::textSecondary);
    genres_->setPreferredSize(kTextWidth, -1);
    overview_ = header_->add<OverviewText>(3);
    overview_->setPreferredSize(kTextWidth - 40, -1);

    auto* buttons = header_->add<Box>(Axis::Horizontal);
    buttons->setSpacing(12).setMargin({0, 12, 0, 0});
    buttons->setRememberFocus(true);
    play_ = buttons->add<ActionButton>("Play", gfx::Icon::Play, ButtonStyle::Primary);
    download_ = buttons->add<DownloadButton>(false);
    fav_ = buttons->add<ActionButton>("", gfx::Icon::Heart, ButtonStyle::Icon, "Favorite");
    fav_->setActiveColor(theme::color::danger);
    watched_ = buttons->add<ActionButton>("", gfx::Icon::Check, ButtonStyle::Icon, "Watched");
    buttons->setRememberedFocus(play_);
    header_->setRememberedFocus(buttons);
    setRememberedFocus(play_);
    play_->onActivate = [this] {
        if (nextUp_) playItem(*nextUp_);
        else playSeries(seriesId_, token_);
    };
    fav_->onActivate = [this] { toggleFavorite(); };
    watched_->onActivate = [this] { toggleWatched(); };

    tabsRow_ = page_->content()->add<Box>(Axis::Horizontal);
    tabsRow_->setPadding({theme::layout::marginX - 4, 6, theme::layout::marginX, 0}).setAlign(Align::Center);
    tabsRow_->setVisible(false);

    seasonBar_ = page_->content()->add<Box>(Axis::Horizontal);
    // Bottom padding leaves room for the focused button's caption above the first episode.
    seasonBar_->setPadding({theme::layout::marginX + 4, 4, theme::layout::marginX, 24}).setSpacing(14).setAlign(Align::Center);
    seasonLabel_ = seasonBar_->add<Label>("", theme::type::title3);
    seasonLabel_->setFlex(1);
    seasonWatched_ = seasonBar_->add<ActionButton>("", gfx::Icon::Check, ButtonStyle::Icon, "Season watched");
    seasonWatched_->onActivate = [this] { toggleSeasonWatched(); };
    seasonDownload_ = seasonBar_->add<DownloadButton>(false);
    seasonRetry_ = seasonBar_->add<ActionButton>("Try again", gfx::Icon::Refresh, ButtonStyle::Secondary);
    seasonRetry_->onActivate = [this] { timeline().after(0.0f, [this] { loadSeasons(); }, lifetime()); };
    seasonRetry_->setVisible(false);
    seasonBar_->setVisible(false);

    skeleton_ = add<Skeleton>();
    skeleton_->addRect({0, 0, 460, 52}, 10)
        .addRect({0, 70, 320, 20}, 6)
        .addRect({0, 108, 680, 15}, 6)
        .addRect({0, 132, 600, 15}, 6)
        .addRect({0, 176, 150, 46}, theme::radius::md)
        .addRect({164, 176, 214, 46}, theme::radius::md);
    skeleton_->setFrame({theme::layout::marginX, 330, 700, 240});

    status_ = add<StatusView>();
    status_->setFrame({0, 80, theme::layout::screenWidth, theme::layout::screenHeight - 140});
    load();
}

SeriesScreen::~SeriesScreen() {
    token_.cancel();
    seasonToken_.cancel();
    actionToken_.cancel();
}

void SeriesScreen::load() {
    token_.cancel();
    token_.reset();
    auto client = jf::currentClient();
    if (!client) return;
    status_->hide();
    page_->setVisible(false);
    skeleton_->setVisible(true);
    seasonsReady_ = nextUpReady_ = false;
    client->item(seriesId_).then(
        [this](jf::Result<jf::BaseItem> r) {
            skeleton_->setVisible(false);
            if (!r) {
                status_->showError(errorMessage(r.error), [this] { load(); }, {}, "Can't load this show");
                status_->focusAction();
                return;
            }
            populate(*r);
        },
        token_);
    loadSeasons();
    client->nextUp(1, seriesId_).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (r && !r->items.empty()) nextUp_ = r->items.front();
            else nextUp_.reset();
            nextUpReady_ = true;
            updatePlayButton();
            onSeasonsLoaded();
        },
        token_);
}

void SeriesScreen::loadSeasons() {
    auto client = jf::currentClient();
    if (!client) return;
    seasonsReady_ = false;
    seasonsError_.clear();
    if (seasonRetry_->hasFocusWithin()) focus().setFocus(play_);
    seasonRetry_->setVisible(false);
    client->seasons(seriesId_).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            seasons_.clear();
            if (r) {
                for (auto& s : r->items)
                    if (!s.isVirtual() || s.childCount > 0) seasons_.push_back(s);
            } else {
                seasonsError_ = errorMessage(r.error);
            }
            seasonsReady_ = true;
            onSeasonsLoaded();
        },
        token_);
}

void SeriesScreen::populate(const jf::BaseItem& series) {
    const bool first = !loaded_;
    series_ = series;
    loaded_ = true;
    page_->setVisible(true);
    backdrop_->setRequest(backdropRequest(series_));
    title_->setText(series_.name);
    title_->setVisible(!logoShown_);
    if (first) {
        if (const std::string url = logoUrl(series_); !url.empty()) {
            gfx::ImageRequest req = imageRequest(url, kLogoMaxW, kLogoMaxH);
            req.fit = gfx::ImageFit::Contain;
            logoImage_ = gfx::ImageCache::instance().load(req);
            checkLogo();
        }
    }
    std::vector<Chip> chips = itemChips(series_);
    if (!series_.status.empty() && series_.status != "Ended" && series_.status != "Continuing")
        chips.push_back({series_.status, ChipStyle::Text});
    chips_->setChips(std::move(chips));
    genres_->setText(genresText(series_, 4));
    genres_->setVisible(!genres_->text().empty());
    overview_->setText(series_.overview);
    overview_->setSheetTitle(series_.name);
    overview_->setVisible(!series_.overview.empty());
    download_->setGroup(series_);
    updateUserButtons();
    if (first) {
        header_->alpha.set(0.0f);
        header_->alpha.tweenTo(1.0f, 0.45f, ease::outCubic);
        header_->offset.set({0, 18});
        header_->offset.springTo({0, 0}, theme::motion::gentle);
        focus().setFocus(play_);
        loadSimilar();
    }
    updatePlayButton();
    onSeasonsLoaded();
}

void SeriesScreen::updateUserButtons() {
    fav_->setActive(series_.userData.isFavorite);
    watched_->setActive(series_.userData.played);
    watched_->setTooltip(series_.userData.played ? "Mark unwatched" : "Mark all watched");
}

void SeriesScreen::updatePlayButton() {
    if (nextUp_) {
        const bool resume = nextUp_->userData.playbackPositionTicks > 0 && !nextUp_->userData.played;
        const std::string tag = nextUp_->episodeTag();
        play_->setLabel(std::format("{} {}", resume ? "Resume" : "Play", tag));
        play_->setTooltip(nextUp_->name);
    } else {
        play_->setLabel(series_.userData.played ? "Play again" : "Play");
        play_->setTooltip({});
    }
}

void SeriesScreen::onSeasonsLoaded() {
    // Runs when seasons, next up and the series item have all arrived (whichever is last).
    if (!seasonsReady_ || !nextUpReady_ || !loaded_ || tabsRow_->visible()) return;
    if (seasons_.empty()) {
        const bool failed = !seasonsError_.empty();
        seasonLabel_->setText(failed ? "Couldn't load the seasons. " + seasonsError_ : "No episodes yet");
        seasonBar_->setVisible(true);
        seasonWatched_->setVisible(false);
        seasonDownload_->setVisible(false);
        seasonRetry_->setVisible(failed);
        return;
    }
    size_t initial = 0;
    bool found = false;
    const std::string want = !preferredSeason_.empty() ? preferredSeason_ : (nextUp_ ? nextUp_->seasonId : std::string());
    for (size_t i = 0; i < seasons_.size() && !want.empty(); ++i)
        if (seasons_[i].id == want) {
            initial = i;
            found = true;
        }
    if (!found)
        for (size_t i = 0; i < seasons_.size(); ++i)
            if (seasons_[i].indexNumber != 0 && !seasons_[i].userData.played) {
                initial = i;
                found = true;
                break;
            }
    if (!found && seasons_.size() > 1 && seasons_[0].indexNumber == 0) initial = 1;
    selected_ = initial;
    buildSeasonSelector();
    tabsRow_->setVisible(true);
    seasonBar_->setVisible(true);
    selectSeason(initial, true);
}

void SeriesScreen::buildSeasonSelector() {
    tabsRow_->clearChildren();
    tabs_ = nullptr;
    seasonPicker_ = nullptr;
    if (seasons_.size() <= 14) {
        const bool shortForm = seasons_.size() > 7;
        std::vector<std::string> titles;
        for (const auto& s : seasons_) titles.push_back(seasonTitle(s, shortForm));
        tabs_ = tabsRow_->add<TabBar>(std::move(titles));
        tabs_->select(selected_, false);
        tabs_->onSelect = [this](size_t i) { selectSeason(i, false); };
        tabs_->bindShoulderButtons(this);
        setHints({{Key::L, "Season"}});
    } else {
        seasonPicker_ = tabsRow_->add<Button>(seasonTitle(seasons_[selected_], false), gfx::Icon::ChevronDown);
        seasonPicker_->onActivate = [this] { openSeasonPicker(); };
    }
}

void SeriesScreen::openSeasonPicker() {
    std::vector<ListItem> items;
    for (size_t i = 0; i < seasons_.size(); ++i) {
        const auto& s = seasons_[i];
        items.push_back({seasonTitle(s, false), s.childCount > 0 ? std::format("{} episodes", s.childCount) : "",
                         s.userData.played ? "Watched" : "", gfx::Icon::None, i == selected_, true,
                         guard([this, i] { selectSeason(i, true); })});
    }
    present(SideSheet::make("Seasons", std::move(items), static_cast<int>(selected_)));
}

void SeriesScreen::selectSeason(size_t index, bool immediate) {
    if (index >= seasons_.size()) return;
    selected_ = index;
    if (seasonPicker_) seasonPicker_->setLabel(seasonTitle(seasons_[index], false));
    updateSeasonBar();
    if (seasonTimer_) {
        timeline().cancel(seasonTimer_);
        seasonTimer_ = 0;
    }
    if (cache_.count(seasons_[index].id)) {
        showEpisodes(cache_[seasons_[index].id]);
        return;
    }
    // Switching season with L/R while an episode is focused: keep focus on the selector rather
    // than letting it fall back to the top of the page while the list reloads.
    if (episodesHaveFocus()) {
        if (tabs_) focus().setFocus(tabs_);
        else if (seasonPicker_) focus().setFocus(seasonPicker_);
    }
    clearEpisodes();
    auto sk = std::make_unique<Skeleton>();
    const float w = theme::layout::screenWidth - 2 * theme::layout::marginX;
    for (int i = 0; i < 3; ++i) {
        const float y = static_cast<float>(i) * (EpisodeRow::kHeight + 14);
        sk->addRect({theme::layout::marginX, y + 4, w, EpisodeRow::kHeight - 8}, theme::radius::lg);
    }
    episodeRows_.push_back(page_->content()->insertChild(episodeInsertIndex(), std::move(sk)));
    if (immediate) {
        loadEpisodes(index);
    } else {
        seasonTimer_ = timeline().after(0.25f, [this, index] {
            seasonTimer_ = 0;
            loadEpisodes(index);
        }, lifetime());
    }
}

void SeriesScreen::loadEpisodes(size_t index) {
    auto client = jf::currentClient();
    if (!client || index >= seasons_.size()) return;
    seasonToken_.cancel();
    seasonToken_.reset();
    const std::string seasonId = seasons_[index].id;
    client->episodes(seriesId_, seasonId).then(
        [this, seasonId, index](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (selected_ >= seasons_.size() || seasons_[selected_].id != seasonId) {
                if (r) cache_[seasonId] = r->items;
                return;
            }
            if (!r) {
                showEpisodesError(index, errorMessage(r.error));
                return;
            }
            cache_[seasonId] = r->items;
            showEpisodes(r->items);
            updateSeasonBar();
        },
        seasonToken_);
}

void SeriesScreen::showEpisodesError(size_t index, const std::string& message) {
    clearEpisodes();
    auto sv = std::make_unique<StatusView>();
    sv->setPreferredSize(theme::layout::screenWidth, 220);
    sv->showError(message, [this, index] { selectSeason(index, true); }, {}, "Couldn't load the episodes");
    episodeRows_.push_back(page_->content()->insertChild(episodeInsertIndex(), std::move(sv)));
}

size_t SeriesScreen::episodeInsertIndex() const { return page_->content()->indexOf(seasonBar_) + 1; }

bool SeriesScreen::episodesHaveFocus() const {
    return std::any_of(episodeRows_.begin(), episodeRows_.end(), [](const View* v) { return v->hasFocusWithin(); });
}

void SeriesScreen::clearEpisodes() {
    for (View* v : episodeRows_) page_->content()->removeChild(v);
    episodeRows_.clear();
}

void SeriesScreen::showEpisodes(const std::vector<jf::BaseItem>& episodes) {
    size_t focusIdx = SIZE_MAX;
    for (size_t i = 0; i < episodeRows_.size(); ++i)
        if (episodeRows_[i]->hasFocusWithin()) focusIdx = i;
    clearEpisodes();
    size_t idx = episodeInsertIndex();
    for (const auto& ep : episodes) {
        auto row = std::make_unique<EpisodeRow>(ep);
        row->onWatchedChanged = guard([this](const jf::BaseItem& changed) {
            for (auto& [season, list] : cache_)
                for (auto& e : list)
                    if (e.id == changed.id) e.userData = changed.userData;
            updateSeasonBar();
        });
        episodeRows_.push_back(page_->content()->insertChild(idx++, std::move(row)));
    }
    if (episodes.empty()) {
        auto empty = std::make_unique<Label>("No episodes in this season.", theme::type::body, theme::color::textSecondary);
        empty->setMargin({theme::layout::marginX + 4, 8, theme::layout::marginX, 8});
        episodeRows_.push_back(page_->content()->insertChild(idx, std::move(empty)));
    }
    if (focusIdx == SIZE_MAX) return;
    View* target = nullptr;
    for (size_t i = std::min(focusIdx, episodeRows_.size() - 1) + 1; i-- > 0 && !target;)
        if (episodeRows_[i]->canFocus()) target = episodeRows_[i];
    if (!target) target = tabs_ ? static_cast<View*>(tabs_) : static_cast<View*>(play_);
    focus().setFocus(target);
}

void SeriesScreen::updateSeasonBar() {
    if (selected_ >= seasons_.size()) return;
    const jf::BaseItem& s = seasons_[selected_];
    int count = s.childCount;
    int unwatched = s.userData.unplayedItemCount;
    bool allPlayed = s.userData.played;
    if (auto it = cache_.find(s.id); it != cache_.end()) {
        count = static_cast<int>(it->second.size());
        unwatched = static_cast<int>(std::count_if(it->second.begin(), it->second.end(),
                                                   [](const jf::BaseItem& e) { return !e.userData.played; }));
        allPlayed = unwatched == 0 && count > 0;
    }
    seasonLabel_->setText(joinDots({seasonTitle(s, false), count > 0 ? std::format("{} episodes", count) : "",
                                    unwatched > 0 && count > 0 ? std::format("{} unwatched", unwatched) : ""}));
    seasonWatched_->setVisible(true);
    seasonWatched_->setActive(allPlayed);
    seasonWatched_->setTooltip(allPlayed ? "Mark season unwatched" : "Mark season watched");
    jf::BaseItem season = s;
    if (auto it = cache_.find(s.id); it != cache_.end()) season.childCount = static_cast<int>(it->second.size());
    seasonDownload_->setVisible(true);
    seasonDownload_->setGroup(season);
}

void SeriesScreen::loadSimilar() {
    auto client = jf::currentClient();
    if (!client) return;
    client->similar(seriesId_, 16).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r || r->items.empty()) return;
            auto row = std::make_unique<RowCarousel>("More Like This");
            row->setMargin({0, 24, 0, 0});
            for (const auto& it : r->items) row->items()->add<ItemCard>(CardShape::Poster)->setItem(it);
            if (similar_) page_->content()->removeChild(similar_);
            similar_ = page_->content()->addChild(std::move(row));
        },
        token_);
}

void SeriesScreen::refreshQuietly() {
    auto client = jf::currentClient();
    if (!client || !loaded_) return;
    actionToken_.cancel();
    actionToken_.reset();
    client->item(seriesId_).then(
        [this](jf::Result<jf::BaseItem> r) {
            if (!r) return;
            series_.userData = r->userData;
            updateUserButtons();
            updatePlayButton();
        },
        actionToken_);
    client->nextUp(1, seriesId_).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) return;
            if (!r->items.empty()) nextUp_ = r->items.front();
            else nextUp_.reset();
            updatePlayButton();
        },
        actionToken_);
    // Season watched state and unplayed counts (the list itself is kept so tabs stay stable).
    client->seasons(seriesId_).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) return;
            for (auto& s : seasons_)
                for (const auto& fresh : r->items)
                    if (fresh.id == s.id) {
                        s.userData = fresh.userData;
                        s.childCount = fresh.childCount;
                    }
            updateSeasonBar();
        },
        actionToken_);
    if (selected_ >= seasons_.size()) return;
    const std::string seasonId = seasons_[selected_].id;
    // Other seasons may have changed too (autoplay into the next season): reload them on demand.
    for (auto it = cache_.begin(); it != cache_.end();) it = it->first == seasonId ? std::next(it) : cache_.erase(it);
    client->episodes(seriesId_, seasonId).then(
        [this, seasonId](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) return;
            cache_[seasonId] = r->items;
            if (selected_ >= seasons_.size() || seasons_[selected_].id != seasonId) return;
            // Update rows in place when the list is unchanged (keeps focus and scroll).
            bool same = r->items.size() == episodeRows_.size();
            for (size_t i = 0; same && i < episodeRows_.size(); ++i) {
                auto* row = dynamic_cast<EpisodeRow*>(episodeRows_[i]);
                same = row && row->item().id == r->items[i].id;
            }
            if (same) {
                for (size_t i = 0; i < episodeRows_.size(); ++i)
                    static_cast<EpisodeRow*>(episodeRows_[i])->setItem(r->items[i]);
            } else {
                showEpisodes(r->items);
            }
            updateSeasonBar();
        },
        actionToken_);
}

void SeriesScreen::toggleFavorite() {
    auto client = jf::currentClient();
    if (!client || !loaded_ || favPending_) return;
    const bool fav = !series_.userData.isFavorite;
    favPending_ = true;
    client->setFavorite(series_.id, fav).then(
        [this, fav](jf::Result<jf::UserData> r) {
            favPending_ = false;
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            series_.userData.isFavorite = r->isFavorite;
            fav_->setActive(r->isFavorite);
            toast(fav ? "Added to favorites" : "Removed from favorites", ToastKind::Success, gfx::Icon::Heart, 1.8f);
        },
        token_);
}

void SeriesScreen::toggleWatched() {
    auto client = jf::currentClient();
    if (!client || !loaded_ || watchedPending_) return;
    const bool played = !series_.userData.played;
    watchedPending_ = true;
    client->setPlayed(series_.id, played).then(
        [this, played](jf::Result<jf::UserData> r) {
            watchedPending_ = false;
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            series_.userData.played = r->played;
            watched_->setActive(r->played);
            watched_->setTooltip(r->played ? "Mark unwatched" : "Mark all watched");
            toast(played ? "Marked all episodes as watched" : "Marked as unwatched", ToastKind::Success,
                  gfx::Icon::Check, 2.0f);
            cache_.clear();
            refreshQuietly();
        },
        token_);
}

void SeriesScreen::toggleSeasonWatched() {
    auto client = jf::currentClient();
    if (!client || selected_ >= seasons_.size() || seasonWatchedPending_) return;
    const bool played = !seasonWatched_->active();
    const std::string seasonId = seasons_[selected_].id;
    seasonWatchedPending_ = true;
    client->setPlayed(seasonId, played).then(
        [this, played, seasonId](jf::Result<jf::UserData> r) {
            seasonWatchedPending_ = false;
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            for (auto& s : seasons_)
                if (s.id == seasonId) s.userData = *r;
            toast(played ? "Season marked as watched" : "Season marked as unwatched", ToastKind::Success,
                  gfx::Icon::Check, 1.8f);
            refreshQuietly();
        },
        token_);
}

void SeriesScreen::checkLogo() {
    if (logoShown_ || !logoImage_) return;
    if (logoImage_->failed()) {
        logoImage_.reset();
        return;
    }
    if (!logoImage_->ready()) return;
    const float aspect = logoImage_->aspect();
    if (aspect <= 0.05f) return;
    float h = std::min(kLogoMaxH, kLogoMaxW / aspect);
    float w = h * aspect;
    if (w > kLogoMaxW) {
        w = kLogoMaxW;
        h = w / aspect;
    }
    logo_->setImage(logoImage_);
    logo_->setPreferredSize(std::round(w), std::round(h));
    logo_->setVisible(true);
    title_->setVisible(false);
    logoShown_ = true;
}

void SeriesScreen::onShow() {
    if (!shownOnce_) {
        shownOnce_ = true;
        return;
    }
    refreshQuietly();
    // The player's stop report can arrive after the first refresh: check again shortly.
    if (lateRefresh_) timeline().cancel(lateRefresh_);
    lateRefresh_ = timeline().after(kLateRefreshDelay, [this] {
        lateRefresh_ = 0;
        if (isTop()) refreshQuietly();
    }, lifetime());
}

void SeriesScreen::update(float dt) {
    checkLogo();
    backdrop_->alpha.set(1.0f - 0.6f * std::clamp(page_->scrollOffset() / 360.0f, 0.0f, 1.0f));
    Screen::update(dt);
}

}  // namespace karafin::screens
