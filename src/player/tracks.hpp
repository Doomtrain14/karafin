#pragma once

// Audio / subtitle track model (the "subtitle manager"): Jellyfin MediaStreams merged with
// mpv's track-list, preferred-language auto-selection, and the mapping between Jellyfin stream
// indices (for reporting / PlaybackInfo) and mpv aid/sid.

#include <optional>
#include <string>
#include <vector>

#include <mpv/client.h>

#include "jellyfin/playback.hpp"
#include "player/offline.hpp"

namespace karafin::player {

enum class TrackKind { Audio, Subtitle };

struct Track {
    TrackKind kind = TrackKind::Subtitle;
    int jellyfinIndex = -1;  // MediaStream index; -1 for tracks only mpv knows about
    int mpvId = -1;          // aid / sid; -1 until known (external subtitle not added yet)
    std::string title;       // display title
    std::string language;    // normalized ISO 639-2/B ("" unknown)
    std::string codec;
    std::string url;         // external subtitle (Jellyfin URL with ApiKey, or local path)
    int channels = 0;
    bool external = false;
    bool forced = false;
    bool isDefault = false;
    bool hearingImpaired = false;
    bool selected = false;
    // Transcoding only: not part of the HLS stream, selecting it needs a new transcode
    // (audio track switch, or an image subtitle the server burns in).
    bool needsRestart = false;
};

// Identity of a subtitle stream that survives index renumbering (server path when known).
std::string subtitleKey(const jellyfin::MediaStream& stream);

struct TrackPrefs {
    std::string audioLang;  // "" = file / server default
    std::string subLang;    // default "eng"
    std::string subMode;    // "default" | "always" | "forced" | "off"
    static TrackPrefs fromSettings();
};

class TrackManager {
public:
    void clear();
    // Online: tracks of the plan's media source. Transcodes keep only what the HLS output carries.
    void setFromPlan(const jellyfin::PlaybackPlan& plan);
    // Offline: embedded streams from the stored metadata plus the bundled subtitle files.
    void setFromOffline(const offline::Media& media);
    // A subtitle downloaded during playback (sub-add URL known, not yet added to mpv).
    Track& addExternalSubtitle(const jellyfin::MediaStream& stream, const std::string& url);

    // Maps mpv ids and selection from the track-list node; adds tracks only mpv knows.
    void mergeMpv(const mpv_node& trackList);

    std::vector<Track>& audio() { return audio_; }
    std::vector<Track>& subtitles() { return subs_; }
    const std::vector<Track>& audio() const { return audio_; }
    const std::vector<Track>& subtitles() const { return subs_; }
    const Track* selectedAudio() const;
    const Track* selectedSubtitle() const;  // null = off
    Track* findSubtitleByJellyfin(int index);
    Track* findAudioByJellyfin(int index);
    Track* findSubtitleByUrl(const std::string& url);

    // Jellyfin indices of the initial selection (-1 = off / default).
    struct Choice {
        int audio = -1;
        int subtitle = -1;
    };
    Choice autoSelect(const TrackPrefs& prefs, std::optional<int> requestedAudio,
                      std::optional<int> requestedSubtitle, int serverDefaultAudio) const;

    // Language of the current audio (for "forced" / foreign-audio decisions).
    std::string audioLanguage(int jellyfinAudioIndex) const;
    bool transcoding() const { return transcoding_; }

    static std::string describe(const Track& t);  // "English - AAC 5.1 - Default"
    static std::string detail(const Track& t);     // secondary line ("Forced", "SDH", "External")

private:
    std::vector<Track> audio_;
    std::vector<Track> subs_;
    bool transcoding_ = false;
    int transcodeAudioIndex_ = -1;
};

}  // namespace karafin::player
