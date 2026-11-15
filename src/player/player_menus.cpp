#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "core/log.hpp"
#include "core/main_queue.hpp"
#include "core/settings.hpp"
#include "player/languages.hpp"
#include "player/player_screen.hpp"
#include "ui/root.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/list_menu.hpp"
#include "ui/widgets/progress.hpp"

namespace karafin::player {

namespace theme = ui::theme;
using gfx::Icon;
using jellyfin::Result;

namespace {

ui::ListItem makeItem(std::string title, std::string subtitle, std::string detail, Icon icon, bool checked,
                      std::function<void()> onSelect, bool enabled = true) {
    ui::ListItem it;
    it.title = std::move(title);
    it.subtitle = std::move(subtitle);
    it.detail = std::move(detail);
    it.icon = icon;
    it.checked = checked;
    it.enabled = enabled;
    it.onSelect = std::move(onSelect);
    return it;
}

std::unique_ptr<ui::SideSheet> makeSheet(std::string title, std::vector<ui::ListItem> items, int focus,
                                         std::string subtitle = {}) {
    auto sheet = ui::SideSheet::make(std::move(title), std::move(items), focus, std::move(subtitle));
    sheet->setDim(0.2f);  // keep the video (and subtitle changes) visible behind
    return sheet;
}

std::string friendlySubtitleError(const jellyfin::ApiError& e) {
    switch (e.kind) {
        case jellyfin::ErrorKind::Forbidden:
        case jellyfin::ErrorKind::Unauthorized:
            return "You don't have permission to download subtitles. Ask the server administrator to allow "
                   "subtitle management for your account.";
        case jellyfin::ErrorKind::Network:
        case jellyfin::ErrorKind::Offline: return "Couldn't reach the server. Check your connection and try again.";
        case jellyfin::ErrorKind::NotFound:
            return e.message.empty() ? "No subtitles found. The server may have no subtitle provider (such as the "
                                       "OpenSubtitles plugin) configured."
                                     : e.message;
        default: return e.message.empty() ? "Something went wrong while talking to the server." : e.message;
    }
}

std::string formatCount(int n) {
    if (n >= 1000000) return std::format("{:.1f}M", n / 1e6);
    if (n >= 1000) return std::format("{:.1f}k", n / 1e3);
    return std::to_string(n);
}

std::string formatTimeShort(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    const int h = total / 3600, m = (total / 60) % 60, s = total % 60;
    return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
}

// Side sheet with a loading spinner / status message over the list (remote subtitle results).
class ResultsSheet : public ui::SideSheet {
public:
    static constexpr float kWidth = 580;

    ResultsSheet(std::string title, std::string subtitle) : SideSheet(std::move(title), std::move(subtitle)) {
        setPanelWidth(kWidth);
        setDim(0.3f);
        spinner_ = add<ui::Spinner>(36, theme::color::accentBright);
        status_ = add<ui::Label>("Searching for subtitles", theme::type::callout, theme::color::textSecondary);
        status_->setMaxLines(5);
        status_->setAlign(gfx::TextAlign::Center);
    }

    void showLoading(const std::string& text) {
        spinner_->setVisible(true);
        status_->setText(text);
        status_->setVisible(true);
    }
    void showMessage(const std::string& text) {
        spinner_->setVisible(false);
        status_->setText(text);
        status_->setVisible(true);
    }
    void showResults(std::vector<ui::ListItem> items) {
        spinner_->setVisible(false);
        status_->setVisible(false);
        menu()->setItems(std::move(items), 0);
        menu()->focusItem(0);
    }

