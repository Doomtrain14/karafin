#include "jellyfin/detail/random.hpp"

#include <cstdint>
#include <vector>

#include <switch.h>

namespace karafin::jellyfin::detail {

std::string randomHex(size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::vector<uint8_t> buf(bytes);
    randomGet(buf.data(), buf.size());
    std::string out;
    out.reserve(bytes * 2);
    for (uint8_t b : buf) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

}  // namespace karafin::jellyfin::detail
