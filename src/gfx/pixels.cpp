#include "gfx/pixels.hpp"

#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>

#include <jpeglib.h>
#include <png.h>
#include <stb_image.h>
#include <webp/decode.h>

namespace karafin::gfx {
namespace {

struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf jump;
};

void jpegErrorExit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegError*>(cinfo->err);
    longjmp(err->jump, 1);
}

void jpegSilence(j_common_ptr) {}

// A damaged header (e.g. a cache file cut short by a crash) can claim up to 65535x65535 pixels;
// refuse anything above 64 MiB of RGBA instead of trying to allocate it. Decoders that scale while
// decoding (JPEG, lossy WebP, non-interlaced PNG) accept that much source; the ones that always
// materialize the full image (stb_image, lossless WebP) get a lower cap (fits a 3840x2160 original).
constexpr int64_t kMaxPixels = 4096LL * 4096;
constexpr int64_t kMaxFullDecodePixels = 3072LL * 3072;

bool sizeAllowed(int64_t w, int64_t h, int64_t limit = kMaxPixels) { return w > 0 && h > 0 && w * h <= limit; }

// Process-wide budget for the transient buffers of large decodes, so the worker threads cannot
// all hold a full-size image at once. Reservations are served FIFO; one bigger than the whole
// budget waits until it is the only one. Reservations never nest on a thread.
constexpr size_t kDecodeBudgetBytes = 48u * 1024 * 1024;
constexpr size_t kDecodeReserveMin = 4u * 1024 * 1024;  // smaller decodes skip the bookkeeping

std::mutex g_decodeMutex;
std::condition_variable g_decodeCv;
size_t g_decodeBytes = 0;
uint64_t g_decodeNextTicket = 0;
uint64_t g_decodeServing = 0;

class DecodeReservation {
public:
    explicit DecodeReservation(size_t bytes) {
        if (bytes < kDecodeReserveMin) return;
        bytes_ = std::min(bytes, kDecodeBudgetBytes);
        std::unique_lock lock(g_decodeMutex);
        const uint64_t ticket = g_decodeNextTicket++;
        g_decodeCv.wait(lock, [&] {
            return ticket == g_decodeServing && g_decodeBytes + bytes_ <= kDecodeBudgetBytes;
        });
        g_decodeBytes += bytes_;
        ++g_decodeServing;
        lock.unlock();
        g_decodeCv.notify_all();
    }
    ~DecodeReservation() {
        if (bytes_ == 0) return;
        {
            std::lock_guard lock(g_decodeMutex);
            g_decodeBytes -= std::min(g_decodeBytes, bytes_);
        }
        g_decodeCv.notify_all();
    }
    DecodeReservation(const DecodeReservation&) = delete;
    DecodeReservation& operator=(const DecodeReservation&) = delete;

private:
    size_t bytes_ = 0;
};

size_t rgbaBytes(int64_t w, int64_t h) { return static_cast<size_t>(w) * static_cast<size_t>(h) * 4; }

struct StbFree {
    void operator()(stbi_uc* p) const { stbi_image_free(p); }
};

// Uniform scale the decoder may apply before fitTo() without losing needed detail (0 = none).
double decodeScale(int w, int h, int tw, int th, ImageFit fit) {
    if (w <= 0 || h <= 0 || (tw <= 0 && th <= 0)) return 0;
    int fw = 0, fh = 0;
    fitSize(w, h, tw, th, fit, fw, fh);
    return std::max(static_cast<double>(fw) / w, static_cast<double>(fh) / h);
}

// Kept free of locals that change after setjmp (avoids -Wclobbered); results go through `out`.
bool decodeJpegInto(const uint8_t* data, size_t size, unsigned scaleDenom, Pixmap* out) {
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpegErrorExit;
    err.mgr.output_message = jpegSilence;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, const_cast<unsigned char*>(data), static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK || !sizeAllowed(cinfo.image_width, cinfo.image_height)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    cinfo.scale_num = 1;
    cinfo.scale_denom = scaleDenom;
    if (scaleDenom > 1) cinfo.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&cinfo);
    out->width = static_cast<int>(cinfo.output_width);
    out->height = static_cast<int>(cinfo.output_height);
    out->rgba.resize(static_cast<size_t>(out->width) * out->height * 4);
    const size_t stride = static_cast<size_t>(out->width) * 4;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = out->rgba.data() + stride * cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