    void layout() override {
        SideSheet::layout();
        const float x = width() - kWidth;
        spinner_->setFrame({x + kWidth * 0.5f - 18, height() * 0.42f - 18, 36, 36});
        status_->setFrame({x + 40, height() * 0.42f + 36, kWidth - 80, 120});
    }
    void update(float dt) override {
        SideSheet::update(dt);
        const ui::Vec2 off{(1.0f - appear()) * (kWidth + 40), 0};
        spinner_->offset.set(off);
        status_->offset.set(off);
    }

private:
    ui::Spinner* spinner_ = nullptr;
    ui::Label* status_ = nullptr;
};

}  // namespace

void PlayerScreen::presentSheet(std::unique_ptr<ui::Modal> sheet) {
    std::erase_if(sheets_, [](const ui::Ref<ui::Modal>& r) { return !r.get(); });
    sheets_.emplace_back(sheet.get());
    ui::present(std::move(sheet));
}

void PlayerScreen::dismissSheets() {
    for (auto& r : sheets_)
        if (ui::Modal* m = r.get(); m && !m->dismissing()) m->dismiss();
    sheets_.clear();
}

// --- Audio & subtitles --------------------------------------------------------------------------

void PlayerScreen::openTrackMenu() {
    if (!fileLoaded_ || modalOpen_ || phase_ == Phase::Error) return;
    refreshTrackList();
    std::vector<ui::ListItem> items;
    int focus = -1;

    if (!tracks_.audio().empty()) {
        const Track* a = tracks_.selectedAudio();
        items.push_back(makeItem("Audio", a ? TrackManager::describe(*a) : "Default", tracks_.audio().size() > 1 ? "Change" : "",
                                 Icon::Audio, false, guard([this] { openAudioMenu(); }), tracks_.audio().size() > 1));
    }

    const int burned = currentSubtitleIndex();
    const Track* selected = tracks_.selectedSubtitle();
    const bool off = !selected && burned < 0;
    items.push_back(makeItem("Off", "", "", Icon::Close, off, guard([this] { selectSubtitle(nullptr); })));
    if (off) focus = static_cast<int>(items.size()) - 1;

    for (const auto& t : tracks_.subtitles()) {
        const bool checked = t.selected || (burned >= 0 && t.jellyfinIndex == burned && t.needsRestart);
        const int jf = t.jellyfinIndex;
        const int mpvId = t.mpvId;
        const std::string url = t.url;
        items.push_back(makeItem(TrackManager::describe(t), TrackManager::detail(t), "", Icon::Subtitles, checked,
                                 guard([this, jf, mpvId, url] {
                                     for (auto& s : tracks_.subtitles()) {
                                         const bool match = jf >= 0 ? s.jellyfinIndex == jf
                                                                    : (!url.empty() ? s.url == url : s.mpvId == mpvId);
                                         if (match) return selectSubtitle(&s);
                                     }
                                 })));
        if (checked) focus = static_cast<int>(items.size()) - 1;
    }

    if (!offline_ && client_ && plan_) {
        items.push_back(makeItem("Download subtitles", "Search the server's subtitle providers", "", Icon::Download,
                                 false, guard([this] { openDownloadLanguagePicker(); })));
        if (subtitleCheckPending_)
            items.push_back(makeItem("Check for downloaded subtitle", "The server may have finished processing it",
                                     "", Icon::Refresh, false, guard([this] { checkForNewSubtitles(true); })));
    }
    presentSheet(makeSheet("Audio & Subtitles", std::move(items), focus));
}

void PlayerScreen::openAudioMenu() {
    std::vector<ui::ListItem> items;
    int focus = -1;
    for (const auto& t : tracks_.audio()) {
        const int jf = t.jellyfinIndex;
        const int mpvId = t.mpvId;
        items.push_back(makeItem(TrackManager::describe(t), TrackManager::detail(t), "", Icon::Audio, t.selected,
                                 guard([this, jf, mpvId] {
                                     for (auto& a : tracks_.audio())
                                         if (jf >= 0 ? a.jellyfinIndex == jf : a.mpvId == mpvId) return selectAudio(&a);
                                 })));
        if (t.selected) focus = static_cast<int>(items.size()) - 1;
    }
    presentSheet(makeSheet("Audio", std::move(items), focus));
}

// --- Options ------------------------------------------------------------------------------------

void PlayerScreen::openMoreMenu() {
    if (modalOpen_ || phase_ == Phase::Error) return;
    std::vector<ui::ListItem> items;
    items.push_back(makeItem("Playback speed", "", std::format("{:g}x", speed_), Icon::Speed, false,
                             guard([this] { openSpeedMenu(); }), fileLoaded_));
    items.push_back(makeItem("Subtitle delay", "", std::format("{:+.1f} s", subDelay_), Icon::Clock, false,
                             guard([this] { openSubtitleDelayMenu(); }), fileLoaded_));
    items.push_back(makeItem("Subtitle size", "", std::format("{:.0f}%", subScale_ * 100), Icon::Subtitles, false,
                             guard([this] { openSubtitleSizeMenu(); })));
    items.push_back(makeItem("Subtitle position", "", subPos_ >= 100 ? "Bottom" : std::format("{}", subPos_),
                             Icon::ChevronUp, false, guard([this] { openSubtitlePositionMenu(); })));
    static const std::map<std::string, std::string> kAspectNames = {
        {"auto", "Original"}, {"fill", "Fill screen"}, {"stretch", "Stretch"},
        {"16:9", "16:9"},     {"4:3", "4:3"},          {"2.35:1", "2.35:1"}};
    items.push_back(makeItem("Aspect ratio & zoom", "", kAspectNames.count(aspect_) ? kAspectNames.at(aspect_) : aspect_,
                             Icon::Tv, false, guard([this] { openAspectMenu(); }), fileLoaded_));
    if (chapters_.size() >= 2)
        items.push_back(makeItem("Chapters", "", std::to_string(chapters_.size()), Icon::List, false,
                                 guard([this] { openChapterMenu(); }), fileLoaded_));
    items.push_back(makeItem("Playback info", "Codec, decoder, bitrate, dropped frames", showInfo_ ? "On" : "Off",
                             Icon::Info, false, guard([this] {
                                 showInfo_ = !showInfo_;
                                 infoTimer_ = 0;
                             })));
    presentSheet(makeSheet("Options", std::move(items), 0));
}

void PlayerScreen::openSpeedMenu() {
    static constexpr double kSpeeds[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    std::vector<ui::ListItem> items;
    int focus = 2;
    for (double s : kSpeeds) {
        const bool checked = std::fabs(speed_ - s) < 0.01;
        if (checked) focus = static_cast<int>(items.size());
        items.push_back(makeItem(s == 1.0 ? "Normal" : std::format("{:g}x", s), "", "", Icon::None, checked,
                                 guard([this, s] {
                                     if (!mpv_) return;
                                     mpv_->setDouble("speed", s);
                                     speed_ = s;
                                     reportProgress("PlaybackRateChange");
                                 })));
    }
    presentSheet(makeSheet("Playback speed", std::move(items), focus));
}

void PlayerScreen::presentAdjuster(const std::string& title, std::function<std::string()> value,
                                   std::vector<std::pair<std::string, std::function<void()>>> actions) {
    auto sheet = std::make_unique<ui::SideSheet>(title);
    sheet->setDismissOnSelect(false);
    sheet->setDim(0.15f);
    ui::Ref<ui::ListMenu> menu(sheet->menu());
    using Builder = std::function<std::vector<ui::ListItem>()>;
    auto build = std::make_shared<Builder>();
    std::weak_ptr<Builder> weak = build;
    *build = [value, actions, menu, weak]() {
        std::vector<ui::ListItem> items;
        items.push_back(makeItem("Current", "", value(), Icon::None, false, {}, false));
        for (size_t i = 0; i < actions.size(); ++i) {
            auto act = actions[i].second;
            const int index = static_cast<int>(i) + 1;
            items.push_back(makeItem(actions[i].first, "", "", Icon::None, false, [act, menu, weak, index] {
                act();
                // Rebuilt next frame: the row that triggered this is still on the call stack.
                main_thread::post([menu, weak, index] {
                    auto b = weak.lock();
                    ui::ListMenu* m = menu.get();
                    if (!b || !m) return;
                    m->setItems((*b)(), index);
                    m->focusItem(static_cast<size_t>(index));
                });
            }));
        }
        return items;
    };
    sheet->menu()->setItems((*build)(), 1);
    sheet->onDismissed = [build] { (void)build; };  // keeps the builder alive with the sheet
    presentSheet(std::move(sheet));
}

void PlayerScreen::openSubtitleDelayMenu() {
    auto life = lifetime();
    auto value = [this, life] { return life.expired() ? std::string() : std::format("{:+.1f} s", subDelay_); };
    auto step = [this](double d) {
        return guard([this, d] {
            subDelay_ = d == 0 ? 0.0 : std::round((subDelay_ + d) * 10.0) / 10.0;
            if (mpv_) mpv_->setDouble("sub-delay", subDelay_);
        });
    };
    presentAdjuster("Subtitle delay", value,
                    {{"Earlier by 1 s", step(-1.0)},
                     {"Earlier by 0.1 s", step(-0.1)},
                     {"Later by 0.1 s", step(0.1)},
                     {"Later by 1 s", step(1.0)},
                     {"Reset", step(0.0)}});
}

void PlayerScreen::openSubtitleSizeMenu() {
    static constexpr double kSizes[] = {0.5, 0.75, 0.9, 1.0, 1.15, 1.3, 1.5, 1.75, 2.0};
    std::vector<ui::ListItem> items;
    int focus = 3;
    double best = 1e9;
    for (size_t i = 0; i < std::size(kSizes); ++i)
        if (std::fabs(kSizes[i] - subScale_) < best) {
            best = std::fabs(kSizes[i] - subScale_);
            focus = static_cast<int>(i);
        }
    for (size_t i = 0; i < std::size(kSizes); ++i) {
        const double s = kSizes[i];
        items.push_back(makeItem(std::format("{:.0f}%", s * 100), "", "", Icon::None, static_cast<int>(i) == focus,
                                 guard([this, s] {
                                     subScale_ = s;
                                     if (mpv_) mpv_->setDouble("sub-scale", s);
                                     Settings::instance().set("player.sub_scale", s);
                                 })));
    }
    presentSheet(makeSheet("Subtitle size", std::move(items), focus));
}

void PlayerScreen::openSubtitlePositionMenu() {
    static constexpr int kPositions[] = {100, 95, 90, 85, 80, 70};
    static constexpr const char* kNames[] = {"Bottom", "Slightly higher", "Higher", "Higher still", "Raised", "Middle-low"};
    std::vector<ui::ListItem> items;
    int focus = 0;
    for (size_t i = 0; i < std::size(kPositions); ++i) {
        const int p = kPositions[i];
        const bool checked = p == subPos_;
        if (checked) focus = static_cast<int>(i);
        items.push_back(makeItem(kNames[i], "", std::to_string(p), Icon::None, checked, guard([this, p] {
                                     subPos_ = p;
                                     if (mpv_) mpv_->setInt("sub-pos", p);
                                     Settings::instance().set("player.sub_pos", p);
                                 })));
    }
    presentSheet(makeSheet("Subtitle position", std::move(items), focus));
}

void PlayerScreen::applyAspect(const std::string& mode) {
    if (!mpv_) return;
    if (mpv_->setString("video-aspect-override", "no") < 0) mpv_->setString("video-aspect-override", "-1");
    mpv_->setDouble("panscan", 0.0);
    mpv_->setFlag("keepaspect", true);
    if (mode == "fill") mpv_->setDouble("panscan", 1.0);
    else if (mode == "stretch") mpv_->setFlag("keepaspect", false);
    else if (mode == "16:9" || mode == "4:3") mpv_->setString("video-aspect-override", mode);
    else if (mode == "2.35:1") mpv_->setString("video-aspect-override", "2.35");
    aspect_ = mode;
}

void PlayerScreen::openAspectMenu() {
    static const std::pair<const char*, const char*> kModes[] = {{"auto", "Original"},
                                                                 {"fill", "Fill screen (crop)"},
                                                                 {"stretch", "Stretch"},
                                                                 {"16:9", "Force 16:9"},
                                                                 {"4:3", "Force 4:3"},
                                                                 {"2.35:1", "Force 2.35:1"}};
    std::vector<ui::ListItem> items;
    int focus = 0;
    for (size_t i = 0; i < std::size(kModes); ++i) {
        const std::string mode = kModes[i].first;
        if (mode == aspect_) focus = static_cast<int>(i);
        items.push_back(
            makeItem(kModes[i].second, "", "", Icon::None, mode == aspect_, guard([this, mode] { applyAspect(mode); })));
    }
    presentSheet(makeSheet("Aspect ratio & zoom", std::move(items), focus));
}

void PlayerScreen::openChapterMenu() {
    std::vector<ui::ListItem> items;
    int focus = 0;
    const double pos = displayPosition();
    for (size_t i = 0; i < chapters_.size(); ++i) {
        const auto& c = chapters_[i];
        const bool current = c.time <= pos + 0.25 && (i + 1 == chapters_.size() || chapters_[i + 1].time > pos + 0.25);
        if (current) focus = static_cast<int>(i);
        const double t = c.time;
        items.push_back(makeItem(c.title.empty() ? std::format("Chapter {}", i + 1) : c.title, "", formatTimeShort(t),
                                 Icon::None, current, guard([this, t] { seekAbsolute(t); })));
    }
    presentSheet(makeSheet("Chapters", std::move(items), focus));
}

// --- Remote subtitle download ---------------------------------------------------------------

void PlayerScreen::openDownloadLanguagePicker() {
    if (!client_ || !plan_ || offline_) return;
    auto showPicker = guard([this] {
        const std::string preferred = lang::normalize(Settings::instance().get<std::string>("player.sub_lang", "eng"));
        std::vector<ui::ListItem> items;
        int focus = 0;
        for (const auto& l : lang::common()) {
            const std::string code = l.code;
            const bool pref = code == preferred;
            if (pref) focus = static_cast<int>(items.size());
            items.push_back(makeItem(l.name, "", code, Icon::None, pref, guard([this, code] { searchRemoteSubtitles(code); })));
        }
        presentSheet(makeSheet("Download subtitles", std::move(items), focus, "Choose a language"));
    });
    if (!client_->user().canManageSubtitles()) {
        ui::DialogSpec spec;
        spec.title = "Subtitle downloads not allowed";
        spec.message = "Your account doesn't have permission to manage subtitles on this server. Ask the server "
                       "administrator to enable subtitle management for your user.";
        spec.icon = gfx::Icon::Lock;
        spec.iconColor = theme::color::warning;
        spec.buttons = {{"OK", ui::ButtonStyle::Primary, {}}, {"Try anyway", ui::ButtonStyle::Secondary, showPicker}};
        spec.cancelIndex = 0;
        ui::present(std::make_unique<ui::Dialog>(std::move(spec)));
        return;
    }
    showPicker();
}

void PlayerScreen::searchRemoteSubtitles(const std::string& language) {
    if (!client_ || !haveItem_) return;
    auto sheet = std::make_unique<ResultsSheet>("Download subtitles", lang::displayName(language));
    ui::Ref<ResultsSheet> ref(sheet.get());
    sheet->showLoading("Searching for " + lang::displayName(language) + " subtitles");
    presentSheet(std::move(sheet));
    log::info("player", "remote subtitle search {} for {}", language, item_.id);

    client_->searchRemoteSubtitles(item_.id, language)
        .then(
            [this, ref](Result<std::vector<jellyfin::RemoteSubtitleInfo>> r) {
                ResultsSheet* s = ref.get();
                if (!r) {
                    log::warn("player", "subtitle search failed: {}", r.error.describe());
                    const std::string msg = friendlySubtitleError(r.error);
                    if (s) s->showMessage(msg);
                    else ui::toast(msg, ui::ToastKind::Error, gfx::Icon::Subtitles, 4.0f);
                    return;
                }
                if (!s) return;
                std::vector<ui::ListItem> items;
                for (const auto& info : r.value) {
                    std::vector<std::string> parts;
                    if (!info.providerName.empty()) parts.push_back(info.providerName);
                    if (!info.format.empty()) {
                        std::string f = info.format;
                        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        parts.push_back(f);
                    }
                    if (info.communityRating > 0) parts.push_back(std::format("Rating {:.1f}", info.communityRating));
                    if (info.downloadCount > 0) parts.push_back(formatCount(info.downloadCount) + " downloads");
                    if (info.hearingImpaired) parts.push_back("HI");
                    if (info.forced) parts.push_back("Forced");
                    if (info.aiTranslated || info.machineTranslated) parts.push_back("Machine translated");
                    std::string sub;
                    for (const auto& p : parts) sub += (sub.empty() ? "" : "  ·  ") + p;
                    const std::string title = !info.name.empty() ? info.name : info.providerName;
                    items.push_back(makeItem(title, sub, info.isHashMatch ? "Exact match" : "", Icon::Subtitles, false,
                                             guard([this, info] { downloadRemoteSubtitle(info); })));
                }
                s->showResults(std::move(items));
            },
            token_);
}

void PlayerScreen::downloadRemoteSubtitle(const jellyfin::RemoteSubtitleInfo& info) {
    if (!client_ || !plan_) return;
    ui::toast("Downloading subtitle", ui::ToastKind::Info, gfx::Icon::Download, 3.0f);
    log::info("player", "downloading remote subtitle {} ({})", info.id, info.providerName);
    client_->downloadRemoteSubtitleAndRefresh(item_.id, plan_->mediaSourceId, info.id)
        .then(
            [this](Result<jellyfin::SubtitleAddResult> r) {
                if (!r) {
                    log::warn("player", "subtitle download failed: {}", r.error.describe());
                    ui::present(ui::Dialog::alert("Subtitle download failed", friendlySubtitleError(r.error)));
                    return;
                }
                if (r->newStream && !r->url.empty()) {
                    addDownloadedSubtitle(*r->newStream, r->url);
                    return;
                }
                // Downloaded, but the server's refresh hasn't indexed the file yet: keep looking
                // in the background; the subtitle menu offers a manual check meanwhile.
                subtitleCheckPending_ = true;
                ui::toast("Subtitle downloaded, the server is still processing it. It will appear shortly "
                          "(or use \"Check for downloaded subtitle\").",
                          ui::ToastKind::Warning, gfx::Icon::Clock, 5.0f);
                for (double delay : {6.0, 15.0, 30.0}) {
                    ui::timeline().after(static_cast<float>(delay), guard([this] {
                                             if (subtitleCheckPending_) checkForNewSubtitles(false);
                                         }),
                                         lifetime());
                }
            },
            token_);
}

void PlayerScreen::addDownloadedSubtitle(const jellyfin::MediaStream& stream, const std::string& url) {
    knownSubtitleKeys_.insert(subtitleKey(stream));
    subtitleCheckPending_ = false;
    Track& t = tracks_.addExternalSubtitle(stream, url);
    log::info("player", "adding downloaded subtitle #{} ({})", stream.index, stream.displayTitle);
    selectSubtitle(&t);
    ui::toast("Subtitle added", ui::ToastKind::Success, gfx::Icon::Subtitles);
}

void PlayerScreen::checkForNewSubtitles(bool userInitiated) {
    if (!client_ || !plan_) return;
    if (userInitiated) ui::toast("Checking for the subtitle", ui::ToastKind::Info, gfx::Icon::Refresh, 2.0f);
    client_->refreshMediaSource(item_.id, plan_->mediaSourceId)
        .then(
            [this, userInitiated](Result<jellyfin::MediaSource> r) {
                if (!r) {
                    if (userInitiated) ui::toast(friendlySubtitleError(r.error), ui::ToastKind::Error, gfx::Icon::Warning, 4.0f);
                    return;
                }
                for (const auto& s : r->mediaStreams) {
                    if (!s.isSubtitle() || knownSubtitleKeys_.count(subtitleKey(s))) continue;
                    addDownloadedSubtitle(s, client_->subtitleUrl(item_.id, r->id, s.index, s.subtitleFormat()));
                    return;
                }
                if (!userInitiated) return;
                ui::DialogSpec spec;
                spec.title = "Not ready yet";
                spec.message = "The server is still processing the subtitle. Try again in a few seconds.";
                spec.icon = gfx::Icon::Clock;
                spec.buttons = {{"Check again", ui::ButtonStyle::Primary, guard([this] { checkForNewSubtitles(true); })},
                                {"Close", ui::ButtonStyle::Secondary, {}}};
                spec.cancelIndex = 1;
                ui::present(std::make_unique<ui::Dialog>(std::move(spec)));
            },
            token_);
}

}  // namespace karafin::player
