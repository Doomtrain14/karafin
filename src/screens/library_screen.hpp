#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/error.hpp"
#include "jellyfin/models.hpp"
#include "jellyfin/query.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/skeleton.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/screen.hpp"
#include "ui/widgets/grid.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

// What a library grid shows.
struct LibrarySpec {
    std::string title;
    std::string parentId;
    std::string collectionType;  // "movies", "tvshows", "boxsets", ... (derives item types)
    std::string includeTypes;    // explicit IncludeItemTypes (overrides collectionType)
    bool recursive = true;
    std::string personIds;       // filmography
    std::string genre;           // preset genre filter
    std::string filters;         // fixed filters, e.g. "IsFavorite"
};

class LibraryFilterChip;

// Virtualized poster grid over /Items with sparse paging (pages load as cells come into view),
// sort / filter side sheets, genre filter (shown as a chip while active), A-Z jump and an item
// count. While LibraryLock hides titles, grids that can mix libraries (people, favorites,
// collections, folders) first list the matching ids, drop the hidden ones and then load pages by
// id, so counts, paging and the A-Z jump only see what is shown.
class LibraryScreen : public ui::Screen {
public:
    explicit LibraryScreen(LibrarySpec spec);
    ~LibraryScreen() override;

    void onShow() override;

private:
    enum class Sort : int { Name, DateAdded, Release, Rating, Runtime, Random };
    enum class Filter : int { All, Unplayed, Played, Favorites, Resumable };

    static constexpr float kLateRefreshDelay = 2.0f;
    // A failed page is retried after kPageRetryDelay, doubling per consecutive failure up to
    // kPageRetryMax, and only while some of its cells are on screen.
    static constexpr float kPageRetryDelay = 3.0f;
    static constexpr float kPageRetryMax = 30.0f;

    void reload();
    void loadIndex();
    void showResults();
    bool needsLockFilter() const;
    size_t indexForLetter(const std::string& letter) const;
    void refreshVisiblePages();
    void ensurePage(size_t page, bool force = false);
    void pageFailed(size_t page, const jellyfin::ApiError& error);
    bool pageVisible(size_t page) const;
    void bindCell(ui::View& cell, size_t index);
    void updateHeader();
    void openSort();
    void openFilter();
    void openGenres();
    void openJump();
    void jumpTo(const std::string& letter);
    std::string itemTypes() const;
    std::string typeNoun(int count) const;
    jellyfin::ItemsQuery baseQuery() const;

    LibrarySpec spec_;
    ui::Label* title_ = nullptr;
    ui::Label* subtitle_ = nullptr;
    LibraryFilterChip* chip_ = nullptr;
    ActionButton* sortBtn_ = nullptr;
    ActionButton* filterBtn_ = nullptr;
    ActionButton* jumpBtn_ = nullptr;
    ui::Grid* grid_ = nullptr;
    SkeletonGrid* skeleton_ = nullptr;
    StatusView* status_ = nullptr;
    ui::Timeline::Handle lateRefresh_ = 0;

    std::vector<std::unique_ptr<jellyfin::BaseItem>> items_;  // null = page not loaded yet
    std::set<size_t> loadingPages_;
    std::set<size_t> backoffPages_;       // failed pages waiting for their retry timer
    std::map<size_t, int> pageFailures_;  // consecutive failures per page (backoff exponent)
    bool pageErrorShown_ = false;         // one toast per failure streak, not per page / retry
    int total_ = 0;
    bool shownOnce_ = false;
    Sort sort_ = Sort::Name;
    bool descending_ = false;
    Filter filter_ = Filter::All;
    std::string genre_;
    std::vector<std::string> genres_;
    CancelToken token_;
    CancelToken genreToken_;
    size_t pendingFocus_ = SIZE_MAX;
    // Lock filtering: the shown ids in grid order and their lower-cased sort names (A-Z jump).
    bool lockFiltered_ = false;
    std::vector<std::string> index_;
    std::vector<std::string> indexNames_;
    std::string pendingJump_;  // letter to focus once the re-sorted index is loaded
    uint64_t lockRevision_ = 0;
};

}  // namespace karafin::screens
