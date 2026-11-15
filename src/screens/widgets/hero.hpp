#pragma once

#include <functional>
#include <string>
#include <vector>

#include "gfx/image.hpp"
#include "jellyfin/models.hpp"
#include "screens/widgets/action_button.hpp"
#include "screens/widgets/chips.hpp"
#include "screens/widgets/skeleton.hpp"
#include "ui/anim.hpp"
#include "ui/layout.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::screens {

struct HeroEntry {
    jellyfin::BaseItem item;
    std::string tag;  // overline: "CONTINUE WATCHING", "NEW IN MOVIES", ...
};

// Featured items carousel for the top of Home: logo (or title), metadata chips, overview and
// Play / More info buttons over the screen backdrop. Auto-advances with a progress dot; Right
// on the last button / Left on the first pages manually. The owner swaps the backdrop in
// onCurrentChanged.
class HeroCarousel : public ui::View {
public:
    static constexpr float kHeight = 556;

    HeroCarousel();

    void setLoading();
    void setEntries(std::vector<HeroEntry> entries);
    size_t count() const { return entries_.size(); }
    const HeroEntry* current() const { return index_ < entries_.size() ? &entries_[index_] : nullptr; }
    void advance(int dir);
    // Auto-advance runs only while this is true (owner: section visible and scrolled to top).
    void setAutoAdvance(bool on) { auto_ = on; }
    ActionButton* playButton() const { return play_; }

    std::function<void(const HeroEntry&)> onCurrentChanged;
    std::function<void(const jellyfin::BaseItem&)> onPlay;
    std::function<void(const jellyfin::BaseItem&)> onInfo;

    ui::Vec2 measure(ui::Vec2 available) override;
    void layout() override;
    ui::View* navigateFocus(ui::View* from, ui::NavDir dir) override;
    bool onInput(const ui::InputEvent& e) override;
    void update(float dt) override;
    void draw(ui::Renderer& r) override;

private:
    void show(size_t index, int dir);
    void bind();
    void checkLogo();

    std::vector<HeroEntry> entries_;
    size_t index_ = 0;
    ui::Box* content_ = nullptr;
    ui::Label* overline_ = nullptr;
    ui::ImageView* logo_ = nullptr;
    ui::Label* title_ = nullptr;
    ChipRow* chips_ = nullptr;
    ui::Label* overview_ = nullptr;
    ui::Box* buttons_ = nullptr;
    ActionButton* play_ = nullptr;
    ActionButton* info_ = nullptr;
    Skeleton* skeleton_ = nullptr;
    gfx::ImagePtr logoImage_;
    bool logoShown_ = false;
    bool auto_ = false;
    float timer_ = 0;
    ui::Timeline::Handle anim_ = 0;
};

}  // namespace karafin::screens
