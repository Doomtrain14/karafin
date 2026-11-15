#pragma once

// Shared helpers for the concrete screens: text formatting, Jellyfin image requests, card
// binding, item navigation and starting playback (with the resume / start-over prompt).

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "core/cancel.hpp"
#include "gfx/icons.hpp"
#include "gfx/image.hpp"
#include "jellyfin/client.hpp"
#include "ui/widgets/poster_card.hpp"

namespace karafin::screens {

namespace jf = karafin::jellyfin;

// ---- Formatting ---------------------------------------------------------------------------------
std::string formatRuntime(int64_t ticks);         // "2h 11m", "43m", "" for 0
std::string formatClock(int64_t ticks);           // "1:02:03", "42:17"
std::string formatBytes(uint64_t bytes);          // "1.2 GB", "640 MB"
std::string formatSpeed(double bytesPerSecond);   // "3.1 MB/s"
std::string formatEta(double seconds);            // "4 min left", "1 h 12 min left"
std::string formatRating(double rating);          // "7.8"
std::string formatCount(int64_t n);               // "1,234"
std::string remainingText(const jf::BaseItem& item);  // "23 min left" or ""
std::string yearText(const jf::BaseItem& item);       // "2019", "2008 - 2013", "2021 - Present"
// Joins the non-empty parts with a middle dot.
std::string joinDots(std::initializer_list<std::string> parts);
std::string lowerAscii(std::string_view s);
std::string upperAscii(std::string_view s);
// ISO 639-2 code -> English name ("eng" -> "English"); unknown codes are returned upper-cased.
std::string languageName(std::string_view code);

// ---- Images -------------------------------------------------------------------------------------
// Request sized in framebuffer pixels for a logical box.
gfx::ImageRequest imageRequest(std::string url, float logicalW, float logicalH, float blur = 0.0f);
// Full-screen backdrop request for ui::Backdrop ("" url -> empty request).
gfx::ImageRequest backdropRequest(const jf::BaseItem& item, float blur = 0.0f);
std::string posterUrl(const jf::BaseItem& item);
std::string wideUrl(const jf::BaseItem& item);   // episode still / thumb / backdrop
std::string logoUrl(const jf::BaseItem& item);
gfx::Color placeholderColor(std::string_view seed);
gfx::Icon typeIcon(const jf::BaseItem& item);

// ---- Cards --------------------------------------------------------------------------------------
// Fills a PosterCard from an item: art, title/subtitle, progress, watched check, unplayed badge.
// `boundUrl` (optional) remembers the art URL so rebinding the same item keeps the loaded image
// instead of re-requesting it (which would flash the placeholder).
void bindCard(ui::PosterCard& card, const jf::BaseItem& item, std::string* boundUrl = nullptr);
std::string cardTitle(const jf::BaseItem& item);
std::string cardSubtitle(const jf::BaseItem& item, bool wide);

// ---- Navigation ---------------------------------------------------------------------------------
// Pushes the right page for an item: series / season -> series page, playable -> detail page,
// folders / collections / libraries -> library grid, person -> filmography.
void openItem(const jf::BaseItem& item);

// ---- Playback -----------------------------------------------------------------------------------
struct PlayOptions {
    std::optional<int> audioStreamIndex;
    std::optional<int> subtitleStreamIndex;  // -1 = off
    std::string mediaSourceId;
};

// Asks "Resume / Start over" when the item has a saved position, then starts the player.
void playItem(const jf::BaseItem& item, const PlayOptions& options = {});
// Starts at an explicit position (0 = from the beginning).
void playAt(const jf::BaseItem& item, int64_t startTicks, const PlayOptions& options = {});
// Series: plays the next-up episode, or the first unwatched / first episode.
void playSeries(const std::string& seriesId, CancelToken token);
// Offline: plays a downloaded item (asks resume when `positionTicks` > 0).
void playOffline(const std::string& itemId, const std::string& title, int64_t positionTicks, int64_t runTimeTicks);

// ---- Errors -------------------------------------------------------------------------------------
// Short user-facing message for an API error (falls back to a generic sentence).
std::string errorMessage(const jf::ApiError& error);

}  // namespace karafin::screens
