#pragma once

#include <cstdint>
#include <optional>
#include <string>

// Switch platform services. Everything here is safe to call from any thread unless noted.
namespace karafin::platform {

// Called first thing in main(): romfs, sockets (tuned for streaming), nxlink stdio redirection,
// nifm, setsys. Idempotent. Failures are logged and non-fatal.
void init();
void shutdown();

// True when launched as a library applet (album / hbmenu without title override). Applet mode
// has ~400 MB less memory; video playback needs a title override (hold R while launching a game).
bool isAppletMode();
const char* appletTypeName();

bool isDocked();             // AppletOperationMode_Console
bool hasFocus();             // false while HOME menu / an overlay applet is in front
bool nxlinkConnected();      // stdout goes to `nxlink -s` on the PC
bool romfsMounted();
// Stable opaque id for the Switch profile selected at launch (or last opened profile when
// no launch selection is available). Empty if the account service is unavailable.
std::string switchProfileId();
// Network link is up: nifm reports internet, or the console has a local IP (LAN without internet,
// DNS-blocked setups). Cached for about a second.
bool networkConnected();

struct BatteryState {
    int percent = 0;
    bool charging = false;
};
std::optional<BatteryState> battery();  // nullopt if psm is unavailable

uint64_t totalMemory();      // bytes available to this process
uint64_t usedMemory();
std::string firmwareVersion();  // e.g. "22.0.0", or "" if unavailable
std::string describe();          // one-line summary for the log

// Performance clocks, three levels each (settings `performance.cpu_level` / `performance.gpu_level`,
// default 2):
//   CPU: 0 = system, 1 = 1428 MHz, 2 = 1785 MHz
//   GPU: 0 = system, 1 = 460.8 MHz, 2 = 768 MHz (docked never goes below 768 MHz)
// Level 0 restores the clock captured at startup, as does app exit. No-op in applet mode.
// `tickPerformanceClocks` re-applies about once a second, because sys-clk (if installed) resets
// clocks to its own profile. Call it from the main loop.
void setPerformanceClocks(int cpuLevel, int gpuLevel);
void tickPerformanceClocks();
void refreshPerformanceClocks();

// Temporary handheld backlight adjustment. Captures brightness and automatic control on first
// use; restore() (also called by the destructor) gives both back to the system.
class PlaybackBrightness {
public:
    ~PlaybackBrightness();
    PlaybackBrightness() = default;
    PlaybackBrightness(const PlaybackBrightness&) = delete;
    PlaybackBrightness& operator=(const PlaybackBrightness&) = delete;
    std::optional<float> current();
    bool set(float value);
    void restore();

private:
    bool ready_ = false;
    bool changed_ = false;
    bool autoBrightness_ = false;
    float original_ = 0;
};

// Reference-counted CPU boost: lifts the CPU to 1785 MHz while held and never touches the GPU.
// Hold one while playback starts or while decoding bursts of images. No-op in applet mode.
//   { platform::CpuBoost boost; decodeLotsOfImages(); }
class CpuBoost {
public:
    CpuBoost();
    ~CpuBoost();
    CpuBoost(const CpuBoost&) = delete;
    CpuBoost& operator=(const CpuBoost&) = delete;
};

// Reference-counted "keep awake": disables auto-sleep while any instance is alive (video
// playback, active downloads). With `screenOn` it also disables screen dimming.
class AwakeLock {
public:
    explicit AwakeLock(bool screenOn = true);
    ~AwakeLock();
    AwakeLock(const AwakeLock&) = delete;
    AwakeLock& operator=(const AwakeLock&) = delete;

private:
    bool screenOn_;
};

}  // namespace karafin::platform
