#include "gfx/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <GLES3/gl3.h>
#include <hb.h>
#include <switch.h>

#include "core/fs.hpp"
#include "core/log.hpp"
#include "core/paths.hpp"
#include "core/perf.hpp"
#include "gfx/renderer.hpp"
#include "gfx/texture.hpp"

namespace karafin::gfx::text {
namespace {

constexpr int kAtlasSize = 1024;
constexpr int kMaxAtlasPages = 4;
constexpr int kGlyphPad = 1;

struct Face {
    std::string name;
    std::string storage;  // owns the bytes for romfs faces; system faces point into pl shared memory
    const uint8_t* data = nullptr;
    size_t size = 0;
    FT_Face ft = nullptr;
    hb_blob_t* blob = nullptr;
    hb_face_t* hbFace = nullptr;
    hb_font_t* hbFont = nullptr;
    float ftSize = -1.0f;
    float ascender = 0.9f;   // in em
    float descender = 0.25f; // in em, positive
    float capHeight = 0.7f;  // in em
};

struct Page {
    TexturePtr texture;
    std::vector<uint8_t> pixels;  // CPU copy; glyphs are written here, uploaded by flushAtlas()
    int x = 0, y = 0, shelfH = 0;
    int dirtyX0 = kAtlasSize, dirtyY0 = kAtlasSize, dirtyX1 = 0, dirtyY1 = 0;

    void markDirty(int x0, int y0, int x1, int y1) {
        dirtyX0 = std::min(dirtyX0, x0);
        dirtyY0 = std::min(dirtyY0, y0);
        dirtyX1 = std::max(dirtyX1, x1);
        dirtyY1 = std::max(dirtyY1, y1);
    }
    bool dirty() const { return dirtyX1 > dirtyX0 && dirtyY1 > dirtyY0; }
};

struct LayoutKey {
    std::string text;
    float size, lineHeight, tracking, maxWidth, scale;
    int maxLines;
    FontWeight weight;
    TextAlign align;
    bool ellipsis;

    bool operator==(const LayoutKey&) const = default;
};

struct CacheEntry {
    LayoutKey key;
    TextLayoutPtr layout;
    uint64_t lastFrame = 0;
};

struct GlyphRec {
    uint16_t face;
    uint32_t glyph;
    float advance, xoff, yoff;
    uint32_t cluster;  // codepoint index within the paragraph
    bool space;
};

struct System {
    FT_Library lib = nullptr;
    std::vector<std::unique_ptr<Face>> faces;
    int primary[5] = {-1, -1, -1, -1, -1};
    std::vector<int> fallbacks;
    bool plReady = false;
    hb_buffer_t* buffer = nullptr;

    std::vector<Page> pages;
    std::unordered_map<uint64_t, GlyphQuad> glyphs;

