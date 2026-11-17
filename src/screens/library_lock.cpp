#include "screens/library_lock.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <memory>
#include <random>

#include "core/log.hpp"
#include "core/settings.hpp"
#include "jellyfin/client.hpp"
#include "screens/widgets/pin_pad.hpp"
#include "ui/actions.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/list_menu.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;
using json = nlohmann::json;

namespace {

constexpr const char* kProfilesKey = "lock.profiles";
// Global keys from 1.0.4, moved into the first profile that signs in.
constexpr const char* kOldCodeKey = "lock.code";
constexpr const char* kOldLibrariesKey = "lock.libraries";
constexpr int kHashRounds = 20000;
constexpr int kTriesPerPause = 5;
constexpr int kFirstPause = 30;   // seconds
constexpr int kLongestPause = 300;

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

std::string hashCode(const std::string& salt, const std::string& code) {
    Digest digest{};
    const std::string seed = salt + code;
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(), digest.data(), 0);
    std::string buf(digest.size() + salt.size(), '\0');
    std::memcpy(buf.data() + digest.size(), salt.data(), salt.size());
    for (int i = 0; i < kHashRounds; ++i) {
        std::memcpy(buf.data(), digest.data(), digest.size());
        mbedtls_sha256(reinterpret_cast<const unsigned char*>(buf.data()), buf.size(), digest.data(), 0);
    }
    return toHex(digest.data(), digest.size());
}

