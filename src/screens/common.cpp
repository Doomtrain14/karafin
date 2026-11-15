#include "screens/common.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <memory>

#include "player/player.hpp"
#include "screens/detail_screen.hpp"
#include "screens/library_lock.hpp"
#include "screens/library_screen.hpp"
#include "screens/series_screen.hpp"
#include "ui/root.hpp"
#include "ui/widgets/dialog.hpp"

namespace karafin::screens {

// ---- Formatting ---------------------------------------------------------------------------------

std::string formatRuntime(int64_t ticks) {
    if (ticks <= 0) return {};
    const int64_t minutes = std::max<int64_t>(1, (ticks / jf::kTicksPerSecond + 30) / 60);
    if (minutes < 60) return std::format("{}m", minutes);
    if (minutes % 60 == 0) return std::format("{}h", minutes / 60);
    return std::format("{}h {}m", minutes / 60, minutes % 60);
}

std::string formatClock(int64_t ticks) {
    const int64_t total = std::max<int64_t>(0, ticks / jf::kTicksPerSecond);
    const int64_t h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) return std::format("{}:{:02}:{:02}", h, m, s);
    return std::format("{}:{:02}", m, s);
}

std::string formatBytes(uint64_t bytes) {
    constexpr double kKB = 1024.0, kMB = kKB * 1024.0, kGB = kMB * 1024.0, kTB = kGB * 1024.0;
    const auto b = static_cast<double>(bytes);
    if (b >= kTB) return std::format("{:.2f} TB", b / kTB);
    if (b >= kGB) return b >= 100 * kGB ? std::format("{:.0f} GB", b / kGB) : std::format("{:.1f} GB", b / kGB);
    if (b >= kMB) return b >= 100 * kMB ? std::format("{:.0f} MB", b / kMB) : std::format("{:.1f} MB", b / kMB);
    if (b >= kKB) return std::format("{:.0f} KB", b / kKB);
    return std::format("{} B", bytes);
}

std::string formatSpeed(double bytesPerSecond) {
    if (bytesPerSecond <= 0) return {};
    return formatBytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
}

std::string formatEta(double seconds) {
    if (seconds < 0) return {};
    if (seconds < 60) return "Less than a minute left";
    const auto minutes = static_cast<int64_t>(std::ceil(seconds / 60.0));
    if (minutes < 60) return std::format("{} min left", minutes);
    return std::format("{} h {} min left", minutes / 60, minutes % 60);
}

std::string formatRating(double rating) { return rating > 0 ? std::format("{:.1f}", rating) : std::string(); }

std::string formatCount(int64_t n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return n < 0 ? "-" + out : out;
}

std::string remainingText(const jf::BaseItem& item) {
    if (item.runTimeTicks <= 0 || item.userData.playbackPositionTicks <= 0) return {};
    const int64_t left = item.runTimeTicks - item.userData.playbackPositionTicks;
    if (left <= 0) return {};
    return formatRuntime(left) + " left";
}

std::string yearText(const jf::BaseItem& item) {
    if (item.productionYear <= 0) return {};
    if (!item.isSeries()) return std::to_string(item.productionYear);
    if (item.status == "Continuing") return std::format("{} - Present", item.productionYear);
    if (item.endDate.size() >= 4) {
        const std::string end = item.endDate.substr(0, 4);
        if (end != std::to_string(item.productionYear)) return std::format("{} - {}", item.productionYear, end);
    }
    return std::to_string(item.productionYear);
}

std::string joinDots(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += "  \xC2\xB7  ";
        out += p;
    }
    return out;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

std::string upperAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return out;
}

