#include "core/libinfo.hpp"

#include <format>

#include <SDL.h>
#include <sys/select.h>  // curl/multi.h uses fd_set without including it on newlib
#include <curl/curl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <webp/decode.h>
#include <zlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

#include "core/log.hpp"

namespace karafin::libinfo {
namespace {

std::string ver3(unsigned v) { return std::format("{}.{}.{}", v >> 16, (v >> 8) & 0xff, v & 0xff); }

// Referenced so the render API (and with it mpv's GL video output) is linked now rather than
// first showing up when the player module lands.
[[gnu::used]] const void* const g_mpvRenderSymbols[] = {
    reinterpret_cast<const void*>(&mpv_render_context_create),
    reinterpret_cast<const void*>(&mpv_render_context_render),
    reinterpret_cast<const void*>(&mpv_render_context_free),
};

}  // namespace

std::string versions() {
    SDL_version sdl;
    SDL_GetVersion(&sdl);

    const curl_version_info_data* curl = curl_version_info(CURLVERSION_NOW);

    std::string freetype = "?";
    FT_Library ft = nullptr;
    if (FT_Init_FreeType(&ft) == 0) {
        FT_Int major = 0, minor = 0, patch = 0;
        FT_Library_Version(ft, &major, &minor, &patch);
        freetype = std::format("{}.{}.{}", major, minor, patch);
        FT_Done_FreeType(ft);
    }

    const unsigned long mpvApi = mpv_client_api_version();
    const int webp = WebPGetDecoderVersion();

    return std::format(
        "SDL {}.{}.{}, mpv client API {}.{}, FFmpeg {} (avcodec {}, avformat {}), curl {} ({}), FreeType {}, "
        "HarfBuzz {}, libwebp {}.{}.{}, zlib {}",
        sdl.major, sdl.minor, sdl.patch, mpvApi >> 16, mpvApi & 0xffff, av_version_info(), ver3(avcodec_version()),
        ver3(avformat_version()), curl->version, curl->ssl_version ? curl->ssl_version : "no TLS", freetype,
        hb_version_string(), (webp >> 16) & 0xff, (webp >> 8) & 0xff, webp & 0xff, zlibVersion());
}

std::string hwdecTypes() {
    std::string out;
    for (AVHWDeviceType t = av_hwdevice_iterate_types(AV_HWDEVICE_TYPE_NONE); t != AV_HWDEVICE_TYPE_NONE;
         t = av_hwdevice_iterate_types(t)) {
        if (!out.empty()) out += ", ";
        out += av_hwdevice_get_type_name(t);
    }
    return out.empty() ? "none" : out;
}

bool probeMpv() {
    mpv_handle* mpv = mpv_create();
    if (!mpv) {
        log::error("libinfo", "mpv_create failed");
        return false;
    }
    mpv_set_option_string(mpv, "vo", "null");
    mpv_set_option_string(mpv, "ao", "null");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    const int rc = mpv_initialize(mpv);
    if (rc < 0) {
        log::error("libinfo", "mpv_initialize failed: {}", mpv_error_string(rc));
    } else {
        char* version = mpv_get_property_string(mpv, "mpv-version");
        log::info("libinfo", "mpv core up: {}", version ? version : "?");
        mpv_free(version);
    }
    mpv_terminate_destroy(mpv);
    return rc >= 0;
}

}  // namespace karafin::libinfo
