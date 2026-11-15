#pragma once

#include <cstdint>
#include <string>

#include "downloads/manager.hpp"
#include "jellyfin/models.hpp"
#include "ui/view.hpp"

namespace karafin::screens {

// Download control with live state from the DownloadManager: Download / Queued / Preparing /
// percent ring / Paused / Waiting / Downloaded / Failed. A opens the matching actions (pause,
// resume, cancel, retry, play offline, delete).
//
// Item mode: a movie or episode. Group mode: every episode of a season or series.
// Compact: a 44 px circle (episode rows); otherwise a pill with a label (detail pages).
class DownloadButton : public ui::View {
public:
    explicit DownloadButton(bool compact = false);

    void setItem(const jellyfin::BaseItem& item);
    void setGroup(const jellyfin::BaseItem& seasonOrSeries);

    ui::Vec2 measure(ui::Vec2 available) override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    enum class Visual : uint8_t { None, Queued, Preparing, Downloading, Paused, Waiting, Done, Failed, Partial };

    void refresh();
    void refreshState();
    void openItemMenu();
    void openGroupMenu();
    void startDownload();
    std::string serverId() const;

    bool compact_;
    bool group_ = false;
    jellyfin::BaseItem item_;
    std::string key_;
    Visual visual_ = Visual::None;
    std::string label_;
    std::string error_;
    float progress_ = 0;
    downloads::GroupSummary summary_;
    downloads::Subscription sub_;
    ui::Animated<float> focus_{0.0f};
    ui::Animated<float> ring_{0.0f};
    float spin_ = 0;
    float pulse_ = 0;
};

}  // namespace karafin::screens
