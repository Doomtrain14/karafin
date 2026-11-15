#include "screens/library_screen.hpp"

#include <algorithm>
#include <format>

#include "core/settings.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/library_lock.hpp"
#include "screens/widgets/item_card.hpp"
#include "ui/root.hpp"
#include "ui/widgets/list_menu.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr size_t kPageSize = 60;
constexpr int kRandomLimit = 120;
constexpr float kGridTop = 132;

const char* sortName(int s) {
    static const char* names[] = {"Name", "Date added", "Release date", "Rating", "Runtime", "Random"};
    return s >= 0 && s < 6 ? names[s] : "Name";
}
const char* filterName(int f) {
    static const char* names[] = {"All", "Unwatched", "Watched", "Favorites", "In progress"};
    return f >= 0 && f < 5 ? names[f] : "All";
}
}  // namespace

LibraryScreen::LibraryScreen(LibrarySpec spec) : spec_(std::move(spec)) {
    genre_ = spec_.genre;
    sort_ = static_cast<Sort>(std::clamp(Settings::instance().get<int>("ui.library_sort", 0), 0, 5));
    descending_ = Settings::instance().get<bool>("ui.library_desc", false);
    if (!spec_.personIds.empty() && sort_ == Sort::Name) {
        sort_ = Sort::Release;
        descending_ = true;
    }

    title_ = add<Label>(spec_.title.empty() ? "Library" : spec_.title, theme::type::largeTitle);
    title_->setFrame({theme::layout::marginX, 34, 700, 44});
    subtitle_ = add<Label>("", theme::type::callout, theme::color::textSecondary);
    subtitle_->setFrame({theme::layout::marginX, 82, 760, 22});

    const bool canScan = !spec_.parentId.empty() && isAdmin();
    const bool canRelock = !spec_.parentId.empty() && LibraryLock::instance().hasCode() &&
                           LibraryLock::instance().isLocked(spec_.parentId);
    const float buttonCount = 3.0f + (canScan ? 1.0f : 0.0f) + (canRelock ? 1.0f : 0.0f);
    const float buttonsW = buttonCount * 48 + (buttonCount - 1) * 12;
    auto* buttons = add<Box>(Axis::Horizontal);
    buttons->setSpacing(12);
    buttons->setFrame({theme::layout::screenWidth - theme::layout::marginX - buttonsW, 40, buttonsW, 48});
    sortBtn_ = buttons->add<ActionButton>("", gfx::Icon::List, ButtonStyle::Icon, "Sort");
    filterBtn_ = buttons->add<ActionButton>("", gfx::Icon::Filter, ButtonStyle::Icon, "Filter");
    jumpBtn_ = buttons->add<ActionButton>("", gfx::Icon::ChevronDown, ButtonStyle::Icon, "Jump to");
    sortBtn_->onActivate = [this] { openSort(); };
    filterBtn_->onActivate = [this] { openFilter(); };
    jumpBtn_->onActivate = [this] { openJump(); };
    if (canScan) {
        auto* scan = buttons->add<ActionButton>("", gfx::Icon::Refresh, ButtonStyle::Icon, "Scan library");
        scan->onActivate = [this] { scanLibrary(spec_.parentId, spec_.title); };
    }
    if (canRelock) {
        auto* relock = buttons->add<ActionButton>("", gfx::Icon::Lock, ButtonStyle::Icon, "Lock again");
        relock->onActivate = [id = spec_.parentId] {
            LibraryLock::instance().relock(id);
            nav().pop();
        };
    }

    const Vec2 cell = PosterCard(CardShape::Poster).measure({});
    grid_ = add<Grid>(cell);
    // Clipped so scrolled rows don't draw over the title / count line; the frame starts a bit
    // higher (cards stay put) to leave room for a focused card's glow and ring in the top row.
    const float lift = 20;
    const float o = theme::layout::focusOverscan;
    grid_->setFrame({0, kGridTop - lift, theme::layout::screenWidth, theme::layout::screenHeight - kGridTop + lift});
    grid_->setPadding({theme::layout::marginX, o + lift, theme::layout::marginX, o + 40});
    grid_->setRevealInsets(o + 8 + lift, o + 30);
    grid_->setClipsChildren(true);
    grid_->setZ(-1);
    grid_->count = [this] { return static_cast<size_t>(total_); };
    grid_->createCell = [] { return std::make_unique<ItemCard>(CardShape::Poster); };
    grid_->bindCell = [this](View& v, size_t i) { bindCell(v, i); };
    grid_->setVisible(false);

    skeleton_ = add<SkeletonGrid>(cell, PosterCard::artSize(CardShape::Poster));
    skeleton_->setFrame({0, kGridTop, theme::layout::screenWidth, theme::layout::screenHeight - kGridTop});
    skeleton_->setVisible(false);

    status_ = add<StatusView>();
    status_->setFrame({0, kGridTop, theme::layout::screenWidth, theme::layout::screenHeight - kGridTop - 50});

    addShortcut(Key::X, [this] { openSort(); });
    addShortcut(Key::Y, [this] { openFilter(); });
    addShortcut(Key::Minus, [this] { openJump(); });
    setHints({{Key::X, "Sort"}, {Key::Y, "Filter"}, {Key::Minus, "A-Z"}, {Key::ZR, "Page"}});
    reload();
}

