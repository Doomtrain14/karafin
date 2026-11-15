#pragma once

#include <string>
#include <string_view>

// Well-known locations. All persistent state lives under sdmc:/switch/karafin/.
namespace karafin::paths {

inline constexpr std::string_view kDataDir = "sdmc:/switch/karafin";
inline constexpr std::string_view kRomfsRoot = "romfs:";

std::string dataDir();                       // sdmc:/switch/karafin
std::string dataFile(std::string_view name);  // sdmc:/switch/karafin/<name>
std::string logFile();                       // sdmc:/switch/karafin/log.txt
std::string settingsFile();                  // sdmc:/switch/karafin/settings.json
std::string cacheDir();                      // sdmc:/switch/karafin/cache       (images, disposable)
std::string downloadsDir();                  // sdmc:/switch/karafin/downloads   (offline media)
std::string romfs(std::string_view rel);     // romfs:/<rel>

// Creates dataDir, cacheDir and downloadsDir. Returns false if the SD card is not writable.
bool ensureAppDirs();

}  // namespace karafin::paths
