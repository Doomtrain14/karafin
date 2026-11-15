#pragma once

// Private template definitions for Client; include only from src/jellyfin/*.cpp.

#include <exception>
#include <type_traits>

#include "jellyfin/client.hpp"

namespace karafin::jellyfin {

template <class T, class Parse>
Result<T> Client::parseResponse(const http::Response& resp, ErrorContext ctx, const Parse& parse) {
    const bool authed = !accessToken().empty();
    if (ApiError e = check(resp, ctx, authed)) return Result<T>::failure(std::move(e));
    const nlohmann::json j = nlohmann::json::parse(resp.body, nullptr, false);
    if (j.is_discarded()) return Result<T>::failure(ApiError::parse("invalid JSON from " + resp.effectiveUrl));
    try {
        if constexpr (std::is_same_v<std::invoke_result_t<const Parse&, const nlohmann::json&>, Result<T>>) {
            return parse(j);
        } else {
            return Result<T>::success(parse(j));
        }
    } catch (const std::exception& e) {
        return Result<T>::failure(ApiError::parse(e.what()));
    }
}

template <class T, class Parse>
Call<T> Client::jsonCall(http::Request req, ErrorContext ctx, Parse parse) {
    auto self = shared_from_this();
    return Call<T>([self, req = std::move(req), ctx, parse](const detail::Exec& exec, Callback<T> done) {
        exec.send(req, [self, ctx, parse, done](http::Response resp) {
            done(self->template parseResponse<T>(resp, ctx, parse));
        });
    });
}

}  // namespace karafin::jellyfin