    std::unordered_map<uint64_t, CacheEntry> cache;
    uint64_t frame = 0;
    float scale = 1.0f;
    bool ready = false;
};

System* g = nullptr;
const GlyphQuad kEmptyGlyph{};

uint64_t fnv1a(const void* data, size_t len, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

template <class T>
uint64_t mix(uint64_t h, const T& v) {
    return fnv1a(&v, sizeof(v), h);
}

bool loadFaceFromMemory(Face& f) {
    if (FT_New_Memory_Face(g->lib, f.data, static_cast<FT_Long>(f.size), 0, &f.ft) != 0) {
        log::error("text", "FreeType cannot open {}", f.name);
        return false;
    }
    f.blob = hb_blob_create(reinterpret_cast<const char*>(f.data), static_cast<unsigned>(f.size),
                            HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f.hbFace = hb_face_create(f.blob, 0);
    f.hbFont = hb_font_create(f.hbFace);
    const float upem = static_cast<float>(f.ft->units_per_EM > 0 ? f.ft->units_per_EM : 1000);
    f.ascender = static_cast<float>(f.ft->ascender) / upem;
    f.descender = static_cast<float>(-f.ft->descender) / upem;
    if (auto* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(f.ft, FT_SFNT_OS2)); os2 && os2->sCapHeight > 0)
        f.capHeight = static_cast<float>(os2->sCapHeight) / upem;
    else
        f.capHeight = f.ascender * 0.75f;
    return true;
}

int addRomfsFace(const char* file) {
    auto bytes = fs::readFile(paths::romfs(std::string("fonts/") + file));
    if (!bytes || bytes->empty()) {
        log::warn("text", "missing romfs font {}", file);
        return -1;
    }
    auto f = std::make_unique<Face>();
    f->name = file;
    f->storage = std::move(*bytes);
    f->data = reinterpret_cast<const uint8_t*>(f->storage.data());
    f->size = f->storage.size();
    if (!loadFaceFromMemory(*f)) return -1;
    g->faces.push_back(std::move(f));
    return static_cast<int>(g->faces.size() - 1);
}

int addSharedFace(PlSharedFontType type, const char* name) {
    if (!g->plReady) return -1;
    PlFontData font{};
    if (R_FAILED(plGetSharedFontByType(&font, type)) || !font.address || font.size == 0) {
        log::warn("text", "shared font {} unavailable", name);
        return -1;
    }
    auto f = std::make_unique<Face>();
    f->name = name;
    f->data = static_cast<const uint8_t*>(font.address);
    f->size = font.size;
    if (!loadFaceFromMemory(*f)) return -1;
    g->faces.push_back(std::move(f));
    return static_cast<int>(g->faces.size() - 1);
}

void setFtSize(Face& f, float pixelSize) {
    if (f.ftSize == pixelSize) return;
    FT_Set_Char_Size(f.ft, 0, static_cast<FT_F26Dot6>(std::lround(pixelSize * 64.0f)), 72, 72);
    f.ftSize = pixelSize;
}

void resetAtlas() {
    if (Renderer::available()) Renderer::instance().flush();
    g->glyphs.clear();
    for (size_t i = 1; i < g->pages.size(); ++i) g->pages[i].texture.reset();
    if (g->pages.size() > 1) g->pages.resize(1);
    if (!g->pages.empty()) {
        Page& p = g->pages[0];
        p.x = p.y = p.shelfH = 0;
        // Padding between glyphs must read as zero for bilinear sampling.
        std::fill(p.pixels.begin(), p.pixels.end(), uint8_t{0});
        p.markDirty(0, 0, kAtlasSize, kAtlasSize);
    }
}

Page& newPage() {
    Page p;
    p.pixels.assign(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0);
    p.texture = Texture::create(kAtlasSize, kAtlasSize, TextureFormat::R8, p.pixels.data(), true);
    g->pages.push_back(std::move(p));
    return g->pages.back();
}

// Finds space for a w x h bitmap; may add a page or reset the whole atlas.
bool allocate(int w, int h, Page*& outPage, int& outX, int& outY) {
    const int pw = w + kGlyphPad, ph = h + kGlyphPad;
    if (pw > kAtlasSize || ph > kAtlasSize) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (g->pages.empty()) newPage();
        Page* p = &g->pages.back();
        if (p->x + pw > kAtlasSize) {
            p->y += p->shelfH;
            p->x = 0;
            p->shelfH = 0;
        }
        if (p->y + ph > kAtlasSize) {
            if (static_cast<int>(g->pages.size()) < kMaxAtlasPages) {
                p = &newPage();
            } else {
                log::debug("text", "glyph atlas full; resetting");
                resetAtlas();
                continue;
            }
        }
        outPage = p;
        outX = p->x + kGlyphPad;
        outY = p->y + kGlyphPad;
        p->x += pw;
        p->shelfH = std::max(p->shelfH, ph);
        return true;
    }
    return false;
}

uint32_t decodeUtf8(std::string_view s, size_t& i) {
    const auto c = static_cast<uint8_t>(s[i++]);
    if (c < 0x80) return c;
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return 0xFFFD;
    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (static_cast<uint8_t>(s[i++]) & 0x3F);
    }
    return cp;
}

bool isSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == 0xA0; }

bool isCjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

bool isJoiner(uint32_t cp) {
    if (cp == 0x200D || cp == 0x200C || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF)) return true;
    const hb_unicode_general_category_t cat = hb_unicode_general_category(hb_unicode_funcs_get_default(), cp);
    return cat == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || cat == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
           cat == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK;
}

int faceForCodepoint(int primary, uint32_t cp) {
    if (FT_Get_Char_Index(g->faces[static_cast<size_t>(primary)]->ft, cp) != 0) return primary;
    for (int fb : g->fallbacks)
        if (fb != primary && FT_Get_Char_Index(g->faces[static_cast<size_t>(fb)]->ft, cp) != 0) return fb;
    return primary;
}

