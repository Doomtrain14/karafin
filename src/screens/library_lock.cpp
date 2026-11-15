#include "screens/library_lock.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <memory>
#include <random>

#include "core/log.hpp"
#include "core/settings.hpp"
#include "jellyfin/client.hpp"
#include "screens/keyboard.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {

constexpr const char* kHashKey = "lock.password_hash";
constexpr const char* kLibrariesKey = "lock.libraries";
constexpr int kHashRounds = 20000;
constexpr size_t kMinPassword = 4;
constexpr int kMaxPassword = 32;

using Digest = std::array<unsigned char, 32>;

std::string toHex(const unsigned char* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

std::string randomSalt() {
    std::array<unsigned char, 16> bytes{};
#ifdef __SWITCH__
    randomGet(bytes.data(), bytes.size());
#else
    std::random_device rd;
    for (auto& b : bytes) b = static_cast<unsigned char>(rd());
#endif
    return toHex(bytes.data(), bytes.size());
}

std::string hashPassword(const std::string& salt, const std::string& password) {
    Digest digest{};
    const std::string seed = salt + password;
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(), digest.data(), 0);
    std::string buf(digest.size() + salt.size(), '\0');
    std::memcpy(buf.data() + digest.size(), salt.data(), salt.size());
    for (int i = 0; i < kHashRounds; ++i) {
        std::memcpy(buf.data(), digest.data(), digest.size());
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(buf.data()), buf.size(), digest.data(), 0);
    }
    return toHex(digest.data(), digest.size());
}

KeyboardOptions passwordField(std::string header, std::string sub) {
    KeyboardOptions opts;
    opts.header = std::move(header);
    opts.sub = std::move(sub);
    opts.guide = "Password";
    opts.kind = KeyboardKind::Password;
    opts.maxLength = kMaxPassword;
    return opts;
}

}  // namespace

LibraryLock& LibraryLock::instance() {
    static LibraryLock lock;
    return lock;
}

bool LibraryLock::hasPassword() const {
    return !Settings::instance().get<std::string>(kHashKey, "").empty();
}

std::vector<std::string> LibraryLock::lockedIds() const {
    std::vector<std::string> ids;
    const auto json = Settings::instance().getJson(kLibrariesKey);
    if (json.is_array())
        for (const auto& v : json)
            if (v.is_string()) ids.push_back(v.get<std::string>());
    return ids;
}

void LibraryLock::setLockedIds(const std::vector<std::string>& ids) {
    if (ids.empty()) Settings::instance().remove(kLibrariesKey);
    else Settings::instance().setJson(kLibrariesKey, ids);
    Settings::instance().saveIfDirty(0.0);
}

bool LibraryLock::isLocked(const std::string& libraryId) const {
    const auto ids = lockedIds();
    return std::find(ids.begin(), ids.end(), libraryId) != ids.end();
}

bool LibraryLock::blocks(const std::string& libraryId) const {
    return !unlocked_ && hasPassword() && isLocked(libraryId);
}

bool LibraryLock::active() const { return !unlocked_ && hasPassword() && !lockedIds().empty(); }

bool LibraryLock::hides(const jf::BaseItem& item) const {
    if (hidden_.empty() || !active()) return false;
    for (const std::string* id : {&item.id, &item.seriesId, &item.parentId})
        if (!id->empty() && hidden_.contains(*id)) return true;
    return false;
}

std::vector<jf::BaseItem> LibraryLock::filter(const std::vector<jf::BaseItem>& items, bool* removed) const {
    std::vector<jf::BaseItem> out;
    out.reserve(items.size());
    for (const auto& it : items)
        if (!hides(it)) out.push_back(it);
    if (removed) *removed = out.size() != items.size();
    return out;
}

std::string LibraryLock::cacheKey() const {
    auto client = jf::currentClient();
    auto ids = lockedIds();
    std::sort(ids.begin(), ids.end());
    std::string key = client ? client->serverId() + "/" + client->userId() : std::string();
    for (const auto& id : ids) key += ";" + id;
    return key;
}

void LibraryLock::prepare(std::function<void()> done) {
    auto client = jf::currentClient();
    if (!active() || !client) {
        if (done) done();
        return;
    }
    const std::string key = cacheKey();
    if (hiddenKey_ == key) {
        if (done) done();
        return;
    }
    if (done) waiters_.push_back(std::move(done));
    if (loadingKey_ == key) return;
    loadingKey_ = key;

    const auto ids = lockedIds();
    auto found = std::make_shared<std::set<std::string>>();
    auto remaining = std::make_shared<size_t>(ids.size());
    auto failed = std::make_shared<bool>(false);
    for (const auto& id : ids) {
        found->insert(id);
        jf::ItemsQuery q;
        q.parentId(id)
            .recursive()
            .includeItemTypes("Movie,Series,Video,MusicVideo,BoxSet")
            .fields("ParentId")
            .enableUserData(false)
            .enableImageTypes("")
            .param("EnableImages", "false");
        client->items(q).then(
            [this, key, found, remaining, failed](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
                if (r) {
                    for (const auto& it : r->items) found->insert(it.id);
                } else {
                    *failed = true;
                    log::warn("lock", "couldn't list a locked library: {}", r.error.describe());
                }
                if (--*remaining > 0 || loadingKey_ != key) return;
                loadingKey_.clear();
                hidden_ = std::move(*found);
                // On failure the partial list is used now and the next prepare() tries again.
                hiddenKey_ = *failed ? std::string() : key;
                auto waiters = std::move(waiters_);
                waiters_.clear();
                for (auto& w : waiters) w();
            },
            token_);
    }
}