LibraryScreen::~LibraryScreen() {
    token_.cancel();
    genreToken_.cancel();
}

std::string LibraryScreen::itemTypes() const {
    if (!spec_.includeTypes.empty()) return spec_.includeTypes;
    const std::string& c = spec_.collectionType;
    if (c == "movies") return "Movie";
    if (c == "tvshows") return "Series";
    if (c == "boxsets") return "BoxSet";
    if (c == "musicvideos") return "MusicVideo";
    if (c == "homevideos") return "Video";
    return {};
}

std::string LibraryScreen::typeNoun(int count) const {
    const std::string types = itemTypes();
    const bool one = count == 1;
    if (types == "Movie") return one ? "movie" : "movies";
    if (types == "Series") return one ? "show" : "shows";
    if (types == "BoxSet") return one ? "collection" : "collections";
    if (types == "Video" || types == "MusicVideo") return one ? "video" : "videos";
    return one ? "item" : "items";
}

jf::ItemsQuery LibraryScreen::baseQuery() const {
    jf::ItemsQuery q;
    if (!spec_.parentId.empty()) q.parentId(spec_.parentId);
    const std::string types = itemTypes();
    if (!types.empty()) {
        q.includeItemTypes(types);
        q.recursive(spec_.recursive);
    } else if (spec_.parentId.empty() || spec_.recursive) {
        q.recursive(spec_.parentId.empty());
    }
    if (!spec_.personIds.empty()) q.personIds(spec_.personIds);
    if (!genre_.empty()) q.genres(genre_);
    std::string filters = spec_.filters;
    auto addFilter = [&](const char* f) {
        if (!filters.empty()) filters += ",";
        filters += f;
    };
    switch (filter_) {
        case Filter::Unplayed: addFilter("IsUnplayed"); break;
        case Filter::Played: addFilter("IsPlayed"); break;
        case Filter::Favorites: addFilter("IsFavorite"); break;
        case Filter::Resumable: addFilter("IsResumable"); break;
        case Filter::All: break;
    }
    if (!filters.empty()) q.filters(filters);
    const char* order = descending_ ? "Descending" : "Ascending";
    switch (sort_) {
        case Sort::Name: q.sortBy("SortName", order); break;
        case Sort::DateAdded: q.sortBy("DateCreated,SortName", order); break;
        case Sort::Release: q.sortBy("PremiereDate,ProductionYear,SortName", order); break;
        case Sort::Rating: q.sortBy("CommunityRating,SortName", order); break;
        case Sort::Runtime: q.sortBy("Runtime,SortName", order); break;
        case Sort::Random: q.sortBy("Random"); break;
    }
    return q;
}

