#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include "core/log.hpp"
#include "jellyfin/client.hpp"
#include "jellyfin/detail/client_impl.hpp"
#include "jellyfin/detail/json_get.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;
using namespace detail;

namespace {

std::string itemPath(const Client& c, const std::string& itemId) {
    return c.modernApi() ? "/Items/" + http::urlEncode(itemId) : "/Users/" + c.userId() + "/Items/" + http::urlEncode(itemId);
}

std::string streamKey(const MediaStream& s) {
    if (!s.path.empty()) return s.path;
    return std::to_string(s.index) + "|" + s.language + "|" + s.codec + "|" + s.displayTitle;
}

std::set<std::string> subtitleKeys(const MediaSource& src) {
    std::set<std::string> keys;
    for (const auto& s : src.mediaStreams)
        if (s.isSubtitle()) keys.insert(streamKey(s));
    return keys;
}

std::string unquote(std::string v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
}

std::string fileNameFromDisposition(const std::string& cd) {
    // filename*=UTF-8''name.mkv takes precedence over filename="name.mkv".
    std::string lower = cd;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto valueAt = [&](size_t pos) {
        size_t end = cd.find(';', pos);
        std::string v = cd.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return unquote(v);
    };
    if (auto p = lower.find("filename*="); p != std::string::npos) {
        std::string v = valueAt(p + 10);
        if (auto q = v.find("''"); q != std::string::npos) v = v.substr(q + 2);
        std::string out;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
                out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                out += v[i];
            }
        }
        return out;
    }
    if (auto p = lower.find("filename="); p != std::string::npos) return valueAt(p + 9);
    return {};
}

}  // namespace

// --- Trickplay / segments ----------------------------------------------------------------------

Call<TrickplayMap> Client::trickplay(std::string itemId) {
    if (!modernApi()) return Call<TrickplayMap>::ready(Result<TrickplayMap>::success({}));
    QueryParams q = userParam();
    q.emplace_back("Fields", "Trickplay");
    return jsonCall<TrickplayMap>(request(http::Method::Get, itemPath(*this, itemId), q), ErrorContext::Generic,
                                  [](const Json& j) { return parseTrickplayMap(getObj(j, "Trickplay")); });
}

std::string Client::trickplayTileUrl(const std::string& itemId, const std::string& mediaSourceId, int width,
                                     int sheetIndex) const {
    std::string path = "/Videos/" + http::urlEncode(itemId) + "/Trickplay/" + std::to_string(width) + "/" +
                       std::to_string(sheetIndex) + ".jpg";
    if (!mediaSourceId.empty()) path += "?mediaSourceId=" + http::urlEncode(mediaSourceId);
    return withApiKey(path);
}

Call<std::vector<MediaSegment>> Client::mediaSegments(std::string itemId) {
    auto self = shared_from_this();
    auto req = request(http::Method::Get, "/MediaSegments/" + http::urlEncode(itemId));
    req.timeoutMs = 10000;
    using V = std::vector<MediaSegment>;
    return Call<V>([self, req](const detail::Exec& exec, Callback<V> done) {
        exec.send(req, [self, done](http::Response resp) {
            // Servers before 10.10 have no such route; treat as "no segments".
            if (resp.error == http::Error::None && (resp.status == 404 || resp.status == 405)) {
                self->noteReachable(true);
                return done(Result<V>::success({}));
            }
            done(self->parseResponse<V>(resp, ErrorContext::Generic, [](const Json& j) {
                V out = parseQueryResult<MediaSegment>(j, parseMediaSegment).items;
                std::sort(out.begin(), out.end(),
                          [](const MediaSegment& a, const MediaSegment& b) { return a.startTicks < b.startTicks; });
                return out;
            }));
        });
    });
}

// --- Remote subtitles --------------------------------------------------------------------------

Call<std::vector<RemoteSubtitleInfo>> Client::searchRemoteSubtitles(std::string itemId, std::string language) {
    using V = std::vector<RemoteSubtitleInfo>;
    if (language.empty()) language = "eng";
    auto req = request(http::Method::Get,
                       "/Items/" + http::urlEncode(itemId) + "/RemoteSearch/Subtitles/" + http::urlEncode(language),
                       {{"isPerfectMatch", "false"}});
    req.timeoutMs = 60000;
    return jsonCall<V>(std::move(req), ErrorContext::Subtitles, [language](const Json& j) -> Result<V> {
        V out;
        if (j.is_array())
            for (const auto& e : j)
                if (e.is_object()) out.push_back(parseRemoteSubtitle(e));
        if (out.empty())
            return Result<V>::failure(ApiError::make(
                ErrorKind::NotFound,
                "No subtitles found for this language. If this keeps happening, the server may have no subtitle "
                "provider (such as the OpenSubtitles plugin) configured.",
                0, "empty remote subtitle search for " + language));
        std::stable_sort(out.begin(), out.end(), [](const RemoteSubtitleInfo& a, const RemoteSubtitleInfo& b) {
            if (a.isHashMatch != b.isHashMatch) return a.isHashMatch;
            return a.downloadCount > b.downloadCount;
        });
        return Result<V>::success(std::move(out));
    });
}

