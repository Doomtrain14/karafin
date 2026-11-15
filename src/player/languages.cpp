#include "player/languages.hpp"

#include <algorithm>
#include <cctype>

namespace karafin::player::lang {
namespace {

const std::vector<Language> kLanguages = {
    {"alb", "sqi", "sq", "Albanian"},
    {"ara", "", "ar", "Arabic"},
    {"arm", "hye", "hy", "Armenian"},
    {"baq", "eus", "eu", "Basque"},
    {"ben", "", "bn", "Bengali"},
    {"bos", "", "bs", "Bosnian"},
    {"bul", "", "bg", "Bulgarian"},
    {"bur", "mya", "my", "Burmese"},
    {"cat", "", "ca", "Catalan"},
    {"chi", "zho", "zh", "Chinese"},
    {"hrv", "", "hr", "Croatian"},
    {"cze", "ces", "cs", "Czech"},
    {"dan", "", "da", "Danish"},
    {"dut", "nld", "nl", "Dutch"},
    {"eng", "", "en", "English"},
    {"est", "", "et", "Estonian"},
    {"fil", "", "", "Filipino"},
    {"fin", "", "fi", "Finnish"},
    {"fre", "fra", "fr", "French"},
    {"glg", "", "gl", "Galician"},
    {"geo", "kat", "ka", "Georgian"},
    {"ger", "deu", "de", "German"},
    {"gre", "ell", "el", "Greek"},
    {"heb", "", "he", "Hebrew"},
    {"hin", "", "hi", "Hindi"},
    {"hun", "", "hu", "Hungarian"},
    {"ice", "isl", "is", "Icelandic"},
    {"ind", "", "id", "Indonesian"},
    {"ita", "", "it", "Italian"},
    {"jpn", "", "ja", "Japanese"},
    {"kor", "", "ko", "Korean"},
    {"lav", "", "lv", "Latvian"},
    {"lit", "", "lt", "Lithuanian"},
    {"mac", "mkd", "mk", "Macedonian"},
    {"may", "msa", "ms", "Malay"},
    {"nor", "", "no", "Norwegian"},
    {"nob", "", "nb", "Norwegian Bokmål"},
    {"per", "fas", "fa", "Persian"},
    {"pol", "", "pl", "Polish"},
    {"por", "", "pt", "Portuguese"},
    {"pob", "", "", "Portuguese (Brazil)"},
    {"rum", "ron", "ro", "Romanian"},
    {"rus", "", "ru", "Russian"},
    {"srp", "", "sr", "Serbian"},
    {"slo", "slk", "sk", "Slovak"},
    {"slv", "", "sl", "Slovenian"},
    {"spa", "", "es", "Spanish"},
    {"swe", "", "sv", "Swedish"},
    {"tam", "", "ta", "Tamil"},
    {"tha", "", "th", "Thai"},
    {"tur", "", "tr", "Turkish"},
    {"ukr", "", "uk", "Ukrainian"},
    {"urd", "", "ur", "Urdu"},
    {"vie", "", "vi", "Vietnamese"},
    {"wel", "cym", "cy", "Welsh"},
};

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const Language* find(std::string_view code) {
    const std::string c = lower(code);
    if (c.empty()) return nullptr;
    for (const auto& l : kLanguages)
        if (c == l.code || c == l.alt || c == l.iso1) return &l;
    // "pt-br", "en-us", "zh-hans" style tags: match the primary subtag.
    if (auto dash = c.find_first_of("-_"); dash != std::string::npos && dash > 0) {
        if (c.rfind("pt-br", 0) == 0 || c.rfind("pt_br", 0) == 0) return find("pob");
        return find(c.substr(0, dash));
    }
    return nullptr;
}

}  // namespace

const std::vector<Language>& common() { return kLanguages; }

std::string normalize(std::string_view code) {
    if (const Language* l = find(code)) return l->code;
    return lower(code);
}

bool same(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return false;
    return normalize(a) == normalize(b);
}

std::string displayName(std::string_view code) {
    const std::string c = lower(code);
    if (c.empty() || c == "und" || c == "unk") return "Unknown";
    if (const Language* l = find(c)) return l->name;
    return std::string(code);
}

}  // namespace karafin::player::lang
