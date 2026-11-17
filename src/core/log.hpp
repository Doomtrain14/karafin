#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

// Thread-safe logger. Lines go to sdmc:/switch/karafin/log.txt and, when the app was launched
// through nxlink (`nxlink -s karafin.nro`), to stdout as well.
//
//   karafin::log::info("http", "GET {} -> {}", url, status);
namespace karafin::log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error };

// Opens (appends to) the log file. Rotates to <path>.old when it exceeds ~1 MiB at start or
// 4 MiB within a session. Warn/Error lines are written and flushed by the caller at once;
// Trace..Info lines are queued and written by a background thread every 250 ms (flush() wakes
// it early), so logging never puts SD card I/O on the main thread.
void init(const std::string& filePath);
void shutdown();
void flush();

void setLevel(Level level);
Level level();
Level parseLevel(std::string_view name, Level fallback = Level::Info);

// Mirrors lines to stdout (enabled by platform::init when nxlink is connected).
void setStdoutMirror(bool enabled);

void write(Level level, std::string_view tag, std::string_view message, bool sync = false);

// For the next `seconds`, every line is written and flushed at once (like warn) and step() lines
// are logged, so a crash in that window still leaves the steps before it in the file. Armed when
// a session starts (sign-in to home screen).
void syncFor(double seconds);
bool syncing();

inline bool enabled(Level l) { return static_cast<int>(l) >= static_cast<int>(level()); }

template <class... Args>
void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Trace)) write(Level::Trace, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...));
}
// Info-level breadcrumb written and flushed at once (like warn), so the last step before a crash
// reaches the file. For rare milestones only (screen changes, session steps).
template <class... Args>
void mark(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...), true);
}
// Per-step detail (each image load, each home row) logged only inside a syncFor() window.
template <class... Args>
void step(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (syncing() && enabled(Level::Info)) write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...), true);
}
template <class... Args>
void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, tag, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace karafin::log
