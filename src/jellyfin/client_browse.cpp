#include <algorithm>

#include "jellyfin/client.hpp"
#include "jellyfin/detail/client_impl.hpp"
#include "jellyfin/detail/json_get.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;

namespace {

void appendCommon(QueryParams& q, const char* fieldList) {
    q.emplace_back("Fields", fieldList);
    q.emplace_back("EnableImageTypes", "Primary,Backdrop,Thumb,Logo");
    q.emplace_back("ImageTypeLimit", "1");
    q.emplace_back("EnableUserData", "true");
}

}  // namespace

std::string_view imageTypeName(ImageType t) {
    switch (t) {
        case ImageType::Primary: return "Primary";
        case ImageType::Backdrop: return "Backdrop";
        case ImageType::Thumb: return "Thumb";
        case ImageType::Logo: return "Logo";
        case ImageType::Banner: return "Banner";
        case ImageType::Art: return "Art";
        case ImageType::Disc: return "Disc";
    }
    return "Primary";
}

// --- Browsing ----------------------------------------------------------------------------------

Call<QueryResult<BaseItem>> Client::userViews() {
    QueryParams q = userParam();
    q.emplace_back("IncludeExternalContent", "false");
    return jsonCall<QueryResult<BaseItem>>(request(http::Method::Get, userPath("/UserViews", "/Views"), q),
                                           ErrorContext::Generic, parseItems);
}

Call<QueryResult<BaseItem>> Client::resumeItems(int limit) {
    QueryParams q = userParam();
    q.emplace_back("Limit", std::to_string(limit));
    q.emplace_back("MediaTypes", "Video");
    q.emplace_back("Recursive", "true");
    appendCommon(q, fields::kCard);
    return jsonCall<QueryResult<BaseItem>>(
        request(http::Method::Get, userPath("/UserItems/Resume", "/Items/Resume"), q), ErrorContext::Generic,
        parseItems);
}

Call<QueryResult<BaseItem>> Client::nextUp(int limit, std::string seriesId) {
    QueryParams q = userParam();
    q.emplace_back("Limit", std::to_string(limit));
    if (!seriesId.empty()) q.emplace_back("SeriesId", seriesId);
    q.emplace_back("EnableResumable", "false");
    q.emplace_back("EnableRewatching", "false");
    appendCommon(q, fields::kCard);
    return jsonCall<QueryResult<BaseItem>>(request(http::Method::Get, "/Shows/NextUp", q), ErrorContext::Generic,
                                           parseItems);
}

Call<std::vector<BaseItem>> Client::latest(std::string parentId, int limit) {
    QueryParams q = userParam();
    if (!parentId.empty()) q.emplace_back("ParentId", parentId);
    q.emplace_back("Limit", std::to_string(limit));
    q.emplace_back("GroupItems", "true");
    appendCommon(q, fields::kCard);
    return jsonCall<std::vector<BaseItem>>(
        request(http::Method::Get, userPath("/Items/Latest", "/Items/Latest"), q), ErrorContext::Generic,
        [](const Json& j) { return parseItems(j).items; });
}

Call<QueryResult<BaseItem>> Client::items(const ItemsQuery& query) {
    QueryParams q = userParam();
    for (auto& kv : query.toParams()) q.push_back(std::move(kv));
    return jsonCall<QueryResult<BaseItem>>(request(http::Method::Get, userPath("/Items", "/Items"), q),
                                           ErrorContext::Generic, parseItems);
}

Call<BaseItem> Client::item(std::string itemId) {
    QueryParams q = userParam();
    q.emplace_back("Fields", withTrickplay(fields::kDetail));
    const std::string path = modernApi() ? "/Items/" + http::urlEncode(itemId)
                                         : "/Users/" + userId() + "/Items/" + http::urlEncode(itemId);
    return jsonCall<BaseItem>(request(http::Method::Get, path, q), ErrorContext::Generic, parseItem);
}

Call<QueryResult<BaseItem>> Client::seasons(std::string seriesId) {
    QueryParams q = userParam();
    appendCommon(q, "PrimaryImageAspectRatio,ChildCount,RecursiveItemCount,Overview");
    return jsonCall<QueryResult<BaseItem>>(
        request(http::Method::Get, "/Shows/" + http::urlEncode(seriesId) + "/Seasons", q), ErrorContext::Generic,
        parseItems);
}

Call<QueryResult<BaseItem>> Client::episodes(std::string seriesId, std::string seasonId) {
    QueryParams q = userParam();
    if (!seasonId.empty()) q.emplace_back("SeasonId", seasonId);
    const std::string f = withTrickplay(fields::kEpisode);
    appendCommon(q, f.c_str());
    return jsonCall<QueryResult<BaseItem>>(
        request(http::Method::Get, "/Shows/" + http::urlEncode(seriesId) + "/Episodes", q), ErrorContext::Generic,
        parseItems);
}

