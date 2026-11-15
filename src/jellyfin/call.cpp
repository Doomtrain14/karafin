#include "jellyfin/call.hpp"

#include <cmath>

#include "core/thread.hpp"
#include "core/thread_pool.hpp"

namespace karafin::jellyfin::detail {

void Exec::send(http::Request req, std::function<void(http::Response)> next) const {
    req.cancel = token_;
    if (blocking_) {
        next(http::Client::instance().perform(std::move(req)));
        return;
    }
    http::Client::instance().send(
        std::move(req),
        [next = std::move(next)](http::Response&& resp) {
            auto shared = std::make_shared<http::Response>(std::move(resp));
            workers().post([next, shared] { next(std::move(*shared)); });
        },
        http::CallbackThread::Http);
}

void Exec::delay(double seconds, std::function<void()> next) const {
    if (blocking_) {
        const int ms = static_cast<int>(std::lround(seconds * 1000.0));
        // Sleep in slices so a cancelled token cuts the wait short.
        for (int waited = 0; waited < ms && !token_.cancelled(); waited += 50) sleepMs(50);
        next();
        return;
    }
    main_thread::postDelayed(seconds, [next = std::move(next)] { workers().post(next); });
}

}  // namespace karafin::jellyfin::detail