bool isJpeg(std::string_view b) {
    return b.size() > 3 && static_cast<uint8_t>(b[0]) == 0xFF && static_cast<uint8_t>(b[1]) == 0xD8 &&
           static_cast<uint8_t>(b[2]) == 0xFF;
}

bool isWebp(std::string_view b) { return b.size() > 12 && b.substr(0, 4) == "RIFF" && b.substr(8, 4) == "WEBP"; }

struct Contrib {
    int start = 0;
    std::vector<float> weights;
};

std::vector<Contrib> contributions(int srcLen, int dstLen) {
    std::vector<Contrib> out(static_cast<size_t>(dstLen));
    const double ratio = static_cast<double>(srcLen) / dstLen;
    for (int i = 0; i < dstLen; ++i) {
        Contrib& c = out[static_cast<size_t>(i)];
        if (ratio > 1.0) {
            // Box filter over the exact source footprint (area average).
            const double s0 = i * ratio, s1 = (i + 1) * ratio;
            const int a = static_cast<int>(std::floor(s0));
            const int b = std::min(srcLen, static_cast<int>(std::ceil(s1)));
            c.start = a;
            double total = 0;
            for (int k = a; k < b; ++k) {
                const double w = std::min<double>(k + 1, s1) - std::max<double>(k, s0);
                c.weights.push_back(static_cast<float>(w));
                total += w;
            }
            for (float& w : c.weights) w = static_cast<float>(w / total);
        } else {
            // Bilinear when enlarging.
            const double center = (i + 0.5) * ratio - 0.5;
            int a = static_cast<int>(std::floor(center));
            const float t = static_cast<float>(center - a);
            a = std::clamp(a, 0, srcLen - 1);
            const int b = std::min(a + 1, srcLen - 1);
            c.start = a;
            if (b == a) c.weights = {1.0f};
            else c.weights = {1.0f - t, t};
        }
    }
    return out;
}

void boxBlurH(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(r + r + 1);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * w * 4;
        uint8_t* out = dst + static_cast<size_t>(y) * w * 4;
        for (int ch = 0; ch < 4; ++ch) {
            auto at = [&](int x) { return static_cast<float>(row[std::clamp(x, 0, w - 1) * 4 + ch]); };
            float acc = 0;
            for (int k = -r; k <= r; ++k) acc += at(k);
            for (int x = 0; x < w; ++x) {
                out[x * 4 + ch] = static_cast<uint8_t>(acc * inv + 0.5f);
                acc += at(x + r + 1) - at(x - r);
            }
        }
    }
}

void boxBlurV(const uint8_t* src, uint8_t* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(r + r + 1);
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int x = 0; x < w; ++x) {
        for (int ch = 0; ch < 4; ++ch) {
            auto at = [&](int y) { return static_cast<float>(src[std::clamp(y, 0, h - 1) * stride + x * 4 + ch]); };
            float acc = 0;
            for (int k = -r; k <= r; ++k) acc += at(k);
            for (int y = 0; y < h; ++y) {
                dst[y * stride + x * 4 + ch] = static_cast<uint8_t>(acc * inv + 0.5f);
                acc += at(y + r + 1) - at(y - r);
            }
        }
    }
}

void premultiplyRaw(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        const unsigned a = p[i + 3];
        if (a == 255) continue;
        p[i + 0] = static_cast<uint8_t>((p[i + 0] * a + 127) / 255);
        p[i + 1] = static_cast<uint8_t>((p[i + 1] * a + 127) / 255);
        p[i + 2] = static_cast<uint8_t>((p[i + 2] * a + 127) / 255);
    }
}

