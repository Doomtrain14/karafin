#include <algorithm>

#include "core/log.hpp"
#include "jellyfin/client.hpp"
#include "jellyfin/detail/client_impl.hpp"
#include "jellyfin/detail/json_get.hpp"
#include "jellyfin/device_profile.hpp"
#include "jellyfin/report_queue.hpp"

namespace karafin::jellyfin {

using Json = nlohmann::json;
using namespace detail;

// --- Enums / value types -----------------------------------------------------------------------

std::string_view playMethodName(PlayMethod m) {
    switch (m) {
        case PlayMethod::DirectPlay: return "DirectPlay";
        case PlayMethod::DirectStream: return "DirectStream";
        case PlayMethod::Transcode: return "Transcode";
    }
    return "DirectPlay";
}

PlayMethod parsePlayMethod(std::string_view s) {
    if (s == "DirectStream") return PlayMethod::DirectStream;
    if (s == "Transcode") return PlayMethod::Transcode;
    return PlayMethod::DirectPlay;
}

int PlaybackPlan::mpvTrackId(int jellyfinStreamIndex) const {
    if (method == PlayMethod::Transcode) return -1;
    const MediaStream* target = source.stream(jellyfinStreamIndex);
    if (!target || target->isExternal) return -1;
    int ordinal = 1;
    for (const auto& s : source.mediaStreams)
        if (s.type == target->type && !s.isExternal && s.index < target->index) ++ordinal;
    return ordinal;
}

const ExternalSubtitle* PlaybackPlan::externalSubtitle(int jellyfinStreamIndex) const {
    for (const auto& s : externalSubtitles)
        if (s.index == jellyfinStreamIndex) return &s;
    return nullptr;
}

PlaybackReport PlaybackReport::fromPlan(const PlaybackPlan& plan, int64_t positionTicks) {
    PlaybackReport r;
    r.itemId = plan.itemId;
    r.mediaSourceId = plan.mediaSourceId;
    r.playSessionId = plan.playSessionId;
    r.liveStreamId = plan.liveStreamId;
    r.positionTicks = positionTicks;
    r.audioStreamIndex = plan.audioStreamIndex;
    r.subtitleStreamIndex = plan.subtitleStreamIndex;
    r.playMethod = plan.method;
    return r;
}

Json PlaybackReport::toJson() const {
    Json j = {{"ItemId", itemId},
              {"MediaSourceId", mediaSourceId},
              {"PositionTicks", positionTicks},
              {"IsPaused", isPaused},
              {"IsMuted", isMuted},
              {"PlayMethod", playMethodName(playMethod)},
              {"VolumeLevel", volumeLevel},
              {"PlaybackRate", playbackRate},
              {"CanSeek", true},
              {"RepeatMode", "RepeatNone"},
              {"SubtitleStreamIndex", subtitleStreamIndex}};
    if (!playSessionId.empty()) j["PlaySessionId"] = playSessionId;
    if (!liveStreamId.empty()) j["LiveStreamId"] = liveStreamId;
    if (audioStreamIndex >= 0) j["AudioStreamIndex"] = audioStreamIndex;
    if (!eventName.empty()) j["EventName"] = eventName;
    if (failed) j["Failed"] = true;
    return j;
}

PlaybackReport PlaybackReport::fromJson(const Json& j) {
    PlaybackReport r;
    r.itemId = getStr(j, "ItemId");
    r.mediaSourceId = getStr(j, "MediaSourceId");
    r.playSessionId = getStr(j, "PlaySessionId");
    r.liveStreamId = getStr(j, "LiveStreamId");
    r.positionTicks = getI64(j, "PositionTicks");
    r.isPaused = getBool(j, "IsPaused");
    r.isMuted = getBool(j, "IsMuted");
    r.audioStreamIndex = getInt(j, "AudioStreamIndex", -1);
    r.subtitleStreamIndex = getInt(j, "SubtitleStreamIndex", -1);
    r.playMethod = parsePlayMethod(getStr(j, "PlayMethod"));
    r.volumeLevel = getInt(j, "VolumeLevel", 100);
    r.playbackRate = getDouble(j, "PlaybackRate", 1.0);
    r.eventName = getStr(j, "EventName");
    r.failed = getBool(j, "Failed");
    return r;
}

// --- URLs --------------------------------------------------------------------------------------

std::string Client::staticStreamUrl(const std::string& itemId, const MediaSource& source,
                                    const std::string& playSessionId) const {
    QueryParams q = {{"static", "true"}, {"mediaSourceId", source.id.empty() ? itemId : source.id},
                     {"deviceId", deviceId_}};
    if (!source.eTag.empty()) q.emplace_back("Tag", source.eTag);
    if (!playSessionId.empty()) q.emplace_back("PlaySessionId", playSessionId);
    if (!source.liveStreamId.empty()) q.emplace_back("LiveStreamId", source.liveStreamId);
    const std::string token = accessToken();
    if (!token.empty()) q.emplace_back("ApiKey", token);
    return http::joinUrl(baseUrl(), "/Videos/" + http::urlEncode(itemId) + "/stream") + "?" + http::buildQuery(q);
}

std::string Client::subtitleUrl(const std::string& itemId, const std::string& mediaSourceId, int streamIndex,
                                const std::string& format) const {
    const std::string path = "/Videos/" + http::urlEncode(itemId) + "/" +
                             http::urlEncode(mediaSourceId.empty() ? itemId : mediaSourceId) + "/Subtitles/" +
                             std::to_string(streamIndex) + "/Stream." + (format.empty() ? "srt" : format);
    return withApiKey(path);
}

// --- PlaybackInfo ------------------------------------------------------------------------------

Result<PlaybackPlan> Client::resolvePlaybackInfo(const Json& info, const PlaybackRequest& req) const {
    using R = Result<PlaybackPlan>;
    const std::string errorCode = getStr(info, "ErrorCode");
    if (errorCode == "NotAllowed")
        return R::failure(ApiError::make(ErrorKind::Forbidden, "Your account isn't allowed to play this item.", 0,
                                         "PlaybackInfo ErrorCode NotAllowed"));
    if (errorCode == "NoCompatibleStream")
        return R::failure(ApiError::make(ErrorKind::Server,
                                         "The server found no stream the Switch can play (transcoding may be "
                                         "disabled for your account).",
                                         0, "PlaybackInfo ErrorCode NoCompatibleStream"));
    if (errorCode == "RateLimitExceeded")
        return R::failure(ApiError::make(ErrorKind::Server, "Too many active streams. Stop another stream and retry.",
                                         0, "PlaybackInfo ErrorCode RateLimitExceeded"));
    if (!errorCode.empty())
        return R::failure(ApiError::make(ErrorKind::Server, "The server can't play this item.", 0,
                                         "PlaybackInfo ErrorCode " + errorCode));

    std::vector<MediaSource> sources;
    for (const auto& s : getArr(info, "MediaSources"))
        if (s.is_object()) sources.push_back(parseMediaSource(s));
    if (sources.empty())
        return R::failure(ApiError::make(ErrorKind::NotFound, "No playable media was found for this item.", 0,
                                         "PlaybackInfo without MediaSources"));

    const MediaSource* chosen = &sources.front();
    if (!req.mediaSourceId.empty()) {
        for (const auto& s : sources)
            if (s.id == req.mediaSourceId) chosen = &s;
    }

    PlaybackPlan plan;
    plan.itemId = req.itemId;
    plan.source = *chosen;
    plan.mediaSourceId = chosen->id.empty() ? req.itemId : chosen->id;
    plan.playSessionId = getStr(info, "PlaySessionId");
    plan.liveStreamId = chosen->liveStreamId;
    plan.startTimeTicks = req.startTimeTicks;
    plan.audioStreamIndex = req.audioStreamIndex.value_or(chosen->defaultAudioStreamIndex);
    plan.subtitleStreamIndex = req.subtitleStreamIndex.value_or(chosen->defaultSubtitleStreamIndex);

    if (chosen->supportsDirectPlay && req.enableDirectPlay) {
        plan.method = PlayMethod::DirectPlay;
        plan.url = staticStreamUrl(req.itemId, *chosen, plan.playSessionId);
    } else if (chosen->supportsDirectStream && req.enableDirectStream) {
        plan.method = PlayMethod::DirectStream;
        plan.url = staticStreamUrl(req.itemId, *chosen, plan.playSessionId);
    } else if (chosen->supportsTranscoding && !chosen->transcodingUrl.empty() && req.enableTranscoding) {
        plan.method = PlayMethod::Transcode;
        plan.url = withApiKey(chosen->transcodingUrl);
    } else {
        return R::failure(ApiError::make(ErrorKind::Server,
                                         "This video can't be played directly and the server offered no transcode.",
                                         0, "no usable play method"));
    }

    for (const auto& s : chosen->mediaStreams) {
        if (!s.isSubtitle()) continue;
        ExternalSubtitle ext;
        if (s.deliveryMethod == SubtitleDeliveryMethod::External && !s.deliveryUrl.empty()) {
            ext.url = withApiKey(s.deliveryUrl);
        } else if (s.isExternal && s.deliveryMethod != SubtitleDeliveryMethod::Encode &&
                   s.deliveryMethod != SubtitleDeliveryMethod::Drop) {
            ext.url = subtitleUrl(req.itemId, plan.mediaSourceId, s.index, s.subtitleFormat());
        } else {
            continue;
        }
        ext.index = s.index;
        ext.format = s.subtitleFormat();
        ext.language = s.language;
        ext.title = s.displayTitle;
        ext.isDefault = s.isDefault;
        ext.isForced = s.isForced;
        ext.isHearingImpaired = s.isHearingImpaired;
        plan.externalSubtitles.push_back(std::move(ext));
    }
    return R::success(std::move(plan));
}

Call<PlaybackPlan> Client::playbackPlan(PlaybackRequest req) {
    if (req.itemId.empty()) return Call<PlaybackPlan>::fail(ApiError::make(ErrorKind::Invalid, "No item to play."));
    DeviceProfileOptions opts = DeviceProfileOptions::fromSettings();
    if (req.maxStreamingBitrate) opts.maxStreamingBitrate = std::clamp<int64_t>(*req.maxStreamingBitrate, 500'000, kMaxProfileBitrate);
    opts.maxStaticBitrate = std::max(opts.maxStaticBitrate, opts.maxStreamingBitrate);

    // PlaybackInfoDto.UserId is a Guid?: an empty string fails model binding (HTTP 400).
    const std::string uid = userId();
    Json body = {{"MaxStreamingBitrate", opts.maxStreamingBitrate},
                 {"StartTimeTicks", req.startTimeTicks},
                 {"DeviceProfile", buildDeviceProfile(opts)},
                 {"EnableDirectPlay", req.enableDirectPlay},
                 {"EnableDirectStream", req.enableDirectStream},
                 {"EnableTranscoding", req.enableTranscoding},
                 {"AllowVideoStreamCopy", true},
                 {"AllowAudioStreamCopy", true},
                 {"AutoOpenLiveStream", req.autoOpenLiveStream},
                 {"IsPlayback", true},
                 {"AlwaysBurnInSubtitleWhenTranscoding", false}};
    if (!uid.empty()) body["UserId"] = uid;
    if (!req.mediaSourceId.empty()) body["MediaSourceId"] = req.mediaSourceId;
    if (req.audioStreamIndex) body["AudioStreamIndex"] = *req.audioStreamIndex;
    if (req.subtitleStreamIndex) body["SubtitleStreamIndex"] = *req.subtitleStreamIndex;

    auto httpReq =
        request(http::Method::Post, "/Items/" + http::urlEncode(req.itemId) + "/PlaybackInfo", userParam());
    httpReq.contentType = "application/json";
    httpReq.body = dumpSafe(body);
    auto self = shared_from_this();
    return jsonCall<PlaybackPlan>(std::move(httpReq), ErrorContext::Playback, [self, req](const Json& j) {
        auto plan = self->resolvePlaybackInfo(j, req);
        if (plan.ok())
            log::info("jellyfin", "playback plan for {}: {} source={} subs={}", req.itemId,
                      playMethodName(plan->method), plan->mediaSourceId, plan->externalSubtitles.size());
        return plan;
    });
}

// --- Reporting ---------------------------------------------------------------------------------

Call<Unit> Client::postReport(const char* path, PlaybackReport report, bool queueOnFailure) {
    auto req = request(http::Method::Post, path);
    req.contentType = "application/json";
    req.body = dumpSafe(report.toJson());
    req.timeoutMs = 10000;
    auto inner = emptyCall(std::move(req), ErrorContext::Generic);
    if (!queueOnFailure) return inner;
    auto self = shared_from_this();
    return Call<Unit>([self, inner, report](const detail::Exec& exec, Callback<Unit> done) {
        inner.run(exec, [self, report, done](Status s) {
            if (!s.ok() && s.error.isConnectivity()) {
                log::info("jellyfin", "queueing stopped report for {} (server unreachable)", report.itemId);
                ReportQueue::instance().enqueueStopped(self->serverId(), self->userId(), report);
            }
            done(std::move(s));
        });
    });
}

Call<Unit> Client::reportPlaybackStart(PlaybackReport report) {
    return postReport("/Sessions/Playing", std::move(report), false);
}

Call<Unit> Client::reportPlaybackProgress(PlaybackReport report) {
    if (report.eventName.empty()) report.eventName = "timeupdate";
    return postReport("/Sessions/Playing/Progress", std::move(report), false);
}

Call<Unit> Client::reportPlaybackStopped(PlaybackReport report, bool queueIfOffline) {
    return postReport("/Sessions/Playing/Stopped", std::move(report), queueIfOffline);
}

}  // namespace karafin::jellyfin
