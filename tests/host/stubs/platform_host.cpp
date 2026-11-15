// Host replacement for core/platform.cpp: a docked, focused "application" with a network.

#include "core/platform.hpp"

namespace karafin::platform {

void init() {}
void shutdown() {}
bool isAppletMode() { return false; }
const char* appletTypeName() { return "Host"; }
bool isDocked() { return true; }
bool hasFocus() { return true; }
bool nxlinkConnected() { return false; }
bool romfsMounted() { return false; }
bool networkConnected() { return true; }
std::optional<BatteryState> battery() { return std::nullopt; }
uint64_t totalMemory() { return 0; }
uint64_t usedMemory() { return 0; }
std::string firmwareVersion() { return "host"; }
std::string describe() { return "host"; }

CpuBoost::CpuBoost() = default;
CpuBoost::~CpuBoost() = default;
AwakeLock::AwakeLock() = default;
AwakeLock::~AwakeLock() = default;

}  // namespace karafin::platform
