#include "screens/home_section.hpp"

#include <algorithm>
#include <format>

#include "core/log.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/library_lock.hpp"
#include "screens/widgets/error_row.hpp"
#include "screens/widgets/item_card.hpp"
#include "screens/widgets/skeleton.hpp"
#include "ui/actions.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {

bool showLatestFor(const std::string& collectionType) {
    return collectionType == "movies" || collectionType == "tvshows" || collectionType == "homevideos" ||
           collectionType == "musicvideos" || collectionType == "mixed" || collectionType.empty();
}

bool showInLibraries(const jf::BaseItem& view) {
    const std::string& c = view.collectionType;
    return c != "livetv" && c != "music" && c != "books" && c != "photos" && c != "playlists";
}

CardShape latestShape(const std::string& collectionType) {
    return collectionType == "homevideos" || collectionType == "musicvideos" ? CardShape::Wide : CardShape::Poster;
}

}  // namespace

HomeSection::HomeSection() {
    backdrop_ = add<Backdrop>();
    backdrop_->setFrame({0, 0, theme::layout::screenWidth, 640});
    backdrop_->setIntensity(0.9f);

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, 0, theme::layout::screenWidth, theme::layout::screenHeight});
    page_->setRevealWholeChild(true);
    page_->setRevealInsets(104, 64);
    page_->setShowsIndicator(true);
    page_->content()->setSpacing(theme::layout::rowSpacing);
    page_->content()->setPadding({0, 0, 0, 70});

    hero_ = page_->content()->add<HeroCarousel>();
    hero_->onCurrentChanged = [this](const HeroEntry& e) {
        if (page_->scrollOffset() < 200 || hero_->hasFocusWithin()) showBackdropFor(e.item, 0.0f);
    };
    hero_->onPlay = [this](const jf::BaseItem& item) {
        if (item.isSeries()) playSeries(item.id, token_);
        else playItem(item);
    };
    hero_->onInfo = [](const jf::BaseItem& item) { openItem(item); };
    hero_->actions = [this](ActionMenu& m) {
        const HeroEntry* e = hero_->current();
        if (!e || e->item.id.empty()) return;
        const jf::BaseItem item = e->item;
        m.setTitle(cardTitle(item), e->tag);
        m.section("Playback").add(item.isSeries() ? "Play next episode" : "Play", gfx::Icon::Play,
                                  guard([this, item] { hero_->onPlay(item); }));
        m.section("Title").add("Details", gfx::Icon::Info, [item] { openItem(item); });
        if (hero_->count() > 1) {
            m.add("Next featured title", gfx::Icon::ChevronRight, guard([this] { hero_->advance(1); }));
            m.add("Previous featured title", gfx::Icon::ChevronLeft, guard([this] { hero_->advance(-1); }));
        }
    };
    rows_ = std::make_unique<RowStack>(page_->content(), 1);

    status_ = add<StatusView>();
    status_->setFrame({0, kHeaderHeight, theme::layout::screenWidth, theme::layout::screenHeight - kHeaderHeight - 60});

    load();
}

HomeSection::~HomeSection() {
    token_.cancel();
    refreshToken_.cancel();
}

void HomeSection::load() {
    token_.cancel();
    token_.reset();
    refreshToken_.cancel();
    refreshToken_.reset();
    status_->hide();
    page_->setVisible(true);
    rows_->clear();
    signatures_.clear();
    latestRows_.clear();
    focusClaimed_ = false;
    heroDone_ = false;
    hero_->setVisible(true);
    hero_->setLoading();
    rows_->place(kResume, std::make_unique<SkeletonRow>(CardShape::Wide, 5));
    rows_->place(kLatest, std::make_unique<SkeletonRow>(CardShape::Poster, 8));
    rows_->place(kLatest + 1, std::make_unique<SkeletonRow>(CardShape::Poster, 8));

    if (!jf::currentClient()) {
        status_->showError("You're not signed in.", nullptr);
        return;
    }
    pending_ = 3;
    failures_ = 0;
    // Titles from locked libraries are left out, so their list has to be known first.
    lockRevision_ = LibraryLock::instance().revision();
    LibraryLock::instance().prepare([this, tok = token_] {
        if (tok.cancelled()) return;
        lockRevision_ = LibraryLock::instance().revision();
        loadHero();
        loadResume(false);
        loadNextUp(false);
        loadViews();
    });
}

void HomeSection::finishRequest(const jf::ApiError* error) {
    if (error) {
        ++failures_;
        lastError_ = *error;
    }
    if (--pending_ > 0) return;
    if (failures_ >= 3) {
        page_->setVisible(false);
        status_->showError(errorMessage(lastError_), [this] { load(); }, {}, "Can't load your home screen");
        if (visibleInTree()) status_->focusAction();
        return;
    }
    checkEmpty();
}

