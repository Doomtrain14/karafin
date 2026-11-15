#include "core/platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>

#include <netinet/in.h>
#include <unistd.h>
#include <switch.h>

#include "core/log.hpp"

namespace karafin::platform {
namespace {

std::mutex g_mutex;
bool g_initialized = false;
bool g_romfs = false;
bool g_sockets = false;
bool g_nifm = false;
bool g_psm = false;
int g_nxlinkSock = -1;
std::string g_firmware;

int g_boostCount = 0;
int g_awakeCount = 0;

// Valid Switch PLL steps. Level 0 leaves the system's clock alone.
constexpr u32 kCpuLevelHz[] = {0, 1428000000, 1785000000};
constexpr u32 kGpuLevelHz[] = {0, 460800000, 768000000};
constexpr u32 kCpuBoostHz = 1785000000;
constexpr u32 kGpuDockedHz = 768000000;

bool g_clkrstReady = false;
bool g_clkrstFailed = false;
int g_cpuLevel = 0;
int g_gpuLevel = 0;
bool g_clocksCaptured = false;
bool g_cpuChanged = false;
bool g_gpuChanged = false;
u32 g_origCpuHz = 0;
u32 g_origGpuHz = 0;
u32 g_appliedCpuHz = 0;
u32 g_appliedGpuHz = 0;
bool g_fastLoadFallback = false;
std::chrono::steady_clock::time_point g_nextClockCheck{};

bool ensureClkrstLocked() {
    if (g_clkrstReady) return true;
    if (g_clkrstFailed) return false;
    if (R_SUCCEEDED(clkrstInitialize())) {
        g_clkrstReady = true;
        return true;
    }
    g_clkrstFailed = true;
    log::warn("clock", "clkrst is unavailable; performance clocks stay off");
    return false;
}

bool readClockLocked(PcvModuleId id, u32* out) {
    ClkrstSession session{};
    if (R_FAILED(clkrstOpenSession(&session, id, 3))) return false;
    const Result rc = clkrstGetClockRate(&session, out);
    clkrstCloseSession(&session);
    return R_SUCCEEDED(rc) && *out > 0;
}

bool writeClockLocked(PcvModuleId id, u32 hz, bool report) {
    ClkrstSession session{};
    const Result open = clkrstOpenSession(&session, id, 3);
    if (R_FAILED(open)) {
        if (report) log::warn("clock", "cannot open {:#x}: {:#x}", static_cast<u32>(id), open);
        return false;
    }
    const Result rc = clkrstSetClockRate(&session, hz);
    clkrstCloseSession(&session);
    if (R_FAILED(rc)) {
        if (report) log::warn("clock", "cannot set {:#x} to {} MHz: {:#x}", static_cast<u32>(id), hz / 1000000, rc);
        return false;
    }
    return true;
}

void captureClocksLocked() {
    if (g_clocksCaptured || !ensureClkrstLocked()) return;
    u32 cpu = 0, gpu = 0;
    if (readClockLocked(PcvModuleId_CpuBus, &cpu) && readClockLocked(PcvModuleId_GPU, &gpu)) {
        g_origCpuHz = cpu;
        g_origGpuHz = gpu;
        g_clocksCaptured = true;
        log::info("clock", "stock clocks: CPU {} MHz, GPU {} MHz", cpu / 1000000, gpu / 1000000);
    }
}

// 0 = leave the system clock. A boost lifts only the CPU; the GPU never drops for it.
u32 wantedCpuHzLocked() {
    const u32 level = kCpuLevelHz[g_cpuLevel];
    return g_boostCount > 0 ? std::max(level, kCpuBoostHz) : level;
}

u32 wantedGpuHzLocked() {
    const u32 level = kGpuLevelHz[g_gpuLevel];
    if (level == 0) return 0;
    return isDocked() ? std::max(level, kGpuDockedHz) : level;
}

// Writes one clock (or restores it when `want` is 0). Re-reads the live rate first so a
// sys-clk reset is noticed without spamming writes.
void syncClockLocked(PcvModuleId id, const char* name, u32 want, u32 orig, u32& applied, bool& changed) {
    if (want == 0) {
        if (changed && orig > 0 && writeClockLocked(id, orig, true)) {
            log::info("clock", "{} back to {} MHz", name, orig / 1000000);
            changed = false;
            applied = 0;
        }
        return;
    }
    const bool announce = applied != want;
    if (!announce) {
        u32 cur = 0;
        if (readClockLocked(id, &cur) && cur == want) return;
    }
    if (writeClockLocked(id, want, announce)) {
        if (announce) log::info("clock", "{} {} MHz", name, want / 1000000);
        applied = want;
        changed = true;
    }
}

void applyClocksLocked() {
    if (isAppletMode() || !ensureClkrstLocked()) return;
    captureClocksLocked();
    syncClockLocked(PcvModuleId_CpuBus, "CPU", wantedCpuHzLocked(), g_origCpuHz, g_appliedCpuHz, g_cpuChanged);
    syncClockLocked(PcvModuleId_GPU, "GPU", wantedGpuHzLocked(), g_origGpuHz, g_appliedGpuHz, g_gpuChanged);
}

void restoreClocksLocked() {
    if (!g_clkrstReady || !g_clocksCaptured) return;
    if (g_cpuChanged) writeClockLocked(PcvModuleId_CpuBus, g_origCpuHz, true);
    if (g_gpuChanged) writeClockLocked(PcvModuleId_GPU, g_origGpuHz, true);
    if (g_cpuChanged || g_gpuChanged)
        log::info("clock", "restored clocks: CPU {} MHz, GPU {} MHz", g_origCpuHz / 1000000, g_origGpuHz / 1000000);
    g_cpuChanged = g_gpuChanged = false;
    g_appliedCpuHz = g_appliedGpuHz = 0;
}

// Larger TCP receive windows than libnx defaults: mpv/ffmpeg and downloads stream big files.
constexpr SocketInitConfig kSocketConfig = {
    .tcp_tx_buf_size = 0x10000,
    .tcp_rx_buf_size = 0x40000,
    .tcp_tx_buf_max_size = 0x40000,
    .tcp_rx_buf_max_size = 0x100000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 8,
    .num_bsd_sessions = 8,
    .bsd_service_type = BsdServiceType_Auto,
};

}  // namespace

PlaybackBrightness::~PlaybackBrightness() { restore(); }

std::optional<float> PlaybackBrightness::current() {
    if (isDocked()) {
        log::info("brightness", "backlight adjustment is only available in handheld mode");
        return std::nullopt;
    }
    if (!ready_) {
        const Result rc = lblInitialize();
        if (R_FAILED(rc)) {
            log::warn("brightness", "lbl unavailable: {:#x}", rc);
            return std::nullopt;
        }
        const Result brightness = lblGetCurrentBrightnessSetting(&original_);
        const Result automatic = lblIsAutoBrightnessControlEnabled(&autoBrightness_);
        if (R_FAILED(brightness) || R_FAILED(automatic) || !std::isfinite(original_)) {
            log::warn("brightness", "cannot capture backlight settings: {:#x}, {:#x}", brightness, automatic);
            lblExit();
            return std::nullopt;
        }
        ready_ = true;
    }
    float value = 0;
    const Result rc = autoBrightness_ && !changed_ ? lblGetBrightnessSettingAppliedToBacklight(&value)
                                                  : lblGetCurrentBrightnessSetting(&value);
    if (R_FAILED(rc) || !std::isfinite(value)) {
        log::warn("brightness", "cannot read brightness: {:#x}", rc);
        return std::nullopt;
    }
    return value;
}

bool PlaybackBrightness::set(float value) {
    if (!ready_ || !std::isfinite(value)) {
        log::warn("brightness", "invalid adjustment or backlight settings have not been captured");
        return false;
    }
    changed_ = true;
    Result rc = lblDisableAutoBrightnessControl();
    if (R_SUCCEEDED(rc)) rc = lblSetCurrentBrightnessSetting(std::clamp(value, 0.05f, 1.0f));
    if (R_SUCCEEDED(rc)) rc = lblApplyCurrentBrightnessSettingToBacklight();
    if (R_FAILED(rc)) {
        log::warn("brightness", "cannot adjust backlight: {:#x}", rc);
        restore();
        return false;
    }
    return true;
}

void PlaybackBrightness::restore() {
    if (!ready_) return;
    if (changed_) {
        const Result brightness = lblSetCurrentBrightnessSetting(original_);
        const Result apply = lblApplyCurrentBrightnessSettingToBacklight();
        const Result automatic = autoBrightness_ ? lblEnableAutoBrightnessControl() : lblDisableAutoBrightnessControl();
        if (R_FAILED(brightness) || R_FAILED(apply) || R_FAILED(automatic))
            log::warn("brightness", "cannot restore backlight: {:#x}, {:#x}, {:#x}", brightness, apply, automatic);
    }
    lblExit();
    ready_ = changed_ = false;
}

void init() {
    std::lock_guard lock(g_mutex);
    if (g_initialized) return;
    g_initialized = true;

    g_romfs = R_SUCCEEDED(romfsInit());

    Result rc = socketInitialize(&kSocketConfig);
    if (R_FAILED(rc)) rc = socketInitializeDefault();
    g_sockets = R_SUCCEEDED(rc);

    // Only connects when launched through `nxlink`; otherwise __nxlink_host is 0.
    if (g_sockets && __nxlink_host.s_addr != 0) {
        g_nxlinkSock = nxlinkStdio();
        if (g_nxlinkSock >= 0) log::setStdoutMirror(true);
    }

    g_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    g_psm = R_SUCCEEDED(psmInitialize());

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw{};
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) g_firmware = fw.display_version;
        setsysExit();
    }
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return;
    if (g_fastLoadFallback) appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    g_fastLoadFallback = false;
    g_boostCount = 0;
    g_cpuLevel = g_gpuLevel = 0;
    restoreClocksLocked();
    if (g_clkrstReady) {
        clkrstExit();
        g_clkrstReady = false;
    }
    if (g_awakeCount > 0) {
        appletSetMediaPlaybackState(false);
        appletSetAutoSleepDisabled(false);
    }
    g_awakeCount = 0;
    if (g_nifm) nifmExit();
    if (g_psm) psmExit();
    g_psm = false;
    log::setStdoutMirror(false);
    if (g_nxlinkSock >= 0) {
        close(g_nxlinkSock);
        g_nxlinkSock = -1;
    }
    if (g_sockets) socketExit();
    if (g_romfs) romfsExit();
    g_nifm = g_sockets = g_romfs = false;
    g_initialized = false;
}