Call<Unit> Client::downloadRemoteSubtitle(std::string itemId, std::string subtitleId) {
    if (subtitleId.empty()) return Call<Unit>::fail(ApiError::make(ErrorKind::Invalid, "No subtitle selected."));
    auto req = request(http::Method::Post,
                       "/Items/" + http::urlEncode(itemId) + "/RemoteSearch/Subtitles/" + http::urlEncode(subtitleId));
    req.timeoutMs = 90000;
    return emptyCall(std::move(req), ErrorContext::Subtitles);
}

Call<MediaSource> Client::refreshMediaSource(std::string itemId, std::string mediaSourceId) {
    QueryParams q = userParam();
    q.emplace_back("Fields", "MediaSources,MediaStreams");
    return jsonCall<MediaSource>(request(http::Method::Get, itemPath(*this, itemId), q), ErrorContext::Generic,
                                 [mediaSourceId](const Json& j) -> Result<MediaSource> {
                                     BaseItem item = parseItem(j);
                                     if (const MediaSource* ms = item.mediaSource(mediaSourceId))
                                         return Result<MediaSource>::success(*ms);
                                     return Result<MediaSource>::failure(ApiError::make(
                                         ErrorKind::NotFound, "The media source no longer exists.", 0,
                                         "media source " + mediaSourceId + " missing"));
                                 });
}

Call<SubtitleAddResult> Client::downloadRemoteSubtitleAndRefresh(std::string itemId, std::string mediaSourceId,
                                                                 std::string subtitleId) {
    using R = Result<SubtitleAddResult>;
    auto self = shared_from_this();
    return Call<SubtitleAddResult>([self, itemId, mediaSourceId, subtitleId](const detail::Exec& exec,
                                                                             Callback<SubtitleAddResult> done) {
        self->refreshMediaSource(itemId, mediaSourceId).run(exec, [=](Result<MediaSource> before) {
            if (!before.ok()) return done(R::failure(before.error));
            auto known = std::make_shared<std::set<std::string>>(subtitleKeys(before.value));
            self->downloadRemoteSubtitle(itemId, subtitleId).run(exec, [=](Status dl) {
                if (!dl.ok()) return done(R::failure(dl.error));
                // The server indexes the new file in a queued refresh: poll for it. The loop
                // function holds itself weakly; pending continuations keep it alive.
                using Loop = std::function<void(int)>;
                auto attempt = std::make_shared<Loop>();
                std::weak_ptr<Loop> weakAttempt = attempt;
                *attempt = [=](int left) {
                    std::shared_ptr<Loop> loop = weakAttempt.lock();
                    exec.delay(1.0, [=] {
                        self->refreshMediaSource(itemId, mediaSourceId).run(exec, [=](Result<MediaSource> now) {
                            if (!now.ok()) {
                                if (now.error.kind == ErrorKind::Cancelled || left <= 0)
                                    return done(R::failure(now.error));
                                return (*loop)(left - 1);
                            }
                            SubtitleAddResult out;
                            out.source = now.value;
                            for (const auto& s : now.value.mediaStreams) {
                                if (s.isSubtitle() && !known->count(streamKey(s))) {
                                    out.newStream = s;
                                    out.url = self->subtitleUrl(itemId, now.value.id, s.index, s.subtitleFormat());
                                    break;
                                }
                            }
                            if (out.newStream || left <= 0 || exec.token().cancelled()) {
                                if (!out.newStream)
                                    log::info("jellyfin", "downloaded subtitle not indexed yet for {}", itemId);
                                return done(R::success(std::move(out)));
                            }
                            (*loop)(left - 1);
                        });
                    });
                };
                (*attempt)(7);
            });
        });
    });
}

// --- Downloads data layer ----------------------------------------------------------------------

std::string Client::downloadUrl(const std::string& itemId) const {
    return withApiKey("/Items/" + http::urlEncode(itemId) + "/Download");
}

http::Request Client::downloadRequest(const std::string& itemId) const {
    auto req = request(http::Method::Get, "/Items/" + http::urlEncode(itemId) + "/Download");
    req.headers.emplace_back("Accept", "*/*");
    std::erase_if(req.headers, [](const auto& h) { return h.first == "Accept" && h.second == "application/json"; });
    req.timeoutMs = 0;
    req.connectTimeoutMs = 15000;
    req.lowSpeedLimit = 1;
    req.lowSpeedTimeSec = 60;
    req.acceptCompressed = false;
    return req;
}

