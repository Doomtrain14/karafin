#pragma once

#include <cstddef>
#include <functional>

// Post-to-main: the only sanctioned way for worker / HTTP / download threads to touch UI state,
// GL objects, or mpv render state. Tasks run at the start of the next frame, in FIFO order.
//
//   workers().post([=] {
//       auto pixels = decode(bytes);
//       main_thread::post([=] { texture.upload(pixels); });
//   });
namespace karafin::main_thread {

// Thread-safe; callable from any thread (including the main thread itself). At most 1024 tasks
// posted from other threads are pending at once: when the queue is full, the caller blocks until
// the main thread drains it. Never call it while holding a lock the main thread may take. The
// main thread never waits (it cannot drain its own queue while blocked). After close(), tasks
// are dropped.
void post(std::function<void()> task);

// Never blocks and does not count against the 1024 limit. Only for posters that already keep at
// most one task outstanding (coalesced wakeups) or must not block (mpv callbacks, threads the
// main thread joins).
void postNoWait(std::function<void()> task);

// Runs `task` on the main thread after `seconds` (measured from the call, frame-granular).
void postDelayed(double seconds, std::function<void()> task);

// True on the thread that runs the app loop.
bool isCurrent();

// Logs an error (once per call site string) when not on the main thread. Use in debug checks.
void assertCurrent(const char* where);

// --- App loop internals ---
void bindCurrentThread();
// Runs due tasks, stopping early once `budgetMs` has elapsed (remaining tasks wait a frame).
size_t runPending(double budgetMs = 6.0);
size_t pendingCount();
// Called once the app loop has exited: wakes blocked posters and drops every later post (nothing
// would run them).
void close();
// Drops all queued tasks (used during shutdown).
void clear();

}  // namespace karafin::main_thread
