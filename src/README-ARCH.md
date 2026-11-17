# Karafin source architecture

C++20, single executable (`karafin.elf` -> `karafin.nro`). Build with `./build.ps1` from the repo
root (Docker or Podman-in-WSL); output is `build/out/karafin.nro`.

## Adding source files

`CMakeLists.txt` compiles every `src/**/*.cpp` and `src/**/*.c` via `GLOB_RECURSE CONFIGURE_DEPENDS`.
New files in a module directory are picked up automatically; there is no need to list them in
`CMakeLists.txt`.
Include paths are rooted at `src/`: `#include "core/http.hpp"`.

- Vendored headers: `#include <nlohmann/json.hpp>`, `#include <stb_image.h>`. The stb_image
  implementation lives in `core/third_party_impl.cpp`; never define `STB_IMAGE_IMPLEMENTATION` elsewhere.
- All portlibs (SDL2, GLES3/EGL, mpv, FFmpeg, curl, mbedtls, FreeType, HarfBuzz, libass, libwebp,
  libpng, libjpeg, zlib) are already linked and on the include path. Use `<GLES3/gl3.h>` for GL.
- romfs: put assets under `romfs/` (repo root) -> `romfs:/...` at runtime. `romfs:/certs/cacert.pem`
  is added by the build.
- App code compiles with `-Wall -Wextra`; keep it warning-free.

## Modules and dependency direction

```
core  <-  gfx  <-  ui  <-  screens
  ^                         |
  +----  jellyfin  <--------+
            ^               |
            +-- downloads <-+
            +-- player  <---+   (player also uses gfx/ui for its OSD, downloads for offline files)
```

| Dir | Namespace | Owns |
| --- | --- | --- |
| `src/core/` | `karafin`, `karafin::log`, `karafin::http`, `karafin::platform`, `karafin::fs`, `karafin::paths`, `karafin::main_thread`, `karafin::libinfo` | entry point, app loop, logging, settings, threads, HTTP, platform services |
| `src/gfx/` | `karafin::gfx` | GL renderer, text, textures, image loading/cache, blur |
| `src/ui/` | `karafin::ui` | widget tree, focus/navigation, animation, screen stack, the `AppDelegate` |
| `src/screens/` | `karafin::screens` | concrete pages (home, library, detail, login, settings, ...) |
| `src/jellyfin/` | `karafin::jellyfin` | API models and client (no gfx/ui dependency) |
| `src/player/` | `karafin::player` | mpv playback, OSD, subtitles |
| `src/downloads/` | `karafin::downloads` | download queue, offline store |

Lower layers never include higher ones (core includes nothing from other modules; jellyfin
includes only core).

## Core API overview

| Header | Use |
| --- | --- |
| `core/app.hpp` | `App::instance()`, `AppDelegate` (onStart/onEvent/onUpdate(dt)/onRender/onDisplayChanged/onFocusChanged/onStop), `DisplayInfo` (1280x720 logical, `scale` 1.0 or 1.5), `App::glProcAddress` for mpv |
| `core/log.hpp` | `log::info("tag", "fmt {}", x)` (+ trace/debug/warn/error; `log::mark` = info flushed at once for crash breadcrumbs) -> `sdmc:/switch/karafin/log.txt` and nxlink |
| `core/settings.hpp` | `Settings::instance().get<T>("module.key", fallback)` / `.set("module.key", v)`; debounced auto-save |
| `core/thread_pool.hpp` | `workers().post(fn)`, `workers().submit(fn) -> std::future` |
| `core/thread.hpp` | `Thread(ThreadOptions{name, stackSize, priority, core}, fn)` for long-lived loops |
| `core/perf.hpp` | per-frame counters (`perf::frame().uploads++` ...), frame history for the stats overlay, slow-frame debug log |
| `core/main_queue.hpp` | `main_thread::post(fn)`, `postNoWait(fn)`, `postDelayed(sec, fn)`, `isCurrent()` |
| `core/cancel.hpp` | `CancelToken` (shared flag; copy it into requests, `cancel()` from the owner) |
| `core/http.hpp` | `http::Client::instance().send(Request, cb)` (async), `.perform(Request)` (blocking, worker threads only), `Response::json()`, `urlEncode/buildQuery/joinUrl` |
| `core/platform.hpp` | `isAppletMode()`, `isDocked()`, `hasFocus()`, `networkConnected()`, RAII `CpuBoost`, RAII `AwakeLock` |
| `core/fs.hpp`, `core/paths.hpp` | `writeFileAtomic`, `readFileAtomic`, `ensureDir`, `space()`; `paths::dataDir()/cacheDir()/downloadsDir()/romfs()` |

