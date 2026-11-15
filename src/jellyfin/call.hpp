#pragma once

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "core/cancel.hpp"
#include "core/http.hpp"
#include "core/main_queue.hpp"
#include "jellyfin/error.hpp"

namespace karafin::jellyfin {

// Value-or-error. `value` is default-constructed when there is an error.
template <class T>
struct Result {
    T value{};
    ApiError error;

    bool ok() const { return error.ok(); }
    explicit operator bool() const { return ok(); }
    T& operator*() { return value; }
    const T& operator*() const { return value; }
    T* operator->() { return &value; }
    const T* operator->() const { return &value; }

    static Result success(T v) {
        Result r;
        r.value = std::move(v);
        return r;
    }
    static Result failure(ApiError e) {
        Result r;
        r.error = std::move(e);
        return r;
    }
};

struct Unit {};
using Status = Result<Unit>;

template <class T>
using Callback = std::function<void(Result<T>)>;

namespace detail {

// Runs the network steps of a Call either asynchronously (http thread -> continuation on
// workers()) or blocking on the calling thread. Composite operations are written once in
// continuation-passing style against this interface.
class Exec {
public:
    Exec(bool blocking, CancelToken token) : blocking_(blocking), token_(std::move(token)) {}

    bool blocking() const { return blocking_; }
    const CancelToken& token() const { return token_; }

    // The continuation runs on a worker thread (async) or inline (blocking). The request's
    // cancel token is replaced by the Exec's token.
    void send(http::Request req, std::function<void(http::Response)> next) const;
    // Waits without blocking a thread (async: main-thread timer) or with sleep (blocking).
    void delay(double seconds, std::function<void()> next) const;

private:
    bool blocking_;
    CancelToken token_;
};

}  // namespace detail

// A not-yet-started API operation. Nothing happens until then() or wait() is called; a Call can
// be started more than once (each start re-sends the requests).
//
//   client->items(q).then([this](Result<QueryResult<BaseItem>> r) { ... }, token_);   // UI
//   auto r = client->item(id).wait();                                                 // worker
template <class T>
class Call {
public:
    using Op = std::function<void(const detail::Exec&, Callback<T>)>;

    Call() = default;
    explicit Call(Op op) : op_(std::move(op)) {}

    static Call ready(Result<T> result) {
        return Call([result](const detail::Exec&, Callback<T> done) { done(result); });
    }
    static Call fail(ApiError error) { return ready(Result<T>::failure(std::move(error))); }

    // Async. `cb` runs on the main thread. If `token` is cancelled before delivery the callback is
    // NOT invoked, so screens can capture `this` and cancel their token when they go away.
    void then(Callback<T> cb, CancelToken token = {}) const {
        if (!op_) return;
        detail::Exec exec(false, token);
        op_(exec, [cb = std::move(cb), token](Result<T> r) {
            main_thread::post([cb, token, r = std::move(r)]() mutable {
                if (token.cancelled() || !cb) return;
                cb(std::move(r));
            });
        });
    }

    // Blocking on the calling thread (dedicated threads such as the download worker; never the
    // main thread).
    Result<T> wait(CancelToken token = {}) const {
        Result<T> out = Result<T>::failure(ApiError::cancelled());
        if (!op_) return out;
        detail::Exec exec(true, token);
        op_(exec, [&out](Result<T> r) { out = std::move(r); });
        return out;
    }

    // Raw start with an explicit executor; used to chain Calls inside composite operations.
    void run(const detail::Exec& exec, Callback<T> done) const {
        if (!op_) {
            done(Result<T>::failure(ApiError::cancelled()));
            return;
        }
        op_(exec, std::move(done));
    }

    // Transforms the successful value (runs on the continuation thread).
    template <class F>
    auto map(F f) const -> Call<std::invoke_result_t<F, T>> {
        using U = std::invoke_result_t<F, T>;
        auto op = op_;
        return Call<U>([op, f](const detail::Exec& exec, Callback<U> done) {
            op(exec, [f, done](Result<T> r) {
                if (!r.ok()) return done(Result<U>::failure(std::move(r.error)));
                done(Result<U>::success(f(std::move(r.value))));
            });
        });
    }

    // Lets the caller adjust an error (kind/message) before delivery.
    template <class F>
    Call mapError(F f) const {
        auto op = op_;
        return Call([op, f](const detail::Exec& exec, Callback<T> done) {
            op(exec, [f, done](Result<T> r) {
                if (!r.ok() && r.error.kind != ErrorKind::Cancelled) f(r.error);
                done(std::move(r));
            });
        });
    }

    bool valid() const { return static_cast<bool>(op_); }

private:
    Op op_;
};

}  // namespace karafin::jellyfin
