#include "player/player_screen.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <format>
#include <limits>
#include <mutex>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/paths.hpp"
#include "core/settings.hpp"
#include "core/thread_pool.hpp"
#include "jellyfin/report_queue.hpp"
#include "player/languages.hpp"
#include "player/sub_fonts.hpp"
#include "ui/root.hpp"
#include "ui/sound.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::player {

using jellyfin::BaseItem;
using jellyfin::PlaybackPlan;
using jellyfin::Result;
using jellyfin::secondsToTicks;
using jellyfin::ticksToSeconds;

namespace {

enum PropId : uint64_t {
    kTimePos = 1,
    kDuration,
    kPause,
    kPausedForCache,
    kSeeking,
    kEofReached,
    kCacheTime,
    kBuffering,
    kTrackList,
    kChapterList,
    kHwdec,
    kSpeed,
    kVolume,
};

constexpr double kReportInterval = 10.0;
constexpr double kOfflineSaveInterval = 10.0;
constexpr int kMaxRecoveryAttempts = 3;
constexpr double kRecoveryNetworkWait = 30.0;  // then reload anyway
constexpr double kStablePlayReset = 90.0;      // playing this long forgives earlier recoveries
constexpr double kSeekConfirmTimeout = 15.0;   // a seek without PLAYBACK_RESTART by then is treated as failed
constexpr int64_t kFocusResumeSeconds = 30;    // auto-resume after a short HOME menu visit

std::string redact(const std::string& url) { return url.substr(0, url.find('?')); }

std::vector<PlayerScreen*>& liveScreens() {
    static std::vector<PlayerScreen*> screens;
    return screens;
}

// Stopped reports sent asynchronously whose answer hasn't been delivered (main thread only).
// At quit they are sent again, blocking: the http client is about to drop them.
struct StoppedFlight {
    jellyfin::ClientPtr client;
    jellyfin::PlaybackReport report;
    bool done = false;
};
std::vector<std::shared_ptr<StoppedFlight>>& stoppedFlights() {
    static std::vector<std::shared_ptr<StoppedFlight>> flights;
    return flights;
}

// Sends the final Stopped report and waits for the answer until `deadline` (quit path: the
// main loop no longer runs, so the continuation is taken on a worker thread). Returns success.
bool sendStoppedBlocking(const jellyfin::ClientPtr& client, const jellyfin::PlaybackReport& report,
                         std::chrono::steady_clock::time_point deadline) {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool ok = false;
    };
    auto st = std::make_shared<State>();
    CancelToken token;
    client->reportPlaybackStopped(report, false)
        .run(jellyfin::detail::Exec(false, token), [st](jellyfin::Status s) {
            std::lock_guard lock(st->mutex);
            st->done = true;
            st->ok = s.ok();
            if (!s.ok()) log::info("player", "final stopped report failed: {}", s.error.describe());
            st->cv.notify_all();
        });
    std::unique_lock lock(st->mutex);
    st->cv.wait_until(lock, deadline, [&] { return st->done; });
    if (!st->done) {
        token.cancel();
        log::info("player", "final stopped report timed out");
    }
    return st->ok;
}

// Sends the report before `deadline`, or persists it in the ReportQueue (sent on next launch).
void deliverStoppedAtQuit(const jellyfin::ClientPtr& client, const jellyfin::PlaybackReport& report,
                          std::chrono::steady_clock::time_point deadline) {
    if (sendStoppedBlocking(client, report, deadline)) {
        log::info("player", "quit: stopped {} at {:.0f}s", report.itemId, ticksToSeconds(report.positionTicks));
        return;
    }
    jellyfin::ReportQueue::instance().enqueueStopped(client->serverId(), client->userId(), report);
    log::info("player", "quit: queued stopped report for {}", report.itemId);
}

// mpv id of an embedded track before mpv's track-list is known: position among the embedded
// tracks of that type in container order.
int embeddedOrdinal(const std::vector<Track>& tracks, int jellyfinIndex) {
    if (jellyfinIndex < 0) return -1;
    int ordinal = 1;
    bool found = false;
    for (const auto& t : tracks) {
        if (t.jellyfinIndex < 0 || !t.url.empty() || t.external) continue;
        if (t.jellyfinIndex == jellyfinIndex) found = true;
        else if (t.jellyfinIndex < jellyfinIndex) ++ordinal;
    }
    return found ? ordinal : -1;
}

}  // namespace

PlayerScreen::PlayerScreen(PlayRequest request) : request_(std::move(request)) {
    setName("player");
    setOpaque(true);
    setBackgroundColor(ui::Color::black());
    setShowsHintBar(false);
    setNavigationSounds(false);
    ui::sound::suspend();  // releases audren before mpv's ao=hos opens it
    auto& s = Settings::instance();
    subScale_ = std::clamp(s.get<double>("player.sub_scale", 1.0), 0.3, 3.0);
    subPos_ = std::clamp(s.get<int>("player.sub_pos", 100), 0, 150);
    const bool chinese = !platform::isAppletMode();
    workers().post([chinese] { subfonts::prepare(chinese); });
    hits_.reserve(16);
    liveScreens().push_back(this);
    showOsd(4.0);
    startItem(request_);
}

PlayerScreen::~PlayerScreen() {
    std::erase(liveScreens(), this);
    token_.cancel();
    if (sessionOpen_) finishSession(false);
    if (throttled_) offline::setStreamingThrottle(false);
    awake_.reset();
    boost_.reset();
    mpv_.reset();
    ui::sound::resume([] { return Mpv::waitForTeardown(0); });
}

void PlayerScreen::shutdownAll(int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
    const std::vector<PlayerScreen*> screens = liveScreens();
    for (PlayerScreen* s : screens) s->shutdownNow(deadline);
    // Sessions that ended just before quitting (e.g. exited the player, then quit right away).
    const auto flights = std::move(stoppedFlights());
    stoppedFlights().clear();
    for (const auto& f : flights)
        if (!f->done && f->client) deliverStoppedAtQuit(f->client, f->report, deadline);
    if (!screens.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (!Mpv::waitForTeardown(static_cast<int>(std::max<int64_t>(100, left.count()))))
            log::warn("player", "mpv is still shutting down");
    }
}

void PlayerScreen::shutdownNow(std::chrono::steady_clock::time_point deadline) {
    if (shutDown_) return;
    shutDown_ = exiting_ = true;
    brightness_.restore();
    token_.cancel();
    pendingLoad_.reset();
    recovering_ = reloading_ = false;
    if (mpv_) {
        if (auto t = mpv_->getDouble("time-pos"); t && started_ && !pendingSeek_) position_ = std::max(0.0, *t);
        mpv_->command({"stop"});
    }

    std::optional<jellyfin::PlaybackReport> report;
    if (sessionOpen_) {
        sessionOpen_ = false;
        const bool finished = phase_ == Phase::Ended && duration_ > 0;
        const double pos = finished ? duration_ : finalPosition();
        if (offline_) {
            offline::saveProgress(offline_->key, secondsToTicks(pos), finished, true);
            log::info("player", "quit: saved offline progress {:.0f}s", pos);
        } else if (client_ && reportedStart_) {
            report = makeReport();
            report->positionTicks = secondsToTicks(pos);
            report->eventName.clear();
            report->failed = phase_ == Phase::Error;
        }
        reportedStart_ = false;
    }

    // Render context freed now (GL context current); the core terminates on its reaper thread.
    mpv_.reset();
    awake_.reset();
    boost_.reset();
    if (throttled_) {
        offline::setStreamingThrottle(false);
        throttled_ = false;
    }

    if (report) deliverStoppedAtQuit(client_, *report, deadline);
}

// --- Lifecycle ------------------------------------------------------------------------------

void PlayerScreen::onShow() {
    // Free textures of the screens below; mpv needs the memory more.
    gfx::ImageCache::instance().trimMemory();
}

void PlayerScreen::onHide() {
    brightness_.restore();
    if (touchDragging_) cancelScrub();
    touchActive_ = touchOnBar_ = touchDragging_ = false;
    swipe_.cancel();
    if (!exiting_ && mpv_ && fileLoaded_ && !paused_) setPaused(true);
}

