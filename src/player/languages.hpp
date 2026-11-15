#pragma once

#include <string>
#include <string_view>
#include <vector>

// ISO 639 helpers for track matching and the subtitle-download language picker.
namespace karafin::player::lang {

struct Language {
    const char* code;    // ISO 639-2/B as Jellyfin uses it ("fre", "ger", "chi")
    const char* alt;     // ISO 639-2/T variant ("fra", "deu", "zho") or ""
    const char* iso1;    // two-letter code or ""
    const char* name;    // English name
};

// Common languages for the download picker, ordered by name.
const std::vector<Language>& common();

// Canonical 639-2/B code for any of the variants ("fra" -> "fre", "en" -> "eng", "EN" -> "eng").
// Unknown codes are returned lower-cased.
std::string normalize(std::string_view code);
bool same(std::string_view a, std::string_view b);
// "English" for "eng"; the code itself when unknown; "Unknown" for "" / "und".
std::string displayName(std::string_view code);

}  // namespace karafin::player::lang