void shapeRun(const std::vector<uint32_t>& cps, size_t begin, size_t end, int face, float pixelSize, float tracking,
              std::vector<GlyphRec>& out) {
    Face& f = *g->faces[static_cast<size_t>(face)];
    const int scale26 = static_cast<int>(std::lround(pixelSize * 64.0f));
    hb_font_set_scale(f.hbFont, scale26, scale26);
    hb_buffer_clear_contents(g->buffer);
    hb_buffer_add_codepoints(g->buffer, cps.data(), static_cast<int>(cps.size()), static_cast<unsigned>(begin),
                             static_cast<int>(end - begin));
    hb_buffer_guess_segment_properties(g->buffer);
    hb_buffer_set_direction(g->buffer, HB_DIRECTION_LTR);
    hb_shape(f.hbFont, g->buffer, nullptr, 0);
    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(g->buffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(g->buffer, &count);
    for (unsigned i = 0; i < count; ++i) {
        GlyphRec r;
        r.face = static_cast<uint16_t>(face);
        r.glyph = info[i].codepoint;
        r.advance = static_cast<float>(pos[i].x_advance) / 64.0f + tracking;
        r.xoff = static_cast<float>(pos[i].x_offset) / 64.0f;
        r.yoff = static_cast<float>(pos[i].y_offset) / 64.0f;
        r.cluster = info[i].cluster;
        r.space = r.cluster < cps.size() && isSpace(cps[r.cluster]);
        out.push_back(r);
    }
}

struct Line {
    std::vector<GlyphRec> glyphs;
    std::vector<float> pen;  // x of each glyph relative to line start
    float width = 0;
};

// Builds a Line from glyphs[a, b) trimming trailing whitespace from the width.
Line makeLine(const std::vector<GlyphRec>& glyphs, size_t a, size_t b) {
    Line line;
    float x = 0;
    float lastInk = 0;
    for (size_t i = a; i < b; ++i) {
        line.glyphs.push_back(glyphs[i]);
        line.pen.push_back(x);
        x += glyphs[i].advance;
        if (!glyphs[i].space) lastInk = x;
    }
    line.width = lastInk;
    return line;
}

void appendEllipsis(Line& line, int primary, float pixelSize, float maxWidth) {
    Face& f = *g->faces[static_cast<size_t>(primary)];
    const int scale26 = static_cast<int>(std::lround(pixelSize * 64.0f));
    hb_font_set_scale(f.hbFont, scale26, scale26);
    hb_codepoint_t gid = 0;
    int count = 1;
    if (!hb_font_get_nominal_glyph(f.hbFont, 0x2026, &gid)) {
        hb_font_get_nominal_glyph(f.hbFont, '.', &gid);
        count = 3;
    }
    const float adv = static_cast<float>(hb_font_get_glyph_h_advance(f.hbFont, gid)) / 64.0f;
    const float ellW = adv * static_cast<float>(count);
    // Drop glyphs until the ellipsis fits, then drop trailing spaces.
    auto endX = [&](size_t n) { return n == 0 ? 0.0f : line.pen[n - 1] + line.glyphs[n - 1].advance; };
    size_t n = line.glyphs.size();
    if (maxWidth > 0)
        while (n > 0 && endX(n) + ellW > maxWidth) --n;
    while (n > 0 && line.glyphs[n - 1].space) --n;
    line.glyphs.resize(n);
    line.pen.resize(n);
    float x = endX(n);
    for (int i = 0; i < count; ++i) {
        line.glyphs.push_back({static_cast<uint16_t>(primary), gid, adv, 0, 0, 0, false});
        line.pen.push_back(x);
        x += adv;
    }
    line.width = x;
}

std::shared_ptr<TextLayout> buildLayout(std::string_view str, const TextStyle& style, const TextOptions& opt) {
    auto out = std::make_shared<TextLayout>();
    const float scale = g->scale;
    const float pixelSize = std::max(1.0f, std::round(style.size * scale * 4.0f) / 4.0f);
    out->pixelSize = pixelSize;
    out->scale = scale;
    out->lineHeight = style.size * style.lineHeight;

    int primary = g->primary[static_cast<int>(style.weight)];
    if (primary < 0) primary = g->primary[0];
    if (primary < 0) return out;
    const Face& pf = *g->faces[static_cast<size_t>(primary)];

    const float maxWidthPx = opt.maxWidth > 0 ? opt.maxWidth * scale : 0.0f;
    const bool wrap = maxWidthPx > 0 && opt.maxLines != 1;
    const float trackingPx = style.tracking * scale;

    std::vector<Line> lines;
    bool truncated = false;
    const size_t maxLines = opt.maxLines > 0 ? static_cast<size_t>(opt.maxLines) : SIZE_MAX;

    // Paragraphs separated by '\n'.
    size_t pos = 0;
    while (pos <= str.size() && lines.size() <= maxLines) {
        size_t nl = str.find('\n', pos);
        if (nl == std::string_view::npos) nl = str.size();
        const std::string_view para = str.substr(pos, nl - pos);
        pos = nl + 1;

        std::vector<uint32_t> cps;
        cps.reserve(para.size());
        for (size_t i = 0; i < para.size();) {
            uint32_t cp = decodeUtf8(para, i);
            if (cp == '\r') continue;
            cps.push_back(cp);
        }

        std::vector<GlyphRec> glyphs;
        if (!cps.empty()) {
            size_t runStart = 0;
            int runFace = faceForCodepoint(primary, cps[0]);
            for (size_t i = 1; i <= cps.size(); ++i) {
                int f = runFace;
                if (i < cps.size() && !isJoiner(cps[i])) f = faceForCodepoint(primary, cps[i]);
                if (i == cps.size() || f != runFace) {
                    shapeRun(cps, runStart, i, runFace, pixelSize, trackingPx, glyphs);
                    runStart = i;
                    runFace = f;
                }
            }
        }

        const size_t n = glyphs.size();
        if (!wrap || n == 0) {
            lines.push_back(makeLine(glyphs, 0, n));
        } else {
            std::vector<float> px(n + 1, 0.0f);
            for (size_t i = 0; i < n; ++i) px[i + 1] = px[i] + glyphs[i].advance;
            auto breakAfter = [&](size_t i) {
                const uint32_t cp = cps[glyphs[i].cluster];
                if (isSpace(cp) || cp == '-' || cp == '/' || isCjk(cp)) return true;
                return i + 1 < n && isCjk(cps[glyphs[i + 1].cluster]);
            };
            size_t a = 0;
            while (a < n) {
                size_t b = a;
                size_t lastBreakEnd = SIZE_MAX;
                while (b < n && (b == a || px[b + 1] - px[a] <= maxWidthPx || glyphs[b].space)) {
                    if (breakAfter(b)) lastBreakEnd = b + 1;
                    ++b;
                }
                size_t end;
                if (b >= n) end = n;
                else if (lastBreakEnd != SIZE_MAX && lastBreakEnd > a) end = lastBreakEnd;
                else end = b;
                lines.push_back(makeLine(glyphs, a, end));
                a = end;
                while (a < n && glyphs[a].space) ++a;
                if (lines.size() > maxLines) break;
            }
        }
        if (nl >= str.size()) break;
    }

    if (lines.size() > maxLines) {
        lines.resize(maxLines);
        truncated = true;
    }
    if (!lines.empty() && maxWidthPx > 0) {
        Line& last = lines.back();
        if (truncated || last.width > maxWidthPx + 0.5f) {
            truncated = true;
            if (opt.ellipsis) appendEllipsis(last, primary, pixelSize, maxWidthPx);
        }
    }

    const float lineH = out->lineHeight * scale;
    const float asc = pf.ascender * pixelSize, desc = pf.descender * pixelSize;
    const float baselineInLine = (lineH - (asc + desc)) * 0.5f + asc;
    float widest = 0;
    for (const Line& l : lines) widest = std::max(widest, l.width);
    size_t total = 0;
    for (const Line& l : lines) total += l.glyphs.size();
    out->glyphs.reserve(total);
    for (size_t li = 0; li < lines.size(); ++li) {
        const Line& l = lines[li];
        float offset = 0;
        if (opt.align == TextAlign::Center) offset = std::round((widest - l.width) * 0.5f);
        else if (opt.align == TextAlign::Right) offset = widest - l.width;
        const float baseline = static_cast<float>(li) * lineH + baselineInLine;
        for (size_t i = 0; i < l.glyphs.size(); ++i) {
            const GlyphRec& gr = l.glyphs[i];
            out->glyphs.push_back({gr.face, gr.glyph, offset + l.pen[i] + gr.xoff, baseline - gr.yoff});
        }
    }
    out->lineCount = static_cast<int>(lines.size());
    out->width = widest / scale;
    out->height = static_cast<float>(std::max<size_t>(1, lines.size())) * out->lineHeight;
    out->firstBaseline = baselineInLine / scale;
    out->truncated = truncated;
    return out;
}

}  // namespace

