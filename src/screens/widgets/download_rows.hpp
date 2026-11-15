#pragma once

#include <functional>
#include <string>

#include "downloads/types.hpp"
#include "gfx/image.hpp"
#include "screens/widgets/action_button.hpp"
#include "ui/view.hpp"

namespace karafin::screens {

// Unfinished download: poster, title, progress bar, "42% - 3.1 MB/s - 4 min left - 1.2 of
// 2.8 GB", and Pause/Resume/Retry + Cancel buttons. Progress updates in place via setInfo().
class PendingDownloadRow : public ui::View {
public:
    static constexpr float kHeight = 120;  // room for the icon buttons' captions inside the plate

    explicit PendingDownloadRow(const downloads::DownloadInfo& info);
    void setInfo(const downloads::DownloadInfo& info);
    const std::string& key() const { return info_.key; }
    ActionButton* primaryButton() const { return toggle_; }
    // Jump the row plate to the focused look. Used when this row is rebuilt under an
    // already-focused download so the highlight does not spring in from zero.
    void snapFocusHighlight() { focus_.set(1.0f); }

    ui::Vec2 measure(ui::Vec2 available) override;
    void layout() override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    void applyButtons();

    downloads::DownloadInfo info_;
    std::string title_;
    std::string subtitle_;
    std::string status_;
    gfx::ImagePtr poster_;
    ActionButton* toggle_ = nullptr;
    ActionButton* cancel_ = nullptr;
    ui::Animated<float> progress_{0.0f};
    ui::Animated<float> focus_{0.0f};
    float phase_ = 0;
};

// Downloaded movie / episode, or a downloaded series group. Focusable; A runs onActivate (play
// or open), X toggles watched, Y deletes (the owner wires onToggleWatched / onDelete).
class OfflineItemRow : public ui::View {
public:
    static constexpr float kHeight = 96;

    OfflineItemRow(std::string title, std::string subtitle, std::string detail, const std::string& posterPath,
                   bool wideArt = false);

    void setProgress(float p) { progress_ = p; }  // < 0 hides
    void setWatched(bool w) { watched_ = w; }
    void setChevron(bool v) { chevron_ = v; }
    bool watched() const { return watched_; }
    // Title / subtitle / detail only. Does not touch focus, scale, or the poster.
    void setText(std::string title, std::string subtitle, std::string detail) {
        title_ = std::move(title);
        subtitle_ = std::move(subtitle);
        detail_ = std::move(detail);
    }

    std::function<void()> onToggleWatched;
    std::function<void()> onDelete;
    // Call after wiring the callbacks: only the X / Y actions that exist are shown as hints.
    void refreshHints();

    ui::Vec2 measure(ui::Vec2 available) override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    std::string title_;
    std::string subtitle_;
    std::string detail_;
    gfx::ImagePtr poster_;
    bool wide_;
    float progress_ = -1;
    bool watched_ = false;
    bool chevron_ = false;
    ui::Animated<float> focus_{0.0f};
};

}  // namespace karafin::screens