std::string languageName(std::string_view code) {
    struct Lang {
        const char* code;
        const char* name;
    };
    static constexpr std::array kLangs = {
        Lang{"eng", "English"},    Lang{"jpn", "Japanese"},   Lang{"fre", "French"},     Lang{"fra", "French"},
        Lang{"ger", "German"},     Lang{"deu", "German"},     Lang{"spa", "Spanish"},    Lang{"ita", "Italian"},
        Lang{"por", "Portuguese"}, Lang{"rus", "Russian"},    Lang{"chi", "Chinese"},    Lang{"zho", "Chinese"},
        Lang{"kor", "Korean"},     Lang{"dut", "Dutch"},      Lang{"nld", "Dutch"},      Lang{"swe", "Swedish"},
        Lang{"nor", "Norwegian"},  Lang{"dan", "Danish"},     Lang{"fin", "Finnish"},    Lang{"pol", "Polish"},
        Lang{"cze", "Czech"},      Lang{"ces", "Czech"},      Lang{"hun", "Hungarian"},  Lang{"tur", "Turkish"},
        Lang{"gre", "Greek"},      Lang{"ell", "Greek"},      Lang{"heb", "Hebrew"},     Lang{"ara", "Arabic"},
        Lang{"hin", "Hindi"},      Lang{"tha", "Thai"},       Lang{"vie", "Vietnamese"}, Lang{"ind", "Indonesian"},
        Lang{"ukr", "Ukrainian"},  Lang{"rum", "Romanian"},   Lang{"ron", "Romanian"},   Lang{"bul", "Bulgarian"},
        Lang{"hrv", "Croatian"},   Lang{"srp", "Serbian"},    Lang{"slv", "Slovenian"},  Lang{"slo", "Slovak"},
        Lang{"cat", "Catalan"},    Lang{"may", "Malay"},      Lang{"msa", "Malay"},      Lang{"fil", "Filipino"},
        Lang{"und", "Unknown"},
    };
    const std::string lower = lowerAscii(code);
    for (const auto& l : kLangs)
        if (lower == l.code) return l.name;
    return upperAscii(code);
}

// ---- Images -------------------------------------------------------------------------------------

gfx::ImageRequest imageRequest(std::string url, float logicalW, float logicalH, float blur) {
    gfx::ImageRequest req;
    req.url = std::move(url);
    const float s = ui::displayScale();
    req.width = static_cast<int>(std::lround(logicalW * s));
    req.height = static_cast<int>(std::lround(logicalH * s));
    req.fit = gfx::ImageFit::Cover;
    req.blur = blur;
    return req;
}

gfx::ImageRequest backdropRequest(const jf::BaseItem& item, float blur) {
    auto client = jf::currentClient();
    if (!client) return {};
    const int maxW = ui::displayScale() > 1.01f ? 1920 : 1280;
    std::string url = client->imageUrl(item, jf::ImageType::Backdrop, {.maxWidth = maxW, .quality = 85});
    if (url.empty()) url = client->imageUrl(item, jf::ImageType::Thumb, {.maxWidth = maxW, .quality = 85});
    if (url.empty()) return {};
    return imageRequest(std::move(url), 1280, 720, blur);
}

std::string posterUrl(const jf::BaseItem& item) {
    auto client = jf::currentClient();
    if (!client) return {};
    return client->imageUrl(item, jf::ImageType::Primary, {.maxHeight = 360});
}

std::string wideUrl(const jf::BaseItem& item) {
    auto client = jf::currentClient();
    if (!client) return {};
    const jf::ImageOptions opts{.maxWidth = 440};
    if (item.isEpisode()) {
        if (auto u = client->imageUrl(item, jf::ImageType::Primary, {.maxWidth = 440, .fallback = false}); !u.empty())
            return u;
    }
    if (item.type == "CollectionFolder" || item.type == "UserView" || item.type == "Folder") {
        if (auto u = client->imageUrl(item, jf::ImageType::Primary, opts); !u.empty()) return u;
    }
    if (auto u = client->imageUrl(item, jf::ImageType::Thumb, opts); !u.empty()) return u;
    return client->imageUrl(item, jf::ImageType::Primary, opts);
}

std::string logoUrl(const jf::BaseItem& item) {
    auto client = jf::currentClient();
    if (!client) return {};
    return client->imageUrl(item, jf::ImageType::Logo, {.maxWidth = 640});
}

gfx::Color placeholderColor(std::string_view seed) {
    static constexpr std::array kPalette = {
        gfx::Color::hex(0x2E2563), gfx::Color::hex(0x173F5F), gfx::Color::hex(0x1F4F44), gfx::Color::hex(0x4F2445),
        gfx::Color::hex(0x5A2E2E), gfx::Color::hex(0x2F4B6B), gfx::Color::hex(0x3E3A1F), gfx::Color::hex(0x2A2F4F),
    };
    uint32_t h = 2166136261u;
    for (unsigned char c : seed) h = (h ^ c) * 16777619u;
    return kPalette[h % kPalette.size()];
}

gfx::Icon typeIcon(const jf::BaseItem& item) {
    if (item.isSeries() || item.isSeason() || item.isEpisode() || item.collectionType == "tvshows") return gfx::Icon::Tv;
    if (item.type == "CollectionFolder" || item.type == "UserView" || item.type == "Folder" || item.type == "BoxSet")
        return gfx::Icon::Library;
    if (item.type == "Person") return gfx::Icon::User;
    return gfx::Icon::Movie;
}

