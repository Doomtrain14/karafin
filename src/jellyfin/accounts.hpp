#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "jellyfin/models.hpp"

namespace karafin::jellyfin {

struct StoredUser {
    std::string id;
    std::string name;
    std::string token;            // empty after logout / revocation (user stays listed)
    std::string primaryImageTag;
    int64_t lastUsed = 0;         // unix seconds

    bool hasToken() const { return !token.empty(); }
};

struct StoredServer {
    std::string id;
    std::string name;
    std::string url;              // normalized base URL
    std::string version;
    int64_t lastUsed = 0;
    std::string lastUserId;
    std::vector<StoredUser> users;  // most recently used first

    const StoredUser* user(const std::string& userId) const;
};

// Multi-server / multi-user persistence in sdmc:/switch/karafin/jellyfin_accounts.json (atomic
// writes; tokens live here rather than in settings.json so a shared settings file leaks nothing).
// Also owns the installation's stable DeviceId. Thread-safe; loads lazily on first use.
class AccountStore {
public:
    static AccountStore& instance();

    // Stable per-installation id sent as DeviceId in the Authorization header.
    std::string deviceId();

    std::vector<StoredServer> servers();  // most recently used first
    std::optional<StoredServer> server(const std::string& serverId);

    struct SessionRef {
        StoredServer server;
        StoredUser user;
    };
    // The last server + user used by this Switch profile, or the legacy most-recent session
    // when the profile has not signed in before.
    std::optional<SessionRef> lastSession(const std::string& switchProfileId = {});

    void saveServer(const ServerInfo& info, const std::string& baseUrl);
    void saveUser(const std::string& serverId, const UserDto& user, const std::string& token);
    void markUsed(const std::string& serverId, const std::string& userId,
                  const std::string& switchProfileId = {});
    void clearToken(const std::string& serverId, const std::string& userId);
    void removeUser(const std::string& serverId, const std::string& userId);
    void removeServer(const std::string& serverId);

    std::string filePath() const;

private:
    AccountStore() = default;
    void ensureLoaded();  // mutex held
    void persist();       // mutex held
    StoredServer* findServer(const std::string& id);

    std::mutex mutex_;
    bool loaded_ = false;
    std::string deviceId_;
    std::vector<StoredServer> servers_;
    std::map<std::string, std::pair<std::string, std::string>> switchSessions_;
};

}  // namespace karafin::jellyfin
