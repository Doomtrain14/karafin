#include "player/sub_fonts.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include <switch.h>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "player/languages.hpp"

namespace karafin::player::subfonts {
namespace {

std::mutex g_mutex;
std::atomic<bool> g_ready{false};
std::map<std::string, FontSet> g_sets;  // "latin", "jpn", "kor", "chi"

std::string familyOf(const void* data, size_t size, const std::string& fallback) {
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) != 0) return fallback;
    std::string family = fallback;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(lib, static_cast<const FT_Byte*>(data), static_cast<FT_Long>(size), 0, &face) == 0) {
        if (face->family_name) family = face->family_name;
        FT_Done_Face(face);
    }
    FT_Done_FreeType(lib);
    return family;
}

bool writeIfChanged(const std::string& path, std::string_view bytes) {
    if (auto sz = fs::fileSize(path); sz && *sz == bytes.size()) return true;
    if (!fs::writeFileAtomic(path, bytes)) {
        log::warn("player", "cannot write subtitle font {}", path);
        return false;
    }
    return true;
}

struct SharedFont {
    std::string bytes;
    std::string family;
};

std::optional<SharedFont> sharedFont(PlSharedFontType type) {
    PlFontData font{};
    if (R_FAILED(plGetSharedFontByType(&font, type)) || !font.address || font.size == 0) return std::nullopt;
    SharedFont f;
    f.bytes.assign(static_cast<const char*>(font.address), font.size);
    f.family = familyOf(font.address, font.size, "");
    return f;
}

}  // namespace

void prepare(bool includeChinese) {
    if (g_ready.load()) return;
    std::lock_guard lock(g_mutex);
    if (g_ready.load()) return;

    const std::string root = paths::cacheDir() + "/subfonts";
    auto inter = fs::readFile(paths::romfs("fonts/Inter-Regular.ttf"));
    auto interBold = fs::readFile(paths::romfs("fonts/Inter-Bold.ttf"));
    const std::string interFamily = inter ? familyOf(inter->data(), inter->size(), "Inter") : "Inter";

    auto makeSet = [&](const std::string& name, const std::vector<std::pair<std::string, const std::string*>>& extra,
                       const std::string& family) {
        const std::string dir = root + "/" + name;
        if (!fs::ensureDir(dir)) return;
        bool ok = true;
        if (inter) ok &= writeIfChanged(dir + "/Inter-Regular.ttf", *inter);
        if (interBold) ok &= writeIfChanged(dir + "/Inter-Bold.ttf", *interBold);
        for (const auto& [file, bytes] : extra) ok &= writeIfChanged(dir + "/" + file, *bytes);
        if (ok) g_sets[name] = FontSet{dir, family};
    };

    makeSet("latin", {}, interFamily);

    const bool pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (pl) {
        if (auto jp = sharedFont(PlSharedFontType_Standard); jp && !jp->family.empty())
            makeSet("jpn", {{"system-standard.ttf", &jp->bytes}}, jp->family);
        if (auto ko = sharedFont(PlSharedFontType_KO); ko && !ko->family.empty())
            makeSet("kor", {{"system-ko.ttf", &ko->bytes}}, ko->family);
        if (includeChinese) {
            auto hans = sharedFont(PlSharedFontType_ChineseSimplified);
            auto ext = sharedFont(PlSharedFontType_ExtChineseSimplified);
            if (hans && !hans->family.empty()) {
                std::vector<std::pair<std::string, const std::string*>> files = {{"system-zh-hans.ttf", &hans->bytes}};
                if (ext) files.emplace_back("system-zh-hans-ext.ttf", &ext->bytes);
                makeSet("chi", files, hans->family);
            }
        }
        plExit();
    } else {
        log::warn("player", "pl unavailable; CJK subtitles may render as boxes");
    }
    std::string summary;
    for (const auto& [k, v] : g_sets) summary += k + "=" + v.family + " ";
    log::info("player", "subtitle fonts: {}", summary);
    g_ready.store(true);
}

bool ready() { return g_ready.load(); }

FontSet forLanguage(const std::string& language) {
    std::lock_guard lock(g_mutex);
    const std::string l = lang::normalize(language);
    const char* key = "latin";
    if (l == "jpn") key = "jpn";
    else if (l == "kor") key = "kor";
    else if (l == "chi" || l == "yue" || l == "cmn") key = "chi";
    auto it = g_sets.find(key);
    if (it == g_sets.end()) it = g_sets.find("latin");
    return it == g_sets.end() ? FontSet{} : it->second;
}

}  // namespace karafin::player::subfonts
