#pragma once

#include <atomic>
#include <memory>

namespace karafin {

// Shared cancellation flag. Copies observe the same state, so the requester keeps one copy and
// hands another to the operation (HTTP request, image decode, download, ...).
//
//   CancelToken tok;
//   http::Request req; req.cancel = tok;
//   ...
//   tok.cancel();   // e.g. when the screen that asked for it is popped
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() const { flag_->store(true, std::memory_order_release); }
    bool cancelled() const { return flag_->load(std::memory_order_acquire); }

    // Fresh, uncancelled token (convenient for "cancel previous and start again").
    void reset() { flag_ = std::make_shared<std::atomic<bool>>(false); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace karafin
