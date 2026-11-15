#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"

namespace karafin::screens {

// In-app lock for chosen libraries, opened with a 4 or 6 step D-pad code (a parental
// convenience, not real security: the server account itself can still see everything).
//
// A locked library needs the code to open, and its titles are left out of Home and Search.
// Unlocking opens that library until it is locked again (from its tile or page, or "Lock now")
// or the app restarts. Stored in settings as "lock.code" ("<steps>:<salt>:<iterated SHA-256>")
// and "lock.libraries" (library ids). Main thread only.
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
    // Bumped when locks, the code or the unlocked state change; views reload when it differs
    // from what they showed.
    uint64_t revision() const { return revision_; }

    // Asks for the code, then unlocks the library until it is locked again or the app restarts.
    void requestUnlock(const jellyfin::BaseItem& library, std::function<void()> onUnlocked);
    // Locks an unlocked library again (no code needed).
    void relock(const std::string& libraryId);
    // Locks every unlocked library again.
    void lockNow();
    // Lock / unlock / lock again / remove lock options for one library tile. Locking first asks
    // for a new code if none is set; unlocking and removing a lock need the code.
    void libraryActions(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    // Set (no code yet) or change (asks for the current one) the code.
    void changeCode(std::function<void()> onDone);
    // Asks for the code, then removes it and every lock.
    void removeCode(std::function<void()> onDone);

private:
    LibraryLock() = default;

    std::vector<std::string> lockedIds() const;
    void setLockedIds(const std::vector<std::string>& ids);
    int codeLength() const;
    bool verify(const std::string& code) const;
    void storeCode(const std::string& code);
    void askCode(std::string title, std::string message, std::function<void()> onCorrect);
    void askNewCode(std::function<void()> onSet);
    void lockLibrary(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    void removeLock(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    std::string cacheKey() const;
    void changed();

    std::set<std::string> unlocked_;
    uint64_t revision_ = 1;
    std::map<std::string, std::set<std::string>> hidden_;  // library id -> its titles
    std::string hiddenKey_;   // cacheKey() that hidden_ was loaded for
    std::string loadingKey_;  // cacheKey() being loaded
    std::vector<std::function<void()>> waiters_;
    CancelToken token_;
};

}  // namespace karafin::screens
