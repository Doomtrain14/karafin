#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace karafin {

// Persistent JSON settings in sdmc:/switch/karafin/settings.json.
//
// Keys are dotted paths into a nested object: "player.subtitle_lang" -> {"player":{"subtitle_lang":..}}.
// Each module owns its own top-level prefix: "app.", "ui.", "jellyfin.", "player.", "subs.",
// "downloads.". get/set are thread-safe; writes are debounced and flushed by the app loop
// (saveIfDirty), and always on shutdown.
//
//   auto lang = Settings::instance().get<std::string>("subs.preferred_lang", "eng");
//   Settings::instance().set("player.hwdec", true);
class Settings {
public:
    static Settings& instance();

    bool load(const std::string& path);
    // Thread-safe; concurrent saves are serialized.
    bool save();
    // Saves when there are changes older than `debounceSeconds`.
    void saveIfDirty(double debounceSeconds = 1.0);
    // True when there are changes older than `debounceSeconds`. App checks this once per frame
    // and runs save() on a worker so the SD card write never stalls a frame.
    bool saveDue(double debounceSeconds = 1.0) const;

    template <class T>
    T get(std::string_view key, T fallback) const {
        std::lock_guard lock(mutex_);
        const nlohmann::json* node = find(key);
        if (!node || node->is_null()) return fallback;
        try {
            return node->get<T>();
        } catch (const nlohmann::json::exception&) {
            return fallback;
        }
    }

    // Convenience overload so string literals work as fallbacks.
    std::string get(std::string_view key, const char* fallback) const { return get<std::string>(key, fallback); }

    template <class T>
    void set(std::string_view key, T&& value) {
        setJson(key, nlohmann::json(std::forward<T>(value)));
    }

    void setJson(std::string_view key, nlohmann::json value);
    nlohmann::json getJson(std::string_view key) const;  // null if missing
    bool contains(std::string_view key) const;
    void remove(std::string_view key);
    nlohmann::json snapshot() const;

    // Listeners always run on the main thread: synchronously when set()/remove() is called there,
    // otherwise posted via main_thread::post (so they may run a frame later, and are dropped if
    // the main queue is cleared at shutdown). Returns an id for removeListener. Once
    // removeListener(id) returns on the main thread the listener never runs again (including
    // already-posted notifications and notifications in progress for other listeners).
    using Listener = std::function<void(std::string_view key)>;
    int addListener(Listener listener);
    void removeListener(int id);

private:
    struct ListenerEntry {
        int id = 0;
        Listener fn;
        std::atomic<bool> alive{true};
    };

    Settings() = default;
    const nlohmann::json* find(std::string_view key) const;
    void notify(std::string_view key);
    void notifyNow(std::string_view key);

    mutable std::mutex mutex_;
    std::mutex writeMutex_;  // fs::writeFileAtomic uses a fixed temp path
    nlohmann::json doc_ = nlohmann::json::object();
    std::string path_;
    bool dirty_ = false;
    double dirtySince_ = 0.0;

    std::mutex listenerMutex_;
    std::vector<std::shared_ptr<ListenerEntry>> listeners_;
    int nextListenerId_ = 1;
};

}  // namespace karafin