void LibraryScreen::reload() {
    token_.cancel();
    token_.reset();
    items_.clear();
    loadingPages_.clear();
    backoffPages_.clear();
    pageFailures_.clear();
    pageErrorShown_ = false;
    total_ = 0;
    // Keep focus on the screen (not on a cell that is about to disappear) while reloading.
    if (grid_->hasFocusWithin()) focus().setFocus(sortBtn_);
    grid_->setVisible(false);
    status_->hide();
    skeleton_->setVisible(true);
    skeleton_->alpha.set(0.0f);
    skeleton_->alpha.tweenTo(1.0f, theme::motion::normal, ease::outCubic);
    updateHeader();
    auto client = jf::currentClient();
    if (!client) return;
    jf::ItemsQuery q = baseQuery();
    const bool random = sort_ == Sort::Random;
    q.startIndex(0).limit(random ? kRandomLimit : static_cast<int>(kPageSize)).enableTotalRecordCount(true);
    client->items(q).then(
        [this, random](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            skeleton_->setVisible(false);
            if (!r) {
                status_->showError(errorMessage(r.error), [this] { reload(); });
                status_->focusAction();
                return;
            }
            total_ = random ? static_cast<int>(r->items.size())
                            : std::max(r->totalRecordCount, static_cast<int>(r->items.size()));
            items_.clear();
            items_.resize(static_cast<size_t>(total_));
            for (size_t i = 0; i < r->items.size() && i < items_.size(); ++i)
                items_[i] = std::make_unique<jf::BaseItem>(std::move(r->items[i]));
            updateHeader();
            if (total_ == 0) {
                const bool filtered = filter_ != Filter::All || genre_ != spec_.genre;
                std::vector<StatusAction> actions;
                if (filtered)
                    actions.push_back({"Clear filters", gfx::Icon::Close, [this] {
                                           filter_ = Filter::All;
                                           genre_ = spec_.genre;
                                           reload();
                                       }, true});
                status_->showEmpty(gfx::Icon::Library, filtered ? "No matches" : "Nothing here yet",
                                   filtered ? "No items match the current filters." : "This library is empty.",
                                   std::move(actions));
                if (filtered) status_->focusAction();
                else focus().setFocus(sortBtn_);
                return;
            }
            status_->hide();
            grid_->setVisible(true);
            grid_->alpha.set(0.0f);
            grid_->alpha.tweenTo(1.0f, theme::motion::slow, ease::outCubic);
            grid_->reload();
            grid_->scrollTo(0, false);
            const size_t target = pendingFocus_ != SIZE_MAX ? std::min(pendingFocus_, items_.size() - 1) : 0;
            pendingFocus_ = SIZE_MAX;
            grid_->focusIndex(target);
        },
        token_);
}

void LibraryScreen::ensurePage(size_t page, bool force) {
    if (sort_ == Sort::Random || loadingPages_.count(page) || backoffPages_.count(page)) return;
    const size_t start = page * kPageSize;
    if (start >= items_.size()) return;
    bool missing = force;
    for (size_t i = start; !missing && i < std::min(items_.size(), start + kPageSize); ++i)
        if (!items_[i]) missing = true;
    if (!missing) return;
    auto client = jf::currentClient();
    if (!client) return;
    loadingPages_.insert(page);
    jf::ItemsQuery q = baseQuery();
    q.startIndex(static_cast<int>(start)).limit(static_cast<int>(kPageSize)).enableTotalRecordCount(false);
    client->items(q).then(
        [this, page, start](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            loadingPages_.erase(page);
            if (!r) {
                pageFailed(page, r.error);
                return;
            }
            pageFailures_.erase(page);
            if (pageFailures_.empty()) pageErrorShown_ = false;
            for (size_t k = 0; k < r->items.size() && start + k < items_.size(); ++k) {
                items_[start + k] = std::make_unique<jf::BaseItem>(std::move(r->items[k]));
                if (View* cell = grid_->cellAt(start + k)) bindCell(*cell, start + k);
            }
        },
        token_);
}

