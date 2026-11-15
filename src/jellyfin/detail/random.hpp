#pragma once

#include <cstddef>
#include <string>

namespace karafin::jellyfin::detail {

// Lower-case hex of `bytes` bytes from the kernel CSPRNG.
std::string randomHex(size_t bytes);

}  // namespace karafin::jellyfin::detail