void HomeSection::checkEmpty() {
    // Everything answered and there is nothing to show (new server, no libraries yet).
    if (pending_ > 0 || !heroDone_ || !rows_->rows().empty() || hero_->count() > 0) return;
    page_->setVisible(false);
    status_->showEmpty(gfx::Icon::Library, "Nothing to watch yet",
                       "Add a library on your Jellyfin server, then refresh.",
                       {{"Refresh", gfx::Icon::Refresh, [this] { load(); }, true}});
    if (visibleInTree() && !hasFocusWithin()) status_->focusAction();
}

void HomeSection::loadHero() {
    auto client = jf::currentClient();
    if (!client) return;
    heroDone_ = false;
    jf::ItemsQuery resumeQ;
    resumeQ.filters("IsResumable").mediaTypes("Video").recursive().sortBy("DatePlayed", "Descending").limit(2)
        .fields("Overview,Genres,PrimaryImageAspectRatio");
    jf::ItemsQuery latestQ;
    latestQ.includeItemTypes("Movie,Series").recursive().sortBy("DateCreated", "Descending").limit(8)
        .filters("IsUnplayed").fields("Overview,Genres,PrimaryImageAspectRatio,ChildCount").param("ImageTypes", "Backdrop");
    // Both steps use the token of this load, so a reload() in between drops the second step too.
    const CancelToken tok = token_;
    client->items(resumeQ).then(
        [this, client, latestQ, tok](jf::Result<jf::QueryResult<jf::BaseItem>> resume) {
            auto entries = std::make_shared<std::vector<HeroEntry>>();
            if (resume)
                for (auto& it : LibraryLock::instance().filter(resume->items))
                    if (!backdropRequest(it).url.empty()) entries->push_back({it, "CONTINUE WATCHING"});
            const jf::ApiError resumeError = resume.error;  // kind None on success
            client->items(latestQ).then(
                [this, entries, resumeError](jf::Result<jf::QueryResult<jf::BaseItem>> latest) {
                    if (latest) {
                        for (auto& it : LibraryLock::instance().filter(latest->items)) {
                            if (entries->size() >= 7) break;
                            const bool dup = std::any_of(entries->begin(), entries->end(),
                                                         [&](const HeroEntry& e) { return e.item.id == it.id; });
                            if (!dup) entries->push_back({it, it.isSeries() ? "NEW SERIES" : "RECENTLY ADDED"});
                        }
                    }
                    heroDone_ = true;
                    log::step("home", "hero: {} entries{}{}", entries->size(), entries->empty() ? "" : ", first ",
                              entries->empty() ? std::string() : entries->front().item.id);
                    if (entries->empty() && (!latest || resumeError)) {
                        // Nothing to feature because a request failed: offer a retry instead of
                        // silently dropping the hero.
                        showHeroError(latest ? resumeError : latest.error);
                        tryClaimFocus();
                        checkEmpty();
                        return;
                    }
                    View* errorRow = rows_->get(kHeroError);
                    const bool errorFocused = errorRow && errorRow->hasFocusWithin();
                    rows_->remove(kHeroError);
                    if (entries->empty()) {
                        hero_->setEntries({});
                        hero_->setVisible(false);
                        page_->content()->setPadding({0, kHeaderHeight + 6, 0, 70});
                    } else {
                        hero_->setVisible(true);
                        page_->content()->setPadding({0, 0, 0, 70});
                        hero_->setEntries(std::move(*entries));
                        if (errorFocused) focus().setFocus(hero_->playButton());
                    }
                    tryClaimFocus();
                    checkEmpty();
                },
                tok);
        },
        tok);
}

void HomeSection::showHeroError(const jf::ApiError& error) {
    hero_->setEntries({});
    hero_->setVisible(false);
    page_->content()->setPadding({0, kHeaderHeight + 6, 0, 70});
    std::string message = "Couldn't load featured titles. " + errorMessage(error);
    if (auto* row = dynamic_cast<ErrorRow*>(rows_->get(kHeroError))) {
        row->setMessage(std::move(message));
        row->setBusy(false);
        return;
    }
    rows_->place(kHeroError, std::make_unique<ErrorRow>("Featured", std::move(message), [this] { loadHero(); }));
}

void HomeSection::showLatestError(const LatestRow& row, const jf::ApiError& error) {
    std::string message = "Couldn't load this row. " + errorMessage(error);
    if (auto* er = dynamic_cast<ErrorRow*>(rows_->get(row.order))) {
        er->setMessage(std::move(message));
        er->setBusy(false);
        return;
    }
    rows_->place(row.order,
                 std::make_unique<ErrorRow>(row.title, std::move(message), [this, row] { loadLatest(row, false); }));
}

