// Host replacement for jellyfin/detail/random.cpp (libnx randomGet).

#include <random>

#include "jellyfin/detail/random.hpp"

namespace karafin::jellyfin::detail {

std::string randomHex(size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned b = rd() & 0xff;
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

}  // namespace karafin::jellyfin::detail