// Separable resample, vertical pass first so the only scratch memory is one float source row
// (the source can be a decoder's buffer that is about to be freed).
Pixmap resizeRaw(const uint8_t* src, int srcW, int srcH, int width, int height) {
    const auto cx = contributions(srcW, width);
    const auto cy = contributions(srcH, height);
    const size_t srcStride = static_cast<size_t>(srcW) * 4;
    std::vector<float> row(srcStride);
    Pixmap dst;
    dst.width = width;
    dst.height = height;
    dst.rgba.resize(rgbaBytes(width, height));
    for (int y = 0; y < height; ++y) {
        const Contrib& c = cy[static_cast<size_t>(y)];
        std::fill(row.begin(), row.end(), 0.0f);
        for (size_t k = 0; k < c.weights.size(); ++k) {
            const uint8_t* s = src + static_cast<size_t>(c.start + static_cast<int>(k)) * srcStride;
            const float wt = c.weights[k];
            for (size_t i = 0; i < srcStride; ++i) row[i] += s[i] * wt;
        }
        uint8_t* out = dst.rgba.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const Contrib& h = cx[static_cast<size_t>(x)];
            float r = 0, gg = 0, b = 0, a = 0;
            for (size_t k = 0; k < h.weights.size(); ++k) {
                const float* p = row.data() + static_cast<size_t>(h.start + static_cast<int>(k)) * 4;
                const float wt = h.weights[k];
                r += p[0] * wt;
                gg += p[1] * wt;
                b += p[2] * wt;
                a += p[3] * wt;
            }
            out[x * 4 + 0] = static_cast<uint8_t>(std::clamp(r + 0.5f, 0.0f, 255.0f));
            out[x * 4 + 1] = static_cast<uint8_t>(std::clamp(gg + 0.5f, 0.0f, 255.0f));
            out[x * 4 + 2] = static_cast<uint8_t>(std::clamp(b + 0.5f, 0.0f, 255.0f));
            out[x * 4 + 3] = static_cast<uint8_t>(std::clamp(a + 0.5f, 0.0f, 255.0f));
        }
    }
    return dst;
}

void unpremultiplyRaw(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        const unsigned a = p[i + 3];
        if (a == 255) continue;
        for (int c = 0; c < 3; ++c)
            p[i + c] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255u, (p[i + c] * 255u + a / 2) / a));
    }
}

// Size a decoder that can scale while decoding should produce: fitSize(), except Fill keeps the
// source aspect (fitTo() stretches afterwards). The source size when no shrink applies.
void scaledDecodeSize(int w, int h, int tw, int th, ImageFit fit, int& ow, int& oh) {
    ow = w;
    oh = h;
    const double s = decodeScale(w, h, tw, th, fit);
    if (!(s > 0 && s < 1)) return;
    if (fit == ImageFit::Fill) {
        ow = std::max(1, static_cast<int>(std::ceil(w * s)));
        oh = std::max(1, static_cast<int>(std::ceil(h * s)));
    } else {
        fitSize(w, h, tw, th, fit, ow, oh);
    }
    if (ow >= w || oh >= h) {
        ow = w;
        oh = h;
    }
}

// ---- PNG: streamed through libpng a row at a time and area-averaged straight into the output
// size, so a large PNG never exists at full resolution in memory.

bool isPng(std::string_view b) { return b.size() > 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0; }

// Adam7 rows are only final after the last pass, which needs the whole image; those go to stb.
bool isInterlacedPng(std::string_view b) {
    return b.size() > 28 && b.substr(12, 4) == "IHDR" && static_cast<uint8_t>(b[28]) != 0;
}

struct PngSource {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

void pngRead(png_structp png, png_bytep out, png_size_t n) {
    auto* s = static_cast<PngSource*>(png_get_io_ptr(png));
    if (n > s->size - s->pos) png_error(png, "truncated");
    std::memcpy(out, s->data + s->pos, n);
    s->pos += n;
}

[[noreturn]] void pngError(png_structp png, png_const_charp) { png_longjmp(png, 1); }
void pngWarning(png_structp, png_const_charp) {}

// Everything the setjmp-protected steps touch lives here, constructed before setjmp, so a libpng
// error can longjmp out without skipping destructors or reading clobbered locals.
struct PngStream {
    png_structp png = nullptr;
    png_infop info = nullptr;
    PngSource src;
    int w = 0, h = 0, ow = 0, oh = 0;
    std::vector<uint8_t> row;  // one source row, RGBA
    std::vector<float> hrow;   // that row resampled to ow, premultiplied
    std::vector<float> acc;    // two output rows being accumulated (consecutive rows share at most one source row)
    std::vector<Contrib> cx, cy;
    Pixmap out;

    PngStream(const uint8_t* data, size_t size) : src{data, size, 0} {
        png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, pngError, pngWarning);
        if (png) info = png_create_info_struct(png);
    }
    ~PngStream() {
        if (png) png_destroy_read_struct(&png, &info, nullptr);
    }
    PngStream(const PngStream&) = delete;
    PngStream& operator=(const PngStream&) = delete;
};

