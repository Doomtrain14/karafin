#include "jellyfin/discovery.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/platform.hpp"
#include "core/thread.hpp"

namespace karafin::jellyfin {

namespace {

constexpr uint16_t kDiscoveryPort = 7359;
constexpr char kProbe[] = "who is JellyfinServer?";

struct Runner {
    std::mutex mutex;
    Thread thread;
    CancelToken stop;  // internal: lets a new discovery stop the previous one
};

Runner& runner() {
    static Runner r;
    return r;
}

Result<std::vector<DiscoveredServer>> discover(int timeoutMs, const CancelToken& token, const CancelToken& stop);

}  // namespace

Result<std::vector<DiscoveredServer>> discoverServersBlocking(int timeoutMs, CancelToken token) {
    return discover(timeoutMs, token, CancelToken{});
}

namespace {

Result<std::vector<DiscoveredServer>> discover(int timeoutMs, const CancelToken& userToken, const CancelToken& stop) {
    using R = Result<std::vector<DiscoveredServer>>;
    struct {
        const CancelToken& a;
        const CancelToken& b;
        bool cancelled() const { return a.cancelled() || b.cancelled(); }
    } token{userToken, stop};
    if (!platform::networkConnected())
        return R::failure(ApiError::make(ErrorKind::Offline, "No network connection."));

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return R::failure(ApiError::make(ErrorKind::Network, "Could not open a network socket."));
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kDiscoveryPort);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const auto sendProbe = [&] {
        return sendto(fd, kProbe, sizeof(kProbe) - 1, 0, reinterpret_cast<const sockaddr*>(&dst), sizeof dst);
    };
    if (sendProbe() < 0) {
        close(fd);
        return R::failure(ApiError::make(ErrorKind::Network, "Could not send the discovery broadcast."));
    }

    std::vector<DiscoveredServer> found;
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(200, timeoutMs));
    const auto resendAt = Clock::now() + std::chrono::milliseconds(std::max(100, timeoutMs / 3));
    bool resent = false;
    char buf[2048];
    while (!token.cancelled()) {
        const auto now = Clock::now();
        if (now >= deadline) break;
        if (!resent && now >= resendAt) {
            sendProbe();  // UDP is lossy; one retry catches most drops
            resent = true;
        }
        const int waitMs = static_cast<int>(
            std::min<int64_t>(100, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, std::max(1, waitMs)) <= 0 || !(pfd.revents & POLLIN)) continue;
        sockaddr_in from{};
        socklen_t fromLen = sizeof from;
        const ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n <= 0) continue;
        const nlohmann::json j = nlohmann::json::parse(std::string_view(buf, static_cast<size_t>(n)), nullptr, false);
        if (!j.is_object()) continue;
        DiscoveredServer s = parseDiscoveryReply(j);
        char ip[16] = {};  // dotted IPv4 + NUL
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        s.senderIp = ip;
        if (s.address.empty() && !s.senderIp.empty()) s.address = "http://" + s.senderIp + ":8096";
        if (s.id.empty() || s.address.empty()) continue;
        const bool dup = std::any_of(found.begin(), found.end(), [&](const DiscoveredServer& f) { return f.id == s.id; });
        if (!dup) {
            log::info("jellyfin", "discovered \"{}\" at {} (from {})", s.name, s.address, s.senderIp);
            found.push_back(std::move(s));
        }
    }
    close(fd);
    if (token.cancelled()) return R::failure(ApiError::cancelled());
    return R::success(std::move(found));
}

}  // namespace

void discoverServers(std::function<void(Result<std::vector<DiscoveredServer>>)> cb, int timeoutMs,
                     CancelToken token) {
    Runner& r = runner();
    std::lock_guard lock(r.mutex);
    r.stop.cancel();
    if (r.thread.joinable()) r.thread.join();  // exits within one 100 ms poll slice
    r.stop.reset();
    const CancelToken stop = r.stop;
    ThreadOptions opts;
    opts.name = "jf-discovery";
    opts.stackSize = 64 * 1024;
    opts.priority = kPriorityBackground;
    r.thread = Thread(std::move(opts), [cb = std::move(cb), timeoutMs, token, stop] {
        auto result = discover(timeoutMs, token, stop);
        // No wait: the main thread joins this thread when discovery restarts.
        main_thread::postNoWait([cb, token, stop, result = std::move(result)]() mutable {
            if (token.cancelled() || stop.cancelled() || !cb) return;
            cb(std::move(result));
        });
    });
}

}  // namespace karafin::jellyfin
