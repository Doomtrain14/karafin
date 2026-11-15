#include "screens/detail_screen.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "jellyfin/client.hpp"
#include "screens/item_meta.hpp"
#include "screens/widgets/item_card.hpp"
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
}  // namespace

DetailScreen::DetailScreen(std::string itemId) : itemId_(std::move(itemId)) {
    backdrop_ = add<Backdrop>();
    backdrop_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    backdrop_->setIntensity(1.0f);

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    page_->setRevealWholeChild(true);
    page_->setRevealInsets(60, 64);
    page_->setShowsIndicator(true);
    page_->content()->setSpacing(theme::layout::rowSpacing);
    page_->content()->setPadding({0, 0, 0, 70});

    header_ = page_->content()->add<Box>(Axis::Vertical);
    header_->setPadding({theme::layout::marginX, 150, theme::layout::marginX, 8}).setSpacing(12).setAlign(Align::Start);
    header_->setRememberFocus(true);
    logo_ = header_->add<ImageView>();
    logo_->setFit(gfx::ImageFit::Contain);
    logo_->setShimmer(false);
    logo_->setPlaceholderColor(Color::transparent());
    logo_->setVisible(false);
    title_ = header_->add<Label>("", theme::type::hero);
    title_->setMaxLines(2);
    title_->setPreferredSize(kTextWidth, -1);
    subtitle_ = header_->add<Label>("", theme::type::title2);
    subtitle_->setPreferredSize(kTextWidth, -1);
    chips_ = header_->add<ChipRow>();
    media_ = header_->add<ChipRow>();
    genres_ = header_->add<Label>("", theme::type::callout, theme::color::textSecondary);
    genres_->setPreferredSize(kTextWidth, -1);
    overview_ = header_->add<Label>("", theme::type::body, theme::color::textSecondary);
    overview_->setMaxLines(4);
    overview_->setPreferredSize(kTextWidth - 40, -1);
    credits_ = header_->add<Label>("", theme::type::caption, theme::color::textTertiary);
    credits_->setPreferredSize(kTextWidth, -1);

    buttons_ = header_->add<Box>(Axis::Horizontal);
    buttons_->setSpacing(12).setMargin({0, 12, 0, 0});
    buttons_->setRememberFocus(true);
    play_ = buttons_->add<ActionButton>("Play", gfx::Icon::Play, ButtonStyle::Primary);
    restart_ = buttons_->add<ActionButton>("From start", gfx::Icon::Rewind, ButtonStyle::Secondary);
    audio_ = buttons_->add<ActionButton>("", gfx::Icon::Audio, ButtonStyle::Icon, "Audio");
    subs_ = buttons_->add<ActionButton>("", gfx::Icon::Subtitles, ButtonStyle::Icon, "Subtitles");
    version_ = buttons_->add<ActionButton>("", gfx::Icon::List, ButtonStyle::Icon, "Version");
    series_ = buttons_->add<ActionButton>("", gfx::Icon::Tv, ButtonStyle::Icon, "Go to series");
    fav_ = buttons_->add<ActionButton>("", gfx::Icon::Heart, ButtonStyle::Icon, "Favorite");
    fav_->setActiveColor(theme::color::danger);
    watched_ = buttons_->add<ActionButton>("", gfx::Icon::Check, ButtonStyle::Icon, "Watched");
    download_ = buttons_->add<DownloadButton>(false);
    buttons_->setRememberedFocus(play_);
    header_->setRememberedFocus(buttons_);
    setRememberedFocus(play_);

    // "Resume" and "From start" are both on screen when there is a saved position, so the
    // primary button starts right away instead of asking again.
    play_->onActivate = [this] {
        if (!loaded_) return;
        const int64_t pos = item_.userData.playbackPositionTicks;
        playAt(item_, pos > 0 && !item_.userData.played ? pos : 0, playOptions());
    };
    restart_->onActivate = [this] {
        if (loaded_) playAt(item_, 0, playOptions());
    };
    audio_->onActivate = [this] { openAudio(); };
    subs_->onActivate = [this] { openSubtitles(); };
    version_->onActivate = [this] { openVersions(); };
    series_->onActivate = [this] {
        jf::BaseItem s;
        s.id = item_.seriesId;
        s.type = "Series";
        openItem(s);
    };
    fav_->onActivate = [this] { toggleFavorite(); };
    watched_->onActivate = [this] { toggleWatched(); };

    progressRow_ = header_->add<Box>(Axis::Horizontal);
    // Clears the focused button's caption (drawn under the button row).
    progressRow_->setSpacing(14).setAlign(Align::Center).setMargin({0, 26, 0, 0});
    progress_ = progressRow_->add<ProgressBar>();
    progress_->setPreferredSize(240, 6);
    progressLabel_ = progressRow_->add<Label>("", theme::type::caption, theme::color::textSecondary);

    rows_ = std::make_unique<RowStack>(page_->content(), 1);

    skeleton_ = add<Skeleton>();
    skeleton_->addRect({0, 0, 460, 52}, 10)
        .addRect({0, 70, 320, 20}, 6)
        .addRect({0, 108, 680, 15}, 6)
        .addRect({0, 132, 640, 15}, 6)
        .addRect({0, 156, 520, 15}, 6)
        .addRect({0, 200, 120, 46}, theme::radius::md)
        .addRect({134, 200, 48, 48}, 24)
        .addRect({194, 200, 48, 48}, 24)
        .addRect({254, 200, 48, 48}, 24);
    skeleton_->setFrame({theme::layout::marginX, 330, 700, 260});

    status_ = add<StatusView>();
    status_->setFrame({0, 80, theme::layout::screenWidth, theme::layout::screenHeight - 140});

    load();
}

