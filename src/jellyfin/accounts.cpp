#include "jellyfin/accounts.hpp"

#include <algorithm>
#include <ctime>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "jellyfin/detail/json_get.hpp"
#include "jellyfin/detail/random.hpp"

namespace karafin::jellyfin {

using namespace detail;

namespace {

constexpr int kFormatVersion = 1;

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

void sortByRecent(std::vector<StoredServer>& servers) {
    std::stable_sort(servers.begin(), servers.end(),
                     [](const StoredServer& a, const StoredServer& b) { return a.lastUsed > b.lastUsed; });
    for (auto& s : servers)
        std::stable_sort(s.users.begin(), s.users.end(),
                         [](const StoredUser& a, const StoredUser& b) { return a.lastUsed > b.lastUsed; });
}

}  // namespace

const StoredUser* StoredServer::user(const std::string& userId) const {
    for (const auto& u : users)
        if (u.id == userId) return &u;
    return nullptr;
}

AccountStore& AccountStore::instance() {
    static AccountStore store;
    return store;
}

std::string AccountStore::filePath() const { return paths::dataFile("jellyfin_accounts.json"); }

void AccountStore::ensureLoaded() {
    if (loaded_) return;
    loaded_ = true;
    servers_.clear();
    switchSessions_.clear();
    if (auto data = fs::readFileAtomic(filePath())) {
        const Json doc = Json::parse(*data, nullptr, false);
        if (doc.is_object()) {
            deviceId_ = getStr(doc, "deviceId");
            const Json sessions = doc.value("switchSessions", Json::object());
            if (sessions.is_object()) {
                for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                    if (!it.value().is_object()) continue;
                    switchSessions_[it.key()] = {getStr(it.value(), "serverId"), getStr(it.value(), "userId")};
                }
            }
            for (const auto& js : getArr(doc, "servers")) {
                StoredServer s;
                s.id = getStr(js, "id");
                s.name = getStr(js, "name");
                s.url = getStr(js, "url");
                s.version = getStr(js, "version");
                s.lastUsed = getI64(js, "lastUsed");
                s.lastUserId = getStr(js, "lastUserId");
                for (const auto& ju : getArr(js, "users")) {
                    StoredUser u;
                    u.id = getStr(ju, "id");
                    u.name = getStr(ju, "name");
                    u.token = getStr(ju, "token");
                    u.primaryImageTag = getStr(ju, "primaryImageTag");
                    u.lastUsed = getI64(ju, "lastUsed");
                    if (!u.id.empty()) s.users.push_back(std::move(u));
                }
                if (!s.id.empty() && !s.url.empty()) servers_.push_back(std::move(s));
            }
        } else {
            log::warn("jellyfin", "{} is corrupt; starting with no saved servers", filePath());
        }
    }
    sortByRecent(servers_);
    if (deviceId_.empty()) {
        deviceId_ = "karafin-" + randomHex(16);
        log::info("jellyfin", "generated device id {}", deviceId_);
        persist();
    }
}

void AccountStore::persist() {
    Json doc = {{"version", kFormatVersion},
                {"deviceId", deviceId_},
                {"servers", Json::array()},
                {"switchSessions", Json::object()}};
    for (const auto& [profile, session] : switchSessions_)
        doc["switchSessions"][profile] = {{"serverId", session.first}, {"userId", session.second}};
    for (const auto& s : servers_) {
        Json js = {{"id", s.id},           {"name", s.name},         {"url", s.url},
                   {"version", s.version}, {"lastUsed", s.lastUsed}, {"lastUserId", s.lastUserId},
                   {"users", Json::array()}};
        for (const auto& u : s.users)
            js["users"].push_back({{"id", u.id},
                                   {"name", u.name},
                                   {"token", u.token},
                                   {"primaryImageTag", u.primaryImageTag},
                                   {"lastUsed", u.lastUsed}});
        doc["servers"].push_back(std::move(js));
    }
    if (!fs::writeFileAtomic(filePath(), dumpSafe(doc, 2))) log::error("jellyfin", "failed to write {}", filePath());
}

StoredServer* AccountStore::findServer(const std::string& id) {
    for (auto& s : servers_)
        if (s.id == id) return &s;
    return nullptr;
}