void PlayerScreen::onPopped() {
    brightness_.restore();
    if (!exiting_) exiting_ = true;
    recovering_ = reloading_ = false;
    dismissSheets();
    finishSession(duration_ > 0 && (phase_ == Phase::Ended || finalPosition() >= duration_ - 1.0));
    if (mpv_) mpv_->setFlag("pause", true);
    token_.cancel();
    if (throttled_) {
        offline::setStreamingThrottle(false);
        throttled_ = false;
    }
    awake_.reset();
    boost_.reset();
}

void PlayerScreen::exitPlayer() {
    if (exiting_) return;
    exiting_ = true;
    if (ui::Root::exists() && ui::nav().top() == this && ui::nav().depth() > 1) {
        ui::nav().pop(ui::Transition::Fade);
    } else {
        onPopped();
    }
}

// --- Session --------------------------------------------------------------------------------

void PlayerScreen::resetForNewItem() {
    token_.cancel();
    token_.reset();
    plan_.reset();
    offline_.reset();
    haveItem_ = false;
    triedTranscode_ = false;
    tracks_.clear();
    initialChoice_ = {};
    position_ = duration_ = cacheEnd_ = 0;
    fileLoaded_ = started_ = reportedStart_ = sessionOpen_ = false;
    pausedForCache_ = seeking_ = false;
    bufferingPct_ = 0;
    chapters_.clear();
    chaptersFromMpv_ = false;
    pendingSeek_.reset();
    pendingLoad_.reset();
    segments_.clear();
    activeSegment_ = nullptr;
    invalidateSegmentWindow();
    upNextTriggerFor_ = -1;
    lastSegmentId_.clear();
    handledSegments_.clear();
    skipAppear_.set(0);
    next_.reset();
    upNextVisible_ = upNextDismissed_ = false;
    upNextTimer_.cancel();
    upNextAppear_.set(0);
    nextImage_.reset();
    trickplay_.reset();
    trickplaySheets_.clear();
    knownSubtitleKeys_.clear();
    subtitleCheckPending_ = false;
    pendingCommands_.clear();
    scrubbing_ = false;
    scrubKeyHeld_ = false;
    scrubIdle_ = 0;
    touchActive_ = touchOnBar_ = touchDragging_ = false;
    swipe_.cancel();
    adjustmentTimer_ = 0;
    backdrop_.reset();
    logo_.reset();
    osdTags_.clear();
    tagsTimer_ = 0;
    videoReveal_.set(0);
    subDelay_ = 0;
    recovering_ = reloading_ = false;
    recoveryAttempts_ = 0;
    stablePlay_ = 0;
    errorTitle_.clear();
    errorMessage_.clear();
    title_.clear();
    subtitleLine_.clear();
    startTicks_ = 0;
}

void PlayerScreen::startItem(PlayRequest request) {
    if (shutDown_) return;
    request_ = std::move(request);
    resetForNewItem();
    phase_ = Phase::Loading;
    auto& settings = Settings::instance();
    skipMode_ = settings.get<std::string>("player.skip_segments", "ask");
    autoplayNext_ = settings.get<bool>("player.autoplay_next", true);
    seekStep_ = std::clamp(settings.get<int>("player.seek_step", 10), 3, 120);
    skipSeconds_ = std::clamp(settings.get<int>("player.skip_seconds", 10), 5, 300);
    const auto skipInput = settings.get<std::string>("player.skip_input", "tap");
    buttonSkip_ = skipInput != "tap";
    doubleTapSeek_ = skipInput != "buttons";
    showOsd(4.0);
    log::info("player", "play {} (offline={})", request_.itemId, request_.offline);

    if (request_.offline) {
        auto media = offline::find(request_.itemId);
        if (!media) {
            fail("Not available offline", "This item hasn't finished downloading, or its file is missing.", false);
            return;
        }
        client_ = jellyfin::currentClient();
        item_ = media->item;
        haveItem_ = true;
        loadLogo();
        buildTitles();
        loadChaptersFromItem();
        openOffline(std::move(*media));
        findNextEpisode();
        return;
    }

    client_ = jellyfin::currentClient();
    if (!client_ || !client_->isAuthenticated()) {
        fail("Not signed in", "Sign in to a server to play this item.", false);
        return;
    }
    client_->item(request_.itemId)
        .then(
            [this](Result<BaseItem> r) {
                if (!r) {
                    fail("Can't play this item", r.error.message, r.error.kind != jellyfin::ErrorKind::Forbidden);
                    return;
                }
                onItem(r.value);
            },
            token_);
}

void PlayerScreen::onItem(const BaseItem& item) {
    item_ = item;
    haveItem_ = true;
    loadLogo();
    buildTitles();
    loadChaptersFromItem();
    if (duration_ <= 0) duration_ = item_.runTimeSeconds();

    const std::string backdrop = client_->imageUrl(item_, jellyfin::ImageType::Backdrop, {.maxWidth = 1280, .quality = 80});
    if (!backdrop.empty()) {
        gfx::ImageRequest req;
        req.url = backdrop;
        req.width = static_cast<int>(640 * ui::displayScale());
        req.height = static_cast<int>(360 * ui::displayScale());
        req.blur = 2.0f;
        backdrop_ = gfx::ImageCache::instance().load(req);
    }

    fetchExtras();
    findNextEpisode();
    const int64_t start = request_.startPositionTicks.value_or(item_.userData.playbackPositionTicks);
    requestPlan(std::max<int64_t>(0, start), request_.audioStreamIndex, request_.subtitleStreamIndex, true, false);
}

void PlayerScreen::requestPlan(int64_t startTicks, std::optional<int> audio, std::optional<int> subtitle,
                               bool allowDirect, bool explicitTracks) {
    if (!client_) return;
    phase_ = Phase::Loading;
    startTicks_ = startTicks;
    jellyfin::PlaybackRequest pr;
    pr.itemId = request_.itemId;
    pr.mediaSourceId = plan_ ? plan_->mediaSourceId : request_.mediaSourceId;
    pr.startTimeTicks = startTicks;
    pr.audioStreamIndex = audio;
    pr.subtitleStreamIndex = subtitle;
    pr.enableDirectPlay = allowDirect;
    pr.enableDirectStream = allowDirect;
    client_->playbackPlan(pr).then(
        [this, explicitTracks](Result<PlaybackPlan> r) {
            if (!r) {
                if (reloading_ && r.error.isConnectivity() && beginRecovery(r.error.describe())) return;
                fail("Can't play this item", r.error.message, r.error.kind != jellyfin::ErrorKind::Forbidden);
                return;
            }
            openPlan(std::move(r.value), explicitTracks);
        },
        token_);
}

TrackPrefs PlayerScreen::effectivePrefs() const {
    TrackPrefs p = TrackPrefs::fromSettings();
    if (sessionAudioLang_ && !sessionAudioLang_->empty()) p.audioLang = *sessionAudioLang_;
    if (sessionSubLang_) {
        if (sessionSubLang_->empty()) {
            p.subMode = "off";
        } else {
            p.subLang = *sessionSubLang_;
            p.subMode = "always";
        }
    }
    return p;
}

