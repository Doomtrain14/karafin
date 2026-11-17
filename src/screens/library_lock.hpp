#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"

namespace karafin::jellyfin {
class Client;
}

namespace karafin::ui {
class ActionMenu;
}

namespace karafin::screens {

// In-app lock for chosen libraries, opened with a 4 or 6 step D-pad code (a parental
// convenience, not real security: the server account itself can still see everything).
//
// Everything is per profile (server + user): the code, the locked libraries, "ask for the code
// to open this profile" and the wrong-code lockout. A locked library needs the code to open, and
// its titles are left out of Home, Search and library grids. Unlocking opens that library until
// it is locked again (from its tile or page, or "Lock now"), the profile changes or the app
// restarts. After 5 wrong codes entry is blocked for 30 s, doubling with each further 5 up to
// 5 min; the count and end time are stored, so restarting doesn't skip the wait.
//
// Stored in settings as "lock.profiles": {"<serverId>/<userId>": {"code": "<steps>:<salt>:<iterated
// SHA-256>", "libraries": [ids], "askOnOpen": bool, "failures": n, "lockoutUntil": unix seconds}}.
// Main thread only.
class LibraryLock {
public:
    static LibraryLock& instance();

    bool hasCode() const;
    // The library is set to locked (whether or not it is unlocked right now).
    bool isLocked(const std::string& libraryId) const;
    // The library is locked and hasn't been unlocked.
    bool blocks(const std::string& libraryId) const;
    // Some library is locked and not unlocked.
    bool active() const;
    // Some locked library is unlocked right now.
    bool anyUnlocked() const;

    // True when `item` belongs to a locked library (needs prepare() to have finished).
    bool hides(const jellyfin::BaseItem& item) const;
    // `items` without the ones hides() rejects. Sets *removed when anything was left out.
    std::vector<jellyfin::BaseItem> filter(const std::vector<jellyfin::BaseItem>& items,
                                           bool* removed = nullptr) const;
    // Loads the titles of the locked libraries if needed, then calls `done` (right away when
    // nothing is locked or they are already known).
    void prepare(std::function<void()> done);
    // Bumped when locks, the code, the unlocked state or the profile change; views reload when it
    // differs from what they showed.
    uint64_t revision() const;

    // Asks for the code, then unlocks the library until it is locked again or the app restarts.
    void requestUnlock(const jellyfin::BaseItem& library, std::function<void()> onUnlocked);
    // Locks an unlocked library again (no code needed).
    void relock(const std::string& libraryId);
    // Locks every unlocked library again.
    void lockNow();
    // Lock / unlock / lock again / remove lock actions for one library tile (X menu). Locking first
    // asks for a new code if none is set; unlocking and removing a lock need the code.
    void addLibraryActions(ui::ActionMenu& menu, const jellyfin::BaseItem& library, std::function<void()> onChanged);
    // Set (no code yet) or change (asks for the current one) the code.
    void changeCode(std::function<void()> onDone);
    // Asks for the code, then removes it, every lock and the profile code.
    void removeCode(std::function<void()> onDone);

    // The current profile asks for the code before it is opened.
    bool asksOnOpen() const;
    // Turns that on or off (needs a code, and asks for it).
    void setAsksOnOpen(bool on, std::function<void()> onDone);
    // Whether opening a saved profile needs its code (readable without being signed in to it).
    static bool profileNeedsCode(const std::string& serverId, const std::string& userId);
    // Asks for that profile's code: `onCorrect` when it is right, `onCancel` when backed out.
    void askProfileCode(const std::string& serverId, const std::string& userId, const std::string& name,
                        std::function<void()> onCorrect, std::function<void()> onCancel);

private:
    struct Profile {
        std::string key;  // "<serverId>/<userId>", empty without a signed-in user
        int length = 0;   // code steps, 0 = no code
        std::string salt;
        std::string hash;
        std::set<std::string> libraries;
        bool askOnOpen = false;
        int failures = 0;
        int64_t lockoutUntil = 0;
    };

    LibraryLock() = default;

    // `migrate` moves the 1.0.4 global settings into this profile when it has none yet.
    static Profile readProfile(const std::string& key, bool migrate);
    static void writeProfile(const Profile& p);
    static bool checkCode(Profile& p, const std::string& code, std::string& error);
    static int lockoutLeft(const Profile& p);

    // Reloads the cached profile (and drops unlocks and title lists) when the signed-in profile
    // changed.
    void sync() const;
    bool blocksCached(const std::string& libraryId) const;
    bool activeCached() const;
    bool hidesCached(const jellyfin::BaseItem& item) const;
    void storeCode(const std::string& code);
    void askCode(const std::string& key, int length, std::string title, std::string message,
                 std::function<void()> onCorrect, std::function<void()> onCancel);
    void askCode(std::string title, std::string message, std::function<void()> onCorrect);
    void askNewCode(std::function<void()> onSet);
    void lockLibrary(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    void removeLock(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    std::string cacheKey() const;
    void changed();

    // Caches, refreshed by sync() (hence mutable: the const queries sync first).
    mutable std::weak_ptr<jellyfin::Client> client_;  // client profile_ was loaded for
    mutable bool synced_ = false;
    mutable Profile profile_;
    mutable std::set<std::string> unlocked_;
    mutable uint64_t revision_ = 1;
    mutable std::map<std::string, std::set<std::string>> hidden_;  // library id -> its titles
    mutable std::string hiddenKey_;   // cacheKey() that hidden_ was loaded for
    mutable std::string loadingKey_;  // cacheKey() being loaded
    mutable std::vector<std::function<void()>> waiters_;
    mutable CancelToken token_;
};

}  // namespace karafin::screens
