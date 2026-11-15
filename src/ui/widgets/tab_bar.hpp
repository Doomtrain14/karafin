#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ui/view.hpp"

namespace karafin::ui {

class Screen;

// Capsule tab bar with a sliding pill indicator. Selection follows focus (Apple TV style); L/R
// switch tabs from anywhere on the screen once bindShoulderButtons() is called.
//
//   auto* tabs = add<TabBar>(std::vector<std::string>{"Home", "Movies", "Shows"});
//   tabs->onSelect = [this](size_t i) { showTab(i); };
//   tabs->bindShoulderButtons(this);
class TabBar : public View {
public:
    explicit TabBar(std::vector<std::string> titles = {});

    void setTitles(std::vector<std::string> titles);
    size_t selected() const { return selected_; }
    void select(size_t index, bool notify = true);
    void selectNext() { select(selected_ + 1 < titles_.size() ? selected_ + 1 : selected_); }
    void selectPrevious() { select(selected_ > 0 ? selected_ - 1 : 0); }
    void bindShoulderButtons(Screen* screen);

    std::function<void(size_t index)> onSelect;

    Vec2 measure(Vec2 available) override;
    void layout() override;
    void onFocusWithin(View* descendant) override;
    View* defaultFocus() override;
    View* navigateFocus(View* from, NavDir dir) override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    class Tab;
    std::vector<std::string> titles_;
    std::vector<Tab*> tabs_;
    size_t selected_ = 0;
    Animated<float> pillX_{0.0f};
    Animated<float> pillW_{0.0f};
    Animated<float> focusAmt_{0.0f};
    bool placed_ = false;
};

}  // namespace karafin::ui