DetailScreen::~DetailScreen() {
    token_.cancel();
    actionToken_.cancel();
}

void DetailScreen::load() {
    token_.cancel();
    token_.reset();
    auto client = jf::currentClient();
    if (!client) return;
    status_->hide();
    page_->setVisible(false);
    skeleton_->setVisible(true);
    client->item(itemId_).then(
        [this](jf::Result<jf::BaseItem> r) {
            skeleton_->setVisible(false);
            if (!r) {
                status_->showError(errorMessage(r.error), [this] { load(); }, {}, "Can't load this title");
                status_->focusAction();
                return;
            }
            populate(*r, false);
        },
        token_);
}

const jf::MediaSource* DetailScreen::source() const {
    if (item_.mediaSources.empty()) return nullptr;
    return &item_.mediaSources[std::min(sourceIndex_, item_.mediaSources.size() - 1)];
}

PlayOptions DetailScreen::playOptions() const {
    PlayOptions o;
    o.audioStreamIndex = audioIndex_;
    o.subtitleStreamIndex = subtitleIndex_;
    if (const jf::MediaSource* s = source(); s && item_.mediaSources.size() > 1) o.mediaSourceId = s->id;
    return o;
}

void DetailScreen::populate(const jf::BaseItem& item, bool refresh) {
    const bool first = !loaded_;
    item_ = item;
    loaded_ = true;
    if (!refresh) {
        page_->setVisible(true);
        backdrop_->setRequest(backdropRequest(item_));
        const bool episode = item_.isEpisode();
        title_->setText(episode && !item_.seriesName.empty() ? item_.seriesName : item_.name);
        title_->setStyle(episode ? theme::type::largeTitle : theme::type::hero);
        title_->setVisible(true);
        subtitle_->setText(episode ? joinDots({item_.episodeTag(), item_.name}) : std::string());
        subtitle_->setVisible(episode);
        logo_->setVisible(false);
        logoShown_ = false;
        logoImage_.reset();
        if (const std::string url = logoUrl(item_); !url.empty()) {
            gfx::ImageRequest req = imageRequest(url, kLogoMaxW, kLogoMaxH);
            req.fit = gfx::ImageFit::Contain;
            logoImage_ = gfx::ImageCache::instance().load(req);
            checkLogo();
        }
        chips_->setChips(itemChips(item_));
        if (const jf::MediaSource* s = source()) media_->setChips(mediaChips(*s));
        media_->setVisible(!media_->empty());
        genres_->setText(genresText(item_, 4));
        genres_->setVisible(!genres_->text().empty());
        overview_->setText(item_.overview.empty() && !item_.taglines.empty() ? item_.taglines.front() : item_.overview);
        overview_->setVisible(!overview_->text().empty());

        std::string directors, writers;
        for (const auto& p : item_.people) {
            std::string& list = p.type == "Director" ? directors : writers;
            if (p.type != "Director" && p.type != "Writer") continue;
            if (std::count(list.begin(), list.end(), ',') >= 2) continue;
            if (!list.empty()) list += ", ";
            list += p.name;
        }
        credits_->setText(joinDots({directors.empty() ? "" : "Directed by " + directors,
                                    writers.empty() ? "" : "Written by " + writers}));
        credits_->setVisible(!credits_->text().empty());

        const jf::MediaSource* src = source();
        audio_->setVisible(src && !src->streamsOfType(jf::StreamType::Audio).empty());
        subs_->setVisible(src != nullptr);
        version_->setVisible(item_.mediaSources.size() > 1);
        series_->setVisible(episode && !item_.seriesId.empty());
        download_->setItem(item_);
        play_->setEnabled(item_.isPlayable());
        restart_->setEnabled(item_.isPlayable());
        if (first) {
            header_->alpha.set(0.0f);
            header_->alpha.tweenTo(1.0f, 0.45f, ease::outCubic);
            header_->offset.set({0, 18});
            header_->offset.springTo({0, 0}, theme::motion::gentle);
            focus().setFocus(play_);
            loadRows();
        }
    }
    updateUserState();
}

