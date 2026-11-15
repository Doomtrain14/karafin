#pragma once

#include <string>

// Startup diagnostics for the statically linked media/network stack.
namespace karafin::libinfo {

// "SDL 2.28.5, mpv 2.1, ffmpeg 7.1.x, curl 8.23.0 (mbedTLS/3.6.7), FreeType 2.13.3, ..."
std::string versions();

// Comma-separated FFmpeg hardware device types; should contain "nvtegra" with Switchfin's build.
std::string hwdecTypes();

// Creates and destroys an mpv core (no video output). Proves libmpv links and initializes.
bool probeMpv();

}  // namespace karafin::libinfo
