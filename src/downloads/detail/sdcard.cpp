#include "downloads/detail/sdcard.hpp"

#include <switch.h>

#include "core/fs.hpp"
#include "core/log.hpp"

namespace karafin::downloads::detail {

bool createLargeFile(const std::string& path) {
    fs::removeFile(path);
    const Result rc = fsdevCreateFile(path.c_str(), 0, FsCreateOption_BigFile);
    if (R_FAILED(rc)) {
        log::warn("downloads", "cannot create concatenation file {} (0x{:x})", path, static_cast<unsigned>(rc));
        return false;
    }
    if (!fs::fileSize(path)) {
        // Visible as a directory: stdio could not write it, so undo and use a plain file.
        log::warn("downloads", "concatenation file {} is not usable as a file", path);
        fs::removeTree(path);
        return false;
    }
    return true;
}

std::optional<bool> exFatSupported() {
    bool supported = false;
    if (R_FAILED(fsIsExFatSupported(&supported))) return std::nullopt;
    return supported;
}

}  // namespace karafin::downloads::detail
