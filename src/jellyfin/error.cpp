#include "jellyfin/error.hpp"

#include "core/platform.hpp"

namespace karafin::jellyfin {

std::string_view errorKindName(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::None: return "None";
        case ErrorKind::Network: return "Network";
        case ErrorKind::Offline: return "Offline";
        case ErrorKind::Unauthorized: return "Unauthorized";
        case ErrorKind::Forbidden: return "Forbidden";
        case ErrorKind::NotFound: return "NotFound";
        case ErrorKind::Invalid: return "Invalid";
        case ErrorKind::Server: return "Server";
        case ErrorKind::Parse: return "Parse";
        case ErrorKind::Cancelled: return "Cancelled";
    }
    return "?";
}

ApiError ApiError::make(ErrorKind kind, std::string message, long status, std::string detail) {
    ApiError e;
    e.kind = kind;
    e.status = status;
    e.message = std::move(message);
    e.detail = std::move(detail);
    return e;
}

ApiError ApiError::parse(std::string_view what) {
    return make(ErrorKind::Parse, "The server sent an unexpected response.", 0, std::string(what));
}

ApiError ApiError::cancelled() { return make(ErrorKind::Cancelled, "Cancelled.", 0); }

namespace {

ApiError transportError(const http::Response& resp, ErrorContext ctx) {
    const std::string detail = resp.describe();
    switch (resp.error) {
        case http::Error::Cancelled:
            return ApiError::make(ErrorKind::Cancelled, "Cancelled.", 0, detail);
        case http::Error::TooLarge:
            return ApiError::make(ErrorKind::Server, "The server response was too large.", 0, detail);
        case http::Error::Tls:
            return ApiError::make(ErrorKind::Network,
                                  "Secure connection failed. Check the server certificate, or put your CA in "
                                  "sdmc:/switch/karafin/cacert.pem.",
                                  0, detail);
        default: break;
    }
    if (!platform::networkConnected())
        return ApiError::make(ErrorKind::Offline, "No network connection. Check the console's network settings.", 0,
                              detail);
    switch (resp.error) {
        case http::Error::Resolve:
            return ApiError::make(ErrorKind::Network, "Server address not found. Check the URL.", 0, detail);
        case http::Error::Connect:
            return ApiError::make(ErrorKind::Network,
                                  ctx == ErrorContext::Connect ? "Could not connect. Check the address and port."
                                                               : "Can't reach the server.",
                                  0, detail);
        case http::Error::Timeout:
            return ApiError::make(ErrorKind::Network, "The server took too long to respond.", 0, detail);
        default:
            return ApiError::make(ErrorKind::Network, "Network error while talking to the server.", 0, detail);
    }
}

}  // namespace

ApiError ApiError::fromResponse(const http::Response& resp, ErrorContext ctx) {
    if (resp.error != http::Error::None) return transportError(resp, ctx);
    const long s = resp.status;
    if (s >= 200 && s < 300) return {};
    const std::string detail = "HTTP " + std::to_string(s);

    if (s == 401) {
        switch (ctx) {
            case ErrorContext::Login: return make(ErrorKind::Unauthorized, "Wrong username or password.", s, detail);
            case ErrorContext::QuickConnect:
                return make(ErrorKind::Unauthorized, "Quick Connect was not authorized.", s, detail);
            case ErrorContext::Subtitles:
                return make(ErrorKind::Forbidden,
                            "You don't have permission to download subtitles. Ask the server admin to allow "
                            "subtitle management for your user.",
                            s, detail);
            default: return make(ErrorKind::Unauthorized, "Your session has expired. Please sign in again.", s, detail);
        }
    }
    if (s == 403) {
        switch (ctx) {
            case ErrorContext::Download:
                return make(ErrorKind::Forbidden,
                            "Your account isn't allowed to download media. Ask the server admin to enable "
                            "\"Allow media downloading\".",
                            s, detail);
            case ErrorContext::Subtitles:
                return make(ErrorKind::Forbidden,
                            "You don't have permission to download subtitles. Ask the server admin to allow "
                            "subtitle management for your user.",
                            s, detail);
            case ErrorContext::Playback:
                return make(ErrorKind::Forbidden, "Your account isn't allowed to play this item.", s, detail);
            case ErrorContext::Login:
                return make(ErrorKind::Forbidden, "This account is disabled or not allowed to sign in here.", s,
                            detail);
            default: return make(ErrorKind::Forbidden, "You don't have permission to do that.", s, detail);
        }
    }
    if (s == 404) {
        switch (ctx) {
            case ErrorContext::Connect:
                return make(ErrorKind::Invalid, "No Jellyfin server found at this address.", s, detail);
            case ErrorContext::Subtitles:
                return make(ErrorKind::NotFound,
                            "No subtitle provider is available. The server needs a subtitle plugin such as "
                            "OpenSubtitles.",
                            s, detail);
            case ErrorContext::QuickConnect:
                return make(ErrorKind::NotFound, "Quick Connect is not available on this server.", s, detail);
            default: return make(ErrorKind::NotFound, "Not found on the server.", s, detail);
        }
    }
    // /Items/{id}/Download answers 400 when the item itself can't be downloaded.
    if (s == 400 && ctx == ErrorContext::Download)
        return make(ErrorKind::Invalid, "The server doesn't allow downloading this item.", s, detail);
    if (s == 400) return make(ErrorKind::Invalid, "The server rejected the request.", s, detail);
    if (s == 503) return make(ErrorKind::Server, "The server is starting up or unavailable. Try again shortly.", s, detail);
    if (s >= 500) return make(ErrorKind::Server, "The server hit an error (HTTP " + std::to_string(s) + ").", s, detail);
    return make(ErrorKind::Server, "Unexpected server response (HTTP " + std::to_string(s) + ").", s, detail);
}

std::string ApiError::describe() const {
    std::string out(errorKindName(kind));
    if (status) out += " (HTTP " + std::to_string(status) + ")";
    if (!message.empty()) out += ": " + message;
    if (!detail.empty()) out += " [" + detail + "]";
    return out;
}

}  // namespace karafin::jellyfin