void PlayerScreen::openPlan(PlaybackPlan plan, bool explicitTracks) {
    tracks_.setFromPlan(plan);
    TrackManager::Choice c;
    if (explicitTracks) {
        c.audio = plan.audioStreamIndex;
        c.subtitle = plan.subtitleStreamIndex;
    } else {
        c = tracks_.autoSelect(effectivePrefs(), request_.audioStreamIndex, request_.subtitleStreamIndex,
                               plan.audioStreamIndex);
    }
    if (plan.isTranscoding() && !explicitTracks &&
        (c.audio != plan.audioStreamIndex || c.subtitle != plan.subtitleStreamIndex)) {
        // A transcode is built for specific tracks: ask again with the chosen ones.
        log::info("player", "re-requesting transcode with audio={} subtitle={}", c.audio, c.subtitle);
        plan_ = std::move(plan);
        requestPlan(startTicks_, c.audio >= 0 ? std::optional<int>(c.audio) : std::nullopt, c.subtitle,
                    !triedTranscode_, true);
        return;
    }

    plan_ = std::move(plan);
    initialChoice_ = c;
    knownSubtitleKeys_.clear();
    for (const auto& s : plan_->source.mediaStreams)
        if (s.isSubtitle()) knownSubtitleKeys_.insert(subtitleKey(s));
    if (duration_ <= 0) duration_ = ticksToSeconds(plan_->source.runTimeTicks);

    // Trickplay: the resolution closest to 320 px wide (Jellyfin's default) for this source.
    trickplay_.reset();
    trickplaySheets_.clear();
    auto src = item_.trickplay.find(plan_->mediaSourceId);
    if (src == item_.trickplay.end() && !item_.trickplay.empty()) src = item_.trickplay.begin();
    if (src != item_.trickplay.end()) {
        const jellyfin::TrickplayInfo* best = nullptr;
        for (const auto& [width, info] : src->second) {
            if (!info.valid()) continue;
            if (!best || std::abs(width - 320) < std::abs(best->width - 320)) best = &info;
        }
        if (best) {
            trickplay_ = *best;
            trickplaySource_ = src->first;
        }
    }

    if (!throttled_) {
        offline::setStreamingThrottle(true);
        throttled_ = true;
    }
    log::info("player", "{} {} audio={} subtitle={}", jellyfin::playMethodName(plan_->method), redact(plan_->url),
              c.audio, c.subtitle);
    loadFile(plan_->url, ticksToSeconds(startTicks_), c);
}

void PlayerScreen::openOffline(offline::Media media) {
    offline_ = std::move(media);
    tracks_.setFromOffline(*offline_);
    const jellyfin::MediaSource* src = offline_->item.mediaSource("");
    const int defaultAudio = src ? src->defaultAudioStreamIndex : -1;
    initialChoice_ =
        tracks_.autoSelect(effectivePrefs(), request_.audioStreamIndex, request_.subtitleStreamIndex, defaultAudio);
    startTicks_ = std::max<int64_t>(0, request_.startPositionTicks.value_or(offline_->positionTicks));
    if (duration_ <= 0) duration_ = item_.runTimeSeconds();
    if (throttled_) {
        offline::setStreamingThrottle(false);
        throttled_ = false;
    }
    log::info("player", "offline {} audio={} subtitle={}", offline_->mediaPath, initialChoice_.audio,
              initialChoice_.subtitle);
    loadFile(offline_->mediaPath, ticksToSeconds(startTicks_), initialChoice_);
}