bool pngHeaderSteps(PngStream* s) {
    png_set_read_fn(s->png, &s->src, pngRead);
    png_read_info(s->png, s->info);
    png_uint_32 w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    png_get_IHDR(s->png, s->info, &w, &h, &depth, &color, &interlace, nullptr, nullptr);
    if (interlace != PNG_INTERLACE_NONE || !sizeAllowed(w, h)) return false;
    const bool trns = png_get_valid(s->png, s->info, PNG_INFO_tRNS) != 0;
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(s->png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(s->png);
    if (trns) png_set_tRNS_to_alpha(s->png);
    if (depth == 16) png_set_strip_16(s->png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(s->png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !trns) png_set_filler(s->png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(s->png, s->info);
    if (png_get_rowbytes(s->png, s->info) != static_cast<size_t>(w) * 4) return false;
    s->w = static_cast<int>(w);
    s->h = static_cast<int>(h);
    return true;
}

bool pngReadHeader(PngStream* s) {
    if (!s->png || !s->info) return false;
    if (setjmp(png_jmpbuf(s->png))) return false;
    return pngHeaderSteps(s);
}

bool pngRowSteps(PngStream* s) {
    const int w = s->w, h = s->h, ow = s->ow, oh = s->oh;
    const size_t stride = static_cast<size_t>(ow) * 4;
    float* hrow = s->hrow.data();
    int emit = 0;
    for (int k = 0; k < h; ++k) {
        uint8_t* src = s->row.data();
        png_read_row(s->png, src, nullptr);
        premultiplyRaw(src, s->row.size());
        if (ow == w) {
            for (size_t i = 0; i < stride; ++i) hrow[i] = src[i];
        } else {
            for (int x = 0; x < ow; ++x) {
                const Contrib& c = s->cx[static_cast<size_t>(x)];
                float px[4] = {0, 0, 0, 0};
                for (size_t t = 0; t < c.weights.size(); ++t) {
                    const uint8_t* p = src + static_cast<size_t>(c.start + static_cast<int>(t)) * 4;
                    const float wt = c.weights[t];
                    for (int ch = 0; ch < 4; ++ch) px[ch] += p[ch] * wt;
                }
                std::memcpy(hrow + static_cast<size_t>(x) * 4, px, sizeof px);
            }
        }
        for (int y = emit; y < oh && y < emit + 2; ++y) {
            const Contrib& c = s->cy[static_cast<size_t>(y)];
            if (k < c.start) break;
            const size_t t = static_cast<size_t>(k - c.start);
            if (t >= c.weights.size()) continue;
            const float wt = c.weights[t];
            float* acc = s->acc.data() + static_cast<size_t>(y & 1) * stride;
            for (size_t i = 0; i < stride; ++i) acc[i] += hrow[i] * wt;
        }
        while (emit < oh) {
            const Contrib& c = s->cy[static_cast<size_t>(emit)];
            if (c.start + static_cast<int>(c.weights.size()) - 1 > k) break;
            float* acc = s->acc.data() + static_cast<size_t>(emit & 1) * stride;
            uint8_t* out = s->out.rgba.data() + static_cast<size_t>(emit) * stride;
            for (size_t i = 0; i < stride; ++i) {
                out[i] = static_cast<uint8_t>(std::clamp(acc[i] + 0.5f, 0.0f, 255.0f));
                acc[i] = 0;
            }
            ++emit;
        }
    }
    return emit == oh;
}

bool pngReadRows(PngStream* s) {
    if (setjmp(png_jmpbuf(s->png))) return false;
    return pngRowSteps(s);
}

std::optional<Pixmap> decodePngStreamed(const uint8_t* data, size_t size, int tw, int th, ImageFit fit, bool fitted) {
    PngStream s(data, size);
    if (!pngReadHeader(&s)) return std::nullopt;
    scaledDecodeSize(s.w, s.h, tw, th, fit, s.ow, s.oh);
    DecodeReservation reserve(rgbaBytes(s.ow, s.oh));
    s.row.resize(rgbaBytes(s.w, 1));
    s.hrow.resize(static_cast<size_t>(s.ow) * 4);
    s.acc.assign(static_cast<size_t>(s.ow) * 8, 0.0f);
    if (s.ow != s.w) s.cx = contributions(s.w, s.ow);
    if (s.oh != s.h) {
        s.cy = contributions(s.h, s.oh);
    } else {
        s.cy.resize(static_cast<size_t>(s.oh));
        for (int y = 0; y < s.oh; ++y) s.cy[static_cast<size_t>(y)] = {y, {1.0f}};
    }
    s.out.width = s.ow;
    s.out.height = s.oh;
    s.out.rgba.resize(rgbaBytes(s.ow, s.oh));
    if (!pngReadRows(&s)) return std::nullopt;
    Pixmap out = std::move(s.out);  // premultiplied
    if (fitted) {
        int fw = out.width, fh = out.height;
        fitSize(out.width, out.height, tw, th, fit, fw, fh);
        if (fw != out.width || fh != out.height) return resizeRaw(out.rgba.data(), out.width, out.height, fw, fh);
        return out;
    }
    unpremultiplyRaw(out.rgba.data(), out.rgba.size());
    return out;
}

// Turns a decoder's straight-alpha w x h buffer into the result. `storage`, when given, owns `px`
// and is moved from. With `fitted`, premultiplies and resamples to fitSize() while the (possibly
// full-size) source is still covered by the caller's DecodeReservation.
Pixmap finishDecode(uint8_t* px, int w, int h, std::vector<uint8_t>* storage, int tw, int th, ImageFit fit,
                    bool fitted) {
    const size_t size = rgbaBytes(w, h);
    int fw = w, fh = h;
    if (fitted) {
        premultiplyRaw(px, size);
        fitSize(w, h, tw, th, fit, fw, fh);
        if (fw != w || fh != h) return resizeRaw(px, w, h, fw, fh);
    }
    Pixmap out;
    out.width = w;
    out.height = h;
    if (storage) out.rgba = std::move(*storage);
    else out.rgba.assign(px, px + size);
    return out;
}

std::optional<Pixmap> decodeImpl(std::string_view bytes, int targetWidth, int targetHeight, ImageFit fit,
                                 bool fitted) {
    if (bytes.empty()) return std::nullopt;
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());

    if (isJpeg(bytes)) {
        // Peek the header size cheaply via stb (no full decode) to pick the DCT scale.
        int w = 0, h = 0, n = 0;
        unsigned denom = 1;
        size_t outBytes = kDecodeBudgetBytes;  // unknown size: decode alone
        if (stbi_info_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &n) && w > 0 && h > 0) {
            const double need = decodeScale(w, h, targetWidth, targetHeight, fit);
            if (need > 0 && need < 1)
                while (denom < 8 && 1.0 / (denom * 2) >= need) denom *= 2;
            outBytes = rgbaBytes((w + denom - 1) / denom, (h + denom - 1) / denom);
        }
        {
            DecodeReservation reserve(outBytes);
            Pixmap out;
            if (decodeJpegInto(data, bytes.size(), denom, &out))
                return finishDecode(out.rgba.data(), out.width, out.height, &out.rgba, targetWidth, targetHeight,
                                    fit, fitted);
        }
    }

    if (isWebp(bytes)) {
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config) ||
            WebPGetFeatures(data, bytes.size(), &config.input) != VP8_STATUS_OK)
            return std::nullopt;
        const int w = config.input.width, h = config.input.height;
        // Lossless WebP always reconstructs the full ARGB image internally, scaled or not.
        const bool lossless = config.input.format == 2;
        if (!sizeAllowed(w, h, lossless ? kMaxFullDecodePixels : kMaxPixels)) return std::nullopt;
        int ow = w, oh = h;
        scaledDecodeSize(w, h, targetWidth, targetHeight, fit, ow, oh);
        if (ow != w || oh != h) {
            config.options.use_scaling = 1;
            config.options.scaled_width = ow;
            config.options.scaled_height = oh;
        }
        const size_t internal = lossless ? rgbaBytes(w, h) : (config.input.has_alpha ? rgbaBytes(w, h) / 4 : 0);
        DecodeReservation reserve(rgbaBytes(ow, oh) + internal);
        std::vector<uint8_t> buf(rgbaBytes(ow, oh));
        config.output.colorspace = MODE_RGBA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = buf.data();
        config.output.u.RGBA.stride = ow * 4;
        config.output.u.RGBA.size = buf.size();
        const VP8StatusCode status = WebPDecode(data, bytes.size(), &config);
        WebPFreeDecBuffer(&config.output);
        if (status != VP8_STATUS_OK) return std::nullopt;
        return finishDecode(buf.data(), ow, oh, &buf, targetWidth, targetHeight, fit, fitted);
    }

    if (isPng(bytes) && !isInterlacedPng(bytes))
        return decodePngStreamed(data, bytes.size(), targetWidth, targetHeight, fit, fitted);

    // stb_image (interlaced PNG, GIF, BMP) cannot decode at a reduced size: hold a reservation for
    // the full image plus its intermediate buffers until the result has been resampled.
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &n) ||
        !sizeAllowed(w, h, kMaxFullDecodePixels))
        return std::nullopt;
    DecodeReservation reserve(rgbaBytes(w, h) * 2);
    std::unique_ptr<stbi_uc, StbFree> px(stbi_load_from_memory(data, static_cast<int>(bytes.size()), &w, &h, &n, 4));
    if (!px || !sizeAllowed(w, h, kMaxFullDecodePixels)) return std::nullopt;
    return finishDecode(px.get(), w, h, nullptr, targetWidth, targetHeight, fit, fitted);
}

}  // namespace