Call<QueryResult<BaseItem>> Client::similar(std::string itemId, int limit) {
    QueryParams q = userParam();
    q.emplace_back("Limit", std::to_string(limit));
    q.emplace_back("Fields", fields::kCard);
    return jsonCall<QueryResult<BaseItem>>(
        request(http::Method::Get, "/Items/" + http::urlEncode(itemId) + "/Similar", q), ErrorContext::Generic,
        parseItems);
}

Call<QueryResult<SearchHint>> Client::searchHints(std::string term, int limit, std::string includeItemTypes) {
    QueryParams q = userParam();
    q.emplace_back("searchTerm", term);
    q.emplace_back("Limit", std::to_string(limit));
    if (!includeItemTypes.empty()) q.emplace_back("IncludeItemTypes", includeItemTypes);
    return jsonCall<QueryResult<SearchHint>>(
        request(http::Method::Get, "/Search/Hints", q), ErrorContext::Generic, [](const Json& j) {
            QueryResult<SearchHint> out;
            for (const auto& h : detail::getArr(j, "SearchHints"))
                if (h.is_object()) out.items.push_back(parseSearchHint(h));
            out.totalRecordCount = detail::getInt(j, "TotalRecordCount", static_cast<int>(out.items.size()));
            return out;
        });
}

Call<UserData> Client::setFavorite(std::string itemId, bool favorite) {
    const auto method = favorite ? http::Method::Post : http::Method::Delete;
    const std::string id = http::urlEncode(itemId);
    auto req = modernApi() ? request(method, "/UserFavoriteItems/" + id, userParam())
                           : request(method, "/Users/" + userId() + "/FavoriteItems/" + id);
    return jsonCall<UserData>(std::move(req), ErrorContext::Generic, parseUserData);
}

Call<UserData> Client::setPlayed(std::string itemId, bool played) {
    const auto method = played ? http::Method::Post : http::Method::Delete;
    const std::string id = http::urlEncode(itemId);
    auto req = modernApi() ? request(method, "/UserPlayedItems/" + id, userParam())
                           : request(method, "/Users/" + userId() + "/PlayedItems/" + id);
    return jsonCall<UserData>(std::move(req), ErrorContext::Generic, parseUserData);
}

Call<Unit> Client::refreshLibrary() {
    return emptyCall(request(http::Method::Post, "/Library/Refresh"), ErrorContext::Generic);
}

Call<Unit> Client::refreshItem(std::string itemId) {
    QueryParams q{{"Recursive", "true"},
                  {"MetadataRefreshMode", "Default"},
                  {"ImageRefreshMode", "Default"},
                  {"ReplaceAllMetadata", "false"},
                  {"ReplaceAllImages", "false"}};
    return emptyCall(request(http::Method::Post, "/Items/" + http::urlEncode(itemId) + "/Refresh", q),
                     ErrorContext::Generic);
}
// --- Images ------------------------------------------------------------------------------------

std::string Client::itemImageUrl(const std::string& itemId, ImageType type, const std::string& tag,
                                 const ImageOptions& opts) const {
    if (itemId.empty()) return {};
    std::string path = "/Items/" + http::urlEncode(itemId) + "/Images/" + std::string(imageTypeName(type));
    if (type == ImageType::Backdrop && opts.index > 0) path += "/" + std::to_string(opts.index);
    QueryParams q;
    if (opts.maxWidth > 0) q.emplace_back("maxWidth", std::to_string(opts.maxWidth));
    if (opts.maxHeight > 0) q.emplace_back("maxHeight", std::to_string(opts.maxHeight));
    // Logos, disc art and clearart are transparent PNGs. JPEG flattens that alpha onto black.
    const bool keepAlpha = type == ImageType::Logo || type == ImageType::Art || type == ImageType::Disc;
    if (!keepAlpha && opts.quality > 0) q.emplace_back("quality", std::to_string(opts.quality));
    if (!tag.empty()) q.emplace_back("tag", tag);
    q.emplace_back("format", keepAlpha ? "Png" : "Jpg");
    return http::joinUrl(baseUrl(), path) + "?" + http::buildQuery(q);
}