bool init() {
    if (g) return true;
    g = new System();
    if (FT_Init_FreeType(&g->lib) != 0) {
        log::error("text", "FT_Init_FreeType failed");
        delete g;
        g = nullptr;
        return false;
    }
    g->buffer = hb_buffer_create();
    g->plReady = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (!g->plReady) log::warn("text", "pl service unavailable; no system font fallback");

    g->primary[static_cast<int>(FontWeight::Regular)] = addRomfsFace("Inter-Regular.ttf");
    g->primary[static_cast<int>(FontWeight::Medium)] = addRomfsFace("Inter-Medium.ttf");
    g->primary[static_cast<int>(FontWeight::SemiBold)] = addRomfsFace("Inter-SemiBold.ttf");
    g->primary[static_cast<int>(FontWeight::Bold)] = addRomfsFace("Inter-Bold.ttf");
    g->primary[static_cast<int>(FontWeight::Display)] = addRomfsFace("InterDisplay-Bold.ttf");

    const std::pair<PlSharedFontType, const char*> shared[] = {
        {PlSharedFontType_Standard, "system-standard"},
        {PlSharedFontType_ChineseSimplified, "system-zh-hans"},
        {PlSharedFontType_ExtChineseSimplified, "system-zh-hans-ext"},
        {PlSharedFontType_ChineseTraditional, "system-zh-hant"},
        {PlSharedFontType_KO, "system-ko"},
        {PlSharedFontType_NintendoExt, "system-nintendo-ext"},
    };
    for (const auto& [type, name] : shared)
        if (int idx = addSharedFace(type, name); idx >= 0) g->fallbacks.push_back(idx);

    // Fill missing weights: nearest bundled weight, else the system font.
    const int order[5][5] = {{0, 1, 2, 3, 4}, {1, 0, 2, 3, 4}, {2, 3, 1, 0, 4}, {3, 2, 4, 1, 0}, {4, 3, 2, 1, 0}};
    int resolved[5];
    for (int w = 0; w < 5; ++w) {
        resolved[w] = -1;
        for (int k = 0; k < 5 && resolved[w] < 0; ++k) resolved[w] = g->primary[order[w][k]];
        if (resolved[w] < 0 && !g->fallbacks.empty()) resolved[w] = g->fallbacks.front();
    }
    std::copy(std::begin(resolved), std::end(resolved), std::begin(g->primary));
    if (g->primary[0] < 0) {
        log::error("text", "no usable font");
        return false;
    }
    g->ready = true;
    log::info("text", "fonts ready: {} faces ({} fallbacks)", g->faces.size(), g->fallbacks.size());
    return true;
}