void fitSize(int w, int h, int tw, int th, ImageFit fit, int& outW, int& outH) {
    outW = w;
    outH = h;
    if (w <= 0 || h <= 0 || (tw <= 0 && th <= 0)) return;
    if (fit == ImageFit::Fill && tw > 0 && th > 0) {
        outW = std::min(w, tw);
        outH = std::min(h, th);
        return;
    }
    double sx = tw > 0 ? static_cast<double>(tw) / w : 0.0;
    double sy = th > 0 ? static_cast<double>(th) / h : 0.0;
    double s;
    if (tw <= 0) s = sy;
    else if (th <= 0) s = sx;
    else s = fit == ImageFit::Contain ? std::min(sx, sy) : std::max(sx, sy);
    if (s >= 1.0) return;
    outW = std::max(1, static_cast<int>(std::lround(w * s)));
    outH = std::max(1, static_cast<int>(std::lround(h * s)));
}

std::optional<Pixmap> decodeImage(std::string_view bytes, int targetWidth, int targetHeight, ImageFit fit) {
    return decodeImpl(bytes, targetWidth, targetHeight, fit, false);
}

std::optional<Pixmap> decodeFitted(std::string_view bytes, int targetWidth, int targetHeight, ImageFit fit) {
    return decodeImpl(bytes, targetWidth, targetHeight, fit, true);
}

