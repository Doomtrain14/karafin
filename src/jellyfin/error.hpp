#pragma once

#include <string>
#include <string_view>

#include "core/http.hpp"

namespace karafin::jellyfin {

enum class ErrorKind {
    None,
    Network,       // server unreachable / timeout / TLS / connection dropped
    Offline,       // the console has no network connection at all
    Unauthorized,  // 401: bad credentials or the token was revoked -> go back to login
    Forbidden,     // 403: the user lacks a permission (downloads, subtitle management, ...)
    NotFound,      // 404, or "nothing there" (no subtitle results, feature unsupported by server)
    Invalid,       // 400 or bad client-side input (malformed URL, not a Jellyfin server, ...)
    Server,        // 5xx or another unexpected HTTP status
    Parse,         // response was not the JSON we expected
    Cancelled,     // the CancelToken fired
};

std::string_view errorKindName(ErrorKind kind);

// Which operation failed; selects the user-facing wording for HTTP statuses.
enum class ErrorContext { Generic, Connect, Login, QuickConnect, Playback, Subtitles, Download };

struct ApiError {
    ErrorKind kind = ErrorKind::None;
    long status = 0;        // HTTP status when one was received
    std::string message;    // short, user-facing sentence
    std::string detail;     // technical description for logs

    bool ok() const { return kind == ErrorKind::None; }
    explicit operator bool() const { return kind != ErrorKind::None; }  // true when there IS an error
    // Network or Offline: the server could not be reached (offline mode / retry later).
    bool isConnectivity() const { return kind == ErrorKind::Network || kind == ErrorKind::Offline; }

    static ApiError make(ErrorKind kind, std::string message, long status = 0, std::string detail = {});
    // Maps a failed transport or non-2xx response; returns kind None for 2xx.
    static ApiError fromResponse(const http::Response& resp, ErrorContext ctx = ErrorContext::Generic);
    static ApiError parse(std::string_view what);
    static ApiError cancelled();

    std::string describe() const;  // "Unauthorized (HTTP 401): message [detail]"
};

}  // namespace karafin::jellyfin