void LibraryLock::changed() {
    ++revision_;
    hiddenKey_.clear();
    loadingKey_.clear();
    token_.cancel();
    token_.reset();
    // Views waiting on the old list wait for the new one (or run now when nothing is locked).
    auto waiters = std::move(waiters_);
    waiters_.clear();
    for (auto& w : waiters) prepare(std::move(w));
}

bool LibraryLock::verify(const std::string& password) const {
    const std::string stored = Settings::instance().get<std::string>(kHashKey, "");
    const size_t sep = stored.find(':');
    if (sep == std::string::npos) return false;
    return hashPassword(stored.substr(0, sep), password) == stored.substr(sep + 1);
}

void LibraryLock::storePassword(const std::string& password) {
    const std::string salt = randomSalt();
    Settings::instance().set(kHashKey, salt + ":" + hashPassword(salt, password));
    Settings::instance().saveIfDirty(0.0);
}

void LibraryLock::askPassword(std::string header, std::string sub, std::function<void()> onCorrect) {
    requestText(passwordField(std::move(header), std::move(sub)), [this, onCorrect](std::string text) {
        if (!verify(text)) {
            toast("Wrong password", ToastKind::Error, gfx::Icon::Lock);
            return;
        }
        if (onCorrect) onCorrect();
    });
}

void LibraryLock::askNewPassword(std::function<void()> onSet) {
    requestText(passwordField("New lock password", std::format("At least {} characters", kMinPassword)),
                [this, onSet](std::string first) {
                    if (first.size() < kMinPassword) {
                        toast(std::format("Use at least {} characters", kMinPassword), ToastKind::Warning,
                              gfx::Icon::Lock);
                        return;
                    }
                    requestText(passwordField("Confirm password", "Enter the same password again"),
                                [this, first, onSet](std::string second) {
                                    if (second != first) {
                                        toast("Passwords don't match", ToastKind::Error, gfx::Icon::Lock);
                                        return;
                                    }
                                    storePassword(first);
                                    if (onSet) onSet();
                                });
                });
}

void LibraryLock::requestUnlock(std::function<void()> onUnlocked) {
    if (!active()) {
        if (onUnlocked) onUnlocked();
        return;
    }
    askPassword("Locked library", "Enter the password to unlock", [this, onUnlocked] {
        unlocked_ = true;
        changed();
        toast("Libraries unlocked until you lock them or restart", ToastKind::Success, gfx::Icon::Lock);
        if (onUnlocked) onUnlocked();
    });
}

void LibraryLock::lockNow() {
    if (unlocked_) {
        unlocked_ = false;
        changed();
    }
    toast("Locked libraries are hidden", ToastKind::Success, gfx::Icon::Lock);
}

void LibraryLock::toggleLibrary(const jf::BaseItem& library, std::function<void()> onChanged) {
    const std::string id = library.id;
    if (isLocked(id) && hasPassword()) {
        askPassword("Remove lock", "Enter the password to unlock " + library.name, [this, id, onChanged] {
            auto ids = lockedIds();
            std::erase(ids, id);
            setLockedIds(ids);
            changed();
            toast("Lock removed", ToastKind::Success, gfx::Icon::Lock);
            if (onChanged) onChanged();
        });
        return;
    }
    auto lock = [this, id, onChanged] {
        auto ids = lockedIds();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        setLockedIds(ids);
        // Locking something means it should be hidden now, not after the next restart.
        unlocked_ = false;
        changed();
        toast("Library locked", ToastKind::Success, gfx::Icon::Lock);
        if (onChanged) onChanged();
    };
    if (hasPassword()) {
        lock();
        return;
    }
    present(Dialog::confirm("Lock " + library.name + "?",
                            "Set a password first. It's needed to open locked libraries, and their titles are left "
                            "out of Home and Search until you unlock them.",
                            "Set password", "Cancel", [this, lock](bool ok) {
                                if (ok) askNewPassword(lock);
                            }));
}

void LibraryLock::changePassword(std::function<void()> onDone) {
    auto setNew = [this, onDone] {
        askNewPassword([onDone] {
            toast("Lock password saved", ToastKind::Success, gfx::Icon::Lock);
            if (onDone) onDone();
        });
    };
    if (!hasPassword()) {
        setNew();
        return;
    }
    askPassword("Change password", "Enter the current password", setNew);
}

void LibraryLock::removePassword(std::function<void()> onDone) {
    if (!hasPassword()) return;
    askPassword("Remove password", "Every library lock is removed too", [this, onDone] {
        Settings::instance().remove(kHashKey);
        setLockedIds({});
        unlocked_ = false;
        hidden_.clear();
        changed();
        toast("Library lock turned off", ToastKind::Success, gfx::Icon::Lock);
        if (onDone) onDone();
    });
}

}  // namespace karafin::screens
