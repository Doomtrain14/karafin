#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "core/platform.hpp"
#include "gfx/icons.hpp"
#include "gfx/image.hpp"
#include "jellyfin/client.hpp"
#include "player/mpv.hpp"
#include "player/offline.hpp"
#include "player/player.hpp"
#include "player/swipe.hpp"
#include "player/tracks.hpp"
#include "player/up_next.hpp"
#include "ui/battery_indicator.hpp"
#include "ui/modal.hpp"
#include "ui/screen.hpp"

namespace karafin::player {

// A text layout kept across frames; laid out again only when the string or display scale changes.
struct CachedText {
    std::string text;
    gfx::TextLayoutPtr layout;
    // Null if text rendering is unavailable.
    const gfx::TextLayout* get(std::string_view s, const gfx::TextStyle& style, const gfx::TextOptions& options = {});
    float width(std::string_view s, const gfx::TextStyle& style) {
        const gfx::TextLayout* l = get(s, style);
        return l ? l->width : 0.0f;
    }
};

// Full-screen player: mpv video drawn inside draw() through Renderer::beginExternal(), a custom
// OSD on top, track menus, subtitle download, skip segments, up-next and Jellyfin reporting.
class PlayerScreen : public ui::Screen {
public:
    explicit PlayerScreen(PlayRequest request);
    ~PlayerScreen() override;

    // Quit-time teardown of every live player screen (see player::shutdown). Main thread.
    static void shutdownAll(int timeoutMs);

    void onShow() override;
    void onHide() override;
    void onPopped() override;
    bool onInput(const ui::InputEvent& e) override;
    bool onTouchEvent(const SDL_Event& e) override;
    bool onTap(ui::Vec2 local) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    enum class Phase { Loading, Opening, Playing, Ended, Error };

    struct Chapter {
        double time = 0;
        std::string title;
    };

    struct NextUp {
        jellyfin::BaseItem item;
        std::optional<offline::Media> offline;
        std::string imageUrl;
    };

    // Touch targets registered while drawing the OSD (rebuilt every frame, so no closures).
    enum class HitAction : uint8_t { Back, SeekBack, PlayPause, SeekForward, Next, Tracks, Options, Skip, UpNext, DismissUpNext, ErrorBack };
    struct HitArea {
        ui::Rect rect;
        HitAction action;
    };

    // --- Session (player_screen.cpp) ----------------------------------------------------------
    void startItem(PlayRequest request);
    void onItem(const jellyfin::BaseItem& item);
    void requestPlan(int64_t startTicks, std::optional<int> audio, std::optional<int> subtitle, bool allowDirect,
                     bool explicitTracks);
    void openPlan(jellyfin::PlaybackPlan plan, bool explicitTracks);
    void openOffline(offline::Media media);
    void loadFile(const std::string& url, double startSeconds, const TrackManager::Choice& choice);
    void maybeLoadPending();
    bool ensureMpv();
    void handleEvent(const mpv_event& ev);
    void onPropertyChange(uint64_t id, const mpv_event_property& prop);
    void onFileLoaded();
    void onPlaybackRestart();
    void onEndFile(const mpv_event_end_file& ef);
    void onReachedEnd();
    void retryWithTranscode(const std::string& reason);
    void restartStream(int audioIndex, int subtitleIndex);
    // Network loss mid-stream (sleep/wake, Wi-Fi drop): wait for the network, then reload at
    // the last position instead of failing.
    bool beginRecovery(const std::string& reason);
    void updateRecovery(float dt);
    void reloadAfterRecovery();
    void fail(const std::string& title, const std::string& message, bool retryable);
    void finishSession(bool finished);
    void shutdownNow(std::chrono::steady_clock::time_point deadline);
    void exitPlayer();
    void resetForNewItem();
    void updatePlatformLocks();
    void buildTitles();
    void loadChaptersFromItem();
    void fetchExtras();
    void findNextEpisode();

    TrackPrefs effectivePrefs() const;

    // Reporting.
    jellyfin::PlaybackReport makeReport() const;
    void reportProgress(const char* eventName);
    void saveOfflineProgress(bool final);
    int currentAudioIndex() const;
    int currentSubtitleIndex() const;

    // Playback control.
    void togglePause();
    void setPaused(bool paused);
    void seekAbsolute(double seconds);
    void seekRelative(double delta);
    void chapterJump(int direction);
    void scrubBy(double delta);
    void beginScrub();
    void commitScrub();
    void cancelScrub();
    double seekStep() const { return seekStep_; }
    double displayPosition() const;
    // Position to report / save when the session ends (ignores an uncommitted scrub; the
    // requested start until playback actually began).
    double finalPosition() const;
    void onFocusChange(bool focused);

    // Tracks (subtitle manager glue).
    void refreshTrackList();
    void applyInitialTracks();
    void selectSubtitle(Track* track);
    void selectAudio(Track* track);
    void addExternalSubtitle(const Track& track, bool select);
    void updateSubtitleFont(const std::string& language);
    void applySubtitleStyle();
    uint64_t asyncCommand(const std::vector<std::string>& args, std::function<void(int)> done);