// ---- Cards --------------------------------------------------------------------------------------

std::string cardTitle(const jf::BaseItem& item) {
    if (item.isEpisode() && !item.seriesName.empty()) return item.seriesName;
    return item.name;
}

std::string cardSubtitle(const jf::BaseItem& item, bool wide) {
    if (item.isEpisode()) {
        const std::string tag = item.episodeTag();
        if (wide) {
            if (auto left = remainingText(item); !left.empty() && !item.userData.played) return joinDots({tag, left});
            return joinDots({tag, item.name});
        }
        return tag.empty() ? item.name : tag;
    }
    if (wide)
        if (auto left = remainingText(item); !left.empty() && !item.userData.played) return left;
    if (item.isSeason()) return item.childCount > 0 ? std::format("{} episodes", item.childCount) : std::string();
    if (item.type == "BoxSet" && item.childCount > 0) return std::format("{} items", item.childCount);
    return yearText(item);
}

void bindCard(ui::PosterCard& card, const jf::BaseItem& item, std::string* boundUrl) {
    const bool wide = card.shape() == ui::CardShape::Wide;
    card.setTitle(cardTitle(item));
    card.setSubtitle(cardSubtitle(item, wide));
    card.setPlaceholder(placeholderColor(item.id), typeIcon(item));
    const std::string url = wide ? wideUrl(item) : posterUrl(item);
    if (!boundUrl || *boundUrl != url || url.empty()) {
        if (url.empty()) card.imageView()->clear();
        else card.setImageUrl(url);
        if (boundUrl) *boundUrl = url;
    }
    const double p = item.progress();
    card.setProgress(!item.userData.played && item.userData.playbackPositionTicks > 0 && p > 0
                         ? static_cast<float>(p)
                         : -1.0f);
    card.setWatched(item.userData.played && !item.isFolder);
    if (item.isSeries() || item.isSeason()) {
        const int unplayed = item.userData.unplayedItemCount;
        card.setWatched(item.userData.played);
        card.setBadge(!item.userData.played && unplayed > 0 ? std::to_string(unplayed) : std::string());
    } else {
        card.setBadge({});
    }
}

// ---- Navigation ---------------------------------------------------------------------------------

void openItem(const jf::BaseItem& item) {
    if (item.id.empty()) return;
    if (item.isSeries()) {
        ui::nav().push(std::make_unique<SeriesScreen>(item.id), ui::Transition::Zoom);
        return;
    }
    if (item.isSeason()) {
        if (!item.seriesId.empty())
            ui::nav().push(std::make_unique<SeriesScreen>(item.seriesId, item.id), ui::Transition::Zoom);
        return;
    }
    if (item.type == "Person") {
        LibrarySpec spec;
        spec.title = item.name;
        spec.personIds = item.id;
        spec.includeTypes = "Movie,Series";
        ui::nav().push(std::make_unique<LibraryScreen>(std::move(spec)));
        return;
    }
    if (item.isPlayable()) {
        ui::nav().push(std::make_unique<DetailScreen>(item.id), ui::Transition::Zoom);
        return;
    }
    if (item.isFolder) {
        if (LibraryLock::instance().blocks(item.id)) {
            LibraryLock::instance().requestUnlock([item] { openItem(item); });
            return;
        }
        LibrarySpec spec;
        spec.title = item.name;
        spec.parentId = item.id;
        spec.collectionType = item.collectionType;
        if (item.type == "BoxSet" || item.type == "Folder" || item.type == "Playlist") spec.recursive = false;
        ui::nav().push(std::make_unique<LibraryScreen>(std::move(spec)));
        return;
    }
    ui::toast("This item can't be opened here", ui::ToastKind::Info, gfx::Icon::Info);
}

void scanLibrary(const std::string& libraryId, const std::string& name) {
    auto client = jf::currentClient();
    if (!client) return;
    auto call = libraryId.empty() ? client->refreshLibrary() : client->refreshItem(libraryId);
    const std::string what = libraryId.empty() ? std::string("all libraries") : name;
    call.then([what](jf::Result<jf::Unit> r) {
        if (!r) {
            ui::toast("Couldn't start the scan. " + errorMessage(r.error), ui::ToastKind::Error, gfx::Icon::Warning);
            return;
        }
        ui::toast("Scanning " + what + " on the server", ui::ToastKind::Success, gfx::Icon::Refresh);
    });
}

bool isAdmin() {
    auto client = jf::currentClient();
    return client && client->user().policy.isAdministrator;
}

