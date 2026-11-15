#include "core/settings.hpp"

#include <chrono>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"

namespace karafin {
namespace {

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> splitKey(std::string_view key) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= key.size()) {
        const size_t dot = key.find('.', start);
        const size_t end = dot == std::string_view::npos ? key.size() : dot;
        if (end > start) parts.emplace_back(key.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

}  // namespace

Settings& Settings::instance() {
    static Settings s;
    return s;
}

bool Settings::load(const std::string& path) {
    std::lock_guard lock(mutex_);
    path_ = path;
    auto text = fs::readFileAtomic(path);
    if (!text) {
        log::info("settings", "no settings file at {}, using defaults", path);
        doc_ = nlohmann::json::object();
        return false;
    }
    auto parsed = nlohmann::json::parse(*text, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
    if (parsed.is_discarded() || !parsed.is_object()) {
        log::warn("settings", "{} is corrupt; keeping a backup and starting fresh", path);
        fs::writeFileAtomic(path + ".bad", *text);
        doc_ = nlohmann::json::object();
        return false;
    }
    doc_ = std::move(parsed);
    log::info("settings", "loaded {}", path);
    return true;
}

bool Settings::save() {
    std::lock_guard writeLock(writeMutex_);
    std::string text;
    std::string path;
    {
        std::lock_guard lock(mutex_);
        if (path_.empty()) return false;
        text = doc_.dump(2);
        path = path_;
        dirty_ = false;
    }
    if (!fs::writeFileAtomic(path, text)) {
        log::error("settings", "failed to write {}", path);
        std::lock_guard lock(mutex_);
        dirty_ = true;
        dirtySince_ = nowSeconds();
        return false;
    }
    return true;
}

void Settings::saveIfDirty(double debounceSeconds) {
    if (saveDue(debounceSeconds)) save();
}

bool Settings::saveDue(double debounceSeconds) const {
    std::lock_guard lock(mutex_);
    return dirty_ && nowSeconds() - dirtySince_ >= debounceSeconds;
}

const nlohmann::json* Settings::find(std::string_view key) const {
    const nlohmann::json* node = &doc_;
    for (const auto& part : splitKey(key)) {
        if (!node->is_object()) return nullptr;
        auto it = node->find(part);
        if (it == node->end()) return nullptr;
        node = &*it;
    }
    return node;
}

void Settings::setJson(std::string_view key, nlohmann::json value) {
    {
        std::lock_guard lock(mutex_);
        nlohmann::json* node = &doc_;
        for (const auto& part : splitKey(key)) {
            if (!node->is_object()) *node = nlohmann::json::object();
            node = &(*node)[part];
        }
        if (*node == value) return;
        *node = std::move(value);
        if (!dirty_) dirtySince_ = nowSeconds();
        dirty_ = true;
    }
    notify(key);
}

nlohmann::json Settings::getJson(std::string_view key) const {
    std::lock_guard lock(mutex_);
    const nlohmann::json* node = find(key);
    return node ? *node : nlohmann::json();
}

bool Settings::contains(std::string_view key) const {
    std::lock_guard lock(mutex_);
    return find(key) != nullptr;
}

void Settings::remove(std::string_view key) {
    {
        std::lock_guard lock(mutex_);
        auto parts = splitKey(key);
        if (parts.empty()) return;
        nlohmann::json* node = &doc_;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            if (!node->is_object() || !node->contains(parts[i])) return;
            node = &(*node)[parts[i]];
        }
        if (!node->is_object() || node->erase(parts.back()) == 0) return;
        if (!dirty_) dirtySince_ = nowSeconds();
        dirty_ = true;
    }
    notify(key);
}

nlohmann::json Settings::snapshot() const {
    std::lock_guard lock(mutex_);
    return doc_;
}

int Settings::addListener(Listener listener) {
    auto entry = std::make_shared<ListenerEntry>();
    entry->fn = std::move(listener);
    std::lock_guard lock(listenerMutex_);
    entry->id = nextListenerId_++;
    listeners_.push_back(entry);
    return entry->id;
}

void Settings::removeListener(int id) {
    std::lock_guard lock(listenerMutex_);
    std::erase_if(listeners_, [id](const auto& l) {
        if (l->id != id) return false;
        l->alive.store(false);
        return true;
    });
}

void Settings::notify(std::string_view key) {
    if (main_thread::isCurrent()) {
        notifyNow(key);
        return;
    }
    {
        std::lock_guard lock(listenerMutex_);
        if (listeners_.empty()) return;
    }
    main_thread::post([this, k = std::string(key)] { notifyNow(k); });
}

void Settings::notifyNow(std::string_view key) {
    std::vector<std::shared_ptr<ListenerEntry>> copy;
    {
        std::lock_guard lock(listenerMutex_);
        copy = listeners_;
    }
    for (auto& l : copy)
        if (l->alive.load()) l->fn(key);
}

}  // namespace karafin
