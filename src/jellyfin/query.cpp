#include "jellyfin/query.hpp"

namespace karafin::jellyfin {

namespace {
void add(QueryParams& p, const char* key, const std::string& v) {
    if (!v.empty()) p.emplace_back(key, v);
}
void add(QueryParams& p, const char* key, const std::optional<bool>& v) {
    if (v) p.emplace_back(key, *v ? "true" : "false");
}
void add(QueryParams& p, const char* key, const std::optional<int>& v) {
    if (v) p.emplace_back(key, std::to_string(*v));
}
}  // namespace

QueryParams ItemsQuery::toParams() const {
    QueryParams p;
    add(p, "ParentId", parentIdValue);
    add(p, "IncludeItemTypes", includeItemTypesValue);
    add(p, "ExcludeItemTypes", excludeItemTypesValue);
    add(p, "Recursive", recursiveValue);
    add(p, "SortBy", sortByValue);
    add(p, "SortOrder", sortOrderValue);
    add(p, "Filters", filtersValue);
    p.emplace_back("Fields", fieldsValue.empty() ? std::string(fields::kCard) : fieldsValue);
    add(p, "StartIndex", startIndexValue);
    add(p, "Limit", limitValue);
    add(p, "SearchTerm", searchTermValue);
    add(p, "Genres", genresValue);
    add(p, "Years", yearsValue);
    add(p, "PersonIds", personIdsValue);
    add(p, "StudioIds", studioIdsValue);
    add(p, "Ids", idsValue);
    add(p, "MediaTypes", mediaTypesValue);
    add(p, "EnableImageTypes", enableImageTypesValue);
    add(p, "ImageTypeLimit", imageTypeLimitValue);
    add(p, "EnableUserData", enableUserDataValue);
    add(p, "EnableTotalRecordCount", enableTotalRecordCountValue);
    add(p, "IsPlayed", isPlayedValue);
    add(p, "IsFavorite", isFavoriteValue);
    add(p, "IsMissing", isMissingValue);
    for (const auto& kv : extra) p.push_back(kv);
    return p;
}

}  // namespace karafin::jellyfin