// ---- Playback -----------------------------------------------------------------------------------

void playAt(const jf::BaseItem& item, int64_t startTicks, const PlayOptions& options) {
    player::PlayRequest req;
    req.itemId = item.id;
    req.startPositionTicks = std::max<int64_t>(0, startTicks);
    req.audioStreamIndex = options.audioStreamIndex;
    req.subtitleStreamIndex = options.subtitleStreamIndex;
    req.mediaSourceId = options.mediaSourceId;
    player::play(req);
}

namespace {

void askResume(int64_t position, int64_t runtime, std::function<void(int64_t)> start) {
    ui::DialogSpec spec;
    spec.title = "Resume playback?";
    spec.message = runtime > 0 ? std::format("You stopped at {} of {}.", formatClock(position), formatClock(runtime))
                               : std::format("You stopped at {}.", formatClock(position));
    spec.icon = gfx::Icon::Play;
    spec.buttons = {{"Start over", ui::ButtonStyle::Secondary, [start] { start(0); }},
                    {std::format("Resume from {}", formatClock(position)), ui::ButtonStyle::Primary,
                     [start, position] { start(position); }}};
    spec.cancelIndex = -1;
    spec.defaultIndex = 1;
    ui::present(std::make_unique<ui::Dialog>(std::move(spec)));
}

// Asks resume / start over based on `item.userData`, which must be current.
void promptAndPlay(const jf::BaseItem& item, const PlayOptions& options) {
    const int64_t pos = item.userData.playbackPositionTicks;
    if (pos > 0 && !item.userData.played) {
        askResume(pos, item.runTimeTicks, [item, options](int64_t ticks) { playAt(item, ticks, options); });
        return;
    }
    playAt(item, 0, options);
}

uint64_t g_playGeneration = 0;

}  // namespace

void playItem(const jf::BaseItem& item, const PlayOptions& options) {
    // Cards, hero entries and search results keep the UserData they were loaded with; after
    // watching elsewhere (or in the player) it is stale, so re-read it before offering "Resume".
    auto client = jf::currentClient();
    if (!client || item.id.empty()) {
        promptAndPlay(item, options);
        return;
    }
    const uint64_t generation = ++g_playGeneration;  // a second press supersedes the first
    jf::ItemsQuery q;
    q.ids(item.id).limit(1).enableUserData(true).enableTotalRecordCount(false);
    client->items(q).then([item, options, generation](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
        if (generation != g_playGeneration) return;
        jf::BaseItem fresh = item;
        if (r && !r->items.empty() && r->items.front().id == item.id) fresh.userData = r->items.front().userData;
        promptAndPlay(fresh, options);
    });
}

void playSeries(const std::string& seriesId, CancelToken token) {
    auto client = jf::currentClient();
    if (!client || seriesId.empty()) return;
    client->nextUp(1, seriesId).then(
        [client, seriesId, token](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            if (r && !r->items.empty()) {
                promptAndPlay(r->items.front(), {});
                return;
            }
            client->episodes(seriesId).then(
                [](jf::Result<jf::QueryResult<jf::BaseItem>> eps) {
                    if (!eps) {
                        ui::toast(errorMessage(eps.error), ui::ToastKind::Error, gfx::Icon::Warning);
                        return;
                    }
                    const jf::BaseItem* pick = nullptr;
                    for (const auto& e : eps->items) {
                        if (e.isVirtual()) continue;
                        if (!pick) pick = &e;
                        if (!e.userData.played) {
                            pick = &e;
                            break;
                        }
                    }
                    if (pick) promptAndPlay(*pick, {});
                    else ui::toast("No playable episodes", ui::ToastKind::Info, gfx::Icon::Info);
                },
                token);
        },
        token);
}

void playOffline(const std::string& itemId, const std::string& title, int64_t positionTicks, int64_t runTimeTicks) {
    (void)title;
    auto start = [itemId](int64_t ticks) {
        player::PlayRequest req;
        req.itemId = itemId;
        req.startPositionTicks = std::max<int64_t>(0, ticks);
        req.offline = true;
        player::play(req);
    };
    if (positionTicks > 0) askResume(positionTicks, runTimeTicks, start);
    else start(0);
}

// ---- Errors -------------------------------------------------------------------------------------

std::string errorMessage(const jf::ApiError& error) {
    if (!error.message.empty()) return error.message;
    if (error.isConnectivity()) return "The server can't be reached. Check your connection and try again.";
    return "Something went wrong. Please try again.";
}

}  // namespace karafin::screens
