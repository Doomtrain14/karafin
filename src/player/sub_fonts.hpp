#pragma once

#include <string>

// Font sets for libass. This libass has no font provider (no fontconfig), so it cannot fall
// back per glyph across fonts: the subtitle font is chosen per subtitle language instead. Each
// set is a directory under the cache with Inter plus, for CJK, the matching Nintendo shared
// system font (extracted from pl shared memory once).
namespace karafin::player::subfonts {

struct FontSet {
    std::string dir;     // "" when unavailable
    std::string family;  // family name for mpv's sub-font
};

// Blocking (SD writes on first run): call from a worker thread. Idempotent and cached.
void prepare(bool includeChinese);
bool ready();
// Set for a subtitle language (ISO 639-2/B); Latin set for everything non-CJK. Main thread,
// after ready().
FontSet forLanguage(const std::string& language);

}  // namespace karafin::player::subfonts