void HomeSection::loadResume(bool quiet) {
    const CancelToken& tok = quiet ? refreshToken_ : token_;
    jf::currentClient()->resumeItems(16).then(
        [this, quiet](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) {
                if (!quiet) {
                    rows_->remove(kResume);
                    finishRequest(&r.error);
                }
                return;
            }
            const auto items = LibraryLock::instance().filter(r->items);
            log::step("home", "continue watching: {} items", items.size());
            const std::string sig = signature(items);
            if (quiet && signatures_[kResume] == sig) return;
            signatures_[kResume] = sig;
            applyRow(kResume, "Continue Watching", items, CardShape::Wide);
            if (!quiet) finishRequest(nullptr);
            tryClaimFocus();
        },
        tok);
}

void HomeSection::loadNextUp(bool quiet) {
    const CancelToken& tok = quiet ? refreshToken_ : token_;
    jf::currentClient()->nextUp(16).then(
        [this, quiet](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) {
                if (!quiet) finishRequest(&r.error);
                return;
            }
            const auto items = LibraryLock::instance().filter(r->items);
            log::step("home", "next up: {} items", items.size());
            const std::string sig = signature(items);
            if (quiet && signatures_[kNextUp] == sig) return;
            signatures_[kNextUp] = sig;
            applyRow(kNextUp, "Next Up", items, CardShape::Wide);
            if (!quiet) finishRequest(nullptr);
            tryClaimFocus();
        },
        tok);
}

void HomeSection::loadViews() {
    auto client = jf::currentClient();
    client->userViews().then(
        [this, client](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            rows_->remove(kLatest);
            rows_->remove(kLatest + 1);
            if (!r) {
                finishRequest(&r.error);
                return;
            }
            std::vector<jf::BaseItem> libs;
            int slot = 0;
            for (const auto& view : r->items) {
                if (!showInLibraries(view)) continue;
                libs.push_back(view);
                if (!showLatestFor(view.collectionType) || LibraryLock::instance().blocks(view.id)) continue;
                const int order = kLatest + slot++;
                const CardShape shape = latestShape(view.collectionType);
                rows_->place(order, std::make_unique<SkeletonRow>(shape, 8));
                latestRows_.push_back({order, view.id, "Recently Added in " + view.name, shape});
                loadLatest(latestRows_.back(), false);
            }
            if (!libs.empty()) rows_->place(kLibraries, makeRow("My Libraries", libs, CardShape::Wide));
            log::step("home", "libraries: {} shown, {} latest rows", libs.size(), slot);
            finishRequest(nullptr);
            tryClaimFocus();
        },
        token_);
}

void HomeSection::loadLatest(const LatestRow& row, bool quiet) {
    auto client = jf::currentClient();
    if (!client) return;
    client->latest(row.libraryId, 16).then(
        [this, row, quiet](jf::Result<std::vector<jf::BaseItem>> items) {
            const bool showingError = dynamic_cast<ErrorRow*>(rows_->get(row.order)) != nullptr;
            if (!items) {
                // A quiet refresh keeps whatever is shown; a retry that loses a race with a
                // successful quiet refresh must not replace the loaded row with an error.
                if (!quiet && !dynamic_cast<RowCarousel*>(rows_->get(row.order))) {
                    showLatestError(row, items.error);
                    tryClaimFocus();
                }
                return;
            }
            const auto shown = LibraryLock::instance().filter(*items);
            const std::string sig = signature(shown);
            if (quiet && !showingError && signatures_[row.order] == sig) return;
            signatures_[row.order] = sig;
            applyRow(row.order, row.title, shown, row.shape);
            tryClaimFocus();
        },
        quiet ? refreshToken_ : token_);
}

void HomeSection::applyRow(int order, const std::string& title, const std::vector<jf::BaseItem>& items, CardShape shape) {
    if (items.empty()) {
        rows_->remove(order);
        return;
    }
    if (updateCardsInPlace(rows_->get(order), items)) return;
    rows_->place(order, makeRow(title, items, shape));
}

void HomeSection::refreshContinue() {
    if (!jf::currentClient()) return;
    refreshToken_.cancel();
    refreshToken_.reset();
    loadResume(true);
    loadNextUp(true);
    for (const auto& row : latestRows_) loadLatest(row, true);
    // A failed hero gets another chance whenever Home is shown again.
    if (auto* er = dynamic_cast<ErrorRow*>(rows_->get(kHeroError)); er && !er->busy()) {
        er->setBusy(true);
        loadHero();
    }
}

