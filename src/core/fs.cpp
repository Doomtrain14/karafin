#include "core/fs.hpp"

#include <cerrno>
#include <cstdio>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace karafin::fs {

bool exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

bool isDirectory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::optional<uint64_t> fileSize(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return std::nullopt;
    return static_cast<uint64_t>(st.st_size);
}

std::string parentDir(std::string_view path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string_view::npos) return {};
    // Keep the root of device paths such as "sdmc:/".
    if (pos > 0 && path[pos - 1] == ':') return std::string(path.substr(0, pos + 1));
    return std::string(path.substr(0, pos));
}

bool ensureDir(const std::string& path) {
    if (path.empty()) return false;
    if (isDirectory(path)) return true;
    const std::string parent = parentDir(path);
    if (!parent.empty() && parent != path && !parent.ends_with(":/") && !isDirectory(parent)) {
        if (!ensureDir(parent)) return false;
    }
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST) return isDirectory(path);
    return false;
}

bool removeFile(const std::string& path) { return std::remove(path.c_str()) == 0 || !exists(path); }

bool removeTree(const std::string& path) {
    if (!isDirectory(path)) return removeFile(path);
    if (DIR* dir = opendir(path.c_str())) {
        while (dirent* ent = readdir(dir)) {
            const std::string_view name = ent->d_name;
            if (name == "." || name == "..") continue;
            const std::string child = path + "/" + std::string(name);
            if (isDirectory(child)) {
                removeTree(child);
            } else {
                std::remove(child.c_str());
            }
        }
        closedir(dir);
    }
    return rmdir(path.c_str()) == 0 || !exists(path);
}

std::optional<std::string> readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::nullopt;
    std::string out;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long size = std::ftell(f);
        // Only a hint: a damaged FAT entry can report an absurd size, so never pre-allocate it.
        if (size > 0 && size <= 64L * 1024 * 1024) out.reserve(static_cast<size_t>(size));
        std::fseek(f, 0, SEEK_SET);
    }
    char buf[16 * 1024];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    const bool ok = !std::ferror(f);
    std::fclose(f);
    if (!ok) return std::nullopt;
    return out;
}

bool writeFileAtomic(const std::string& path, std::string_view data) {
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    bool written = true;
    for (size_t off = 0; written && off < data.size();) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno == EINTR) continue;
        else written = false;
    }
    // Without the fsync a power cut after the rename can leave an empty file in place of both copies.
    written = written && ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!written || !closed) {
        std::remove(tmp.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::optional<std::string> readFileAtomic(const std::string& path) {
    if (auto data = readFile(path)) return data;
    return readFile(path + ".tmp");
}

std::optional<SpaceInfo> space(const std::string& path) {
    struct statvfs sv{};
    if (statvfs(path.c_str(), &sv) != 0) return std::nullopt;
    const uint64_t block = sv.f_frsize ? sv.f_frsize : (sv.f_bsize ? sv.f_bsize : 1);
    SpaceInfo info;
    info.freeBytes = static_cast<uint64_t>(sv.f_bavail) * block;
    info.totalBytes = static_cast<uint64_t>(sv.f_blocks) * block;
    return info;
}

}  // namespace karafin::fs
