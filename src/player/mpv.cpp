#include "player/mpv.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include <GLES3/gl3.h>
#include <mpv/render_gl.h>

#include "core/app.hpp"
#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/thread.hpp"
#include "player/swipe.hpp"

namespace karafin::player {

struct Mpv::Shared : std::enable_shared_from_this<Mpv::Shared> {
    Mpv* self = nullptr;  // main thread only
    std::atomic<bool> wakePending{false};
    std::atomic<bool> updatePending{false};
};

namespace {

// mpv_terminate_destroy() can block until a network read times out; run it off the main thread.
class Reaper {
public:
    ~Reaper() {
        std::lock_guard lock(mutex_);
        for (auto& e : threads_) e.thread->join();
    }
    void destroy(mpv_handle* handle) {
        std::lock_guard lock(mutex_);
        std::erase_if(threads_, [](Entry& e) {
            if (!e.done->load()) return false;
            e.thread->join();
            return true;
        });
        auto done = std::make_shared<std::atomic<bool>>(false);
        Entry e;
        e.done = done;
        e.thread = std::make_unique<Thread>(ThreadOptions{"mpv-reap", 128 * 1024, kPriorityBackground, -1},
                                            [handle, done] {
                                                mpv_terminate_destroy(handle);
                                                done->store(true);
                                            });
        threads_.push_back(std::move(e));
    }
    bool waitIdle(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeoutMs));
        for (;;) {
            {
                std::lock_guard lock(mutex_);
                std::erase_if(threads_, [](Entry& e) {
                    if (!e.done->load()) return false;
                    e.thread->join();
                    return true;
                });
                if (threads_.empty()) return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
            sleepMs(10);
        }
    }

private:
    struct Entry {
        std::unique_ptr<Thread> thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex mutex_;
    std::vector<Entry> threads_;
};

Reaper& reaper() {
    static Reaper r;
    return r;
}

log::Level mpvLevel(const char* level) {
    if (!level) return log::Level::Info;
    if (!std::strcmp(level, "fatal") || !std::strcmp(level, "error")) return log::Level::Error;
    if (!std::strcmp(level, "warn")) return log::Level::Warn;
    if (!std::strcmp(level, "info")) return log::Level::Info;
    if (!std::strcmp(level, "v") || !std::strcmp(level, "debug")) return log::Level::Debug;
    return log::Level::Trace;
}

// Hardware decoder selection (before mpv_initialize). nvtegra frames are copied back to RAM:
// this libmpv has no GL interop for them.
void configureHwdec(mpv_handle* mpv, bool enabled) {
    if (!enabled) {
        mpv_set_option_string(mpv, "hwdec", "no");
        log::info("player", "hwdec=no (disabled in settings)");
        return;
    }
    bool set = false;
    for (const char* candidate : {"nvtegra-copy", "nvtegra", "auto-copy"}) {
        if (mpv_set_option_string(mpv, "hwdec", candidate) >= 0) {
            log::info("player", "hwdec={}", candidate);
            set = true;
            break;
        }
    }
    if (!set) log::warn("player", "no hardware decoder accepted; using software decoding");
    mpv_set_option_string(mpv, "hwdec-codecs", "h264,hevc,vp8,vp9,mpeg1video,mpeg2video,mpeg4,vc1,wmv3");
}

}  // namespace

bool Mpv::waitForTeardown(int timeoutMs) { return reaper().waitIdle(timeoutMs); }

std::unique_ptr<Mpv> Mpv::create(const MpvOptions& options, std::string* error) {
    std::unique_ptr<Mpv> m(new Mpv());
    if (!m->init(options, error)) return nullptr;
    return m;
}

void Mpv::setOption(const char* name, const std::string& value) {
    const int rc = mpv_set_option_string(mpv_, name, value.c_str());
    if (rc < 0) log::warn("player", "mpv option {}={} rejected: {}", name, value, mpv_error_string(rc));
}

bool Mpv::init(const MpvOptions& o, std::string* error) {
    mpv_ = mpv_create();
    if (!mpv_) {
        if (error) *error = "mpv_create failed";
        return false;
    }
    shared_ = std::make_shared<Shared>();
    shared_->self = this;

    setOption("config", "no");
    setOption("terminal", "no");
    setOption("input-default-bindings", "no");
    setOption("input-vo-keyboard", "no");
    setOption("osc", "no");
    setOption("osd-level", "0");
    setOption("load-scripts", "no");
    setOption("ytdl", "no");
    setOption("idle", "yes");
    setOption("keep-open", "yes");
    setOption("vo", "libmpv");
    setOption("ao", "hos");
    setOption("audio-channels", "auto-safe");
    setOption("volume-max", std::to_string(PlaybackSwipe::kMaxVolume));

    configureHwdec(mpv_, o.hwdec);
    setOption("vd-lavc-threads", "3");
    setOption("vd-lavc-skiploopfilter", "nonref");
    // Direct rendering needs MPV_RENDER_PARAM_ADVANCED_CONTROL, which we can't enable: the main
    // (render) thread calls synchronous client APIs.
    setOption("vd-lavc-dr", "no");

    // Rendering: cheapest scalers, the Tegra GPU is shared with the UI.
    setOption("scale", "bilinear");
    setOption("dscale", "bilinear");
    setOption("cscale", "bilinear");
    setOption("dither-depth", "no");
    setOption("correct-downscaling", "no");
    setOption("linear-downscaling", "no");
    setOption("sigmoid-upscaling", "no");
    setOption("deband", "no");
    setOption("hdr-compute-peak", "no");
    setOption("video-sync", "audio");
    // We render without blocking (BLOCK_FOR_TARGET_TIME=0); a zero offset hands frames over at
    // their display time instead of 50 ms early.
    setOption("video-timing-offset", "0");
    // Avoids random GPU crashes with mesa/nouveau on the Switch (one GPU sync per video render).
    setOption("opengl-glfinish", "yes");
    setOption("hr-seek", "yes");
    setOption("hr-seek-framedrop", "yes");

    // Network + cache sized for the Switch's memory (much less in applet mode).
    setOption("cache", "yes");
    setOption("cache-on-disk", "no");
    setOption("demuxer-seekable-cache", "yes");
    setOption("demuxer-max-bytes", o.appletMode ? "24MiB" : "80MiB");
    setOption("demuxer-max-back-bytes", o.appletMode ? "4MiB" : "16MiB");
    setOption("demuxer-readahead-secs", o.appletMode ? "20" : "60");
    setOption("cache-pause-wait", "2");
    setOption("network-timeout", "20");
    setOption("hls-bitrate", "max");
    if (!o.userAgent.empty()) setOption("user-agent", o.userAgent);
    if (!o.httpHeaders.empty()) {
        // As a node list so commas inside values (the MediaBrowser header has many) survive.
        std::vector<mpv_node> items(o.httpHeaders.size());
        for (size_t i = 0; i < items.size(); ++i) {
            items[i].format = MPV_FORMAT_STRING;
            items[i].u.string = const_cast<char*>(o.httpHeaders[i].c_str());
        }
        mpv_node_list list{static_cast<int>(items.size()), items.data(), nullptr};
        mpv_node node;
        node.format = MPV_FORMAT_NODE_ARRAY;
        node.u.list = &list;
        const int rc = mpv_set_option(mpv_, "http-header-fields", MPV_FORMAT_NODE, &node);
        if (rc < 0) log::warn("player", "http-header-fields rejected: {}", mpv_error_string(rc));
    }
    if (!o.caFile.empty()) setOption("tls-ca-file", o.caFile);
    setOption("tls-verify", o.verifyTls ? "yes" : "no");

    // Subtitles (libass without fontconfig: fonts come from sub-fonts-dir + embedded fonts).
    setOption("sub-auto", "no");
    setOption("audio-file-auto", "no");
    setOption("sub-font-provider", "none");
    if (!o.subFontsDir.empty()) setOption("sub-fonts-dir", o.subFontsDir);
    if (!o.subFont.empty()) setOption("sub-font", o.subFont);
    setOption("sub-ass-override", "scale");
    setOption("sub-scale", std::to_string(o.subScale));
    setOption("sub-pos", std::to_string(o.subPos));
    setOption("sub-border-size", "2.6");
    setOption("sub-margin-y", "34");  // clear of TV overscan
    if (o.subBackground) {
        // mpv 0.36 switches libass to BorderStyle 4 (box behind each event) when sub-back-color
        // is not fully transparent; the shadow offset is the box padding. mpv >= 0.38 needs
        // sub-border-style for that, which 0.36 doesn't have (rejection is expected there).
        setOption("sub-back-color", "#B0000000");
        setOption("sub-shadow-offset", "4");
        mpv_set_option_string(mpv_, "sub-border-style", "background-box");
    } else {
        // mpv's default shadow color is a translucent light gray.
        setOption("sub-shadow-color", "#80000000");
        setOption("sub-shadow-offset", "1.2");
    }

    mpv_request_log_messages(mpv_, log::enabled(log::Level::Debug) ? "info" : "warn");

    const int rc = mpv_initialize(mpv_);
    if (rc < 0) {
        if (error) *error = std::string("mpv_initialize: ") + mpv_error_string(rc);
        mpv_terminate_destroy(mpv_);
        mpv_ = nullptr;
        return false;
    }
    mpv_set_wakeup_callback(mpv_, &Mpv::onWakeup, shared_.get());

    mpv_opengl_init_params gl{&App::glProcAddress, nullptr};
    // Must stay 0: with advanced control the core may wait on the render thread, and the main
    // thread makes synchronous mpv_command/mpv_get_property calls (deadlock).
    int advanced = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int rrc = mpv_render_context_create(&render_, mpv_, params);
    if (rrc < 0) {
        if (error) *error = std::string("mpv_render_context_create: ") + mpv_error_string(rrc);
        log::error("player", "{}", error ? *error : "render context failed");
        render_ = nullptr;
        return false;  // destructor cleans up mpv_
    }
    mpv_render_context_set_update_callback(render_, &Mpv::onRenderUpdate, shared_.get());
    log::info("player", "mpv {} ready (client API {}.{})", getString("mpv-version"), mpv_client_api_version() >> 16,
              mpv_client_api_version() & 0xFFFF);
    return true;
}

Mpv::~Mpv() {
    if (shared_) shared_->self = nullptr;
    if (render_) {
        mpv_render_context_set_update_callback(render_, nullptr, nullptr);
        mpv_render_context_free(render_);
        render_ = nullptr;
    }
    if (mpv_) {
        mpv_set_wakeup_callback(mpv_, nullptr, nullptr);
        const char* stop[] = {"stop", nullptr};
        mpv_command(mpv_, stop);
        reaper().destroy(mpv_);
        mpv_ = nullptr;
    }
}

void Mpv::onWakeup(void* ctx) {
    auto* s = static_cast<Shared*>(ctx);
    if (s->wakePending.exchange(true)) return;
    main_thread::post([w = s->weak_from_this()] {
        auto sp = w.lock();
        if (!sp) return;
        sp->wakePending.store(false);
        if (sp->self) sp->self->processEvents();
    });
}

void Mpv::onRenderUpdate(void* ctx) {
    auto* s = static_cast<Shared*>(ctx);
    if (s->updatePending.exchange(true)) return;
    main_thread::post([w = s->weak_from_this()] {
        auto sp = w.lock();
        if (!sp) return;
        sp->updatePending.store(false);
        if (sp->self) sp->self->handleRenderUpdate();
    });
}

void Mpv::handleRenderUpdate() {
    if (!render_) return;
    const uint64_t flags = mpv_render_context_update(render_);
    if (flags & MPV_RENDER_UPDATE_FRAME) hasFrame_ = true;
}

void Mpv::render(const gfx::Renderer::ExternalTarget& t) {
    if (!render_ || t.width <= 0 || t.height <= 0) return;
    // FBO 0 is y-up and needs flipping; the renderer's offscreen targets are stored top-down.
    mpv_opengl_fbo fbo{static_cast<int>(t.fbo), t.width, t.height, 0};
    int flip = t.flipY ? 0 : 1;
    int block = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    // mpv only enables blending for passes that need it and would inherit ours on its first
    // pass; an enabled scissor would also clip its intermediate FBO passes.
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    mpv_render_context_render(render_, params);
    // State Renderer::resetState() doesn't cover but texture uploads depend on.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glActiveTexture(GL_TEXTURE0);
}

void Mpv::processEvents() {
    while (mpv_) {
        mpv_event* ev = mpv_wait_event(mpv_, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto* msg = static_cast<const mpv_event_log_message*>(ev->data);
            std::string text = msg->text ? msg->text : "";
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            const log::Level lvl = mpvLevel(msg->level);
            if (log::enabled(lvl)) log::write(lvl, "mpv", std::string(msg->prefix ? msg->prefix : "") + ": " + text);
            continue;
        }
        if (onEvent) onEvent(*ev);
    }
}

int Mpv::command(std::initializer_list<const char*> args) {
    if (!mpv_) return MPV_ERROR_UNINITIALIZED;
    std::vector<const char*> argv(args);
    argv.push_back(nullptr);
    const int rc = mpv_command(mpv_, argv.data());
    if (rc < 0) log::debug("player", "mpv command {} failed: {}", argv.empty() ? "" : argv[0], mpv_error_string(rc));
    return rc;
}

int Mpv::commandAsync(uint64_t id, const std::vector<std::string>& args) {
    if (!mpv_) return MPV_ERROR_UNINITIALIZED;
    std::vector<const char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    const int rc = mpv_command_async(mpv_, id, argv.data());
    if (rc < 0) log::warn("player", "mpv async {} failed: {}", args.empty() ? "" : args[0], mpv_error_string(rc));
    return rc;
}

int Mpv::setString(const char* name, const std::string& value) {
    return mpv_ ? mpv_set_property_string(mpv_, name, value.c_str()) : MPV_ERROR_UNINITIALIZED;
}

int Mpv::setFlag(const char* name, bool value) {
    int v = value ? 1 : 0;
    return mpv_ ? mpv_set_property(mpv_, name, MPV_FORMAT_FLAG, &v) : MPV_ERROR_UNINITIALIZED;
}

int Mpv::setDouble(const char* name, double value) {
    return mpv_ ? mpv_set_property(mpv_, name, MPV_FORMAT_DOUBLE, &value) : MPV_ERROR_UNINITIALIZED;
}

int Mpv::setInt(const char* name, int64_t value) {
    return mpv_ ? mpv_set_property(mpv_, name, MPV_FORMAT_INT64, &value) : MPV_ERROR_UNINITIALIZED;
}

std::optional<double> Mpv::getDouble(const char* name) const {
    double v = 0;
    if (!mpv_ || mpv_get_property(mpv_, name, MPV_FORMAT_DOUBLE, &v) < 0) return std::nullopt;
    return v;
}

std::optional<int64_t> Mpv::getInt(const char* name) const {
    int64_t v = 0;
    if (!mpv_ || mpv_get_property(mpv_, name, MPV_FORMAT_INT64, &v) < 0) return std::nullopt;
    return v;
}

std::optional<bool> Mpv::getFlag(const char* name) const {
    int v = 0;
    if (!mpv_ || mpv_get_property(mpv_, name, MPV_FORMAT_FLAG, &v) < 0) return std::nullopt;
    return v != 0;
}

std::string Mpv::getString(const char* name) const {
    if (!mpv_) return {};
    char* s = mpv_get_property_string(mpv_, name);
    if (!s) return {};
    std::string out = s;
    mpv_free(s);
    return out;
}

void Mpv::observe(uint64_t id, const char* name, mpv_format format) {
    if (mpv_) mpv_observe_property(mpv_, id, name, format);
}

// --- mpv_node helpers ------------------------------------------------------------------------

const mpv_node* nodeMapGet(const mpv_node& map, const char* key) {
    if (map.format != MPV_FORMAT_NODE_MAP || !map.u.list) return nullptr;
    for (int i = 0; i < map.u.list->num; ++i)
        if (map.u.list->keys && map.u.list->keys[i] && !std::strcmp(map.u.list->keys[i], key))
            return &map.u.list->values[i];
    return nullptr;
}

std::string nodeString(const mpv_node* n, const std::string& fallback) {
    if (!n || n->format != MPV_FORMAT_STRING || !n->u.string) return fallback;
    return n->u.string;
}

int64_t nodeInt(const mpv_node* n, int64_t fallback) {
    if (!n) return fallback;
    if (n->format == MPV_FORMAT_INT64) return n->u.int64;
    if (n->format == MPV_FORMAT_DOUBLE) return static_cast<int64_t>(n->u.double_);
    return fallback;
}

double nodeDouble(const mpv_node* n, double fallback) {
    if (!n) return fallback;
    if (n->format == MPV_FORMAT_DOUBLE) return n->u.double_;
    if (n->format == MPV_FORMAT_INT64) return static_cast<double>(n->u.int64);
    return fallback;
}

bool nodeFlag(const mpv_node* n, bool fallback) {
    if (!n || n->format != MPV_FORMAT_FLAG) return fallback;
    return n->u.flag != 0;
}

}  // namespace karafin::player
