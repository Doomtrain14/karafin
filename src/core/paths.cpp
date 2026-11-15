#include "core/paths.hpp"

#include "core/fs.hpp"

namespace karafin::paths {

std::string dataDir() { return std::string(kDataDir); }

std::string dataFile(std::string_view name) {
    std::string out(kDataDir);
    out += '/';
    out += name;
    return out;
}

std::string logFile() { return dataFile("log.txt"); }
std::string settingsFile() { return dataFile("settings.json"); }
std::string cacheDir() { return dataFile("cache"); }
std::string downloadsDir() { return dataFile("downloads"); }

std::string romfs(std::string_view rel) {
    std::string out(kRomfsRoot);
    if (rel.empty() || rel.front() != '/') out += '/';
    out += rel;
    return out;
}

bool ensureAppDirs() {
    return fs::ensureDir(dataDir()) && fs::ensureDir(cacheDir()) && fs::ensureDir(downloadsDir());
}

}  // namespace karafin::paths