Call<DownloadProbe> Client::probeDownload(std::string itemId) {
    using R = Result<DownloadProbe>;
    auto self = shared_from_this();
    return Call<DownloadProbe>([self, itemId](const detail::Exec& exec, Callback<DownloadProbe> done) {
        auto finish = [self, itemId, done](const http::Response& resp) {
            DownloadProbe p;
            p.url = self->downloadUrl(itemId);
            if (const auto* v = resp.header("etag")) p.eTag = *v;
            if (const auto* v = resp.header("last-modified")) p.lastModified = *v;
            if (const auto* v = resp.header("content-type")) p.contentType = *v;
            if (const auto* v = resp.header("content-disposition")) p.fileName = fileNameFromDisposition(*v);
            if (const auto* v = resp.header("accept-ranges")) p.acceptRanges = v->find("bytes") != std::string::npos;
            if (resp.status == 206) p.acceptRanges = true;
            const auto* range = resp.header("content-range");
            if (range && range->find('/') != std::string::npos) {
                const std::string total = range->substr(range->find('/') + 1);
                if (!total.empty() && total != "*") p.contentLength = std::strtoull(total.c_str(), nullptr, 10);
            } else if (auto len = resp.contentLength()) {
                p.contentLength = *len;
            }
            done(R::success(std::move(p)));
        };

        auto head = self->downloadRequest(itemId);
        head.method = http::Method::Head;
        head.timeoutMs = 15000;
        exec.send(std::move(head), [self, itemId, exec, done, finish](http::Response resp) {
            if (resp.cancelled()) return done(R::failure(ApiError::cancelled()));
            const bool headUsable = resp.ok() && resp.contentLength().value_or(0) > 0;
            if (headUsable) {
                self->noteReachable(true);
                return finish(resp);
            }
            if (resp.status == 401 || resp.status == 403 || (resp.error != http::Error::None))
                return done(R::failure(self->check(resp, ErrorContext::Download, true)));
            // HEAD unsupported (405/404) or no length: ask for the first byte instead.
            auto get = self->downloadRequest(itemId);
            get.rangeFrom = 0;
            get.rangeTo = 0;
            get.timeoutMs = 20000;
            get.onData = [](std::string_view) { return false; };
            exec.send(std::move(get), [self, done, finish](http::Response r2) {
                const bool gotHeaders = r2.status >= 200 && r2.status < 300 &&
                                        (r2.error == http::Error::None || r2.error == http::Error::Aborted);
                if (gotHeaders) {
                    self->noteReachable(true);
                    return finish(r2);
                }
                done(R::failure(self->check(r2, ErrorContext::Download, true)));
            });
        });
    });
}

Call<bool> Client::canDownload() {
    return currentUser().map([](UserDto u) { return u.canDownload(); });
}

Call<OfflineMetadata> Client::offlineMetadata(std::string itemId) {
    using R = Result<OfflineMetadata>;
    auto self = shared_from_this();
    return Call<OfflineMetadata>([self, itemId](const detail::Exec& exec, Callback<OfflineMetadata> done) {
        QueryParams q = self->userParam();
        q.emplace_back("Fields", self->withTrickplay(fields::kDetail));
        self->getJson(itemPath(*self, itemId), q).run(exec, [=](Result<Json> item) {
            if (!item.ok()) return done(R::failure(item.error));
            auto meta = std::make_shared<OfflineMetadata>();
            meta->item = std::move(item.value);
            meta->parsed = parseItem(meta->item);
            if (!meta->parsed.isEpisode() || meta->parsed.seriesId.empty())
                return done(R::success(std::move(*meta)));
            QueryParams sq = self->userParam();
            sq.emplace_back("Fields", "Overview,Genres,People,Studios,ProviderIds,PrimaryImageAspectRatio");
            self->getJson(itemPath(*self, meta->parsed.seriesId), sq).run(exec, [=](Result<Json> series) {
                if (series.ok()) meta->series = std::move(series.value);
                else if (series.error.kind == ErrorKind::Cancelled) return done(R::failure(series.error));
                if (meta->parsed.seasonId.empty()) return done(R::success(std::move(*meta)));
                self->getJson(itemPath(*self, meta->parsed.seasonId), sq).run(exec, [=](Result<Json> season) {
                    if (season.ok()) meta->season = std::move(season.value);
                    else if (season.error.kind == ErrorKind::Cancelled) return done(R::failure(season.error));
                    done(R::success(std::move(*meta)));
                });
            });
        });
    });
}

std::vector<SubtitleDownload> Client::externalSubtitleDownloads(const BaseItem& item,
                                                                const std::string& mediaSourceId) const {
    std::vector<SubtitleDownload> out;
    const MediaSource* src = item.mediaSource(mediaSourceId);
    const std::vector<MediaStream>& streams = src ? src->mediaStreams : item.mediaStreams;
    const std::string msId = src && !src->id.empty() ? src->id : item.id;
    for (const auto& s : streams) {
        if (!s.isSubtitle() || !s.isExternal) continue;
        SubtitleDownload d;
        d.index = s.index;
        d.format = s.subtitleFormat();
        d.language = s.language;
        d.title = s.displayTitle;
        d.isDefault = s.isDefault;
        d.isForced = s.isForced;
        d.isHearingImpaired = s.isHearingImpaired;
        d.url = subtitleUrl(item.id, msId, s.index, d.format);
        d.fileName = std::to_string(s.index) + "." + (s.language.empty() ? "und" : s.language) +
                     (s.isForced ? ".forced" : "") + (s.isHearingImpaired ? ".sdh" : "") + "." + d.format;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace karafin::jellyfin