void LibraryScreen::pageFailed(size_t page, const jf::ApiError& error) {
    // The page's cells keep their placeholders; it is fetched again after a backoff (sooner if
    // its cells are re-bound after the timer), never in a tight loop.
    const int failures = ++pageFailures_[page];
    if (!pageErrorShown_) {
        pageErrorShown_ = true;
        toast("Couldn't load some items. " + errorMessage(error), ToastKind::Error, gfx::Icon::Warning);
    }
    const float delay = std::min(kPageRetryMax, kPageRetryDelay * static_cast<float>(1 << std::min(failures - 1, 4)));
    backoffPages_.insert(page);
    const CancelToken tok = token_;
    timeline().after(delay, [this, page, tok] {
        // A reload() since the failure starts over with fresh state.
        if (tok.cancelled()) return;
        backoffPages_.erase(page);
        if (pageVisible(page)) ensurePage(page);
    }, lifetime());
}

bool LibraryScreen::pageVisible(size_t page) const {
    const size_t start = page * kPageSize;
    for (size_t i = start; i < std::min(items_.size(), start + kPageSize); ++i)
        if (grid_->cellAt(i)) return true;
    return false;
}

void LibraryScreen::bindCell(View& cell, size_t index) {
    auto& card = static_cast<ItemCard&>(cell);
    if (index < items_.size() && items_[index]) {
        card.setItem(*items_[index]);
    } else {
        card.setLoading();
        ensurePage(index / kPageSize);
    }
    // Prefetch the next page shortly before it is needed.
    if ((index % kPageSize) > kPageSize - 15) ensurePage(index / kPageSize + 1);
}

void LibraryScreen::updateHeader() {
    std::string count = total_ > 0 ? std::format("{} {}", formatCount(total_), typeNoun(total_)) : std::string();
    std::string filter;
    if (filter_ != Filter::All) filter = filterName(static_cast<int>(filter_));
    if (!genre_.empty()) filter = filter.empty() ? genre_ : filter + ", " + genre_;
    std::string sort = sort_ == Sort::Random ? std::string("Shuffled")
                                             : std::format("Sorted by {} {}", sortName(static_cast<int>(sort_)),
                                                           descending_ ? "\xE2\x86\x93" : "\xE2\x86\x91");
    subtitle_->setText(joinDots({count, filter, sort}));
    filterBtn_->setActive(filter_ != Filter::All || genre_ != spec_.genre);
}

void LibraryScreen::openSort() {
    std::vector<ListItem> items;
    for (int s = 0; s < 6; ++s) {
        const bool current = static_cast<int>(sort_) == s;
        std::string detail;
        if (current && s != static_cast<int>(Sort::Random)) detail = descending_ ? "Descending" : "Ascending";
        items.push_back({sortName(s), "", detail, gfx::Icon::None, current, true, [this, s, current] {
                             if (current) {
                                 descending_ = !descending_;
                             } else {
                                 sort_ = static_cast<Sort>(s);
                                 descending_ = sort_ == Sort::DateAdded || sort_ == Sort::Release || sort_ == Sort::Rating;
                             }
                             Settings::instance().set("ui.library_sort", static_cast<int>(sort_));
                             Settings::instance().set("ui.library_desc", descending_);
                             reload();
                         }});
    }
    present(SideSheet::make("Sort by", std::move(items), static_cast<int>(sort_), "Select again to reverse the order"));
}

void LibraryScreen::openFilter() {
    std::vector<ListItem> items;
    for (int f = 0; f < 5; ++f) {
        const bool current = static_cast<int>(filter_) == f;
        items.push_back({filterName(f), "", "", gfx::Icon::None, current, true, [this, f] {
                             filter_ = static_cast<Filter>(f);
                             reload();
                         }});
    }
    items.push_back({"Genre", "", genre_.empty() ? "Any" : genre_, gfx::Icon::Filter, false, true,
                     [this] { timeline().after(0.15f, [this] { openGenres(); }, lifetime()); }});
    present(SideSheet::make("Filter", std::move(items), static_cast<int>(filter_)));
}

