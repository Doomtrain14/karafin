#pragma once

// Vector icons built from the renderer's SDF primitives (designed on a 24x24 grid, drawn at any
// size without textures). Strokes overlap at joints, so prefer opaque colors; for translucent
// icons draw opaque inside Renderer::pushTarget or accept slightly denser joints.
//
//   gfx::drawIcon(r, gfx::Icon::Play, {x, y, 24, 24}, Color::white());

#include <cstdint>

#include "gfx/types.hpp"

namespace karafin::gfx {

class Renderer;

enum class Icon : uint8_t {
    None,
    Play,
    Pause,
    Stop,
    SkipNext,
    SkipPrevious,
    FastForward,
    Rewind,
    SeekForward,   // circular arrow with "10"
    SeekBack,
    Download,
    Check,
    Close,
    Plus,
    Minus,
    ChevronLeft,
    ChevronRight,
    ChevronUp,
    ChevronDown,
    Back,
    Subtitles,
    Audio,
    Settings,
    Search,
    Home,
    Library,
    Movie,
    Tv,
    User,
    Server,
    Refresh,
    Trash,
    Info,
    Warning,
    Star,
    Heart,
    More,
    Menu,
    List,
    Lock,
    Wifi,
    Filter,
    Clock,
    Exit,
    Speed,
    Count,
};

// Draws `icon` fitted in `box` (square recommended). `weight` scales the stroke width.
void drawIcon(Renderer& r, Icon icon, const Rect& box, Color color, float weight = 1.0f);

const char* iconName(Icon icon);

}  // namespace karafin::gfx
