// Host replacement for downloads/detail/sdcard.cpp: no concatenation files, file system unknown.

#include <cstdio>

#include "downloads/detail/sdcard.hpp"

namespace karafin::downloads::detail {

bool createLargeFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::optional<bool> exFatSupported() { return std::nullopt; }

}  // namespace karafin::downloads::detail
