#pragma once

// libmpv core + OpenGL render context, owned by the player screen. Main thread only.
//
// mpv's wakeup and render-update callbacks fire on mpv threads; they only post a (coalesced)
// task to the main thread, which drains the event queue without blocking and calls
// mpv_render_context_update(). Destruction frees the render context on the main thread (GL
// context current) and hands the core to a background thread for mpv_terminate_destroy, so a
// stalled network read never freezes the UI.

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <mpv/client.h>
#include <mpv/render.h>

#include "gfx/renderer.hpp"

namespace karafin::player {

struct MpvOptions {
    bool hwdec = true;
    bool appletMode = false;
    std::string userAgent;
    std::vector<std::string> httpHeaders;  // "Name: value" (commas allowed)
    std::string caFile;
    bool verifyTls = false;
    std::string subFont;       // family name
    std::string subFontsDir;   // directory with .ttf files for libass ("" = none)
    double subScale = 1.0;
    int subPos = 100;
    bool subBackground = false;
};

class Mpv {
public:
    static std::unique_ptr<Mpv> create(const MpvOptions& options, std::string* error);
    ~Mpv();
    Mpv(const Mpv&) = delete;
    Mpv& operator=(const Mpv&) = delete;

    // Every event, delivered on the main thread from processEvents().
    std::function<void(const mpv_event&)> onEvent;

    // Draws the current video frame into the renderer's external target. Main thread, inside
    // Renderer::beginExternal() / endExternal().
    void render(const gfx::Renderer::ExternalTarget& target);
    // True once mpv reported at least one video frame since the last loadfile.
    bool hasFrame() const { return hasFrame_; }
    void resetFrame() { hasFrame_ = false; }

    // Commands. Synchronous commands must be cheap (no network): set/cycle/seek/stop.
    int command(std::initializer_list<const char*> args);
    // Result arrives as MPV_EVENT_COMMAND_REPLY with reply_userdata == id.
    int commandAsync(uint64_t id, const std::vector<std::string>& args);

    int setString(const char* name, const std::string& value);
    int setFlag(const char* name, bool value);
    int setDouble(const char* name, double value);
    int setInt(const char* name, int64_t value);
    std::optional<double> getDouble(const char* name) const;
    std::optional<int64_t> getInt(const char* name) const;
    std::optional<bool> getFlag(const char* name) const;
    std::string getString(const char* name) const;

    void observe(uint64_t id, const char* name, mpv_format format);

    // Drains pending events (non-blocking). Called from the posted wakeup task.
    void processEvents();

    mpv_handle* handle() const { return mpv_; }

    // Waits (bounded) until every destroyed core finished mpv_terminate_destroy on its reaper
    // thread. For app shutdown, so mpv's threads are gone before sockets/HTTP are torn down.
    static bool waitForTeardown(int timeoutMs);

private:
    struct Shared;
    Mpv() = default;
    bool init(const MpvOptions& options, std::string* error);
    void setOption(const char* name, const std::string& value);
    static void onWakeup(void* ctx);
    static void onRenderUpdate(void* ctx);
    void handleRenderUpdate();

    mpv_handle* mpv_ = nullptr;
    mpv_render_context* render_ = nullptr;
    std::shared_ptr<Shared> shared_;
    bool hasFrame_ = false;
};

// Helpers for mpv_node trees (track-list, chapter-list, demuxer-cache-state).
const mpv_node* nodeMapGet(const mpv_node& map, const char* key);
std::string nodeString(const mpv_node* n, const std::string& fallback = {});
int64_t nodeInt(const mpv_node* n, int64_t fallback = 0);
double nodeDouble(const mpv_node* n, double fallback = 0);
bool nodeFlag(const mpv_node* n, bool fallback = false);

}  // namespace karafin::player
