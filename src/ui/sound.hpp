#pragma once

#include <functional>

namespace karafin::ui::sound {

enum class Cue { Move, Select, Back };

// Main thread only. Opens SDL audio lazily; honours ui.navigation_sounds on every call.
void play(Cue cue);
void stop();
void shutdown();

// On Switch, SDL audio and mpv's ao=hos share the process's single audren renderer, so they
// must never be open together. suspend() closes the device and blocks cues; resume() lifts one
// suspension, and cues stay blocked until readyWhen() returns true (e.g. mpv teardown finished).
void suspend();
void resume(std::function<bool()> readyWhen = {});

}  // namespace karafin::ui::sound
