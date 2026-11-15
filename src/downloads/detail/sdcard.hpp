#pragma once

#include <optional>
#include <string>

// SD card specifics of Horizon (libnx). Kept out of the pure-logic helpers.
namespace karafin::downloads::detail {

// Creates an empty Horizon "concatenation file" at `path` (an archive-bit directory of <4 GiB
// chunks that the file system and stdio treat as one regular file). This is how Horizon stores
// files of 4 GiB or more on FAT32 cards; it works on exFAT too. Replaces whatever is at `path`.
// False if the file system refused (the caller then falls back to a plain file).
bool createLargeFile(const std::string& path);

// nullopt if unknown. false means the console has no exFAT driver, so the SD card is FAT32.
std::optional<bool> exFatSupported();

}  // namespace karafin::downloads::detail
