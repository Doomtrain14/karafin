#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"

namespace karafin::screens {

// In-app password lock for chosen libraries (a parental convenience, not real security: the
// server account itself can still see everything).
//
// While locked, the libraries need the password to open, and their titles are left out of Home
// and Search. Entering the password unlocks every locked library until the app restarts or
// "Lock now" is used. Stored in settings as "lock.password_hash" (salted, iterated SHA-256) and
// "lock.libraries" (library ids). Main thread only.
class LibraryLock {
public:
    static LibraryLock& instance();

    bool hasPassword() const;
    // The library is set to locked (whether or not this session has unlocked it).
    bool isLocked(const std::string& libraryId) const;
    // The library is locked and this session hasn't unlocked it.
    bool blocks(const std::string& libraryId) const;
    // Some library is locked and this session hasn't unlocked them.
    bool active() const;
    bool unlockedThisSession() const { return unlocked_ && !lockedIds().empty(); }

    // True when `item` belongs to a locked library (needs prepare() to have finished).
    bool hides(const jellyfin::BaseItem& item) const;
    // `items` without the ones hides() rejects. Sets *removed when anything was left out.
    std::vector<jellyfin::BaseItem> filter(const std::vector<jellyfin::BaseItem>& items,
                                           bool* removed = nullptr) const;
    // Loads the titles of the locked libraries if needed, then calls `done` (right away when
    // nothing is locked or they are already known).
    void prepare(std::function<void()> done);
    // Bumped when locks, the password or the unlocked state change; views reload when it differs
    // from what they showed.
    uint64_t revision() const { return revision_; }

    // Asks for the password, then unlocks every library for this session.
    void requestUnlock(std::function<void()> onUnlocked);
    void lockNow();
    // Locks or unlocks one library. Locking first asks for a new password if none is set;
    // removing a lock needs the password.
    void toggleLibrary(const jellyfin::BaseItem& library, std::function<void()> onChanged);
    // Set (no password yet) or change (asks for the current one) the password.
    void changePassword(std::function<void()> onDone);
    // Asks for the password, then removes it and every lock.
    void removePassword(std::function<void()> onDone);

private:
    LibraryLock() = default;

    std::vector<std::string> lockedIds() const;
    void setLockedIds(const std::vector<std::string>& ids);
    bool verify(const std::string& password) const;
    void storePassword(const std::string& password);
    void askPassword(std::string header, std::string sub, std::function<void()> onCorrect);
    void askNewPassword(std::function<void()> onSet);
    std::string cacheKey() const;
    void changed();

    bool unlocked_ = false;
    uint64_t revision_ = 1;
    std::set<std::string> hidden_;
    std::string hiddenKey_;   // cacheKey() that hidden_ was loaded for
    std::string loadingKey_;  // cacheKey() being loaded
    std::vector<std::function<void()>> waiters_;
    CancelToken token_;
};

}  // namespace karafin::screens
