#include "player/tracks.hpp"

#include <algorithm>
#include <cctype>
#include <format>

#include "core/settings.hpp"
#include "player/languages.hpp"
#include "player/mpv.hpp"

namespace karafin::player {

namespace {

struct MpvTrack {
    int64_t id = -1;
    std::string type;  // "audio" / "sub" / "video"
    int64_t ffIndex = -1;
    bool external = false;
    std::string externalFilename;
    std::string title;
    std::string lang;
    std::string codec;
    bool selected = false;
    bool isDefault = false;
    bool forced = false;
    int64_t channels = 0;
    bool used = false;
};

std::vector<MpvTrack> parseTrackList(const mpv_node& list) {
    std::vector<MpvTrack> out;
    if (list.format != MPV_FORMAT_NODE_ARRAY || !list.u.list) return out;
    for (int i = 0; i < list.u.list->num; ++i) {
        const mpv_node& e = list.u.list->values[i];
        MpvTrack t;
        t.id = nodeInt(nodeMapGet(e, "id"), -1);
        t.type = nodeString(nodeMapGet(e, "type"));
        t.ffIndex = nodeInt(nodeMapGet(e, "ff-index"), -1);
        t.external = nodeFlag(nodeMapGet(e, "external"));
        t.externalFilename = nodeString(nodeMapGet(e, "external-filename"));
        t.title = nodeString(nodeMapGet(e, "title"));
        t.lang = nodeString(nodeMapGet(e, "lang"));
        t.codec = nodeString(nodeMapGet(e, "codec"));
        t.selected = nodeFlag(nodeMapGet(e, "selected"));
        t.isDefault = nodeFlag(nodeMapGet(e, "default"));
        t.forced = nodeFlag(nodeMapGet(e, "forced"));
        t.channels = nodeInt(nodeMapGet(e, "demux-channel-count"), 0);
        if (t.id >= 0) out.push_back(std::move(t));
    }
    return out;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string codecLabel(const std::string& codec) {
    if (codec == "subrip" || codec == "srt") return "SRT";
    if (codec == "ass" || codec == "ssa") return "ASS";
    if (codec == "hdmv_pgs_subtitle" || codec == "pgssub" || codec == "pgs") return "PGS";
    if (codec == "dvd_subtitle" || codec == "dvdsub") return "VOBSUB";
    if (codec == "webvtt" || codec == "vtt") return "VTT";
    if (codec == "mov_text") return "TX3G";
    if (codec == "eac3") return "E-AC3";
    if (codec == "truehd") return "TrueHD";
    return upper(codec);
}

std::string channelLabel(int ch) {
    switch (ch) {
        case 0: return {};
        case 1: return "Mono";
        case 2: return "Stereo";
        case 6: return "5.1";
        case 8: return "7.1";
        default: return std::format("{} ch", ch);
    }
}

Track fromStream(const jellyfin::MediaStream& s, TrackKind kind) {
    Track t;
    t.kind = kind;
    t.jellyfinIndex = s.index;
    t.title = !s.displayTitle.empty() ? s.displayTitle : s.title;
    t.language = lang::normalize(s.language);
    t.codec = s.codec;
    t.channels = s.channels;
    t.external = s.isExternal;
    t.forced = s.isForced;
    t.isDefault = s.isDefault;
    t.hearingImpaired = s.isHearingImpaired;
    return t;
}

// Maps embedded Jellyfin tracks to mpv tracks of one type by container order (the same rule as
// PlaybackPlan::mpvTrackId, which picks the initial aid/sid). Jellyfin's MediaStream.Index is
// only equal to ff-index when no external streams are numbered before the embedded ones, so
// ff-index is used only when it matches every track.
void mapEmbedded(std::vector<Track>& tracks, std::vector<MpvTrack>& mpv, const std::string& type) {
    std::vector<MpvTrack*> embedded;
    for (auto& m : mpv)
        if (m.type == type && !m.external) embedded.push_back(&m);
    std::sort(embedded.begin(), embedded.end(), [](MpvTrack* a, MpvTrack* b) { return a->id < b->id; });
    std::vector<Track*> ours;
    for (auto& t : tracks)
        if (t.jellyfinIndex >= 0 && t.url.empty() && !t.needsRestart) ours.push_back(&t);
    std::sort(ours.begin(), ours.end(), [](Track* a, Track* b) { return a->jellyfinIndex < b->jellyfinIndex; });
    const bool byFfIndex = !ours.empty() && std::all_of(ours.begin(), ours.end(), [&](const Track* t) {
        return std::any_of(embedded.begin(), embedded.end(),
                           [&](const MpvTrack* m) { return m->ffIndex == t->jellyfinIndex; });
    });
    for (size_t i = 0; i < ours.size(); ++i) {
        Track* t = ours[i];
        MpvTrack* match = nullptr;
        if (byFfIndex) {
            for (MpvTrack* m : embedded)
                if (!m->used && m->ffIndex == t->jellyfinIndex) match = m;
        } else if (i < embedded.size() && !embedded[i]->used) {
            match = embedded[i];
        }
        if (match) {
            match->used = true;
            t->mpvId = static_cast<int>(match->id);
            t->selected = match->selected;
        }
    }
}

}  // namespace

std::string subtitleKey(const jellyfin::MediaStream& s) {
    if (!s.path.empty()) return s.path;
    return std::format("{}|{}|{}|{}", s.index, s.language, s.codec, s.displayTitle);
}

TrackPrefs TrackPrefs::fromSettings() {
    auto& s = Settings::instance();
    TrackPrefs p;
    p.audioLang = s.get<std::string>("player.audio_lang", "");
    p.subLang = s.get<std::string>("player.sub_lang", "eng");
    p.subMode = s.get<std::string>("player.sub_mode", "default");
    if (p.subLang.empty()) p.subLang = "eng";
    return p;
}

void TrackManager::clear() {
    audio_.clear();
    subs_.clear();
    transcoding_ = false;
    transcodeAudioIndex_ = -1;
}

void TrackManager::setFromPlan(const jellyfin::PlaybackPlan& plan) {
    clear();
    transcoding_ = plan.isTranscoding();
    transcodeAudioIndex_ = plan.audioStreamIndex;
    for (const auto& s : plan.source.mediaStreams) {
        if (s.type == jellyfin::StreamType::Audio) {
            Track t = fromStream(s, TrackKind::Audio);
            if (transcoding_) t.needsRestart = s.index != plan.audioStreamIndex;
            audio_.push_back(std::move(t));
        } else if (s.isSubtitle()) {
            Track t = fromStream(s, TrackKind::Subtitle);
            const jellyfin::ExternalSubtitle* ext = plan.externalSubtitle(s.index);
            if (ext && (s.isExternal || transcoding_)) {
                t.url = ext->url;
                t.external = true;
                if (t.codec.empty()) t.codec = ext->format;
            } else if (transcoding_) {
                t.needsRestart = true;  // burned in by the server
            } else if (s.isExternal) {
                continue;  // no way to fetch it
            }
            subs_.push_back(std::move(t));
        }
    }
}

void TrackManager::setFromOffline(const offline::Media& media) {
    clear();
    const jellyfin::MediaSource* src = media.item.mediaSource("");
    const std::vector<jellyfin::MediaStream>& streams = src ? src->mediaStreams : media.item.mediaStreams;
    for (const auto& s : streams) {
        if (s.isExternal) continue;
        if (s.type == jellyfin::StreamType::Audio) audio_.push_back(fromStream(s, TrackKind::Audio));
        else if (s.isSubtitle()) subs_.push_back(fromStream(s, TrackKind::Subtitle));
    }
    for (const auto& l : media.subtitles) {
        Track t;
        t.kind = TrackKind::Subtitle;
        t.jellyfinIndex = l.index;
        t.title = !l.title.empty() ? l.title : lang::displayName(l.language);
        t.language = lang::normalize(l.language);
        t.codec = l.format;
        t.url = l.path;
        t.external = true;
        t.forced = l.isForced;
        t.isDefault = l.isDefault;
        t.hearingImpaired = l.isHearingImpaired;
        subs_.push_back(std::move(t));
    }
}

Track& TrackManager::addExternalSubtitle(const jellyfin::MediaStream& stream, const std::string& url) {
    if (Track* existing = findSubtitleByJellyfin(stream.index)) {
        existing->url = url;
        existing->external = true;
        existing->needsRestart = false;
        return *existing;
    }
    Track t = fromStream(stream, TrackKind::Subtitle);
    t.url = url;
    t.external = true;
    subs_.push_back(std::move(t));
    return subs_.back();
}

void TrackManager::mergeMpv(const mpv_node& trackList) {
    std::vector<MpvTrack> mpv = parseTrackList(trackList);

    auto prepare = [](std::vector<Track>& tracks) {
        std::erase_if(tracks, [](const Track& t) { return t.jellyfinIndex < 0 && t.url.empty(); });
        for (auto& t : tracks) {
            t.selected = false;
            if (t.url.empty()) t.mpvId = -1;
        }
    };
    prepare(audio_);
    prepare(subs_);

    // External subtitles: matched by the file/URL they were added from.
    for (auto& t : subs_) {
        if (t.url.empty()) continue;
        t.mpvId = -1;
        for (auto& m : mpv) {
            if (m.type == "sub" && m.external && !m.used && m.externalFilename == t.url) {
                m.used = true;
                t.mpvId = static_cast<int>(m.id);
                t.selected = m.selected;
                break;
            }
        }
    }

    if (transcoding_) {
        // The HLS output carries one audio track: the one the transcode was started with.
        for (auto& m : mpv) {
            if (m.type != "audio" || m.external || m.used) continue;
            if (Track* t = findAudioByJellyfin(transcodeAudioIndex_)) {
                m.used = true;
                t->mpvId = static_cast<int>(m.id);
                t->selected = m.selected;
            }
            break;
        }
    } else {
        mapEmbedded(audio_, mpv, "audio");
        mapEmbedded(subs_, mpv, "sub");
    }

    // Whatever is left exists only in mpv (e.g. no metadata offline): still selectable.
    for (const auto& m : mpv) {
        if (m.used || (m.type != "audio" && m.type != "sub")) continue;
        Track t;
        t.kind = m.type == "audio" ? TrackKind::Audio : TrackKind::Subtitle;
        t.mpvId = static_cast<int>(m.id);
        t.language = lang::normalize(m.lang);
        t.codec = m.codec;
        t.channels = static_cast<int>(m.channels);
        t.external = m.external;
        t.forced = m.forced;
        t.isDefault = m.isDefault;
        t.selected = m.selected;
        std::string name = !m.title.empty() ? m.title : lang::displayName(m.lang);
        if (name == "Unknown") name = std::format("Track {}", m.id);
        t.title = name;
        (t.kind == TrackKind::Audio ? audio_ : subs_).push_back(std::move(t));
    }
}

const Track* TrackManager::selectedAudio() const {
    for (const auto& t : audio_)
        if (t.selected) return &t;
    return nullptr;
}

const Track* TrackManager::selectedSubtitle() const {
    for (const auto& t : subs_)
        if (t.selected) return &t;
    return nullptr;
}

Track* TrackManager::findSubtitleByJellyfin(int index) {
    if (index < 0) return nullptr;
    for (auto& t : subs_)
        if (t.jellyfinIndex == index) return &t;
    return nullptr;
}

Track* TrackManager::findAudioByJellyfin(int index) {
    if (index < 0) return nullptr;
    for (auto& t : audio_)
        if (t.jellyfinIndex == index) return &t;
    return nullptr;
}

Track* TrackManager::findSubtitleByUrl(const std::string& url) {
    for (auto& t : subs_)
        if (!url.empty() && t.url == url) return &t;
    return nullptr;
}

std::string TrackManager::audioLanguage(int jellyfinAudioIndex) const {
    for (const auto& t : audio_)
        if (t.jellyfinIndex == jellyfinAudioIndex) return t.language;
    return {};
}

TrackManager::Choice TrackManager::autoSelect(const TrackPrefs& prefs, std::optional<int> requestedAudio,
                                              std::optional<int> requestedSubtitle, int serverDefaultAudio) const {
    Choice c;
    auto audioExists = [&](int idx) {
        return std::any_of(audio_.begin(), audio_.end(), [&](const Track& t) { return t.jellyfinIndex == idx; });
    };
    if (requestedAudio && audioExists(*requestedAudio)) {
        c.audio = *requestedAudio;
    } else if (!prefs.audioLang.empty()) {
        int best = -1, bestScore = -1;
        for (const auto& t : audio_) {
            if (t.jellyfinIndex < 0 || !lang::same(t.language, prefs.audioLang)) continue;
            const int score = (t.isDefault ? 2 : 0) + (t.jellyfinIndex == serverDefaultAudio ? 1 : 0);
            if (score > bestScore) {
                best = t.jellyfinIndex;
                bestScore = score;
            }
        }
        c.audio = best;
    }
    if (c.audio < 0) c.audio = audioExists(serverDefaultAudio) ? serverDefaultAudio : -1;
    if (c.audio < 0)
        for (const auto& t : audio_)
            if (t.jellyfinIndex >= 0) {
                c.audio = t.jellyfinIndex;
                break;
            }

    if (requestedSubtitle) {
        c.subtitle = *requestedSubtitle;
        return c;
    }

    const std::string audioLang = audioLanguage(c.audio);
    // Best subtitle in `language`; forced: true = only forced, false = prefer full tracks.
    auto pick = [&](const std::string& language, bool forcedOnly) {
        int best = -1, bestScore = -1;
        for (const auto& t : subs_) {
            if (t.jellyfinIndex < 0 || t.needsRestart) continue;
            if (!language.empty() && !lang::same(t.language, language)) continue;
            if (forcedOnly && !t.forced) continue;
            int score = 0;
            if (!forcedOnly && !t.forced) score += 4;
            if (t.isDefault) score += 2;
            if (!t.hearingImpaired) score += 1;
            if (score > bestScore) {
                best = t.jellyfinIndex;
                bestScore = score;
            }
        }
        return best;
    };

    const std::string& mode = prefs.subMode;
    if (mode == "off") {
        c.subtitle = -1;
    } else if (mode == "forced") {
        c.subtitle = pick(prefs.subLang, true);
        if (c.subtitle < 0 && !audioLang.empty()) c.subtitle = pick(audioLang, true);
    } else if (mode == "always") {
        c.subtitle = pick(prefs.subLang, false);
    } else {
        // "default": subtitles in the preferred language when the audio is foreign; otherwise
        // forced tracks, then tracks the file flags as default (in the preferred language).
        if (!audioLang.empty() && !lang::same(audioLang, prefs.subLang)) c.subtitle = pick(prefs.subLang, false);
        if (c.subtitle < 0) c.subtitle = pick(prefs.subLang, true);
        if (c.subtitle < 0 && !audioLang.empty()) c.subtitle = pick(audioLang, true);
        if (c.subtitle < 0) {
            for (const auto& t : subs_) {
                if (t.jellyfinIndex < 0 || t.needsRestart || !t.isDefault) continue;
                if (t.language.empty() || lang::same(t.language, prefs.subLang)) {
                    c.subtitle = t.jellyfinIndex;
                    break;
                }
            }
        }
    }
    return c;
}

std::string TrackManager::describe(const Track& t) {
    if (!t.title.empty()) return t.title;
    std::string s = lang::displayName(t.language);
    if (!t.codec.empty()) s += " - " + codecLabel(t.codec);
    if (const std::string ch = channelLabel(t.channels); !ch.empty()) s += " - " + ch;
    return s;
}

std::string TrackManager::detail(const Track& t) {
    std::vector<std::string> parts;
    if (t.kind == TrackKind::Subtitle) {
        if (t.forced) parts.push_back("Forced");
        if (t.hearingImpaired) parts.push_back("SDH");
        if (t.external) parts.push_back("External");
        if (!t.codec.empty() && t.title.find(codecLabel(t.codec)) == std::string::npos)
            parts.push_back(codecLabel(t.codec));
        if (t.needsRestart) parts.push_back("Burned in by server");
    } else {
        if (t.needsRestart) parts.push_back("Restarts the stream");
    }
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " · ") + p;
    return out;
}

}  // namespace karafin::player
