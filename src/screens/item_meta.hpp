#pragma once

#include <string>
#include <vector>

#include "jellyfin/models.hpp"
#include "screens/widgets/chips.hpp"

namespace karafin::screens {

// "2019  [PG-13]  2h 11m  * 7.8" (series: years and season count).
std::vector<Chip> itemChips(const jellyfin::BaseItem& item);
// Technical badges from the media streams: resolution, HDR flavour, video codec, audio, CC.
std::vector<Chip> mediaChips(const jellyfin::MediaSource& source);
// "Action, Drama, Thriller" (first `max` genres).
std::string genresText(const jellyfin::BaseItem& item, size_t max = 3);
// One-line description of a stream for track pickers: "English - AAC 5.1", "Japanese (Forced)".
std::string streamTitle(const jellyfin::MediaStream& s);
std::string streamDetail(const jellyfin::MediaStream& s);

}  // namespace karafin::screens