// "<steps>:<salt>:<hash>" -> parts; false when malformed.
bool parseStored(const std::string& stored, int& length, std::string& salt, std::string& hash) {
    const size_t a = stored.find(':');
    const size_t b = a == std::string::npos ? a : stored.find(':', a + 1);
    if (b == std::string::npos) return false;
    const std::string len = stored.substr(0, a);
    if (len != "4" && len != "6") return false;
    length = len[0] - '0';
    salt = stored.substr(a + 1, b - a - 1);
    hash = stored.substr(b + 1);
    return !salt.empty() && !hash.empty();
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// Length of the pause after the n-th batch of wrong codes (n >= 1).
int pauseFor(int batch) { return std::min(kLongestPause, kFirstPause << std::clamp(batch - 1, 0, 4)); }

template <class T>
T field(const json& obj, const char* name, T fallback) {
    const auto it = obj.find(name);
    if (it == obj.end()) return fallback;
    try {
        return it->get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

std::string profileKey(const std::string& serverId, const std::string& userId) {
    return serverId.empty() || userId.empty() ? std::string() : serverId + "/" + userId;
}

}  // namespace

LibraryLock& LibraryLock::instance() {
    static LibraryLock lock;
    return lock;
}

// ---- Stored profiles ----------------------------------------------------------------------------

LibraryLock::Profile LibraryLock::readProfile(const std::string& key, bool migrate) {
    Profile p;
    p.key = key;
    if (key.empty()) return p;
    Settings& s = Settings::instance();
    const json all = s.getJson(kProfilesKey);
    if (!all.is_object() || !all.contains(key)) {
        if (migrate && (s.contains(kOldCodeKey) || s.contains(kOldLibrariesKey))) {
            if (!parseStored(s.get<std::string>(kOldCodeKey, ""), p.length, p.salt, p.hash)) p.length = 0;
            const json ids = s.getJson(kOldLibrariesKey);
            if (ids.is_array())
                for (const auto& v : ids)
                    if (v.is_string()) p.libraries.insert(v.get<std::string>());
            s.remove(kOldCodeKey);
            s.remove(kOldLibrariesKey);
            s.remove("lock.password_hash");  // text password from early 1.0.4 builds
            writeProfile(p);
            log::info("lock", "moved the library lock settings to profile {}", key);
        }
        return p;
    }
    const json& e = all[key];
    if (!e.is_object()) return p;
    if (!parseStored(field<std::string>(e, "code", ""), p.length, p.salt, p.hash)) p.length = 0;
    if (const auto it = e.find("libraries"); it != e.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string()) p.libraries.insert(v.get<std::string>());
    p.askOnOpen = p.length > 0 && field<bool>(e, "askOnOpen", false);
    p.failures = std::max(0, field<int>(e, "failures", 0));
    p.lockoutUntil = field<int64_t>(e, "lockoutUntil", 0);
    return p;
}

void LibraryLock::writeProfile(const Profile& p) {
    if (p.key.empty()) return;
    Settings& s = Settings::instance();
    json all = s.getJson(kProfilesKey);
    if (!all.is_object()) all = json::object();
    const bool empty = p.length == 0 && p.libraries.empty() && p.failures == 0;
    if (empty) {
        all.erase(p.key);
    } else {
        json e = json::object();
        if (p.length > 0) e["code"] = std::to_string(p.length) + ":" + p.salt + ":" + p.hash;
        if (!p.libraries.empty()) e["libraries"] = std::vector<std::string>(p.libraries.begin(), p.libraries.end());
        if (p.askOnOpen) e["askOnOpen"] = true;
        if (p.failures > 0) e["failures"] = p.failures;
        if (p.lockoutUntil > 0) e["lockoutUntil"] = p.lockoutUntil;
        all[p.key] = std::move(e);
    }
    if (all.empty()) s.remove(kProfilesKey);
    else s.setJson(kProfilesKey, std::move(all));
    s.saveIfDirty(0.0);
}

int LibraryLock::lockoutLeft(const Profile& p) {
    const int64_t left = p.lockoutUntil - nowSeconds();
    if (p.lockoutUntil <= 0 || left <= 0) return 0;
    // A clock set back can't stretch the pause beyond its length.
    return static_cast<int>(std::min<int64_t>(left, pauseFor(std::max(1, p.failures / kTriesPerPause))));
}

bool LibraryLock::checkCode(Profile& p, const std::string& code, std::string& error) {
    if (lockoutLeft(p) > 0) {
        error = "Too many tries. Wait a moment.";
        return false;
    }
    if (p.length > 0 && static_cast<int>(code.size()) == p.length && hashCode(p.salt, code) == p.hash) {
        if (p.failures > 0 || p.lockoutUntil > 0) {
            p.failures = 0;
            p.lockoutUntil = 0;
            writeProfile(p);
        }
        return true;
    }
    ++p.failures;
    const int left = kTriesPerPause - p.failures % kTriesPerPause;
    if (left == kTriesPerPause) {
        p.lockoutUntil = nowSeconds() + pauseFor(p.failures / kTriesPerPause);
        error = "Wrong code.";
    } else if (left <= 2) {
        error = left == 1 ? "Wrong code. 1 more try before a pause." : "Wrong code. 2 more tries before a pause.";
    } else {
        error = "Wrong code. Try again.";
    }
    writeProfile(p);
    return false;
}

// ---- Current profile ----------------------------------------------------------------------------

void LibraryLock::sync() const {
    auto client = jf::currentClient();
    if (synced_ && !client_.owner_before(client) && !client.owner_before(client_)) return;
    synced_ = true;
    client_ = client;
    const std::string key = client ? profileKey(client->serverId(), client->userId()) : std::string();
    if (key == profile_.key && !key.empty()) return;
    profile_ = readProfile(key, true);
    unlocked_.clear();
    hidden_.clear();
    hiddenKey_.clear();
    loadingKey_.clear();
    token_.cancel();
    token_.reset();
    // The stack is reset on a profile switch, so nobody is left waiting for the old titles.
    waiters_.clear();
    ++revision_;
}

uint64_t LibraryLock::revision() const {
    sync();
    return revision_;
}

bool LibraryLock::hasCode() const {
    sync();
    return profile_.length > 0;
}

bool LibraryLock::isLocked(const std::string& libraryId) const {
    sync();
    return profile_.libraries.contains(libraryId);
}

bool LibraryLock::blocksCached(const std::string& libraryId) const {
    return profile_.length > 0 && !unlocked_.contains(libraryId) && profile_.libraries.contains(libraryId);
}

bool LibraryLock::blocks(const std::string& libraryId) const {
    sync();
    return blocksCached(libraryId);
}

bool LibraryLock::activeCached() const {
    if (profile_.length == 0) return false;
    for (const auto& id : profile_.libraries)
        if (!unlocked_.contains(id)) return true;
    return false;
}

bool LibraryLock::active() const {
    sync();
    return activeCached();
}

bool LibraryLock::anyUnlocked() const {
    sync();
    if (profile_.length == 0) return false;
    for (const auto& id : profile_.libraries)
        if (unlocked_.contains(id)) return true;
    return false;
}

bool LibraryLock::hidesCached(const jf::BaseItem& item) const {
    for (const auto& [library, titles] : hidden_) {
        if (!blocksCached(library)) continue;
        for (const std::string* id : {&item.id, &item.seriesId, &item.parentId})
            if (!id->empty() && titles.contains(*id)) return true;
    }
    return false;
}

bool LibraryLock::hides(const jf::BaseItem& item) const {
    sync();
    return !hidden_.empty() && activeCached() && hidesCached(item);
}

std::vector<jf::BaseItem> LibraryLock::filter(const std::vector<jf::BaseItem>& items, bool* removed) const {
    sync();
    if (hidden_.empty() || !activeCached()) {
        if (removed) *removed = false;
        return items;
    }
    std::vector<jf::BaseItem> out;
    out.reserve(items.size());
    for (const auto& it : items)
        if (!hidesCached(it)) out.push_back(it);
    if (removed) *removed = out.size() != items.size();
    return out;
}

std::string LibraryLock::cacheKey() const {
    std::string key = profile_.key;
    for (const auto& id : profile_.libraries) key += ";" + id;
    return key;
}

void LibraryLock::prepare(std::function<void()> done) {
    sync();
    auto client = jf::currentClient();
    if (!activeCached() || !client) {
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

    // Every locked library is listed (unlocked ones too), so locking one again needs no reload.
    const std::set<std::string> ids = profile_.libraries;
    auto found = std::make_shared<std::map<std::string, std::set<std::string>>>();
    auto remaining = std::make_shared<size_t>(ids.size());
    auto failed = std::make_shared<bool>(false);
    for (const auto& id : ids) {
        (*found)[id].insert(id);
        jf::ItemsQuery q;
        q.parentId(id)
            .recursive()
            .includeItemTypes("Movie,Series,Video,MusicVideo,BoxSet")
            .fields("ParentId")
            .enableUserData(false)
            .enableImageTypes("")
            .param("EnableImages", "false");
        client->items(q).then(
            [this, id, key, found, remaining, failed](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
                if (r) {
                    for (const auto& it : r->items) (*found)[id].insert(it.id);
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

// ---- Code entry ---------------------------------------------------------------------------------

void LibraryLock::storeCode(const std::string& code) {
    sync();
    profile_.length = static_cast<int>(code.size());
    profile_.salt = randomSalt();
    profile_.hash = hashCode(profile_.salt, code);
    profile_.failures = 0;
    profile_.lockoutUntil = 0;
    writeProfile(profile_);
}

void LibraryLock::askCode(const std::string& key, int length, std::string title, std::string message,
                          std::function<void()> onCorrect, std::function<void()> onCancel) {
    PinPadSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.length = length;
    spec.lockout = [this, key] {
        sync();
        return lockoutLeft(key == profile_.key ? profile_ : readProfile(key, false));
    };
    spec.onSubmit = [this, key, onCorrect](const std::string& code, std::string& error) {
        sync();
        const bool current = key == profile_.key;
        Profile p = current ? profile_ : readProfile(key, false);
        const bool ok = checkCode(p, code, error);
        if (current) {
            profile_.failures = p.failures;
            profile_.lockoutUntil = p.lockoutUntil;
        }
        if (!ok) return false;
        if (onCorrect) onCorrect();
        return true;
    };
    spec.onCancel = std::move(onCancel);
    present(std::make_unique<PinPad>(std::move(spec)));
}

void LibraryLock::askCode(std::string title, std::string message, std::function<void()> onCorrect) {
    sync();
    askCode(profile_.key, profile_.length, std::move(title), std::move(message), std::move(onCorrect), {});
}

void LibraryLock::askNewCode(std::function<void()> onSet) {
    PinPadSpec first;
    first.title = "New lock code";
    first.message = "Enter 4 or 6 directions with the D-pad or the arrows";
    first.onSubmit = [this, onSet](const std::string& code, std::string&) {
        PinPadSpec confirm;
        confirm.title = "Confirm code";
        confirm.message = "Enter the same code again";
        confirm.length = static_cast<int>(code.size());
        confirm.onSubmit = [this, code, onSet](const std::string& again, std::string& error) {
            if (again != code) {
                error = "Codes don't match. Try again.";
                return false;
            }
            storeCode(code);
            if (onSet) onSet();
            return true;
        };
        present(std::make_unique<PinPad>(std::move(confirm)));
        return true;
    };
    present(std::make_unique<PinPad>(std::move(first)));
}

// ---- Library actions ----------------------------------------------------------------------------

void LibraryLock::requestUnlock(const jf::BaseItem& library, std::function<void()> onUnlocked) {
    if (!blocks(library.id)) {
        if (onUnlocked) onUnlocked();
        return;
    }
    askCode("Locked library", "Enter the code to open " + library.name, [this, id = library.id, onUnlocked] {
        sync();
        unlocked_.insert(id);
        ++revision_;
        if (onUnlocked) onUnlocked();
    });
}

void LibraryLock::relock(const std::string& libraryId) {
    sync();
    if (unlocked_.erase(libraryId) > 0) ++revision_;
    toast("Library locked again", ToastKind::Success, gfx::Icon::Lock);
}

void LibraryLock::lockNow() {
    sync();
    if (!unlocked_.empty()) {
        unlocked_.clear();
        ++revision_;
    }
    toast("All locked libraries are locked again", ToastKind::Success, gfx::Icon::Lock);
}

void LibraryLock::lockLibrary(const jf::BaseItem& library, std::function<void()> onChanged) {
    auto lock = [this, id = library.id, onChanged] {
        sync();
        profile_.libraries.insert(id);
        writeProfile(profile_);
        unlocked_.erase(id);
        changed();
        toast("Library locked", ToastKind::Success, gfx::Icon::Lock);
        if (onChanged) onChanged();
    };
    if (hasCode()) {
        lock();
        return;
    }
    present(Dialog::confirm("Lock " + library.name + "?",
                            "Choose a D-pad code first. It's needed to open locked libraries, and their titles are "
                            "left out of Home, Search and other lists while they are locked. The code and locks are "
                            "for this profile only.",
                            "Choose code", "Cancel", [this, lock](bool ok) {
                                if (ok) askNewCode(lock);
                            }));
}

void LibraryLock::removeLock(const jf::BaseItem& library, std::function<void()> onChanged) {
    askCode("Remove lock", "Enter the code to remove the lock from " + library.name, [this, id = library.id, onChanged] {
        sync();
        profile_.libraries.erase(id);
        writeProfile(profile_);
        unlocked_.erase(id);
        changed();
        toast("Lock removed", ToastKind::Success, gfx::Icon::Lock);
        if (onChanged) onChanged();
    });
}

void LibraryLock::addLibraryActions(ActionMenu& m, const jf::BaseItem& library, std::function<void()> onChanged) {
    if (!hasCode() || !isLocked(library.id)) {
        m.add("Lock library", gfx::Icon::Lock, [this, library, onChanged] { lockLibrary(library, onChanged); });
        return;
    }
    if (blocks(library.id)) {
        Action a;
        a.title = "Unlock";
        a.subtitle = "Open it until you lock it again or restart Karafin";
        a.icon = gfx::Icon::Lock;
        a.run = [this, library, onChanged] { requestUnlock(library, onChanged); };
        m.add(std::move(a));
    } else {
        Action a;
        a.title = "Lock again";
        a.subtitle = "Hide it now; the code is needed to open it";
        a.icon = gfx::Icon::Lock;
        a.run = [this, id = library.id, onChanged] {
            relock(id);
            if (onChanged) onChanged();
        };
        m.add(std::move(a));
    }
    Action remove;
    remove.title = "Remove lock";
    remove.subtitle = "Needs the code";
    remove.icon = gfx::Icon::Close;
    remove.destructive = true;
    remove.run = [this, library, onChanged] { removeLock(library, onChanged); };
    m.add(std::move(remove));
}

void LibraryLock::changeCode(std::function<void()> onDone) {
    auto setNew = [this, onDone] {
        askNewCode([onDone] {
            toast("Lock code saved", ToastKind::Success, gfx::Icon::Lock);
            if (onDone) onDone();
        });
    };
    if (!hasCode()) {
        setNew();
        return;
    }
    askCode("Change code", "Enter the current code", setNew);
}

void LibraryLock::removeCode(std::function<void()> onDone) {
    if (!hasCode()) return;
    askCode("Turn off lock code", "Enter the code. Every library lock and the profile code are removed too.",
            [this, onDone] {
                sync();
                profile_ = Profile{.key = profile_.key};
                writeProfile(profile_);
                unlocked_.clear();
                hidden_.clear();
                changed();
                toast("Lock code turned off", ToastKind::Success, gfx::Icon::Lock);
                if (onDone) onDone();
            });
}

// ---- Profile code -------------------------------------------------------------------------------

bool LibraryLock::asksOnOpen() const {
    sync();
    return profile_.length > 0 && profile_.askOnOpen;
}

void LibraryLock::setAsksOnOpen(bool on, std::function<void()> onDone) {
    if (!hasCode() || asksOnOpen() == on) return;
    askCode(on ? "Ask for code" : "Stop asking for code",
            on ? "Enter the code. It will be needed to open this profile." : "Enter the code to stop asking for it",
            [this, on, onDone] {
                sync();
                profile_.askOnOpen = on;
                writeProfile(profile_);
                toast(on ? "This profile now asks for the code" : "This profile opens without the code",
                      ToastKind::Success, gfx::Icon::Lock);
                if (onDone) onDone();
            });
}

bool LibraryLock::profileNeedsCode(const std::string& serverId, const std::string& userId) {
    const Profile p = readProfile(profileKey(serverId, userId), false);
    return p.length > 0 && p.askOnOpen;
}

void LibraryLock::askProfileCode(const std::string& serverId, const std::string& userId, const std::string& name,
                                 std::function<void()> onCorrect, std::function<void()> onCancel) {
    const Profile p = readProfile(profileKey(serverId, userId), false);
    if (p.length == 0 || !p.askOnOpen) {
        if (onCorrect) onCorrect();
        return;
    }
    askCode(p.key, p.length, "Profile locked", "Enter the code to open " + name, std::move(onCorrect),
            std::move(onCancel));
}

}  // namespace karafin::screens