void DetailScreen::updateUserState() {
    const int64_t pos = item_.userData.playbackPositionTicks;
    const bool resumable = pos > 0 && !item_.userData.played;
    play_->setLabel(resumable ? "Resume" : "Play");
    play_->setTooltip(resumable ? "From " + formatClock(pos) : std::string());
    restart_->setVisible(resumable);
    fav_->setActive(item_.userData.isFavorite);
    fav_->setTooltip(item_.userData.isFavorite ? "Remove favorite" : "Add to favorites");
    watched_->setActive(item_.userData.played);
    watched_->setTooltip(item_.userData.played ? "Mark unwatched" : "Mark watched");
    progressRow_->setVisible(resumable && item_.runTimeTicks > 0);
    if (resumable && item_.runTimeTicks > 0) {
        progress_->setValue(static_cast<float>(item_.progress()));
        progressLabel_->setText(joinDots({remainingText(item_), "Stopped at " + formatClock(pos)}));
    }
}

void DetailScreen::checkLogo() {
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

void DetailScreen::loadEpisodes(bool quiet) {
    auto client = jf::currentClient();
    if (!client || !item_.isEpisode() || item_.seriesId.empty()) return;
    const std::string seasonName = item_.seasonName.empty() ? "this season" : item_.seasonName;
    client->episodes(item_.seriesId, item_.seasonId).then(
        [this, seasonName](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r || r->items.size() < 2) return;
            if (updateCardsInPlace(rows_->get(kEpisodes), r->items)) return;
            auto row = std::make_unique<RowCarousel>("More from " + seasonName);
            View* current = nullptr;
            for (const auto& ep : r->items) {
                auto* card = row->items()->add<ItemCard>(CardShape::Wide);
                card->setItem(ep);
                if (ep.id == item_.id) {
                    current = card;
                    card->onOpen = [this](const jf::BaseItem&) {
                        page_->scrollTo(0);
                        focus().setFocus(play_);
                    };
                } else {
                    card->onOpen = [](const jf::BaseItem& it) {
                        nav().replace(std::make_unique<DetailScreen>(it.id), Transition::Fade);
                    };
                }
            }
            if (current) row->setRememberedFocus(current);
            rows_->place(kEpisodes, std::move(row));
        },
        quiet ? actionToken_ : token_);
}