std::string HomeSection::signature(const std::vector<jf::BaseItem>& items) {
    std::string s;
    for (const auto& it : items)
        s += std::format("{}:{}:{}:{};", it.id, it.userData.playbackPositionTicks, it.userData.played ? 1 : 0,
                         it.userData.unplayedItemCount);
    return s;
}

std::unique_ptr<RowCarousel> HomeSection::makeRow(const std::string& title, const std::vector<jf::BaseItem>& items,
                                                  CardShape shape) {
    auto row = std::make_unique<RowCarousel>(title);
    for (const auto& it : items) {
        auto* card = row->items()->add<ItemCard>(shape);
        card->setItem(it);
    }
    return row;
}

void HomeSection::tryClaimFocus() {
    if (focusClaimed_ || !visibleInTree() || hasFocusWithin()) return;
    Screen* s = screen();
    if (!s || !s->isTop() || root().topModal()) return;
    View* target = nullptr;
    if (hero_->visible() && hero_->count() > 0) target = hero_->playButton();
    else
        for (const auto& [order, view] : rows_->rows())
            if (auto* rc = dynamic_cast<RowCarousel*>(view); rc && rc->itemCount() > 0) {
                target = rc->itemAt(0);
                break;
            }
    if (!target && pending_ == 0 && heroDone_ && page_->visible()) {
        // Nothing loaded but some rows failed: land on the first "Try again" once nothing is
        // still loading (a row arriving later would otherwise lose the initial focus).
        const bool loading = std::any_of(rows_->rows().begin(), rows_->rows().end(), [](const auto& entry) {
            return dynamic_cast<SkeletonRow*>(entry.second) != nullptr;
        });
        if (!loading)
            for (const auto& [order, view] : rows_->rows())
                if (auto* er = dynamic_cast<ErrorRow*>(view)) {
                    target = er->retryButton();
                    break;
                }
    }
    if (!target) return;
    focusClaimed_ = true;
    focus().setFocus(target);
}

void HomeSection::showBackdropFor(const jf::BaseItem& item, float delay) {
    backdropItem_ = item;
    backdropDelay_ = delay;
}

void HomeSection::onSectionShown(bool first) {
    shown_ = true;
    if (!first && lockRevision_ != LibraryLock::instance().revision()) {
        load();
    } else if (!first) {
        refreshContinue();
        // The player reports the stop position asynchronously while the pop transition runs, so
        // the first refresh can still see the old position: check once more shortly after.
        if (lateRefresh_) timeline().cancel(lateRefresh_);
        lateRefresh_ = timeline().after(kLateRefreshDelay, [this] {
            lateRefresh_ = 0;
            if (shown_) refreshContinue();
        }, lifetime());
    }
    tryClaimFocus();
}

void HomeSection::onSectionHidden() {
    shown_ = false;
    if (lateRefresh_) {
        timeline().cancel(lateRefresh_);
        lateRefresh_ = 0;
    }
}

float HomeSection::headerScrim() const { return std::clamp(page_->scrollOffset() / 140.0f, 0.0f, 1.0f); }

bool HomeSection::onSectionBack() {
    if (hero_->visible() && hero_->count() > 0 && !hero_->hasFocusWithin()) {
        page_->scrollTo(0);
        focus().setFocus(hero_->playButton());
        return true;
    }
    return false;
}

void HomeSection::onFocusWithin(View* descendant) {
    SectionView::onFocusWithin(descendant);
    for (View* v = descendant; v && v != this; v = v->parent()) {
        if (auto* card = dynamic_cast<ItemCard*>(v)) {
            showBackdropFor(card->item(), 0.45f);
            return;
        }
        if (v == hero_) {
            if (const HeroEntry* e = hero_->current()) showBackdropFor(e->item, 0.0f);
            return;
        }
    }
}

void HomeSection::update(float dt) {
    SectionView::update(dt);
    const float scroll = page_->scrollOffset();
    backdrop_->alpha.set(1.0f - 0.6f * std::clamp(scroll / 320.0f, 0.0f, 1.0f));
    Screen* s = screen();
    const bool active = shown_ && visibleInTree() && s && s->isTop() && !root().topModal();
    hero_->setAutoAdvance(active && scroll < 40);

    if (backdropDelay_ >= 0) {
        backdropDelay_ -= dt;
        if (backdropDelay_ < 0 && backdropItem_.id != backdropShownId_) {
            gfx::ImageRequest req = backdropRequest(backdropItem_);
            if (!req.url.empty()) {
                backdropShownId_ = backdropItem_.id;
                backdrop_->setRequest(req);
            }
        }
    }
}

}  // namespace karafin::screens
