#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Small filesystem helpers that behave well on the Switch SD card (FAT32/exFAT via fsdev).
namespace karafin::fs {

bool exists(const std::string& path);
bool isDirectory(const std::string& path);
std::optional<uint64_t> fileSize(const std::string& path);

// mkdir -p. Returns true if the directory exists afterwards.
bool ensureDir(const std::string& path);
bool removeFile(const std::string& path);
// Recursively deletes a directory tree.
bool removeTree(const std::string& path);

std::optional<std::string> readFile(const std::string& path);

// Writes <path>.tmp, then replaces <path>. Horizon's rename fails if the target exists, so the
// old file is removed first; readFileAtomic() falls back to <path>.tmp if a crash hit in between.
bool writeFileAtomic(const std::string& path, std::string_view data);
std::optional<std::string> readFileAtomic(const std::string& path);

// Free / total bytes on the volume containing `path` (statvfs).
struct SpaceInfo {
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
};
std::optional<SpaceInfo> space(const std::string& path);

std::string parentDir(std::string_view path);

}  // namespace karafin::fs