    // Segments / up next.
    void updateSegments(float dt);
    void skipActiveSegment();
    void updateUpNext(float dt);
    // `finishedCurrent`: the current item counts as watched (credits / up-next / end).
    void playNext(bool finishedCurrent);
    void dismissUpNext();

    // OSD (player_osd.cpp).
    void showOsd(double seconds = 4.0);
    void hideOsd();
    void updateClock();
    bool osdVisible() const { return osd_.target() > 0.5f; }
    void drawVideo(ui::Renderer& r);
    void drawBackdrop(ui::Renderer& r);
    void drawTopBar(ui::Renderer& r, float a);
    void drawTags(ui::Renderer& r, ui::Vec2 at);
    void drawBottomBar(ui::Renderer& r, float a);
    void drawSeekPreview(ui::Renderer& r, float a);
    void drawCenter(ui::Renderer& r);
    void drawSkipButton(ui::Renderer& r);
    void drawUpNext(ui::Renderer& r);
    void drawError(ui::Renderer& r);
    void drawInfoOverlay(ui::Renderer& r);
    void drawAdjustment(ui::Renderer& r);
    void refreshInfo();
    void refreshTags();
    void runHit(HitAction action);
    ui::Rect seekBarRect() const;
    double positionFromX(float x) const;
    void loadLogo();
    const gfx::Texture* trickplaySheet(int sheet, ui::Rect* uv);

    // Menus (player_menus.cpp).
    void openTrackMenu();
    void openAudioMenu();
    void openMoreMenu();
    void openSpeedMenu();
    void openSubtitleDelayMenu();
    void openSubtitleSizeMenu();
    void openSubtitlePositionMenu();
    void openAspectMenu();
    void applyAspect(const std::string& mode);
    void openDownloadLanguagePicker();
    void searchRemoteSubtitles(const std::string& language);
    void downloadRemoteSubtitle(const jellyfin::RemoteSubtitleInfo& info);
    void checkForNewSubtitles(bool userInitiated);
    void addDownloadedSubtitle(const jellyfin::MediaStream& stream, const std::string& url);
    void openChapterMenu();
    void presentAdjuster(const std::string& title, std::function<std::string()> value,
                         std::vector<std::pair<std::string, std::function<void()>>> actions);
    void presentSheet(std::unique_ptr<ui::Modal> sheet);
    void dismissSheets();

    // --- State ---------------------------------------------------------------------------------
    PlayRequest request_;
    jellyfin::ClientPtr client_;
    CancelToken token_;  // per item; cancelled on item change and destruction
    std::unique_ptr<Mpv> mpv_;
    TrackManager tracks_;
    Phase phase_ = Phase::Loading;

    jellyfin::BaseItem item_;
    bool haveItem_ = false;
    std::optional<jellyfin::PlaybackPlan> plan_;
    std::optional<offline::Media> offline_;
    bool triedTranscode_ = false;
    bool exiting_ = false;
    bool shutDown_ = false;  // shutdownNow() ran; nothing may report or touch mpv afterwards
    int64_t startTicks_ = 0;
    TrackManager::Choice initialChoice_;
    std::string title_;
    std::string subtitleLine_;
    std::string errorTitle_;
    std::string errorMessage_;

    // Settings read once per item (not every frame).
    std::string skipMode_ = "ask";
    bool autoplayNext_ = true;
    double seekStep_ = 10;

    // Stream recovery.
    bool recovering_ = false;  // waiting for the network
    bool reloading_ = false;   // the recovery reload is opening
    int recoveryAttempts_ = 0;
    double recoveryWait_ = 0;
    double recoveryPoll_ = 0;
    double recoveryPos_ = 0;
    double stablePlay_ = 0;    // seconds played since the last recovery

    // Focus (HOME menu, sleep).
    bool pausedByFocus_ = false;
    int64_t focusLostAt_ = 0;

    struct PendingLoad {
        std::string url;
        double start = 0;
        TrackManager::Choice choice;
    };
    std::optional<PendingLoad> pendingLoad_;

    // mpv state.
    double position_ = 0;
    double duration_ = 0;
    double cacheEnd_ = 0;
    bool paused_ = false;
    bool pausedForCache_ = false;
    bool seeking_ = false;
    bool fileLoaded_ = false;
    bool started_ = false;  // first playback-restart after load
    bool reportedStart_ = false;
    bool sessionOpen_ = false;
    int64_t bufferingPct_ = 0;
    std::string hwdec_;
    std::vector<Chapter> chapters_;
    bool chaptersFromMpv_ = false;
    std::optional<double> pendingSeek_;
    // Seconds since pendingSeek_ was issued; an unconfirmed seek is dropped after kSeekConfirmTimeout.
    double pendingSeekAge_ = 0;
    double speed_ = 1.0;
    double subDelay_ = 0;
    double subScale_ = 1.0;
    int subPos_ = 100;
    std::string aspect_ = "auto";
    std::string fontDir_;
    double reportTimer_ = 0;
    double offlineSaveTimer_ = 0;
    double startTime_ = 0;
    std::map<uint64_t, std::function<void(int)>> pendingCommands_;
    uint64_t nextCommandId_ = 1000;
    std::set<std::string> knownSubtitleKeys_;
    bool subtitleCheckPending_ = false;
    std::vector<ui::Ref<ui::Modal>> sheets_;