std::string AccountStore::deviceId() {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    return deviceId_;
}

std::vector<StoredServer> AccountStore::servers() {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    return servers_;
}

std::optional<StoredServer> AccountStore::server(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    if (auto* s = findServer(serverId)) return *s;
    return std::nullopt;
}

std::optional<AccountStore::SessionRef> AccountStore::lastSession(const std::string& switchProfileId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    if (!switchProfileId.empty()) {
        const auto session = switchSessions_.find(switchProfileId);
        if (session != switchSessions_.end()) {
            for (const auto& server : servers_) {
                if (server.id != session->second.first) continue;
                if (const StoredUser* user = server.user(session->second.second)) return SessionRef{server, *user};
                break;
            }
            return std::nullopt;
        }
    }
    for (const auto& s : servers_) {
        if (const StoredUser* u = s.user(s.lastUserId); u && u->hasToken()) return SessionRef{s, *u};
        for (const auto& u : s.users)
            if (u.hasToken()) return SessionRef{s, u};
    }
    return std::nullopt;
}

void AccountStore::saveServer(const ServerInfo& info, const std::string& baseUrl) {
    if (info.id.empty() || baseUrl.empty()) return;
    std::lock_guard lock(mutex_);
    ensureLoaded();
    StoredServer* s = findServer(info.id);
    if (!s) {
        servers_.push_back({});
        s = &servers_.back();
        s->id = info.id;
    }
    if (!info.name.empty()) s->name = info.name;
    if (!info.version.empty()) s->version = info.version;
    s->url = baseUrl;
    s->lastUsed = now();
    sortByRecent(servers_);
    persist();
}

void AccountStore::saveUser(const std::string& serverId, const UserDto& user, const std::string& token) {
    if (serverId.empty() || user.id.empty()) return;
    std::lock_guard lock(mutex_);
    ensureLoaded();
    StoredServer* s = findServer(serverId);
    if (!s) {
        log::warn("jellyfin", "saveUser: unknown server {}", serverId);
        return;
    }
    auto it = std::find_if(s->users.begin(), s->users.end(), [&](const StoredUser& u) { return u.id == user.id; });
    if (it == s->users.end()) {
        s->users.push_back({});
        it = s->users.end() - 1;
        it->id = user.id;
    }
    if (!user.name.empty()) it->name = user.name;
    it->primaryImageTag = user.primaryImageTag;
    if (!token.empty()) it->token = token;
    it->lastUsed = s->lastUsed = now();
    s->lastUserId = user.id;
    sortByRecent(servers_);
    persist();
}

void AccountStore::markUsed(const std::string& serverId, const std::string& userId,
                            const std::string& switchProfileId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    StoredServer* s = findServer(serverId);
    if (!s) return;
    s->lastUsed = now();
    if (!userId.empty()) {
        s->lastUserId = userId;
        for (auto& u : s->users)
            if (u.id == userId) u.lastUsed = s->lastUsed;
        if (!switchProfileId.empty()) switchSessions_[switchProfileId] = {serverId, userId};
    }
    sortByRecent(servers_);
    persist();
}

void AccountStore::clearToken(const std::string& serverId, const std::string& userId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    StoredServer* s = findServer(serverId);
    if (!s) return;
    bool changed = false;
    for (auto& u : s->users) {
        if (u.id == userId && !u.token.empty()) {
            u.token.clear();
            changed = true;
        }
    }
    if (changed) persist();
}

void AccountStore::removeUser(const std::string& serverId, const std::string& userId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    StoredServer* s = findServer(serverId);
    if (!s) return;
    std::erase_if(s->users, [&](const StoredUser& u) { return u.id == userId; });
    if (s->lastUserId == userId) s->lastUserId.clear();
    for (auto& [profile, session] : switchSessions_)
        if (session.first == serverId && session.second == userId) session = {};
    persist();
}

void AccountStore::removeServer(const std::string& serverId) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    std::erase_if(servers_, [&](const StoredServer& s) { return s.id == serverId; });
    for (auto& [profile, session] : switchSessions_)
        if (session.first == serverId) session = {};
    persist();
}

}  // namespace karafin::jellyfin