void shutdown() {
    if (!g) return;
    g->cache.clear();
    g->glyphs.clear();
    g->pages.clear();
    for (auto& f : g->faces) {
        if (f->hbFont) hb_font_destroy(f->hbFont);
        if (f->hbFace) hb_face_destroy(f->hbFace);
        if (f->blob) hb_blob_destroy(f->blob);
        if (f->ft) FT_Done_Face(f->ft);
    }
    g->faces.clear();
    if (g->buffer) hb_buffer_destroy(g->buffer);
    if (g->lib) FT_Done_FreeType(g->lib);
    if (g->plReady) plExit();
    delete g;
    g = nullptr;
}

void setDisplayScale(float scale) {
    if (!g || scale <= 0 || scale == g->scale) return;
    g->scale = scale;
    g->cache.clear();
    resetAtlas();
}

float displayScale() { return g ? g->scale : 1.0f; }

void endFrame() {
    if (!g) return;
    ++g->frame;
    if (g->frame % 120 == 0) {
        for (auto it = g->cache.begin(); it != g->cache.end();) {
            if (g->frame - it->second.lastFrame > 600) it = g->cache.erase(it);
            else ++it;
        }
    }
}

TextLayoutPtr layout(std::string_view str, const TextStyle& style, const TextOptions& options) {
    static const TextLayoutPtr kEmpty = std::make_shared<TextLayout>();
    if (!g || !g->ready) return kEmpty;
    uint64_t h = fnv1a(str.data(), str.size());
    h = mix(h, style.size);
    h = mix(h, style.weight);
    h = mix(h, style.lineHeight);
    h = mix(h, style.tracking);
    h = mix(h, options.maxWidth);
    h = mix(h, options.maxLines);
    h = mix(h, options.align);
    h = mix(h, options.ellipsis);
    h = mix(h, g->scale);

    auto it = g->cache.find(h);
    if (it != g->cache.end()) {
        const LayoutKey& k = it->second.key;
        if (k.text == str && k.size == style.size && k.weight == style.weight && k.lineHeight == style.lineHeight &&
            k.tracking == style.tracking && k.maxWidth == options.maxWidth && k.maxLines == options.maxLines &&
            k.align == options.align && k.ellipsis == options.ellipsis && k.scale == g->scale) {
            it->second.lastFrame = g->frame;
            return it->second.layout;
        }
    }
    const auto t0 = perf::Clock::now();
    CacheEntry e;
    e.key = {std::string(str), style.size, style.lineHeight, style.tracking, options.maxWidth, g->scale,
             options.maxLines, style.weight, options.align, options.ellipsis};
    e.layout = buildLayout(str, style, options);
    perf::Frame& pf = perf::frame();
    pf.layouts++;
    pf.layoutMs += perf::msSince(t0);
    e.lastFrame = g->frame;
    TextLayoutPtr result = e.layout;
    g->cache[h] = std::move(e);
    return result;
}