std::string Client::imageUrl(const BaseItem& item, ImageType type, const ImageOptions& opts) const {
    const auto own = [&](const char* name) -> std::string {
        const std::string tag = item.imageTag(name);
        return tag.empty() ? std::string() : itemImageUrl(item.id, type, tag, opts);
    };
    switch (type) {
        case ImageType::Primary: {
            if (auto u = own("Primary"); !u.empty()) return u;
            if (!opts.fallback) return {};
            if (!item.parentPrimaryImageItemId.empty() && !item.parentPrimaryImageTag.empty())
                return itemImageUrl(item.parentPrimaryImageItemId, type, item.parentPrimaryImageTag, opts);
            if (!item.seriesId.empty() && !item.seriesPrimaryImageTag.empty())
                return itemImageUrl(item.seriesId, type, item.seriesPrimaryImageTag, opts);
            return {};
        }
        case ImageType::Backdrop: {
            const size_t idx = static_cast<size_t>(std::max(0, opts.index));
            if (idx < item.backdropImageTags.size())
                return itemImageUrl(item.id, type, item.backdropImageTags[idx], opts);
            if (!opts.fallback) return {};
            if (!item.parentBackdropItemId.empty() && !item.parentBackdropImageTags.empty()) {
                ImageOptions o = opts;
                if (idx >= item.parentBackdropImageTags.size()) o.index = 0;
                return itemImageUrl(item.parentBackdropItemId, type,
                                    item.parentBackdropImageTags[static_cast<size_t>(o.index)], o);
            }
            return {};
        }
        case ImageType::Thumb: {
            if (auto u = own("Thumb"); !u.empty()) return u;
            if (!opts.fallback) return {};
            if (!item.parentThumbItemId.empty() && !item.parentThumbImageTag.empty())
                return itemImageUrl(item.parentThumbItemId, type, item.parentThumbImageTag, opts);
            if (!item.seriesId.empty() && !item.seriesThumbImageTag.empty())
                return itemImageUrl(item.seriesId, type, item.seriesThumbImageTag, opts);
            // Landscape fallback: a backdrop works as a thumb.
            ImageOptions o = opts;
            o.index = 0;
            if (!item.backdropImageTags.empty()) return itemImageUrl(item.id, ImageType::Backdrop, item.backdropImageTags[0], o);
            if (!item.parentBackdropItemId.empty() && !item.parentBackdropImageTags.empty())
                return itemImageUrl(item.parentBackdropItemId, ImageType::Backdrop, item.parentBackdropImageTags[0], o);
            return {};
        }
        case ImageType::Logo: {
            if (auto u = own("Logo"); !u.empty()) return u;
            if (!opts.fallback) return {};
            if (!item.parentLogoItemId.empty() && !item.parentLogoImageTag.empty())
                return itemImageUrl(item.parentLogoItemId, type, item.parentLogoImageTag, opts);
            return {};
        }
        case ImageType::Banner: return own("Banner");
        case ImageType::Art: return own("Art");
        case ImageType::Disc: return own("Disc");
    }
    return {};
}

std::string Client::userImageUrl(const std::string& userId, const std::string& tag, const ImageOptions& opts) const {
    if (userId.empty() || tag.empty()) return {};
    QueryParams q;
    if (opts.maxWidth > 0) q.emplace_back("maxWidth", std::to_string(opts.maxWidth));
    if (opts.maxHeight > 0) q.emplace_back("maxHeight", std::to_string(opts.maxHeight));
    if (opts.quality > 0) q.emplace_back("quality", std::to_string(opts.quality));
    q.emplace_back("tag", tag);
    q.emplace_back("format", "Jpg");
    const std::string path = modernApi() ? "/UserImage" : "/Users/" + http::urlEncode(userId) + "/Images/Primary";
    if (modernApi()) q.insert(q.begin(), {"userId", userId});
    return http::joinUrl(baseUrl(), path) + "?" + http::buildQuery(q);
}

std::string Client::personImageUrl(const PersonInfo& person, const ImageOptions& opts) const {
    if (person.id.empty() || person.primaryImageTag.empty()) return {};
    return itemImageUrl(person.id, ImageType::Primary, person.primaryImageTag, opts);
}

std::string Client::searchHintImageUrl(const SearchHint& hint, const ImageOptions& opts) const {
    if (!hint.primaryImageTag.empty()) return itemImageUrl(hint.id, ImageType::Primary, hint.primaryImageTag, opts);
    if (!hint.thumbImageTag.empty())
        return itemImageUrl(hint.thumbImageItemId.empty() ? hint.id : hint.thumbImageItemId, ImageType::Thumb,
                            hint.thumbImageTag, opts);
    if (!hint.backdropImageTag.empty())
        return itemImageUrl(hint.backdropImageItemId.empty() ? hint.id : hint.backdropImageItemId,
                            ImageType::Backdrop, hint.backdropImageTag, opts);
    return {};
}

}  // namespace karafin::jellyfin