bool isAppletMode() {
    const AppletType t = appletGetAppletType();
    return t != AppletType_Application && t != AppletType_SystemApplication;
}

const char* appletTypeName() {
    switch (appletGetAppletType()) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        case AppletType_OverlayApplet: return "OverlayApplet";
        case AppletType_SystemApplet: return "SystemApplet";
        default: return "Unknown";
    }
}

bool isDocked() { return appletGetOperationMode() == AppletOperationMode_Console; }
bool hasFocus() { return appletGetFocusState() == AppletFocusState_InFocus; }
bool nxlinkConnected() { return g_nxlinkSock >= 0; }
bool romfsMounted() { return g_romfs; }

std::optional<BatteryState> battery() {
    if (!g_psm) return std::nullopt;
    u32 percent = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&percent))) return std::nullopt;
    PsmChargerType charger = PsmChargerType_Unconnected;
    psmGetChargerType(&charger);
    return BatteryState{static_cast<int>(std::min<u32>(percent, 100)), charger != PsmChargerType_Unconnected};
}

bool networkConnected() {
    if (!g_nifm) return false;
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status))) return false;
    return status == NifmInternetConnectionStatus_Connected;
}

uint64_t totalMemory() {
    u64 v = 0;
    svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    return v;
}

uint64_t usedMemory() {
    u64 v = 0;
    svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    return v;
}