## Threading rules

1. **Main thread** runs the app loop: SDL events, `main_thread::runPending()`, `onUpdate`, `onRender`,
   swap (vsync). It owns all UI state, all GL calls and the mpv render context. Never block it on
   I/O; `http::Client::perform` on the main thread logs a warning.
2. **`workers()`** (3 threads, cores 1-2): CPU-bound work (JSON parsing of big responses, image
   decode, blur, small file I/O). Do not block them on network I/O.
   Priorities (`core/thread.hpp`): main 0x2C on core 0; every `karafin::Thread` is confined to
   cores 1-2 (unless placed on core 0 explicitly) at a lower priority (I/O 0x2D, workers 0x2E,
   background 0x30). Horizon does not time-slice equal priorities, so never give a CPU-heavy
   thread the main thread's priority or core.
3. **`http` thread** (curl multi): all async requests. Callbacks default to the main thread. Use
   `CallbackThread::Http` only for tiny hand-offs (e.g. pushing bytes to a worker). Artwork is
   sent with `Request::background`, which caps it at 4 transfers so API calls never queue behind it.
4. **Dedicated `karafin::Thread`s** for long-running loops (download transfers via blocking
   `perform`, mpv's own threads).
5. Cross-thread results go back with `main_thread::post`. Objects captured in posted lambdas must
   outlive the callback: capture `shared_ptr`/`weak_ptr`, or check a `CancelToken` first. At most
   1024 tasks posted from other threads are pending: past that, `post` blocks until the main thread
   drains the queue, so never post while holding a lock the main thread can take. The main thread
   never waits on its own queue. `postNoWait` skips the limit for posters that keep at most one
   task outstanding or must not block: mpv callbacks, server discovery and the download manager's
   coalesced event flush. `main_thread::close()` runs as soon as the loop exits; later posts are
   dropped and blocked posters return, so shutdown can join their threads.
6. `Settings::set` and listeners: call from the main thread.

## Startup / shutdown order (core/main.cpp)

`platform::init()` (romfs, sockets, nxlink, nifm) -> `paths::ensureAppDirs()` -> `log::init` ->
`Settings::load` -> `App::run(delegate)` [worker pool, http client, SDL/GL, applet hooks,
`onStart`: ui::Root, then `DownloadManager::start()`; loop] -> teardown in reverse. The delegate
is `AppHost` in main.cpp, wrapping `ui::Root` (src/ui), constructed with a factory for the first
screen (`screens::SplashScreen`). An unhandled + opens a quit confirmation.

Quit (`onStop`, main thread, everything still running): `player::shutdown(2000)` (final Stopped
report, bounded wait; persisted in the ReportQueue if it doesn't get through) ->
`DownloadManager::shutdown()` -> `ui::Root::onStop()`. App then stops the HTTP client, the worker
pool and SDL/GL.

Applet events: focus changes reach `onFocusChanged` (a lost-and-regained pair between two
frames is delivered as false then true). Waking from sleep resets the HTTP connection pool and
calls `AppDelegate::onResume()`. Dock/undock resizes the window and calls `onDisplayChanged` once.

gfx and ui public APIs: see `src/gfx/README.md` and `src/ui/README.md`.

## Settings keys owned by core

| Key | Default | Meaning |
| --- | --- | --- |
| `app.log_level` | `"info"` | trace / debug / info / warn / error |
| `http.verify_tls` | `true` | global TLS verification switch (a user CA bundle at `sdmc:/switch/karafin/cacert.pem` is also trusted) |