bool PlayerScreen::ensureMpv() {
    if (mpv_) return true;
    if (shutDown_) return false;
    auto& s = Settings::instance();
    MpvOptions o;
    o.hwdec = s.get<bool>("player.hwdec", true);
    o.appletMode = platform::isAppletMode();
    o.userAgent = std::format("Karafin/{} (Nintendo Switch)", KARAFIN_VERSION);
    if (client_) o.httpHeaders.push_back("Authorization: " + client_->authorizationHeader());
    const std::string userCa = paths::dataFile("cacert.pem");
    o.caFile = fs::exists(userCa) ? userCa : paths::romfs("certs/cacert.pem");
    o.verifyTls = s.get<bool>("http.verify_tls", true);
    const subfonts::FontSet fonts = subfonts::forLanguage("");
    o.subFontsDir = fonts.dir;
    o.subFont = fonts.family;
    fontDir_ = fonts.dir;
    o.subScale = subScale_;
    o.subPos = subPos_;
    o.subBackground = s.get<bool>("player.sub_bg", false);

    std::string error;
    mpv_ = Mpv::create(o, &error);
    if (!mpv_) {
        fail("Can't start the player", "The video engine failed to start (" + error + ").", false);
        return false;
    }
    mpv_->onEvent = [this](const mpv_event& ev) { handleEvent(ev); };
    mpv_->observe(kTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_->observe(kDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_->observe(kPause, "pause", MPV_FORMAT_FLAG);
    mpv_->observe(kPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_->observe(kSeeking, "seeking", MPV_FORMAT_FLAG);
    mpv_->observe(kEofReached, "eof-reached", MPV_FORMAT_FLAG);
    mpv_->observe(kCacheTime, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_->observe(kBuffering, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_->observe(kTrackList, "track-list", MPV_FORMAT_NODE);
    mpv_->observe(kChapterList, "chapter-list", MPV_FORMAT_NODE);
    mpv_->observe(kHwdec, "hwdec-current", MPV_FORMAT_STRING);
    mpv_->observe(kSpeed, "speed", MPV_FORMAT_DOUBLE);
    mpv_->observe(kVolume, "volume", MPV_FORMAT_DOUBLE);
    return true;
}

void PlayerScreen::loadFile(const std::string& url, double startSeconds, const TrackManager::Choice& choice) {
    if (!subfonts::ready()) {
        pendingLoad_ = PendingLoad{url, startSeconds, choice};
        phase_ = Phase::Loading;
        return;
    }
    if (!ensureMpv()) return;
    phase_ = Phase::Opening;
    fileLoaded_ = false;
    started_ = false;
    mpv_->resetFrame();
    videoReveal_.set(0);

    std::string subLang;
    if (const Track* t = tracks_.findSubtitleByJellyfin(choice.subtitle)) subLang = t->language;
    updateSubtitleFont(subLang);

    std::string opts = std::format("start={:.3f}", std::max(0.0, startSeconds));
    const bool direct = offline_.has_value() || (plan_ && !plan_->isTranscoding());
    if (direct) {
        auto mpvId = [this](const std::vector<Track>& list, int index) {
            return plan_ ? plan_->mpvTrackId(index) : embeddedOrdinal(list, index);
        };
        const int aid = choice.audio >= 0 ? mpvId(tracks_.audio(), choice.audio) : -1;
        if (aid > 0) opts += ",aid=" + std::to_string(aid);
        const Track* st = tracks_.findSubtitleByJellyfin(choice.subtitle);
        const int sid = st && st->url.empty() ? mpvId(tracks_.subtitles(), choice.subtitle) : -1;
        opts += sid > 0 ? ",sid=" + std::to_string(sid) : ",sid=no";
    } else {
        opts += ",sid=no";
    }
    mpv_->setFlag("pause", false);
    mpv_->setDouble("speed", speed_);
    applySubtitleStyle();
    log::info("player", "loadfile {} [{}]", redact(url), opts);
    const int rc = mpv_->command({"loadfile", url.c_str(), "replace", opts.c_str()});
    if (rc < 0) fail("Can't play this item", std::string("The player rejected the stream (") + mpv_error_string(rc) + ").", true);
    updatePlatformLocks();
}

void PlayerScreen::maybeLoadPending() {
    if (!pendingLoad_ || !subfonts::ready()) return;
    PendingLoad p = std::move(*pendingLoad_);
    pendingLoad_.reset();
    loadFile(p.url, p.start, p.choice);
}

void PlayerScreen::buildTitles() {
    if (item_.isEpisode()) {
        title_ = item_.seriesName.empty() ? item_.name : item_.seriesName;
        std::string tag;
        if (item_.parentIndexNumber >= 0 && item_.indexNumber >= 0)
            tag = std::format("S{:02}E{:02}", item_.parentIndexNumber, item_.indexNumber);
        else if (item_.indexNumber >= 0)
            tag = std::format("E{:02}", item_.indexNumber);
        if (item_.indexNumberEnd > item_.indexNumber) tag += std::format("-E{:02}", item_.indexNumberEnd);
        subtitleLine_ = tag.empty() ? item_.name : (item_.name.empty() ? tag : tag + "  ·  " + item_.name);
    } else {
        title_ = item_.name;
        subtitleLine_ = item_.productionYear > 0 ? std::to_string(item_.productionYear) : "";
    }
}

void PlayerScreen::loadChaptersFromItem() {
    chapters_.clear();
    chaptersFromMpv_ = false;
    for (const auto& c : item_.chapters) chapters_.push_back({ticksToSeconds(c.startPositionTicks), c.name});
    std::sort(chapters_.begin(), chapters_.end(), [](const Chapter& a, const Chapter& b) { return a.time < b.time; });
}

void PlayerScreen::fetchExtras() {
    if (!client_ || offline_) return;
    client_->mediaSegments(item_.id)
        .then(
            [this](Result<std::vector<jellyfin::MediaSegment>> r) {
                if (!r) return;
                activeSegment_ = nullptr;
                segments_ = std::move(r.value);
                invalidateSegmentWindow();
                upNextTriggerFor_ = -1;
                log::debug("player", "{} media segments", segments_.size());
            },
            token_);
}

void PlayerScreen::findNextEpisode() {
    auto setNext = [this](const BaseItem& item) {
        NextUp n;
        n.item = item;
        if (client_) n.imageUrl = client_->imageUrl(item, jellyfin::ImageType::Primary, {.maxWidth = 400});
        next_ = std::move(n);
        log::info("player", "up next: {} {}", item.episodeTag(), item.name);
    };

    if (offline_) {
        if (auto m = offline::next(offline_->key)) {
            NextUp n;
            n.item = m->item;
            n.offline = std::move(*m);
            next_ = std::move(n);
        }
        return;
    }
    if (!client_ || !item_.isEpisode() || item_.seriesId.empty()) return;

    const std::string currentId = item_.id;
    const std::string seriesId = item_.seriesId;
    const std::string seasonId = item_.seasonId;
    client_->episodes(seriesId, seasonId)
        .then(
            [this, setNext, currentId, seriesId, seasonId](Result<jellyfin::QueryResult<BaseItem>> r) {
                if (!r) return;
                const auto& eps = r->items;
                auto it = std::find_if(eps.begin(), eps.end(), [&](const BaseItem& e) { return e.id == currentId; });
                if (it != eps.end()) {
                    for (++it; it != eps.end(); ++it)
                        if (!it->isVirtual()) return setNext(*it);
                }
                if (seasonId.empty()) return;
                // Last episode of the season: first episode of the following season.
                client_->seasons(seriesId).then(
                    [this, setNext, seriesId, seasonId](Result<jellyfin::QueryResult<BaseItem>> s) {
                        if (!s) return;
                        const auto& seasons = s->items;
                        auto cur = std::find_if(seasons.begin(), seasons.end(),
                                                [&](const BaseItem& x) { return x.id == seasonId; });
                        if (cur == seasons.end() || cur->indexNumber == 0) return;
                        const BaseItem* following = nullptr;
                        for (auto n = cur + 1; n != seasons.end(); ++n)
                            if (n->indexNumber != 0) {
                                following = &*n;
                                break;
                            }
                        if (!following) return;
                        client_->episodes(seriesId, following->id)
                            .then(
                                [setNext](Result<jellyfin::QueryResult<BaseItem>> e) {
                                    if (!e) return;
                                    for (const auto& ep : e->items)
                                        if (!ep.isVirtual()) return setNext(ep);
                                },
                                token_);
                    },
                    token_);
            },
            token_);
}

void PlayerScreen::fail(const std::string& title, const std::string& message, bool retryable) {
    log::error("player", "{}: {}", title, message);
    if (shutDown_) return;
    const double resumeAt = finalPosition();
    phase_ = Phase::Error;
    errorTitle_ = title;
    errorMessage_ = message;
    pendingLoad_.reset();
    recovering_ = reloading_ = false;
    if (mpv_) mpv_->setFlag("pause", true);
    finishSession(false);
    updatePlatformLocks();
    hideOsd();
    dismissSheets();

    ui::DialogSpec spec;
    spec.title = title;
    spec.message = message;
    spec.icon = gfx::Icon::Warning;
    spec.iconColor = ui::theme::color::danger;
    if (retryable) {
        spec.buttons.push_back({"Retry", ui::ButtonStyle::Primary, guard([this, resumeAt] {
                                    PlayRequest req = request_;
                                    if (resumeAt > 0) req.startPositionTicks = secondsToTicks(resumeAt);
                                    startItem(req);
                                })});
    }
    spec.buttons.push_back({"Close", retryable ? ui::ButtonStyle::Secondary : ui::ButtonStyle::Primary,
                            guard([this] { exitPlayer(); })});
    spec.cancelIndex = static_cast<int>(spec.buttons.size()) - 1;
    ui::present(std::make_unique<ui::Dialog>(std::move(spec)));
}

void PlayerScreen::finishSession(bool finished) {
    if (!sessionOpen_) return;
    sessionOpen_ = false;
    const double pos = finished && duration_ > 0 ? duration_ : finalPosition();
    if (offline_) {
        offline::saveProgress(offline_->key, secondsToTicks(pos), finished, true);
    } else if (client_ && reportedStart_) {
        jellyfin::PlaybackReport rep = makeReport();
        rep.positionTicks = secondsToTicks(pos);
        rep.eventName.clear();
        rep.failed = phase_ == Phase::Error;
        auto flight = std::make_shared<StoppedFlight>(StoppedFlight{client_, rep, false});
        std::erase_if(stoppedFlights(), [](const auto& f) { return f->done; });
        stoppedFlights().push_back(flight);
        client_->reportPlaybackStopped(rep).then([flight](jellyfin::Status) { flight->done = true; });
        log::info("player", "stopped {} at {:.0f}s", rep.itemId, pos);
    }
    reportedStart_ = false;
}

void PlayerScreen::retryWithTranscode(const std::string& reason) {
    log::warn("player", "direct play failed ({}); retrying with a transcode", reason);
    triedTranscode_ = true;
    ui::toast("Direct play failed, switching to transcoding", ui::ToastKind::Warning, gfx::Icon::Refresh, 3.5f);
    const double pos = finalPosition();
    finishSession(false);
    fileLoaded_ = started_ = false;
    const int audio = initialChoice_.audio;
    const int sub = initialChoice_.subtitle;
    requestPlan(secondsToTicks(pos), audio >= 0 ? std::optional<int>(audio) : std::nullopt, sub, false, true);
}

void PlayerScreen::restartStream(int audioIndex, int subtitleIndex) {
    if (!plan_ || !client_) return;
    const double pos = finalPosition();
    log::info("player", "restarting transcode at {:.1f}s (audio={} subtitle={})", pos, audioIndex, subtitleIndex);
    finishSession(false);
    fileLoaded_ = started_ = false;
    initialChoice_ = {audioIndex, subtitleIndex};
    requestPlan(secondsToTicks(pos), audioIndex >= 0 ? std::optional<int>(audioIndex) : std::nullopt, subtitleIndex,
                !triedTranscode_, true);
}

bool PlayerScreen::beginRecovery(const std::string& reason) {
    if (offline_ || !plan_ || !client_ || exiting_ || shutDown_ || phase_ == Phase::Error) return false;
    if (recoveryAttempts_ >= kMaxRecoveryAttempts) return false;
    ++recoveryAttempts_;
    if (started_) {
        // Tracks as they are now (a failed reload has no track-list to read them from).
        recoveryPos_ = position_;
        initialChoice_ = {currentAudioIndex(), currentSubtitleIndex()};
    } else if (!reloading_) {
        recoveryPos_ = ticksToSeconds(startTicks_);
    }
    log::warn("player", "stream interrupted at {:.1f}s ({}); recovery attempt {}/{}", recoveryPos_, reason,
              recoveryAttempts_, kMaxRecoveryAttempts);
    recovering_ = true;
    reloading_ = false;
    recoveryWait_ = 0;
    recoveryPoll_ = 1.0;  // give a waking console a moment before the first check
    stablePlay_ = 0;
    cancelScrub();
    pendingSeek_.reset();
    fileLoaded_ = started_ = false;
    startTicks_ = secondsToTicks(recoveryPos_);
    upNextVisible_ = false;
    phase_ = Phase::Loading;
    if (mpv_) mpv_->setFlag("pause", true);  // keeps the last frame up while we wait
    showOsd(3.0);
    updatePlatformLocks();
    return true;
}

void PlayerScreen::updateRecovery(float dt) {
    if (!recovering_) return;
    recoveryWait_ += dt;
    recoveryPoll_ -= dt;
    if (recoveryPoll_ > 0) return;
    recoveryPoll_ = 0.5;
    if (platform::networkConnected() || recoveryWait_ >= kRecoveryNetworkWait) reloadAfterRecovery();
}

void PlayerScreen::reloadAfterRecovery() {
    recovering_ = false;
    if (!plan_ || !client_) return;
    log::info("player", "reconnecting at {:.1f}s after {:.1f}s", recoveryPos_, recoveryWait_);
    const TrackManager::Choice choice = initialChoice_;
    reloading_ = true;
    if (plan_->isTranscoding()) {
        // The server's transcode job is probably gone: ask for a new one (new play session).
        finishSession(false);
        requestPlan(secondsToTicks(recoveryPos_), choice.audio >= 0 ? std::optional<int>(choice.audio) : std::nullopt,
                    choice.subtitle, false, true);
        return;
    }
    // Same static URL and play session; the session stays open (no new Start report).
    loadFile(plan_->url, recoveryPos_, choice);
}

// --- mpv events -----------------------------------------------------------------------------

void PlayerScreen::handleEvent(const mpv_event& ev) {
    switch (ev.event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            onPropertyChange(ev.reply_userdata, *static_cast<const mpv_event_property*>(ev.data));
            break;
        case MPV_EVENT_FILE_LOADED: onFileLoaded(); break;
        case MPV_EVENT_PLAYBACK_RESTART: onPlaybackRestart(); break;
        case MPV_EVENT_END_FILE: onEndFile(*static_cast<const mpv_event_end_file*>(ev.data)); break;
        case MPV_EVENT_COMMAND_REPLY: {
            auto it = pendingCommands_.find(ev.reply_userdata);
            if (it == pendingCommands_.end()) break;
            auto fn = std::move(it->second);
            pendingCommands_.erase(it);
            if (fn) fn(ev.error);
            break;
        }
        default: break;
    }
}

void PlayerScreen::onPropertyChange(uint64_t id, const mpv_event_property& prop) {
    const bool isDouble = prop.format == MPV_FORMAT_DOUBLE && prop.data;
    const bool isFlag = prop.format == MPV_FORMAT_FLAG && prop.data;
    const double d = isDouble ? *static_cast<const double*>(prop.data) : 0.0;
    const bool f = isFlag && *static_cast<const int*>(prop.data) != 0;
    switch (id) {
        case kTimePos:
            if (isDouble) position_ = std::max(0.0, d);
            break;
        case kDuration:
            if (isDouble && d > 0) duration_ = d;
            break;
        case kPause:
            if (f != paused_) {
                paused_ = f;
                if (started_ && phase_ == Phase::Playing) {
                    reportProgress(f ? "Pause" : "Unpause");
                    if (f) saveOfflineProgress(false);
                }
            }
            break;
        case kPausedForCache: pausedForCache_ = f; break;
        case kSeeking: seeking_ = f; break;
        case kEofReached:
            if (f) onReachedEnd();
            break;
        case kCacheTime: cacheEnd_ = isDouble ? d : 0.0; break;
        case kBuffering:
            bufferingPct_ = prop.format == MPV_FORMAT_INT64 && prop.data ? *static_cast<const int64_t*>(prop.data) : 0;
            break;
        case kTrackList:
            if (prop.format == MPV_FORMAT_NODE && prop.data) tracks_.mergeMpv(*static_cast<const mpv_node*>(prop.data));
            infoStaticDirty_ = true;
            break;
        case kChapterList:
            if (prop.format == MPV_FORMAT_NODE && prop.data && (chapters_.empty() || chaptersFromMpv_)) {
                const auto* node = static_cast<const mpv_node*>(prop.data);
                chapters_.clear();
                chaptersFromMpv_ = true;
                if (node->format == MPV_FORMAT_NODE_ARRAY && node->u.list) {
                    for (int i = 0; i < node->u.list->num; ++i) {
                        const mpv_node& c = node->u.list->values[i];
                        chapters_.push_back({nodeDouble(nodeMapGet(c, "time")), nodeString(nodeMapGet(c, "title"))});
                    }
                }
            }
            break;
        case kHwdec: {
            std::string current = prop.format == MPV_FORMAT_STRING && prop.data && *static_cast<char* const*>(prop.data)
                                      ? *static_cast<char* const*>(prop.data)
                                      : "";
            const bool wasHardware = !hwdec_.empty() && hwdec_ != "no";
            if (fileLoaded_ && wasHardware && (current.empty() || current == "no"))
                log::warn("player", "hardware decoding stopped ({}); now decoding in software", hwdec_);
            hwdec_ = std::move(current);
            infoStaticDirty_ = true;
            break;
        }
        case kSpeed:
            if (isDouble) speed_ = d;
            break;
        case kVolume:
            if (isDouble) volume_ = d;
            break;
        default: break;
    }
}

void PlayerScreen::onFileLoaded() {
    fileLoaded_ = true;
    phase_ = Phase::Playing;
    infoStaticDirty_ = true;
    if (auto d = mpv_->getDouble("duration"); d && *d > 0) duration_ = *d;
    refreshTrackList();
    applyInitialTracks();
    sessionOpen_ = true;
    reportTimer_ = 0;
    offlineSaveTimer_ = 0;
    if (!offline_ && plan_ && client_ && !reportedStart_) {
        client_->reportPlaybackStart(makeReport()).then({});
        reportedStart_ = true;
    }
    log::info("player", "file loaded: duration {:.0f}s, hwdec '{}'", duration_, mpv_->getString("hwdec-current"));
}

void PlayerScreen::onPlaybackRestart() {
    pendingSeek_.reset();
    seeking_ = false;
    if (!started_) {
        started_ = true;
        if (osdTimer_ < 2.5) showOsd(2.5);
        if (reloading_) {
            reloading_ = false;
            ui::toast("Reconnected", ui::ToastKind::Success, gfx::Icon::Wifi, 2.0f);
            reportProgress("TimeUpdate");
        }
    } else {
        reportProgress("TimeUpdate");
    }
    if (phase_ == Phase::Ended && duration_ > 0 && position_ < duration_ - 1.0) phase_ = Phase::Playing;
}

void PlayerScreen::onEndFile(const mpv_event_end_file& ef) {
    if (ef.reason == MPV_END_FILE_REASON_ERROR) {
        const std::string msg = mpv_error_string(ef.error);
        if (phase_ == Phase::Error || exiting_ || recovering_) return;
        // Failing after playback ran (or while reconnecting) is the network, not the format.
        if (started_ || reloading_) {
            if (beginRecovery(msg)) return;
        } else if (!offline_ && plan_ && !plan_->isTranscoding() && !triedTranscode_ && client_) {
            retryWithTranscode(msg);
            return;
        }
        fail("Playback failed",
             offline_ ? "The downloaded file couldn't be played (" + msg + ")."
                      : "The video couldn't be played (" + msg + "). Check your connection and try again.",
             true);
    } else if (ef.reason == MPV_END_FILE_REASON_EOF) {
        onReachedEnd();
    }
}

void PlayerScreen::onReachedEnd() {
    if (phase_ != Phase::Playing || !fileLoaded_ || exiting_) return;
    // A dropped connection ends the stream early (mpv sees EOF once the cache runs dry).
    const double expected = duration_ > 0 ? duration_ : item_.runTimeSeconds();
    if (!offline_ && expected > 60 && position_ < expected - 20 && position_ < expected * 0.97 &&
        beginRecovery("stream ended early"))
        return;
    phase_ = Phase::Ended;
    cancelScrub();
    if (next_ && !upNextDismissed_) {
        if (!upNextVisible_) upNextTimer_.start(autoplayNext_);
        upNextVisible_ = true;
        showOsd(1.0);
        return;
    }
    finishSession(true);
    exitPlayer();
}

// --- Reporting ------------------------------------------------------------------------------

jellyfin::PlaybackReport PlayerScreen::makeReport() const {
    jellyfin::PlaybackReport r;
    if (plan_) r = jellyfin::PlaybackReport::fromPlan(*plan_);
    else r.itemId = request_.itemId;
    r.positionTicks = secondsToTicks(finalPosition());
    r.isPaused = paused_;
    r.audioStreamIndex = currentAudioIndex();
    r.subtitleStreamIndex = currentSubtitleIndex();
    r.playbackRate = speed_;
    return r;
}

void PlayerScreen::reportProgress(const char* eventName) {
    reportTimer_ = 0;
    if (offline_ || !client_ || !reportedStart_ || !sessionOpen_) return;
    jellyfin::PlaybackReport r = makeReport();
    r.eventName = eventName;
    client_->reportPlaybackProgress(r).then({});
}

void PlayerScreen::saveOfflineProgress(bool final) {
    if (!offline_ || !started_) return;
    offline::saveProgress(offline_->key, secondsToTicks(finalPosition()), false, final);
}

int PlayerScreen::currentAudioIndex() const {
    if (const Track* t = tracks_.selectedAudio(); t && t->jellyfinIndex >= 0) return t->jellyfinIndex;
    if (plan_ && plan_->isTranscoding()) return plan_->audioStreamIndex;
    return initialChoice_.audio;
}

int PlayerScreen::currentSubtitleIndex() const {
    if (plan_ && plan_->isTranscoding() && plan_->subtitleStreamIndex >= 0) {
        for (const auto& t : tracks_.subtitles())
            if (t.jellyfinIndex == plan_->subtitleStreamIndex && t.needsRestart) return t.jellyfinIndex;
    }
    if (const Track* t = tracks_.selectedSubtitle()) return t->jellyfinIndex;
    return -1;
}

// --- Playback control -----------------------------------------------------------------------

void PlayerScreen::setPaused(bool paused) {
    if (!mpv_ || !fileLoaded_) return;
    if (!paused && phase_ == Phase::Ended) return;
    mpv_->setFlag("pause", paused);
}

void PlayerScreen::togglePause() {
    if (!mpv_ || !fileLoaded_ || phase_ != Phase::Playing) return;
    const bool pause = !paused_;
    pausedByFocus_ = false;
    setPaused(pause);
    flashIcon_ = pause ? gfx::Icon::Pause : gfx::Icon::Play;
    flash_.set(1.0f);
    flash_.tweenTo(0.0f, 0.6f, ui::ease::outCubic);
    showOsd(pause ? 4.0 : 2.0);
}

double PlayerScreen::displayPosition() const {
    if (scrubbing_) return scrubTarget_;
    if (pendingSeek_) return *pendingSeek_;
    return position_;
}

double PlayerScreen::finalPosition() const {
    if (pendingSeek_) return *pendingSeek_;
    if (!started_) return ticksToSeconds(startTicks_);
    return position_;
}

void PlayerScreen::seekAbsolute(double seconds) {
    if (!mpv_ || !fileLoaded_) return;
    const double maxPos = duration_ > 1 ? duration_ - 0.5 : seconds;
    seconds = std::clamp(seconds, 0.0, std::max(0.0, maxPos));
    const std::string target = std::format("{:.3f}", seconds);
    if (const int rc = mpv_->command({"seek", target.c_str(), "absolute"}); rc < 0) {
        log::warn("player", "seek to {} failed: {}", target, mpv_error_string(rc));
        return;
    }
    pendingSeek_ = seconds;
    pendingSeekAge_ = 0;
    if (phase_ == Phase::Ended && seconds < duration_ - 1.0) {
        phase_ = Phase::Playing;
        mpv_->setFlag("pause", false);
    }
    updatePlatformLocks();
}

void PlayerScreen::seekRelative(double delta) {
    seekAbsolute(displayPosition() + delta);
    showOsd(2.5);
}

void PlayerScreen::chapterJump(int direction) {
    if (chapters_.size() < 2) {
        if (buttonSkip_) seekRelative(direction * skipSeconds_);
        return;
    }
    const double pos = displayPosition();
    if (direction > 0) {
        for (const auto& c : chapters_)
            if (c.time > pos + 1.0) {
                seekAbsolute(c.time);
                showOsd(2.5);
                return;
            }
        if (buttonSkip_) seekRelative(skipSeconds_);
    } else {
        double target = 0;
        for (const auto& c : chapters_)
            if (c.time < pos - 3.0) target = c.time;
        seekAbsolute(target);
        showOsd(2.5);
    }
}

void PlayerScreen::beginScrub() {
    if (scrubbing_) return;
    scrubbing_ = true;
    scrubTarget_ = displayPosition();
    scrubGrow_.springTo(1.0f, ui::theme::motion::snappy);
}

void PlayerScreen::scrubBy(double delta) {
    if (!fileLoaded_ || duration_ <= 0) return;
    beginScrub();
    scrubTarget_ = std::clamp(scrubTarget_ + delta, 0.0, std::max(0.0, duration_ - 1.0));
    scrubIdle_ = 0;
    showOsd(3.0);
}

void PlayerScreen::commitScrub() {
    if (!scrubbing_) return;
    scrubbing_ = false;
    scrubGrow_.springTo(0.0f, ui::theme::motion::snappy);
    seekAbsolute(scrubTarget_);
    showOsd(2.5);
}

void PlayerScreen::cancelScrub() {
    if (!scrubbing_) return;
    scrubbing_ = false;
    scrubGrow_.springTo(0.0f, ui::theme::motion::snappy);
}

// --- Tracks ---------------------------------------------------------------------------------

uint64_t PlayerScreen::asyncCommand(const std::vector<std::string>& args, std::function<void(int)> done) {
    if (!mpv_) return 0;
    const uint64_t id = nextCommandId_++;
    pendingCommands_[id] = std::move(done);
    const int rc = mpv_->commandAsync(id, args);
    if (rc < 0) {
        auto fn = std::move(pendingCommands_[id]);
        pendingCommands_.erase(id);
        if (fn) fn(rc);
    }
    return id;
}

void PlayerScreen::refreshTrackList() {
    if (!mpv_ || !mpv_->handle()) return;
    mpv_node node;
    if (mpv_get_property(mpv_->handle(), "track-list", MPV_FORMAT_NODE, &node) < 0) return;
    tracks_.mergeMpv(node);
    mpv_free_node_contents(&node);
}

void PlayerScreen::applyInitialTracks() {
    if (offline_) {
        // Local files are cheap to add: all of them, so they show up in the menu with mpv ids.
        for (const auto& t : std::vector<Track>(tracks_.subtitles()))
            if (!t.url.empty()) addExternalSubtitle(t, t.jellyfinIndex == initialChoice_.subtitle);
        return;
    }
    if (const Track* t = tracks_.findSubtitleByJellyfin(initialChoice_.subtitle); t && !t->url.empty())
        addExternalSubtitle(*t, true);
}

void PlayerScreen::addExternalSubtitle(const Track& track, bool select) {
    const std::string title = track.title.empty() ? lang::displayName(track.language) : track.title;
    if (select) updateSubtitleFont(track.language);
    asyncCommand({"sub-add", track.url, select ? "select" : "auto", title, track.language},
                 [this, select, title](int err) {
                     if (err < 0) {
                         log::warn("player", "sub-add '{}' failed: {}", title, mpv_error_string(err));
                         if (select) ui::toast("Couldn't load that subtitle", ui::ToastKind::Error, gfx::Icon::Subtitles);
                         return;
                     }
                     refreshTrackList();
                     if (select) reportProgress("SubtitleTrackChange");
                 });
}

void PlayerScreen::selectSubtitle(Track* track) {
    if (!mpv_ || !fileLoaded_) return;
    sessionSubLang_ = track ? track->language : std::string();
    const int target = track ? track->jellyfinIndex : -1;
    bool burnedIn = false;
    if (plan_ && plan_->isTranscoding() && plan_->subtitleStreamIndex >= 0)
        for (const auto& t : tracks_.subtitles())
            if (t.jellyfinIndex == plan_->subtitleStreamIndex && t.needsRestart) burnedIn = true;

    if ((track && track->needsRestart) || (burnedIn && target != plan_->subtitleStreamIndex)) {
        restartStream(currentAudioIndex(), target);
        return;
    }
    if (!track) {
        mpv_->setString("sid", "no");
        refreshTrackList();
        reportProgress("SubtitleTrackChange");
        return;
    }
    updateSubtitleFont(track->language);
    if (track->mpvId > 0) {
        mpv_->setInt("sid", track->mpvId);
        refreshTrackList();
        reportProgress("SubtitleTrackChange");
    } else if (!track->url.empty()) {
        addExternalSubtitle(*track, true);
    }
}

void PlayerScreen::selectAudio(Track* track) {
    if (!track || !mpv_ || !fileLoaded_) return;
    sessionAudioLang_ = track->language;
    if (track->needsRestart) {
        restartStream(track->jellyfinIndex, currentSubtitleIndex());
        return;
    }
    if (track->mpvId > 0) {
        mpv_->setInt("aid", track->mpvId);
        refreshTrackList();
        reportProgress("AudioTrackChange");
    }
}

void PlayerScreen::updateSubtitleFont(const std::string& language) {
    if (!mpv_) return;
    const subfonts::FontSet fonts = subfonts::forLanguage(language);
    if (fonts.dir.empty() || fonts.dir == fontDir_) return;
    mpv_->setString("sub-fonts-dir", fonts.dir);
    mpv_->setString("sub-font", fonts.family);
    fontDir_ = fonts.dir;
}

void PlayerScreen::applySubtitleStyle() {
    if (!mpv_) return;
    mpv_->setDouble("sub-scale", subScale_);
    mpv_->setInt("sub-pos", subPos_);
    mpv_->setDouble("sub-delay", subDelay_);
}

// --- Segments / up next ---------------------------------------------------------------------

namespace {
const char* segmentLabel(jellyfin::MediaSegment::Type t) {
    using T = jellyfin::MediaSegment::Type;
    switch (t) {
        case T::Intro: return "Skip Intro";
        case T::Outro: return "Skip Credits";
        case T::Recap: return "Skip Recap";
        case T::Preview: return "Skip Preview";
        case T::Commercial: return "Skip Ad";
        default: return "Skip";
    }
}
}  // namespace

void PlayerScreen::findActiveSegment(bool eligible, bool upNextOwnsCredits) {
    activeSegment_ = nullptr;
    constexpr double inf = std::numeric_limits<double>::infinity();
    segmentCheckFrom_ = -inf;
    segmentCheckUntil_ = inf;
    if (!eligible) return;
    const int64_t t = secondsToTicks(position_);
    for (const auto& s : segments_) {
        if (s.type == jellyfin::MediaSegment::Type::Unknown) continue;
        // Narrow the window to the nearest boundaries around the position.
        for (const double b : {s.startSeconds(), s.endSeconds() - 1.0, s.endSeconds()}) {
            if (b <= position_) segmentCheckFrom_ = std::max(segmentCheckFrom_, b);
            else segmentCheckUntil_ = std::min(segmentCheckUntil_, b);
        }
        if (activeSegment_ || !s.contains(t)) continue;
        if (s.endSeconds() - position_ < 1.0) continue;
        // Credits with a next episode: the up-next card takes over.
        if (s.type == jellyfin::MediaSegment::Type::Outro && upNextOwnsCredits) continue;
        activeSegment_ = &s;
    }
}

void PlayerScreen::updateSegments(float dt) {
    const std::string& mode = skipMode_;
    const bool eligible = started_ && phase_ == Phase::Playing && !segments_.empty() && mode != "off" && !scrubbing_;
    const bool upNextOwnsCredits = next_.has_value() && !upNextDismissed_;
    const unsigned key = (eligible ? 1u : 0u) | (upNextOwnsCredits ? 2u : 0u);
    if (key != segmentStateKey_ || position_ < segmentCheckFrom_ || position_ >= segmentCheckUntil_) {
        segmentStateKey_ = key;
        findActiveSegment(eligible, upNextOwnsCredits);
    }
    if (activeSegment_) {
        if (activeSegment_->id != lastSegmentId_) {
            lastSegmentId_ = activeSegment_->id;
            segmentShownFor_ = 0;
            skipLabel_ = segmentLabel(activeSegment_->type);
        }
        if (mode == "auto" && activeSegment_->type != jellyfin::MediaSegment::Type::Outro &&
            !handledSegments_.count(activeSegment_->id)) {
            handledSegments_.insert(activeSegment_->id);
            const std::string label = segmentLabel(activeSegment_->type);
            ui::toast(std::string("Skipped ") + (label.size() > 5 ? label.substr(5) : "segment"), ui::ToastKind::Info,
                      gfx::Icon::SkipNext, 2.2f);
            seekAbsolute(activeSegment_->endSeconds() + 0.05);
            activeSegment_ = nullptr;
            invalidateSegmentWindow();
        } else {
            segmentShownFor_ += dt;
        }
    } else {
        lastSegmentId_.clear();
    }
    const bool show = activeSegment_ && (segmentShownFor_ < 8.0 || osdVisible());
    if (skipAppear_.target() != (show ? 1.0f : 0.0f)) skipAppear_.springTo(show ? 1.0f : 0.0f, ui::theme::motion::standard);
}

void PlayerScreen::skipActiveSegment() {
    if (!activeSegment_) return;
    handledSegments_.insert(activeSegment_->id);
    const double end = activeSegment_->endSeconds();
    activeSegment_ = nullptr;
    invalidateSegmentWindow();
    skipAppear_.springTo(0.0f, ui::theme::motion::snappy);
    if (end >= duration_ - 1.0 && next_) {
        playNext(true);
        return;
    }
    seekAbsolute(end + 0.05);
}

void PlayerScreen::updateUpNext(float dt) {
    if (!next_ || !started_) {
        upNextVisible_ = false;
    } else if (phase_ == Phase::Playing) {
        if (upNextTriggerFor_ != duration_) {
            upNextTriggerFor_ = duration_;
            upNextTrigger_ = std::max(duration_ * 0.9, duration_ - 30.0);
            for (const auto& s : segments_)
                if (s.type == jellyfin::MediaSegment::Type::Outro && s.startSeconds() > duration_ * 0.5)
                    upNextTrigger_ = std::min(upNextTrigger_, s.startSeconds());
        }
        const double trigger = upNextTrigger_;
        const bool inZone = duration_ > 0 && displayPosition() >= trigger && !scrubbing_;
        if (inZone && !upNextVisible_ && !upNextDismissed_) {
            upNextVisible_ = true;
            upNextTimer_.start(autoplayNext_);
        } else if (!inZone && !scrubbing_) {
            upNextVisible_ = false;
            upNextDismissed_ = false;
            upNextTimer_.cancel();
        }
    }
    const bool advancing = upNextVisible_ && autoplayNext_ && !modalOpen_ && isTop() && platform::hasFocus() &&
                           !scrubbing_ && !recovering_ && !reloading_ &&
                           (phase_ == Phase::Ended || (phase_ == Phase::Playing && !paused_ && !pausedForCache_ &&
                                                       !seeking_ && !pendingSeek_));
    if (upNextTimer_.tick(dt, advancing)) {
        playNext(true);
        return;
    }
    if (upNextVisible_ && !nextImage_ && next_ && !next_->imageUrl.empty()) {
        gfx::ImageRequest req;
        req.url = next_->imageUrl;
        req.width = static_cast<int>(176 * ui::displayScale());
        req.height = static_cast<int>(99 * ui::displayScale());
        nextImage_ = gfx::ImageCache::instance().load(req);
    }
    const float target = upNextVisible_ ? 1.0f : 0.0f;
    if (upNextAppear_.target() != target) upNextAppear_.springTo(target, ui::theme::motion::standard);
}

void PlayerScreen::dismissUpNext() {
    upNextVisible_ = false;
    upNextDismissed_ = true;
    upNextTimer_.cancel();
    if (phase_ == Phase::Ended) {
        finishSession(true);
        exitPlayer();
    }
}

void PlayerScreen::playNext(bool finishedCurrent) {
    if (!next_ || exiting_) return;
    NextUp n = std::move(*next_);
    next_.reset();
    log::info("player", "playing next: {}", n.item.name);
    dismissSheets();
    finishSession(finishedCurrent || phase_ == Phase::Ended);
    if (mpv_) mpv_->command({"stop"});
    PlayRequest req;
    req.itemId = n.item.id;
    req.offline = n.offline.has_value();
    startItem(req);
}

// --- Per-frame ------------------------------------------------------------------------------

void PlayerScreen::updatePlatformLocks() {
    const bool active = !exiting_ && phase_ != Phase::Error;
    const bool loading = active && (phase_ == Phase::Loading || phase_ == Phase::Opening || pausedForCache_ ||
                                    seeking_ || pendingSeek_.has_value());
    const bool playing = active && fileLoaded_ && !paused_;
    const bool keepAwake = playing || loading;
    if (keepAwake && !awake_) awake_ = std::make_unique<platform::AwakeLock>();
    else if (!keepAwake && awake_) awake_.reset();
    // Boost (CPU up, GPU down on this platform) only while starting / seeking / buffering, or
    // for software decoding where the CPU is the bottleneck.
    const bool software = fileLoaded_ && (hwdec_.empty() || hwdec_ == "no");
    const bool boost = (loading && !recovering_) || (playing && software);
    if (boost && !boost_) boost_ = std::make_unique<platform::CpuBoost>();
    else if (!boost && boost_) boost_.reset();
}

void PlayerScreen::update(float dt) {
    ui::Screen::update(dt);
    osd_.update(dt);
    flash_.update(dt);
    scrubGrow_.update(dt);
    skipAppear_.update(dt);
    upNextAppear_.update(dt);
    videoReveal_.update(dt);
    spinnerPhase_ += dt;
    adjustmentTimer_ = std::max(0.0, adjustmentTimer_ - dt);
    if (platform::isDocked()) brightness_.restore();

    maybeLoadPending();
    updateRecovery(dt);

    const bool focus = platform::hasFocus();
    if (focus != hadFocus_) onFocusChange(focus);
    hadFocus_ = focus;

    const bool modal = ui::Root::exists() && ui::root().topModal() != nullptr;
    if (modal != modalOpen_) {
        modalOpen_ = modal;
        if (!modal && phase_ != Phase::Error) showOsd(3.0);
    }
    if (modalOpen_ || !focus || !isTop()) {
        if (touchDragging_) cancelScrub();
        touchActive_ = touchOnBar_ = touchDragging_ = false;
        swipe_.cancel();
    }

    osdTimer_ -= dt;
    const bool wantOsd = !modalOpen_ && phase_ != Phase::Error &&
                         (osdTimer_ > 0 || scrubbing_ || touchDragging_ || (paused_ && fileLoaded_) ||
                          phase_ == Phase::Loading || phase_ == Phase::Opening || phase_ == Phase::Ended);
    const float osdTarget = wantOsd ? 1.0f : 0.0f;
    if (osd_.target() != osdTarget) osd_.springTo(osdTarget, ui::theme::motion::standard);

    if (scrubbing_ && !touchDragging_) {
        scrubIdle_ += dt;
        if (!scrubKeyHeld_ && scrubIdle_ > 0.65) commitScrub();
    }

    const bool busy = phase_ == Phase::Loading || phase_ == Phase::Opening ||
                      (fileLoaded_ && (pausedForCache_ || (pendingSeek_ && seeking_)));
    busyFor_ = busy ? busyFor_ + dt : 0.0;
    if (pendingSeek_ && (pendingSeekAge_ += dt) > kSeekConfirmTimeout) {
        log::warn("player", "seek to {:.1f}s was never confirmed; using the player position", *pendingSeek_);
        pendingSeek_.reset();
        seeking_ = false;
        if (mpv_)
            if (auto t = mpv_->getDouble("time-pos")) position_ = std::max(0.0, *t);
    }
    updateClock();

    if (mpv_ && mpv_->hasFrame() && started_ && videoReveal_.target() < 1.0f)
        videoReveal_.springTo(1.0f, ui::theme::motion::gentle);

    if (sessionOpen_ && started_ && !paused_ && phase_ == Phase::Playing) {
        stablePlay_ += dt;
        if (recoveryAttempts_ > 0 && stablePlay_ >= kStablePlayReset) recoveryAttempts_ = 0;
        reportTimer_ += dt;
        if (reportTimer_ >= kReportInterval) reportProgress("TimeUpdate");
        if (offline_) {
            offlineSaveTimer_ += dt;
            if (offlineSaveTimer_ >= kOfflineSaveInterval) {
                offlineSaveTimer_ = 0;
                saveOfflineProgress(false);
            }
        }
    }

    updateSegments(dt);
    updateUpNext(dt);

    if (showInfo_) {
        infoTimer_ -= dt;
        infoStaticAge_ += dt;
        if (infoTimer_ <= 0) {
            refreshInfo();
            infoTimer_ = 1.0;
        }
    }
    tapSeekAppear_.update(dt);
    // Codec properties change on track switches and transcode fallbacks; poll only while visible.
    if (osd_.value() > 0.004f) {
        tagsTimer_ -= dt;
        if (tagsTimer_ <= 0) {
            refreshTags();
            tagsTimer_ = 1.0;
        }
    } else {
        tagsTimer_ = 0;
    }
    updatePlatformLocks();
}

void PlayerScreen::onFocusChange(bool focused) {
    if (!focused) {
        brightness_.restore();
        swipe_.cancel();
        touchActive_ = touchOnBar_ = touchDragging_ = false;
        if (scrubbing_) cancelScrub();
        if (fileLoaded_ && !paused_ && phase_ == Phase::Playing) {
            log::info("player", "focus lost: pausing");
            setPaused(true);
            pausedByFocus_ = true;
            focusLostAt_ = static_cast<int64_t>(std::time(nullptr));
        }
        return;
    }
    // Back from a short HOME menu visit: carry on. After sleep or a long absence stay paused
    // (the user may not be watching, and the network needs a moment).
    const int64_t away = static_cast<int64_t>(std::time(nullptr)) - focusLostAt_;
    if (pausedByFocus_ && paused_ && phase_ == Phase::Playing && !modalOpen_ && away >= 0 && away <= kFocusResumeSeconds) {
        log::info("player", "focus regained after {}s: resuming", away);
        setPaused(false);
    }
    pausedByFocus_ = false;
    showOsd(4.0);
}

// --- Input ----------------------------------------------------------------------------------

bool PlayerScreen::onInput(const ui::InputEvent& e) {
    using ui::Key;
    if (e.action == ui::InputAction::Release) {
        if (e.button == Key::Left || e.button == Key::Right) {
            scrubKeyHeld_ = false;
            scrubIdle_ = 0;
        }
        return true;
    }
    if (phase_ == Phase::Error) {
        // The error dialog normally has the input; this is the fallback once it is gone.
        if (e.button == Key::Plus) return false;
        if (e.button == Key::B && e.pressed()) exitPlayer();
        return true;
    }

    switch (e.button) {
        case Key::A:
            if (!e.pressed()) return true;
            if (scrubbing_) commitScrub();
            else if (activeSegment_ && skipAppear_.target() > 0.5f) skipActiveSegment();
            else if (upNextVisible_) playNext(true);
            else if (phase_ == Phase::Ended) seekAbsolute(0);
            else togglePause();
            return true;
        case Key::B:
            if (!e.pressed()) return true;
            if (scrubbing_) cancelScrub();
            else if (upNextVisible_) dismissUpNext();
            else if (osdVisible() && phase_ == Phase::Playing) hideOsd();
            else exitPlayer();
            return true;
        case Key::Left:
        case Key::Right: {
            if (!fileLoaded_) return true;
            scrubKeyHeld_ = true;
            // Accelerates while held: 1x, 3x, 6x, 12x the seek step.
            const double step = seekStep();
            const double mult = e.held < 0.8 ? 1.0 : e.held < 2.0 ? 3.0 : e.held < 4.0 ? 6.0 : 12.0;
            scrubBy((e.button == Key::Left ? -1.0 : 1.0) * step * mult);
            return true;
        }
        case Key::Up:
        case Key::Down:
            if (e.pressed()) showOsd(4.0);
            return true;
        case Key::ZL:
        case Key::ZR:
            if (e.pressedOrRepeat() && fileLoaded_) chapterJump(e.button == Key::ZL ? -1 : 1);
            return true;
        case Key::L:
        case Key::R:
            if (e.pressedOrRepeat() && fileLoaded_ && buttonSkip_)
                seekRelative((e.button == Key::L ? -1.0 : 1.0) * skipSeconds_);
            return true;
        case Key::Y:
            if (e.pressed()) openTrackMenu();
            return true;
        case Key::X:
            if (e.pressed()) openMoreMenu();
            return true;
        case Key::Minus:
            if (e.pressed()) {
                showInfo_ = !showInfo_;
                infoTimer_ = 0;
                infoStaticDirty_ = true;
            }
            return true;
        default: return false;
    }
}

}  // namespace karafin::player
