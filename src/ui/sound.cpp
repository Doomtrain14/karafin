#include "ui/sound.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include <SDL.h>

#include "core/log.hpp"
#include "core/settings.hpp"

namespace karafin::ui::sound {
namespace {

SDL_AudioDeviceID device = 0;
bool attempted = false;
bool audioInitialized = false;
std::array<std::vector<int16_t>, 3> samples;
uint64_t lastPlay = 0;
int suspensions = 0;
std::function<bool()> blockedUntil;

bool available() {
    if (suspensions > 0) return false;
    if (blockedUntil) {
        if (!blockedUntil()) return false;
        blockedUntil = nullptr;
    }
    return true;
}

bool initialize() {
    if (!available()) return false;
    if (attempted) return device != 0;
    attempted = true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log::warn("sound", "SDL audio unavailable: {}", SDL_GetError());
        return false;
    }
    audioInitialized = true;
    SDL_AudioSpec spec{};
    spec.freq = 48000;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 512;
    device = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    if (!device) {
        log::warn("sound", "cannot open navigation audio: {}", SDL_GetError());
        return false;
    }
    constexpr double frequencies[] = {660, 880, 440};
    for (size_t cue = 0; cue < samples.size(); ++cue) {
        const int frames = cue == 0 ? 1680 : 2880;
        auto& pcm = samples[cue];
        pcm.resize(frames * 2);
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i) / (frames - 1);
            const double envelope = std::sin(std::numbers::pi * t);
            const auto value = static_cast<int16_t>(2200 * envelope * envelope *
                std::sin(2 * std::numbers::pi * frequencies[cue] * i / spec.freq));
            pcm[i * 2] = pcm[i * 2 + 1] = value;
        }
    }
    SDL_PauseAudioDevice(device, 0);
    return true;
}

}  // namespace

void play(Cue cue) {
    if (!Settings::instance().get<bool>("ui.navigation_sounds", true)) return;
    if (!initialize()) return;
    const uint64_t now = SDL_GetTicks64();
    if (lastPlay && now - lastPlay < 40) return;
    lastPlay = now;
    const auto& pcm = samples[static_cast<size_t>(cue)];
    SDL_ClearQueuedAudio(device);
    if (SDL_QueueAudio(device, pcm.data(), static_cast<Uint32>(pcm.size() * sizeof(int16_t))) != 0)
        log::warn("sound", "cannot queue navigation sound: {}", SDL_GetError());
}

void stop() {
    if (device) SDL_ClearQueuedAudio(device);
}

void shutdown() {
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    if (audioInitialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    attempted = audioInitialized = false;
    lastPlay = 0;
}

void suspend() {
    ++suspensions;
    shutdown();
}

void resume(std::function<bool()> readyWhen) {
    if (suspensions > 0) --suspensions;
    if (readyWhen) blockedUntil = std::move(readyWhen);
}

}  // namespace karafin::ui::sound
