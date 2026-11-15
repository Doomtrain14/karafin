#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "downloads/detail/limiter.hpp"
#include "downloads/detail/part_writer.hpp"
#include "downloads/detail/queue.hpp"
#include "downloads/detail/util.hpp"
#include "downloads/downloads.hpp"

namespace karafin::downloads {

namespace {

using detail::Queue;
using detail::Task;
using Json = nlohmann::json;

struct Checker {
    int passed = 0;
    int failed = 0;
    void check(bool ok, std::string_view what) {
        if (ok) {
            ++passed;
        } else {
            ++failed;
            log::error("downloads-test", "FAILED: {}", what);
        }
    }
};

Task makeTask(const std::string& itemId, State state, const std::string& type = "Movie") {
    Task t;
    t.info.serverId = "srv1";
    t.info.userId = "user1";
    t.info.itemId = itemId;
    t.info.key = makeKey(t.info.serverId, t.info.itemId);
    t.info.type = type;
    t.info.name = "Item " + itemId;
    t.info.state = state;
    t.info.bytesTotal = 1000;
    return t;
}

void testStateMachine(Checker& c) {
    c.check(canTransition(State::Queued, State::Probing), "Queued -> Probing");
    c.check(canTransition(State::Probing, State::Downloading), "Probing -> Downloading");
    c.check(canTransition(State::Downloading, State::Completed), "Downloading -> Completed");
    c.check(canTransition(State::Downloading, State::WaitingForNetwork), "Downloading -> WaitingForNetwork");
    c.check(canTransition(State::WaitingForNetwork, State::Probing), "WaitingForNetwork -> Probing");
    c.check(canTransition(State::Downloading, State::Paused), "Downloading -> Paused");
    c.check(canTransition(State::Paused, State::Queued), "Paused -> Queued");
    c.check(canTransition(State::Failed, State::Queued), "Failed -> Queued");
    c.check(!canTransition(State::Completed, State::Downloading), "Completed -/-> Downloading");
    c.check(!canTransition(State::Paused, State::Downloading), "Paused -/-> Downloading");
    c.check(!canTransition(State::Failed, State::Downloading), "Failed -/-> Downloading");
    c.check(!canTransition(State::Queued, State::Completed), "Queued -/-> Completed");
    for (State s : {State::Queued, State::Probing, State::Downloading, State::Paused, State::WaitingForNetwork,
                    State::Completed, State::Failed}) {
        c.check(parseState(stateName(s)) == s, std::format("state name round trip {}", stateName(s)));
    }
    c.check(!parseState("Bogus").has_value(), "unknown state name");
    c.check(isActive(State::WaitingForNetwork) && !isActive(State::Paused), "isActive");
}

void testQueueOps(Checker& c) {
    Queue q;
    c.check(q.add(makeTask("a", State::Queued)), "add a");
    c.check(q.add(makeTask("b", State::Queued)), "add b");
    c.check(q.add(makeTask("c", State::Queued)), "add c");
    c.check(!q.add(makeTask("a", State::Queued)), "duplicate key rejected");
    c.check(q.size() == 3, "queue size 3");

    Task* next = q.nextRunnable();
    c.check(next && next->info.itemId == "a", "FIFO: a first");
    next->running = true;
    c.check(q.runningCount() == 1, "running count 1");
    Task* second = q.nextRunnable();
    c.check(second && second->info.itemId == "b", "running task skipped");
    Task* skipB = q.nextRunnable([](const Task& t) { return t.info.itemId == "b"; });
    c.check(skipB && skipB->info.itemId == "c", "skip predicate");

    Task& a = *q.find(makeKey("srv1", "a"));
    c.check(Queue::transition(a, State::Probing), "a Probing");
    c.check(Queue::transition(a, State::Downloading), "a Downloading");
    a.info.bytesPerSecond = 5000;
    a.info.etaSeconds = 10;
    c.check(Queue::transition(a, State::WaitingForNetwork, "Can't reach the server (Timeout)."), "a waiting");
    c.check(a.info.error == "Can't reach the server (Timeout)." && a.info.bytesPerSecond == 0 &&
                a.info.etaSeconds == -1,
            "waiting keeps error, clears speed");
    c.check(Queue::transition(a, State::Probing) && a.info.error.empty(), "waiting -> probing clears error");
    c.check(!Queue::transition(a, State::Queued), "Probing -/-> Queued");
    c.check(Queue::pause(a) && a.info.state == State::Paused && a.info.pausedByUser, "pause active");
    c.check(!Queue::pause(a), "pause twice is a no-op");
    c.check(Queue::resume(a) && a.info.state == State::Queued && !a.info.pausedByUser, "resume");
    c.check(!Queue::retry(a), "retry needs Failed");
    a.running = false;

    Task& b = *q.find(makeKey("srv1", "b"));
    Queue::transition(b, State::Probing);
    c.check(Queue::transition(b, State::Failed, "nope") && b.info.error == "nope", "fail with message");
    c.check(!Queue::resume(b), "resume needs Paused");
    b.info.attempt = 5;
    c.check(Queue::retry(b) && b.info.state == State::Queued && b.info.attempt == 0 && b.info.error.empty(),
            "retry resets");

    Task* found = q.findItem("c");
    c.check(found && found->info.serverId == "srv1", "findItem");
    c.check(q.remove(makeKey("srv1", "c")) && !q.find(makeKey("srv1", "c")), "remove");
    c.check(!q.remove(makeKey("srv1", "zzz")), "remove unknown");
}

void testPersistence(Checker& c) {
    Queue q;
    Task dl = makeTask("dl", State::Downloading, "Episode");
    dl.info.bytesDone = 400;
    dl.info.bytesPerSecond = 123;
    dl.info.seriesId = "series1";
    dl.info.seriesName = "Show";
    dl.info.seasonId = "season1";
    dl.info.indexNumber = 3;
    dl.info.parentIndexNumber = 1;
    dl.info.overview = "A summary.";
    dl.info.fileName = "media.mkv";
    dl.eTag = "\"abc\"";
    dl.syncedBytes = 300;
    dl.assetsDone = true;
    dl.hasPoster = true;
    LocalSubtitle sub;
    sub.index = 4;
    sub.fileName = "4.eng.forced.srt";
    sub.language = "eng";
    sub.format = "srt";
    sub.isForced = true;
    dl.subtitles.push_back(sub);
    q.add(dl);

    Task userPaused = makeTask("up", State::Paused);
    userPaused.info.pausedByUser = true;
    q.add(userPaused);
    Task autoPaused = makeTask("ap", State::Paused);
    q.add(autoPaused);
    Task done = makeTask("done", State::Completed);
    done.info.positionTicks = 12345;
    done.info.played = false;
    done.info.completedAt = 1700000000;
    q.add(done);
    Task failed = makeTask("fail", State::Failed);
    failed.info.error = "No space";
    q.add(failed);
    q.add(makeTask("queued", State::Queued));
    q.add(makeTask("wait", State::WaitingForNetwork));

    const std::string text = detail::dumpJson(q.toJson());
    const Json parsed = Json::parse(text, nullptr, false);
    c.check(!parsed.is_discarded(), "queue json parses");

    Queue r;
    c.check(r.fromJson(parsed, false), "restore (paused)");
    c.check(r.size() == q.size(), "restore keeps every item");
    auto st = [&](Queue& qq, const char* id) { return qq.find(makeKey("srv1", id))->info.state; };
    c.check(st(r, "dl") == State::Paused && !r.find(makeKey("srv1", "dl"))->info.pausedByUser,
            "downloading -> paused (not by user)");
    c.check(st(r, "up") == State::Paused && r.find(makeKey("srv1", "up"))->info.pausedByUser, "user pause kept");
    c.check(st(r, "ap") == State::Paused, "auto paused stays paused");
    c.check(st(r, "done") == State::Completed, "completed kept");
    c.check(st(r, "fail") == State::Failed && r.find(makeKey("srv1", "fail"))->info.error == "No space",
            "failed kept with error");
    c.check(st(r, "queued") == State::Paused, "queued -> paused");
    c.check(st(r, "wait") == State::Paused, "waiting -> paused");

    const Task& rd = *r.find(makeKey("srv1", "dl"));
    c.check(rd.info.bytesDone == 400 && rd.info.bytesTotal == 1000, "bytes restored");
    c.check(rd.info.bytesPerSecond == 0 && rd.info.etaSeconds == -1, "runtime fields cleared");
    c.check(rd.info.seriesId == "series1" && rd.info.indexNumber == 3 && rd.info.parentIndexNumber == 1 &&
                rd.info.overview == "A summary.",
            "episode fields restored");
    c.check(rd.info.fileName == "media.mkv" && rd.eTag == "\"abc\"" && rd.assetsDone && rd.hasPoster,
            "transfer fields restored");
    c.check(rd.syncedBytes == 300 && r.find(makeKey("srv1", "up"))->syncedBytes == detail::kSyncedUnknown,
            "synced bytes restored (absent = unknown)");
    c.check(detail::resumeOffset(400, 300) == 300 && detail::resumeOffset(200, 300) == 200 &&
                detail::resumeOffset(400, detail::kSyncedUnknown) == 400,
            "resume offset never past the synced bytes");
    c.check(rd.subtitles.size() == 1 && rd.subtitles[0].fileName == "4.eng.forced.srt" && rd.subtitles[0].isForced,
            "subtitles restored");
    const Task& rdone = *r.find(makeKey("srv1", "done"));
    c.check(rdone.info.positionTicks == 12345 && rdone.info.completedAt == 1700000000, "progress restored");
    c.check(r.tasks().front().info.itemId == "dl" && r.tasks().back().info.itemId == "wait", "order kept");
    c.check(r.add(makeTask("new", State::Queued)) && r.tasks().back().seq > r.tasks().front().seq,
            "new items get later sequence numbers");

    Queue a;
    c.check(a.fromJson(parsed, true), "restore (auto resume)");
    c.check(st(a, "dl") == State::Queued, "auto resume: downloading -> queued");
    c.check(st(a, "ap") == State::Queued, "auto resume: auto paused -> queued");
    c.check(st(a, "up") == State::Paused, "auto resume: user pause kept");
    c.check(st(a, "done") == State::Completed && st(a, "fail") == State::Failed, "auto resume: terminal kept");

    Queue bad;
    c.check(!bad.fromJson(Json::parse("[1,2]"), false) && bad.size() == 0, "non-queue document rejected");
    c.check(bad.fromJson(Json::parse(R"({"version":1,"items":[{"itemId":"x"},{"serverId":"s","itemId":"y"},5]})"), false) &&
                bad.size() == 1,
            "invalid entries skipped");
    c.check(bad.find(makeKey("s", "y"))->info.state == State::Paused, "missing state -> paused");
}

void testHelpers(Checker& c) {
    using namespace detail;
    c.check(mediaExtension("mkv") == "mkv", "ext mkv");
    c.check(mediaExtension("mov,mp4,m4a,3gp,3g2,mj2") == "mp4", "ext mp4 family");
    c.check(mediaExtension("matroska,webm") == "mkv", "ext matroska");
    c.check(mediaExtension("mkv", "Movie (2020).MP4") == "mp4", "ext from disposition");
    c.check(mediaExtension("", "", "/media/x/file.avi") == "avi", "ext from server path");
    c.check(mediaExtension("", "", "/media/x/file.nfo") == "mkv", "unknown server ext ignored");
    c.check(mediaExtension("") == "mkv", "ext fallback");

    auto cr = parseContentRange("bytes 100-199/1000");
    c.check(cr && cr->start == 100 && cr->end == 199 && cr->total == 1000, "content-range");
    auto star = parseContentRange("bytes 0-0/*");
    c.check(star && star->total == 0, "content-range */");
    c.check(!parseContentRange("bytes */1000"), "content-range unsatisfied form");
    c.check(!parseContentRange("items 1-2/3"), "content-range unit");

    c.check(backoffSeconds(1) == 2 && backoffSeconds(2) == 4 && backoffSeconds(3) == 8, "backoff doubles");
    c.check(backoffSeconds(20) == 120, "backoff cap");
    c.check(isStrongETag("\"x\"") && !isStrongETag("W/\"x\"") && !isStrongETag(""), "strong etag");
    c.check(countsAsPlayed(91, 100) && !countsAsPlayed(50, 100) && !countsAsPlayed(10, 0), "played threshold");
    c.check(formatBytes(1536ull * 1024 * 1024) == "1.5 GB" && formatBytes(10ull * 1024 * 1024) == "10 MB",
            "formatBytes");

    Json meta = {{"item", {{"Id", "x"}, {"RunTimeTicks", 1000}}}};
    patchUserData(meta, 250, false);
    c.check(meta["item"]["UserData"]["PlaybackPositionTicks"] == 250 && meta["item"]["UserData"]["Played"] == false &&
                meta["item"]["UserData"]["PlayedPercentage"] == 25.0,
            "patch progress");
    patchUserData(meta, 990, true);
    c.check(meta["item"]["UserData"]["PlaybackPositionTicks"] == 0 && meta["item"]["UserData"]["Played"] == true,
            "patch played");
    const auto item = jellyfin::parseItem(meta["item"]);
    c.check(item.userData.played && item.userData.playbackPositionTicks == 0, "patched metadata parses");

    double tokens = 0;
    c.check(BandwidthLimiter::take(tokens, 0, 1.0, 1000) == 0, "limiter unlimited");
    tokens = 0;
    const double wait = BandwidthLimiter::take(tokens, 1000, 0.0, 500);
    c.check(wait > 0.49 && wait < 0.51, "limiter debt -> wait");
    tokens = 0;
    c.check(BandwidthLimiter::take(tokens, 1000, 10.0, 100) == 0 && tokens <= 250.0, "limiter burst cap");
}

void testThroughput(Checker& c) {
    auto info = [](State s, uint64_t done, uint64_t total, double speed) {
        DownloadInfo i;
        i.state = s;
        i.bytesDone = done;
        i.bytesTotal = total;
        i.bytesPerSecond = speed;
        return i;
    };
    ThroughputMeter m;
    m.update({info(State::Paused, 0, 1000, 0), info(State::Failed, 0, 1000, 0)}, 0.25);
    c.check(!m.active() && m.bytesPerSecond() == 0 && m.etaSeconds() == -1, "throughput idle");

    // Two transfers plus a queued item; paused bytes don't count.
    std::vector<DownloadInfo> list = {info(State::Downloading, 400, 1000, 100), info(State::Downloading, 0, 600, 50),
                                      info(State::Queued, 0, 500, 0), info(State::Paused, 0, 9000, 0)};
    m.update(list, 0.25);
    c.check(m.active() && m.bytesPerSecond() == 150, "throughput sums speeds");
    c.check(std::abs(m.etaSeconds() - 1700.0 / 150.0) < 1e-9, "throughput eta over queued + active");

    // Between episodes (probing, no speed) the rate decays instead of dropping to zero.
    list = {info(State::Probing, 0, 600, 0), info(State::Queued, 0, 500, 0)};
    m.update(list, 1.0);
    c.check(m.bytesPerSecond() > 100 && m.bytesPerSecond() < 150, "throughput smoothed");
    list[1].bytesTotal = 0;
    m.update(list, 0.25);
    c.check(m.etaSeconds() == -1, "throughput eta unknown without sizes");

    m.update({info(State::WaitingForNetwork, 0, 600, 0)}, 0.25);
    c.check(m.active() && m.bytesPerSecond() == 0 && m.etaSeconds() == -1, "throughput waiting for network");
}

void testPartWriter(Checker& c) {
    const std::string dir = paths::cacheDir() + "/dl-selftest";
    if (!fs::ensureDir(dir)) {
        c.check(false, "part writer: cannot create " + dir);
        return;
    }
    const std::string path = dir + "/media.mkv.part";
    {
        detail::PartWriter w(16);
        c.check(w.open(path, 0), "part open");
        c.check(w.write("0123456789") && w.write("abcdefghij") && w.size() == 20, "part write across buffer");
        c.check(w.written() == 16, "part buffer flushed when full");
        c.check(w.close(true), "part close");
    }
    c.check(fs::readFile(path).value_or("") == "0123456789abcdefghij", "part content");
    {
        detail::PartWriter w(64);
        c.check(w.open(path, 10) && w.size() == 10, "part reopen at 10 (cuts tail)");
        c.check(w.write("XYZ"), "part resume write");
        c.check(w.rebaseToZero() && w.size() == 3, "part rebase while nothing flushed");
        c.check(w.close(true), "part close 2");
    }
    c.check(fs::readFile(path).value_or("") == "XYZ", "part rebased content");
    {
        detail::PartWriter w(4);
        c.check(w.open(path, 3), "part reopen at 3");
        c.check(w.write("garbage!") && w.written() > 3, "part garbage flushed");
        c.check(!w.rebaseToZero(), "part rebase refused after flush");
        c.check(w.truncateToBase() && w.size() == 3, "part truncate to base");
        c.check(w.close(true), "part close 3");
    }
    c.check(fs::readFile(path).value_or("") == "XYZ", "part truncated content");
    fs::removeTree(dir);
}

}  // namespace

bool selfTest(std::string* summary) {
    Checker c;
    testStateMachine(c);
    testQueueOps(c);
    testPersistence(c);
    testHelpers(c);
    testThroughput(c);
    testPartWriter(c);
    const std::string line = std::format("downloads self-test: {} passed, {} failed", c.passed, c.failed);
    if (c.failed == 0) {
        log::info("downloads-test", "{}", line);
    } else {
        log::error("downloads-test", "{}", line);
    }
    if (summary) *summary = line;
    return c.failed == 0;
}

}  // namespace karafin::downloads
