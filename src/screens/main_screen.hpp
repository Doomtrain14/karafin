#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ui/screen.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/tab_bar.hpp"

namespace karafin::screens {

enum class MainMode : uint8_t { Online, Offline };
enum class Section : uint8_t { Home, Libraries, Search, Downloads, Settings, Count };

inline constexpr float kHeaderHeight = 92;  // sections keep their top content below this

// One page of the main screen. Sections fill the whole screen (the header floats on top) and
// are created lazily the first time they are shown.
class SectionView : public ui::View {
public:
    SectionView() { setRememberFocus(true); }
    // Became visible (tab switch) or the main screen became the top screen again. `first` is
    // true the very first time.
    virtual void onSectionShown(bool first) { (void)first; }
    virtual void onSectionHidden() {}
    // How strongly the header scrim should cover the content (0..1).
    virtual float headerScrim() const { return 1.0f; }
    // B pressed inside the section (before focus jumps to the tab bar). True = consumed.
    virtual bool onSectionBack() { return false; }
};

// Top-level page with the brand, a tab bar (L/R or D-pad) and the sections. Online: Home,
// Libraries, Search, Downloads, Settings. Offline: Downloads, Settings.
class MainScreen : public ui::Screen {
public:
    explicit MainScreen(MainMode mode, Section initial = Section::Home);

    void showSection(Section section);
    Section current() const { return current_; }
    MainMode mode() const { return mode_; }

    void onShow() override;
    void onHide() override;
    bool onBack() override;
    void update(float dt) override;

private:
    class Header;
    SectionView* ensure(Section s);
    size_t tabIndex(Section s) const;

    MainMode mode_;
    std::vector<Section> tabs_;
    std::array<SectionView*, static_cast<size_t>(Section::Count)> sections_{};
    std::array<bool, static_cast<size_t>(Section::Count)> shownOnce_{};
    Section current_ = Section::Home;
    ui::View* body_ = nullptr;
    Header* header_ = nullptr;
    ui::TabBar* tabBar_ = nullptr;
    bool visibleNow_ = false;
};

}  // namespace karafin::screens
