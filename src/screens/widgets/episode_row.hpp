#pragma once

#include <functional>
#include <string>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/widgets/download_button.hpp"
#include "ui/view.hpp"
#include "ui/widgets/image_view.hpp"

namespace karafin::screens {

// Episode list entry: 16:9 still with progress / watched check, number + title, runtime and
// rating, two lines of overview, and a compact download button on the right.
// A plays (resume prompt), X opens the episode page, Y toggles watched.
class EpisodeRow : public ui::View {
public:
    static constexpr float kHeight = 132;

    explicit EpisodeRow(const jellyfin::BaseItem& episode, bool showDownload = true);
    ~EpisodeRow() override;

    void setItem(const jellyfin::BaseItem& episode);
    const jellyfin::BaseItem& item() const { return item_; }
    DownloadButton* downloadButton() const { return download_; }

    std::function<void(const jellyfin::BaseItem&)> onWatchedChanged;

    ui::Vec2 measure(ui::Vec2 available) override;
    void layout() override;
    void onFocus() override;
    void onBlur() override;
    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;
    void drawOverlay(ui::Renderer& r) override;

private:
    void toggleWatched();
    void updateText();

    jellyfin::BaseItem item_;
    std::string thumbUrl_;
    std::string title_;
    std::string meta_;
    ui::ImageView* thumb_ = nullptr;
    DownloadButton* download_ = nullptr;
    ui::Animated<float> focus_{0.0f};
    CancelToken token_;
};

}  // namespace karafin::screens
