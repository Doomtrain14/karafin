#pragma once

// Async image loading with a GPU texture LRU and an on-disk cache.
//
//   gfx::ImageRequest req;
//   req.url = server + "/Items/" + id + "/Images/Primary?maxHeight=450&tag=" + tag;
//   req.width = int(150 * scale); req.height = int(225 * scale);   // framebuffer px
//   gfx::ImagePtr img = gfx::ImageCache::instance().load(req);
//   ...each frame:
//   if (const gfx::Texture* t = img->texture()) renderer.drawImage(*t, rect, {.radius = 12});
//
// Pipeline: memory cache -> disk cache (sdmc:/switch/karafin/cache/img, original bytes) -> HTTP
// (core http, async) -> decode + downscale (+ blur) + premultiply on workers() -> texture upload
// on the main thread. Identical requests share one ImageRef. Textures not drawn recently are
// evicted when over the byte budget; evicted refs reload transparently the next time texture()
// is called. Pending loads that nobody holds a reference to anymore are skipped.
//
// URLs: http(s)://..., or local files "romfs:/..." / "sdmc:/..." (not disk-cached).

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/http.hpp"
#include "gfx/pixels.hpp"
#include "gfx/texture.hpp"
#include "gfx/types.hpp"

namespace karafin::gfx {

enum class ImageState : uint8_t { Idle, Loading, Ready, Failed };

struct ImageRequest {
    std::string url;
    http::Headers headers;           // e.g. the Jellyfin Authorization header
    int width = 0;                   // target box in framebuffer pixels (0 = keep source size)
    int height = 0;
    ImageFit fit = ImageFit::Cover;  // how the target box is filled (decides the decode size)
    float blur = 0.0f;               // gaussian sigma in output pixels (backdrops)
    bool diskCache = true;
    std::string cacheKey;            // stable identity instead of the url (e.g. itemId + imageTag)
};

class ImageCache;

class ImageRef {
public:
    ImageState state() const { return state_; }
    bool ready() const { return state_ == ImageState::Ready; }
    bool failed() const { return state_ == ImageState::Failed; }
    bool loading() const { return state_ == ImageState::Loading || state_ == ImageState::Idle; }

    // Marks the image as used this frame (LRU) and requeues it if it was evicted. Null until ready.
    const Texture* texture();
    TexturePtr textureShared();

    // Saturation-weighted average color of the decoded image (valid once ready).
    Color dominantColor() const { return dominant_; }
    bool hasDominantColor() const { return hasDominant_; }
    int width() const { return width_; }
    int height() const { return height_; }
    float aspect() const { return height_ > 0 ? static_cast<float>(width_) / height_ : 0.0f; }

    // App time (seconds) when the texture became available; for fade-ins.
    double readyTime() const { return readyTime_; }
    // 0..1 fade progress since readyTime over `duration` seconds (1 when loaded from memory).
    float revealProgress(double now, float duration) const;

    const std::string& key() const { return key_; }
    const ImageRequest& request() const { return request_; }

    // Runs once the image is ready or failed (immediately if it already is). Main thread.
    void onSettled(std::function<void(ImageRef&)> callback);

private:
    friend class ImageCache;
    std::string key_;
    std::string diskKey_;
    ImageRequest request_;
    ImageState state_ = ImageState::Idle;
    TexturePtr texture_;
    Color dominant_ = Color::hex(0x1C1C26);
    bool hasDominant_ = false;
    int width_ = 0, height_ = 0;
    double readyTime_ = -1000.0;
    uint64_t lastUsedFrame_ = 0;
    uint64_t generation_ = 0;
    CancelToken cancel_;      // in-flight HTTP fetch (cancelled when nobody wants it any more)
    double failTime_ = 0;
    int failures_ = 0;
    bool transientFailure_ = false;  // network / 5xx: retried with backoff when drawn again
    bool fetching_ = false;          // HTTP stage in flight
    bool queued_ = false;
    bool inLru_ = false;
    bool pinned_ = false;  // generated images: never evicted (no source to reload from)
    std::list<ImageRef*>::iterator lruIt_;
    std::vector<std::function<void(ImageRef&)>> settled_;
};

using ImagePtr = std::shared_ptr<ImageRef>;

class ImageCache {
public:
    static ImageCache& instance();

    // Called by the UI host after the renderer is up. Reads settings "ui.image_cache_mb"
    // (default 192, 64 in applet mode) and "ui.image_disk_cache_mb" (default 256).
    void init();
    void shutdown();

    ImagePtr load(const ImageRequest& request);
    // Registers a CPU-generated image (straight alpha) under `key`; ready immediately.
    ImagePtr fromPixmap(const std::string& key, Pixmap pixmap);
    // Existing entry or null.
    ImagePtr find(const std::string& key) const;

    // Once per frame on the main thread: starts queued loads, uploads decoded images (rate
    // limited, see setUploadBudget), cancels fetches nobody wants, enforces the memory budget.
    void update();

    void setMemoryBudget(size_t bytes) { budget_ = bytes; }
    size_t memoryBudget() const { return budget_; }
    size_t memoryUsed() const { return used_; }
    size_t entryCount() const { return entries_.size(); }
    // Texture upload limits per frame: bytes, completed images and main-thread milliseconds.
    // Images are uploaded in bands of rows; at least one band goes up per frame.
    void setUploadBudget(size_t bytesPerFrame, int imagesPerFrame, double msPerFrame = 2.0) {
        uploadBytesPerFrame_ = bytesPerFrame;
        uploadsPerFrame_ = imagesPerFrame;
        uploadMsPerFrame_ = msPerFrame;
    }
    size_t pendingUploads() const { return uploads_.size(); }
    // Evicts every texture not drawn in the current frame (e.g. before starting playback).
    void trimMemory();
    // Deletes the disk cache (async, on a worker).
    void clearDiskCache();

private:
    ImageCache();
    ~ImageCache();
    friend class ImageRef;
    friend struct ImageCacheAccess;

    struct Result;
    void touch(ImageRef& ref);
    void enqueue(ImageRef& ref);
    void start(ImageRef& ref);
    void finish(std::shared_ptr<Result> result);
    ImageRef* uploadTarget(const Result& result);
    void finishUpload(ImageRef& ref, Result& result);
    void fail(ImageRef& ref, const Result& result);
    void settle(ImageRef& ref);
    void cancelUnwanted();
    void evict(ImageRef& ref);
    static std::string memoryKey(const ImageRequest& r);

    std::unordered_map<std::string, ImagePtr> entries_;
    std::list<ImageRef*> lru_;  // ready textures, most recently used first
    std::vector<std::weak_ptr<ImageRef>> queue_;
    std::deque<std::shared_ptr<Result>> uploads_;  // decoded, waiting for a texture upload
    size_t budget_ = 192u * 1024 * 1024;
    size_t used_ = 0;
    size_t diskBudget_ = 256u * 1024 * 1024;
    size_t uploadBytesPerFrame_ = 2u * 1024 * 1024;
    int uploadsPerFrame_ = 4;
    double uploadMsPerFrame_ = 2.0;
    int active_ = 0;
    int maxActive_ = 6;
    uint64_t frame_ = 1;
    uint64_t idleFrames_ = 0;
    bool initialized_ = false;
    bool boosting_ = false;  // CPU boost requested (held on a worker, see image.cpp)
};

}  // namespace karafin::gfx