Pixmap resize(const Pixmap& src, int width, int height) {
    if (src.empty() || width <= 0 || height <= 0) return {};
    if (width == src.width && height == src.height) return src;
    return resizeRaw(src.rgba.data(), src.width, src.height, width, height);
}

Pixmap fitTo(Pixmap src, int targetWidth, int targetHeight, ImageFit fit) {
    int w = 0, h = 0;
    fitSize(src.width, src.height, targetWidth, targetHeight, fit, w, h);
    if (w == src.width && h == src.height) return src;
    return resize(src, w, h);
}

void blur(Pixmap& image, float sigma) {
    if (image.empty() || sigma < 0.5f) return;
    // Box sizes for 3 passes approximating a gaussian (Kovesi).
    const int n = 3;
    const float wIdeal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const float mIdeal = (12 * sigma * sigma - n * wl * wl - 4 * n * wl - 3 * n) / (-4.0f * wl - 4);
    const int m = static_cast<int>(std::lround(mIdeal));
    std::vector<uint8_t> tmp(image.rgba.size());
    for (int i = 0; i < n; ++i) {
        const int r = ((i < m ? wl : wu) - 1) / 2;
        if (r <= 0) continue;
        boxBlurH(image.rgba.data(), tmp.data(), image.width, image.height, r);
        boxBlurV(tmp.data(), image.rgba.data(), image.width, image.height, r);
    }
}

void premultiply(Pixmap& image) { premultiplyRaw(image.rgba.data(), image.rgba.size()); }

Color dominantColor(const Pixmap& image) {
    if (image.empty()) return Color::hex(0x202030);
    const int step = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(image.width) * image.height / 4096.0)));
    double r = 0, g = 0, b = 0, total = 0;
    for (int y = 0; y < image.height; y += step) {
        const uint8_t* row = image.rgba.data() + static_cast<size_t>(y) * image.width * 4;
        for (int x = 0; x < image.width; x += step) {
            const uint8_t* p = row + x * 4;
            if (p[3] < 32) continue;
            const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
            const double sat = mx > 0 ? static_cast<double>(mx - mn) / mx : 0.0;
            const double lum = mx / 255.0;
            // Prefer saturated, mid-bright pixels; near-black/near-white pixels count little.
            const double w = 0.05 + sat * sat * (1.0 - std::fabs(lum - 0.55));
            r += p[0] * w;
            g += p[1] * w;
            b += p[2] * w;
            total += w;
        }
    }
    if (total <= 0) return Color::hex(0x202030);
    return {static_cast<float>(r / total / 255.0), static_cast<float>(g / total / 255.0),
            static_cast<float>(b / total / 255.0), 1.0f};
}

}  // namespace karafin::gfx