void DetailScreen::loadRows() {
    auto client = jf::currentClient();
    if (!client) return;
    loadEpisodes(false);

    std::vector<const jf::PersonInfo*> people;
    for (const auto& p : item_.people)
        if (p.type == "Actor" || p.type == "GuestStar") people.push_back(&p);
    for (const auto& p : item_.people)
        if (p.type != "Actor" && p.type != "GuestStar" && people.size() < 24) people.push_back(&p);
    if (!people.empty()) {
        auto row = std::make_unique<RowCarousel>("Cast & Crew");
        for (const jf::PersonInfo* p : people) {
            if (row->itemCount() >= 24) break;
            jf::BaseItem person;
            person.id = p->id;
            person.name = p->name;
            person.type = "Person";
            if (!p->primaryImageTag.empty()) person.imageTags["Primary"] = p->primaryImageTag;
            auto* card = row->items()->add<ItemCard>(CardShape::Poster);
            card->setItem(person);
            card->setSubtitle(!p->role.empty() ? p->role : p->type);
        }
        rows_->place(kCast, std::move(row));
    }

    rows_->place(kSimilar, std::make_unique<SkeletonRow>(CardShape::Poster, 8));
    client->similar(item_.isEpisode() && !item_.seriesId.empty() ? item_.seriesId : item_.id, 16).then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r || r->items.empty()) {
                rows_->remove(kSimilar);
                return;
            }
            auto row = std::make_unique<RowCarousel>("More Like This");
            for (const auto& it : r->items) row->items()->add<ItemCard>(CardShape::Poster)->setItem(it);
            rows_->place(kSimilar, std::move(row));
        },
        token_);
}

void DetailScreen::openAudio() {
    const jf::MediaSource* src = source();
    if (!src) return;
    std::vector<ListItem> items;
    items.push_back({"Automatic", "Your preferred language, else the default track", "", gfx::Icon::None,
                     !audioIndex_.has_value(), true, guard([this] {
                         audioIndex_.reset();
                         audio_->setTooltip("Audio");
                         audio_->setActive(false);
                     })});
    int focusIdx = audioIndex_ ? -1 : 0;
    for (const jf::MediaStream* s : src->streamsOfType(jf::StreamType::Audio)) {
        const int index = s->index;
        const std::string title = streamTitle(*s);
        if (audioIndex_ && *audioIndex_ == index) focusIdx = static_cast<int>(items.size());
        items.push_back({title, streamDetail(*s), "", gfx::Icon::None, audioIndex_ && *audioIndex_ == index, true,
                         guard([this, index, title] {
                             audioIndex_ = index;
                             audio_->setTooltip("Audio: " + title);
                             audio_->setActive(true);
                         })});
    }
    present(SideSheet::make("Audio", std::move(items), focusIdx, "Used when playback starts"));
}

void DetailScreen::openSubtitles() {
    const jf::MediaSource* src = source();
    if (!src) return;
    std::vector<ListItem> items;
    items.push_back({"Automatic", "Follows your subtitle settings", "", gfx::Icon::None, !subtitleIndex_.has_value(),
                     true, guard([this] {
                         subtitleIndex_.reset();
                         subs_->setTooltip("Subtitles");
                         subs_->setActive(false);
                     })});
    items.push_back({"Off", "", "", gfx::Icon::None, subtitleIndex_ && *subtitleIndex_ == -1, true, guard([this] {
                         subtitleIndex_ = -1;
                         subs_->setTooltip("Subtitles: Off");
                         subs_->setActive(true);
                     })});
    int focusIdx = !subtitleIndex_ ? 0 : (*subtitleIndex_ == -1 ? 1 : -1);
    for (const jf::MediaStream* s : src->streamsOfType(jf::StreamType::Subtitle)) {
        const int index = s->index;
        const std::string title = streamTitle(*s);
        if (subtitleIndex_ && *subtitleIndex_ == index) focusIdx = static_cast<int>(items.size());
        items.push_back({title, streamDetail(*s), "", gfx::Icon::None, subtitleIndex_ && *subtitleIndex_ == index, true,
                         guard([this, index, title] {
                             subtitleIndex_ = index;
                             subs_->setTooltip("Subtitles: " + title);
                             subs_->setActive(true);
                         })});
    }
    if (items.size() == 2)
        items.push_back({"No subtitles in this file", "You can search online from the player", "", gfx::Icon::Info,
                         false, false, {}});
    present(SideSheet::make("Subtitles", std::move(items), focusIdx, "Used when playback starts"));
}