std::string firmwareVersion() { return g_firmware; }

std::string describe() {
    return std::format("fw={} applet={} docked={} mem={}/{} MiB romfs={} sockets={} nxlink={}",
                       g_firmware.empty() ? "?" : g_firmware, appletTypeName(), isDocked(),
                       usedMemory() >> 20, totalMemory() >> 20, g_romfs, g_sockets, nxlinkConnected());
}

void setPerformanceClocks(int cpuLevel, int gpuLevel) {
    std::lock_guard lock(g_mutex);
    g_cpuLevel = std::clamp(cpuLevel, 0, 2);
    g_gpuLevel = std::clamp(gpuLevel, 0, 2);
    g_nextClockCheck = {};
    applyClocksLocked();
}

void refreshPerformanceClocks() {
    std::lock_guard lock(g_mutex);
    g_appliedCpuHz = g_appliedGpuHz = 0;
    g_nextClockCheck = {};
    applyClocksLocked();
}

void tickPerformanceClocks() {
    std::lock_guard lock(g_mutex);
    if (g_cpuLevel == 0 && g_gpuLevel == 0 && g_boostCount == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextClockCheck) return;
    g_nextClockCheck = now + std::chrono::seconds(1);
    applyClocksLocked();
}

// Boost mode (ApmCpuBoostMode_FastLoad) drops the GPU to 76.8 MHz, which made UI animations
// stutter whenever images loaded. Raise only the CPU through clkrst; FastLoad is a last resort
// when clkrst is missing.
CpuBoost::CpuBoost() {
    std::lock_guard lock(g_mutex);
    if (g_boostCount++ != 0 || isAppletMode()) return;
    if (ensureClkrstLocked()) {
        applyClocksLocked();
    } else {
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        g_fastLoadFallback = true;
    }
}

CpuBoost::~CpuBoost() {
    std::lock_guard lock(g_mutex);
    if (g_boostCount == 0 || --g_boostCount != 0 || isAppletMode()) return;
    if (g_fastLoadFallback) {
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        g_fastLoadFallback = false;
    }
    applyClocksLocked();
}

AwakeLock::AwakeLock() {
    std::lock_guard lock(g_mutex);
    if (g_awakeCount++ == 0) {
        appletSetAutoSleepDisabled(true);
        appletSetMediaPlaybackState(true);
    }
}

AwakeLock::~AwakeLock() {
    std::lock_guard lock(g_mutex);
    if (g_awakeCount > 0 && --g_awakeCount == 0) {
        appletSetMediaPlaybackState(false);
        appletSetAutoSleepDisabled(false);
    }
}

}  // namespace karafin::platform