void LibraryScreen::openGenres() {
    auto show = [this] {
        std::vector<ListItem> items;
        items.push_back({"Any genre", "", "", gfx::Icon::None, genre_.empty(), true, [this] {
                             genre_.clear();
                             reload();
                         }});
        for (const auto& g : genres_)
            items.push_back({g, "", "", gfx::Icon::None, g == genre_, true, [this, g] {
                                 genre_ = g;
                                 reload();
                             }});
        present(SideSheet::make("Genre", std::move(items)));
    };
    if (!genres_.empty()) {
        show();
        return;
    }
    auto client = jf::currentClient();
    if (!client) return;
    jf::QueryParams q{{"SortBy", "SortName"}, {"SortOrder", "Ascending"}, {"UserId", client->userId()}};
    if (!spec_.parentId.empty()) q.emplace_back("ParentId", spec_.parentId);
    if (const std::string types = itemTypes(); !types.empty()) q.emplace_back("IncludeItemTypes", types);
    genreToken_.cancel();
    genreToken_.reset();
    client->getJson("/Genres", q).then(
        [this, show](jf::Result<nlohmann::json> r) {
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            genres_.clear();
            if (r->is_object() && r->contains("Items") && (*r)["Items"].is_array())
                for (const auto& g : (*r)["Items"])
                    if (g.is_object() && g.contains("Name") && g["Name"].is_string())
                        genres_.push_back(g["Name"].get<std::string>());
            if (genres_.empty()) {
                toast("No genres in this library", ToastKind::Info, gfx::Icon::Info);
                return;
            }
            show();
        },
        genreToken_);
}

void LibraryScreen::openJump() {
    if (total_ <= 0) return;
    std::vector<ListItem> items;
    items.push_back({"#", "Numbers and symbols", "", gfx::Icon::None, false, true, [this] { jumpTo("#"); }});
    for (char c = 'A'; c <= 'Z'; ++c) {
        const std::string letter(1, c);
        items.push_back({letter, "", "", gfx::Icon::None, false, true, [this, letter] { jumpTo(letter); }});
    }
    auto sheet = SideSheet::make("Jump to", std::move(items), -1,
                                 sort_ == Sort::Name && !descending_ ? "" : "Switches to sorting by name");
    sheet->setPanelWidth(320);
    present(std::move(sheet));
}

void LibraryScreen::jumpTo(const std::string& letter) {
    const bool needsResort = sort_ != Sort::Name || descending_;
    auto client = jf::currentClient();
    if (!client) return;
    if (needsResort) {
        sort_ = Sort::Name;
        descending_ = false;
        Settings::instance().set("ui.library_sort", 0);
        Settings::instance().set("ui.library_desc", false);
    }
    if (letter == "#") {
        if (needsResort) reload();
        else grid_->focusIndex(0);
        return;
    }
    jf::ItemsQuery q = baseQuery();
    // Jellyfin compares against the lower-cased SortName.
    q.param("NameLessThan", lowerAscii(letter)).limit(0).enableTotalRecordCount(true);
    client->items(q).then(
        [this, needsResort](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            const size_t index = static_cast<size_t>(std::max(0, r->totalRecordCount));
            if (needsResort) {
                pendingFocus_ = index;
                reload();
                return;
            }
            if (items_.empty()) return;
            const size_t target = std::min(index, items_.size() - 1);
            ensurePage(target / kPageSize);
            grid_->focusIndex(target);
        },
        token_);
}

void LibraryScreen::onShow() {
    if (!shownOnce_) {
        shownOnce_ = true;
        return;
    }
    // Watched state / progress may have changed in the player or on a detail page: quietly
    // re-fetch the pages around the focused cell, and once more after the player's stop report
    // has had time to reach the server.
    refreshVisiblePages();
    if (lateRefresh_) timeline().cancel(lateRefresh_);
    lateRefresh_ = timeline().after(kLateRefreshDelay, [this] {
        lateRefresh_ = 0;
        if (isTop()) refreshVisiblePages();
    }, lifetime());
}

void LibraryScreen::refreshVisiblePages() {
    if (items_.empty() || sort_ == Sort::Random) return;
    const size_t page = grid_->focusedIndex() / kPageSize;
    for (size_t p = page > 0 ? page - 1 : 0; p <= page + 1; ++p) ensurePage(p, true);
}

}  // namespace karafin::screens
