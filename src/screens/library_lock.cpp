#include "screens/library_lock.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <random>

#include "core/log.hpp"
#include "core/settings.hpp"
#include "jellyfin/client.hpp"
#include "screens/widgets/pin_pad.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/list_menu.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {

constexpr const char* kCodeKey = "lock.code";
constexpr const char* kLibrariesKey = "lock.libraries";
constexpr int kHashRounds = 20000;

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

}  // namespace

LibraryLock& LibraryLock::instance() {
    static LibraryLock lock;
    return lock;
}

int LibraryLock::codeLength() const {
    int length = 0;
    std::string salt, hash;
    return parseStored(Settings::instance().get<std::string>(kCodeKey, ""), length, salt, hash) ? length : 0;
}

bool LibraryLock::hasCode() const { return codeLength() > 0; }

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
    return hasCode() && !unlocked_.contains(libraryId) && isLocked(libraryId);
}

bool LibraryLock::active() const {
    if (!hasCode()) return false;
    for (const auto& id : lockedIds())
        if (!unlocked_.contains(id)) return true;
    return false;
}

bool LibraryLock::anyUnlocked() const {
    if (!hasCode()) return false;
    for (const auto& id : lockedIds())
        if (unlocked_.contains(id)) return true;
    return false;
}

bool LibraryLock::hides(const jf::BaseItem& item) const {
    if (hidden_.empty() || !active()) return false;
    for (const auto& [library, titles] : hidden_) {
        if (!blocks(library)) continue;
        for (const std::string* id : {&item.id, &item.seriesId, &item.parentId})
            if (!id->empty() && titles.contains(*id)) return true;
    }
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

    // Every locked library is listed (unlocked ones too), so locking one again needs no reload.
    const auto ids = lockedIds();
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

bool LibraryLock::verify(const std::string& code) const {
    int length = 0;
    std::string salt, hash;
    if (!parseStored(Settings::instance().get<std::string>(kCodeKey, ""), length, salt, hash)) return false;
    return static_cast<int>(code.size()) == length && hashCode(salt, code) == hash;
}

void LibraryLock::storeCode(const std::string& code) {
    const std::string salt = randomSalt();
    Settings::instance().set(kCodeKey, std::to_string(code.size()) + ":" + salt + ":" + hashCode(salt, code));
    Settings::instance().remove("lock.password_hash");  // text password from early 1.0.4 builds
    Settings::instance().saveIfDirty(0.0);
}

void LibraryLock::askCode(std::string title, std::string message, std::function<void()> onCorrect) {
    PinPadSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.length = codeLength();
    spec.onSubmit = [this, onCorrect](const std::string& code, std::string& error) {
        if (!verify(code)) {
            error = "Wrong code. Try again.";
            return false;
        }
        if (onCorrect) onCorrect();
        return true;
    };
    present(std::make_unique<PinPad>(std::move(spec)));
}

void LibraryLock::askNewCode(std::function<void()> onSet) {
    PinPadSpec first;
    first.title = "New lock code";
    first.message = "Enter 4 or 6 directions with the D-pad";
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

void LibraryLock::requestUnlock(const jf::BaseItem& library, std::function<void()> onUnlocked) {
    if (!blocks(library.id)) {
        if (onUnlocked) onUnlocked();
        return;
    }
    askCode("Locked library", "Enter the code to open " + library.name, [this, id = library.id, onUnlocked] {
        unlocked_.insert(id);
        ++revision_;
        if (onUnlocked) onUnlocked();
    });
}

void LibraryLock::relock(const std::string& libraryId) {
    if (unlocked_.erase(libraryId) > 0) ++revision_;
    toast("Library locked again", ToastKind::Success, gfx::Icon::Lock);
}

void LibraryLock::lockNow() {
    if (!unlocked_.empty()) {
        unlocked_.clear();
        ++revision_;
    }
    toast("All locked libraries are locked again", ToastKind::Success, gfx::Icon::Lock);
}

void LibraryLock::lockLibrary(const jf::BaseItem& library, std::function<void()> onChanged) {
    auto lock = [this, id = library.id, onChanged] {
        auto ids = lockedIds();
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        setLockedIds(ids);
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
                            "left out of Home and Search while they are locked.",
                            "Choose code", "Cancel", [this, lock](bool ok) {
                                if (ok) askNewCode(lock);
                            }));
}

void LibraryLock::removeLock(const jf::BaseItem& library, std::function<void()> onChanged) {
    askCode("Remove lock", "Enter the code to remove the lock from " + library.name, [this, id = library.id, onChanged] {
        auto ids = lockedIds();
        std::erase(ids, id);
        setLockedIds(ids);
        unlocked_.erase(id);
        changed();
        toast("Lock removed", ToastKind::Success, gfx::Icon::Lock);
        if (onChanged) onChanged();
    });
}

void LibraryLock::libraryActions(const jf::BaseItem& library, std::function<void()> onChanged) {
    if (!hasCode() || !isLocked(library.id)) {
        lockLibrary(library, onChanged);
        return;
    }
    std::vector<ListItem> items;
    if (blocks(library.id)) {
        items.push_back({"Unlock", "Open it until you lock it again or restart Karafin", "", gfx::Icon::Lock, false, true,
                         [this, library, onChanged] { requestUnlock(library, onChanged); }});
    } else {
        items.push_back({"Lock again", "Hide it now; the code is needed to open it", "", gfx::Icon::Lock, false, true,
                         [this, id = library.id, onChanged] {
                             relock(id);
                             if (onChanged) onChanged();
                         }});
    }
    items.push_back({"Remove lock", "Needs the code", "", gfx::Icon::Close, false, true,
                     [this, library, onChanged] { removeLock(library, onChanged); }});
    present(SideSheet::make(library.name, std::move(items)));
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
    askCode("Turn off library lock", "Enter the code. Every library lock is removed too.", [this, onDone] {
        Settings::instance().remove(kCodeKey);
        setLockedIds({});
        unlocked_.clear();
        hidden_.clear();
        changed();
        toast("Library lock turned off", ToastKind::Success, gfx::Icon::Lock);
        if (onDone) onDone();
    });
}

}  // namespace karafin::screens