Vec2 measure(std::string_view str, const TextStyle& style, const TextOptions& options) {
    auto l = layout(str, style, options);
    return {l->width, l->height};
}

float lineHeight(const TextStyle& style) { return style.size * style.lineHeight; }

float capHeight(const TextStyle& style) {
    if (!g || !g->ready) return style.size * 0.7f;
    int primary = g->primary[static_cast<int>(style.weight)];
    if (primary < 0) primary = g->primary[0];
    return g->faces[static_cast<size_t>(primary)]->capHeight * style.size;
}

const GlyphQuad& glyph(uint16_t face, uint32_t glyphIndex, float pixelSize, int subpixel) {
    if (!g || face >= g->faces.size()) return kEmptyGlyph;
    const auto sizeQ = static_cast<uint64_t>(std::lround(pixelSize * 4.0f)) & 0xFFFF;
    const uint64_t key = (static_cast<uint64_t>(face) << 56) | (static_cast<uint64_t>(subpixel & 3) << 50) |
                         (sizeQ << 32) | glyphIndex;
    auto it = g->glyphs.find(key);
    if (it != g->glyphs.end()) return it->second;

    Face& f = *g->faces[face];
    setFtSize(f, pixelSize);
    GlyphQuad q;
    if (FT_Load_Glyph(f.ft, glyphIndex, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT) == 0) {
        FT_GlyphSlot slot = f.ft->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE && subpixel > 0)
            FT_Outline_Translate(&slot->outline, subpixel * 64 / kSubpixelSteps, 0);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0) {
            const FT_Bitmap& bm = slot->bitmap;
            const int w = static_cast<int>(bm.width), h = static_cast<int>(bm.rows);
            q.left = slot->bitmap_left;
            q.top = slot->bitmap_top;
            if (w > 0 && h > 0) {
                Page* page = nullptr;
                int x = 0, y = 0;
                if (allocate(w, h, page, x, y)) {
                    for (int row = 0; row < h; ++row)
                        std::memcpy(&page->pixels[static_cast<size_t>(y + row) * kAtlasSize + static_cast<size_t>(x)],
                                    bm.buffer + row * bm.pitch, static_cast<size_t>(w));
                    page->markDirty(x, y, x + w, y + h);
                    perf::frame().glyphs++;
                    q.texture = page->texture.get();
                    q.width = w;
                    q.height = h;
                    const float inv = 1.0f / kAtlasSize;
                    q.u0 = x * inv;
                    q.v0 = y * inv;
                    q.u1 = (x + w) * inv;
                    q.v1 = (y + h) * inv;
                }
            }
        }
    }
    return g->glyphs.emplace(key, q).first->second;
}

void flushAtlas() {
    if (!g) return;
    for (Page& p : g->pages) {
        if (!p.dirty()) continue;
        const int w = p.dirtyX1 - p.dirtyX0, h = p.dirtyY1 - p.dirtyY0;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, kAtlasSize);
        p.texture->update(p.dirtyX0, p.dirtyY0, w, h,
                          &p.pixels[static_cast<size_t>(p.dirtyY0) * kAtlasSize + static_cast<size_t>(p.dirtyX0)]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        perf::Frame& pf = perf::frame();
        pf.atlasUploads++;
        pf.atlasBytes += static_cast<size_t>(w) * static_cast<size_t>(h);
        p.dirtyX0 = p.dirtyY0 = kAtlasSize;
        p.dirtyX1 = p.dirtyY1 = 0;
    }
}

}  // namespace karafin::gfx::text