    // Session track preferences (carried over to the next episode once the user picks tracks).
    std::optional<std::string> sessionAudioLang_;
    std::optional<std::string> sessionSubLang_;  // "" = user turned subtitles off

    // OSD.
    ui::Animated<float> osd_{0.0f};
    double osdTimer_ = 0;
    bool scrubbing_ = false;
    double scrubTarget_ = 0;
    double scrubIdle_ = 0;
    bool scrubKeyHeld_ = false;
    ui::Animated<float> scrubGrow_{0.0f};
    bool touchActive_ = false;
    bool touchOnBar_ = false;
    bool touchDragging_ = false;
    ui::Vec2 touchStart_;
    int64_t touchFinger_ = 0;
    uint32_t touchStartedAt_ = 0;
    PlaybackSwipe swipe_;
    platform::PlaybackBrightness brightness_;
    float swipeInitial_ = 0;
    float adjustmentValue_ = 0;
    bool adjustingBrightness_ = false;
    double adjustmentTimer_ = 0;
    std::string adjustmentLabel_;
    CachedText adjustmentText_;
    ui::Animated<float> flash_{0.0f};
    gfx::Icon flashIcon_ = gfx::Icon::Play;
    bool showInfo_ = false;
    double infoTimer_ = 0;
    struct InfoLine {
        gfx::TextLayoutPtr key;
        gfx::TextLayoutPtr value;
    };
    std::vector<InfoLine> infoLines_;
    // Stream tags in the top bar (play method, resolution, codecs); refreshed while the OSD shows.
    struct OsdTag {
        std::string text;
        bool playMethod = false;
        bool transcoding = false;
        gfx::TextLayoutPtr layout;
    };
    std::vector<OsdTag> osdTags_;
    double tagsTimer_ = 0;
    std::vector<HitArea> hits_;
    gfx::ImagePtr backdrop_;
    gfx::ImagePtr logo_;
    ui::Animated<float> videoReveal_{0.0f};
    double spinnerPhase_ = 0;
    double busyFor_ = 0;
    std::string clockText_;
    ui::BatteryIndicator battery_;
    std::string endsText_;
    double clockNext_ = 0;

    // OSD text, re-laid out only on change.
    CachedText titleText_, subtitleText_, clockLayout_, endsLayout_, chapterText_;
    CachedText elapsedText_, remainingText_, tracksLabel_, optionsLabel_;
    CachedText previewTime_, previewDelta_, previewTimeLarge_, previewDeltaLarge_, centerLabel_, skipText_;
    CachedText upNextTitle_, upNextName_, upNextCountdown_, upNextPlay_, upNextHide_;
    CachedText errorTitleText_, errorBodyText_, errorHint_;
    int64_t elapsedKey_ = -1, remainingKey_ = -1, previewKey_ = INT64_MIN, previewDeltaKey_ = INT64_MIN;
    int64_t countdownKey_ = -1, bufferingKey_ = -1;
    std::string elapsedStr_, remainingStr_, previewStr_, previewDeltaStr_, countdownStr_, bufferingStr_;
    std::string upNextFor_, upNextNameStr_;

    // Trickplay.
    std::optional<jellyfin::TrickplayInfo> trickplay_;
    std::string trickplaySource_;
    std::map<int, gfx::ImagePtr> trickplaySheets_;

    // Segments.
    std::vector<jellyfin::MediaSegment> segments_;
    const jellyfin::MediaSegment* activeSegment_ = nullptr;
    std::set<std::string> handledSegments_;
    std::string lastSegmentId_;
    std::string skipLabel_;  // kept while the button fades out
    double segmentShownFor_ = 0;
    ui::Animated<float> skipAppear_{0.0f};

    // Up next.
    std::optional<NextUp> next_;
    bool nextLookupDone_ = false;
    bool upNextVisible_ = false;
    bool upNextDismissed_ = false;
    UpNextTimer upNextTimer_;
    ui::Animated<float> upNextAppear_{0.0f};
    gfx::ImagePtr nextImage_;

    // Platform.
    std::unique_ptr<platform::AwakeLock> awake_;
    std::unique_ptr<platform::CpuBoost> boost_;
    bool throttled_ = false;
    bool hadFocus_ = true;
    bool modalOpen_ = false;
};

}  // namespace karafin::player