void DetailScreen::openVersions() {
    std::vector<ListItem> items;
    for (size_t i = 0; i < item_.mediaSources.size(); ++i) {
        const jf::MediaSource& ms = item_.mediaSources[i];
        std::string chips;
        for (const auto& c : mediaChips(ms)) {
            if (chips.size() > 40) break;
            chips = chips.empty() ? c.text : chips + " " + c.text;
        }
        items.push_back({ms.name.empty() ? std::format("Version {}", i + 1) : ms.name, chips,
                         ms.size > 0 ? formatBytes(static_cast<uint64_t>(ms.size)) : std::string(), gfx::Icon::None,
                         i == sourceIndex_, true, guard([this, i] {
                             sourceIndex_ = i;
                             audioIndex_.reset();
                             subtitleIndex_.reset();
                             audio_->setActive(false);
                             subs_->setActive(false);
                             if (const jf::MediaSource* s = source()) media_->setChips(mediaChips(*s));
                             version_->setActive(sourceIndex_ != 0);
                         })});
    }
    present(SideSheet::make("Version", std::move(items), static_cast<int>(sourceIndex_)));
}

void DetailScreen::toggleFavorite() {
    auto client = jf::currentClient();
    if (!client || !loaded_) return;
    const bool fav = !item_.userData.isFavorite;
    client->setFavorite(item_.id, fav).then(
        [this, fav](jf::Result<jf::UserData> r) {
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            item_.userData.isFavorite = r->isFavorite;
            updateUserState();
            toast(fav ? "Added to favorites" : "Removed from favorites", ToastKind::Success, gfx::Icon::Heart, 1.8f);
        },
        actionToken_);
}

void DetailScreen::toggleWatched() {
    auto client = jf::currentClient();
    if (!client || !loaded_) return;
    const bool played = !item_.userData.played;
    client->setPlayed(item_.id, played).then(
        [this, played](jf::Result<jf::UserData> r) {
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            item_.userData.played = r->played;
            item_.userData.playbackPositionTicks = r->playbackPositionTicks;
            item_.userData.playedPercentage = r->playedPercentage;
            updateUserState();
            toast(played ? "Marked as watched" : "Marked as unwatched", ToastKind::Success, gfx::Icon::Check, 1.8f);
        },
        actionToken_);
}

void DetailScreen::onShow() {
    if (!shownOnce_) {
        shownOnce_ = true;
        return;
    }
    // Back from the player: refresh the resume point / watched state. The player's stop report
    // may land after the first request, so check again a moment later.
    refreshUserState();
    if (lateRefresh_) timeline().cancel(lateRefresh_);
    lateRefresh_ = timeline().after(kLateRefreshDelay, [this] {
        lateRefresh_ = 0;
        if (isTop()) refreshUserState();
    }, lifetime());
}

void DetailScreen::refreshUserState() {
    auto client = jf::currentClient();
    if (!client || !loaded_) return;
    client->item(itemId_).then(
        [this](jf::Result<jf::BaseItem> r) {
            if (r) populate(*r, true);
        },
        actionToken_);
    loadEpisodes(true);
}

void DetailScreen::update(float dt) {
    checkLogo();
    backdrop_->alpha.set(1.0f - 0.55f * std::clamp(page_->scrollOffset() / 360.0f, 0.0f, 1.0f));
    Screen::update(dt);
}

}  // namespace karafin::screens
