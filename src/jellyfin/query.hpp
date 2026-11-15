#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace karafin::jellyfin {

using QueryParams = std::vector<std::pair<std::string, std::string>>;

// Field sets used by the client; pass your own via ItemsQuery::fields to override.
namespace fields {
inline constexpr const char* kCard = "PrimaryImageAspectRatio,ChildCount,RecursiveItemCount,MediaSourceCount";
inline constexpr const char* kDetail =
    "Overview,Genres,People,Studios,Taglines,MediaSources,MediaStreams,Chapters,CanDownload,Path,"
    "ProviderIds,PrimaryImageAspectRatio,DateCreated,ChildCount,RecursiveItemCount,RemoteTrailers";
inline constexpr const char* kEpisode =
    "Overview,MediaSources,MediaStreams,Chapters,CanDownload,Path,PrimaryImageAspectRatio";
}  // namespace fields

// GET /Items query. Unset members are omitted. Comma lists are passed as given.
//
//   ItemsQuery q;
//   q.parentId(libraryId).includeItemTypes("Movie").recursive().sortBy("SortName").limit(60);
struct ItemsQuery {
    std::string parentIdValue;
    std::string includeItemTypesValue;   // "Movie,Series"
    std::string excludeItemTypesValue;
    std::optional<bool> recursiveValue;
    std::string sortByValue;             // "SortName", "DateCreated,SortName", "Random", "CommunityRating"
    std::string sortOrderValue;          // "Ascending" / "Descending"
    std::string filtersValue;            // "IsFavorite", "IsUnplayed", "IsResumable", "IsPlayed"
    std::string fieldsValue;             // empty -> fields::kCard
    std::optional<int> startIndexValue;
    std::optional<int> limitValue;
    std::string searchTermValue;
    std::string genresValue;             // pipe-separated as Jellyfin expects: "Action|Comedy"
    std::string yearsValue;              // "2019,2020"
    std::string personIdsValue;
    std::string studioIdsValue;
    std::string idsValue;
    std::string mediaTypesValue;         // "Video"
    std::string enableImageTypesValue = "Primary,Backdrop,Thumb,Logo";
    std::optional<int> imageTypeLimitValue = 1;
    std::optional<bool> enableUserDataValue = true;
    std::optional<bool> enableTotalRecordCountValue;
    std::optional<bool> isPlayedValue;
    std::optional<bool> isFavoriteValue;
    std::optional<bool> isMissingValue;
    QueryParams extra;                   // anything else

    ItemsQuery& parentId(std::string v) { parentIdValue = std::move(v); return *this; }
    ItemsQuery& includeItemTypes(std::string v) { includeItemTypesValue = std::move(v); return *this; }
    ItemsQuery& excludeItemTypes(std::string v) { excludeItemTypesValue = std::move(v); return *this; }
    ItemsQuery& recursive(bool v = true) { recursiveValue = v; return *this; }
    ItemsQuery& sortBy(std::string v, std::string order = "Ascending") {
        sortByValue = std::move(v);
        sortOrderValue = std::move(order);
        return *this;
    }
    ItemsQuery& sortOrder(std::string v) { sortOrderValue = std::move(v); return *this; }
    ItemsQuery& filters(std::string v) { filtersValue = std::move(v); return *this; }
    ItemsQuery& fields(std::string v) { fieldsValue = std::move(v); return *this; }
    ItemsQuery& startIndex(int v) { startIndexValue = v; return *this; }
    ItemsQuery& limit(int v) { limitValue = v; return *this; }
    ItemsQuery& searchTerm(std::string v) { searchTermValue = std::move(v); return *this; }
    ItemsQuery& genres(std::string v) { genresValue = std::move(v); return *this; }
    ItemsQuery& years(std::string v) { yearsValue = std::move(v); return *this; }
    ItemsQuery& personIds(std::string v) { personIdsValue = std::move(v); return *this; }
    ItemsQuery& studioIds(std::string v) { studioIdsValue = std::move(v); return *this; }
    ItemsQuery& ids(std::string v) { idsValue = std::move(v); return *this; }
    ItemsQuery& mediaTypes(std::string v) { mediaTypesValue = std::move(v); return *this; }
    ItemsQuery& enableImageTypes(std::string v) { enableImageTypesValue = std::move(v); return *this; }
    ItemsQuery& imageTypeLimit(int v) { imageTypeLimitValue = v; return *this; }
    ItemsQuery& enableUserData(bool v) { enableUserDataValue = v; return *this; }
    ItemsQuery& enableTotalRecordCount(bool v) { enableTotalRecordCountValue = v; return *this; }
    ItemsQuery& isPlayed(bool v) { isPlayedValue = v; return *this; }
    ItemsQuery& isFavorite(bool v) { isFavoriteValue = v; return *this; }
    ItemsQuery& isMissing(bool v) { isMissingValue = v; return *this; }
    ItemsQuery& param(std::string key, std::string value) {
        extra.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    // Query parameters (without userId, which the client adds).
    QueryParams toParams() const;
};

}  // namespace karafin::jellyfin
