#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/error.hpp"
#include "screens/main_screen.hpp"
#include "screens/widgets/row_stack.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/anim.hpp"
#include "ui/layout.hpp"
#include "ui/scroll.hpp"

namespace karafin::screens {

// Search: a field that opens the system keyboard (A, or Y anywhere in the section), debounced
// queries grouped into Movies / Shows / Episodes rows, and recent searches.
class SearchSection : public SectionView {
public:
    SearchSection();
    ~SearchSection() override;

    void onSectionShown(bool first) override;
    float headerScrim() const override { return 1.0f; }
    bool onInput(const ui::InputEvent& e) override;

private:
    class Field;
    void openKeyboard();
    void setQuery(std::string query);
    void run();
    void showSkeletons();
    void showIdle();
    void buildRecent();
    void remember(const std::string& query);

    Field* field_ = nullptr;
    ui::Box* recent_ = nullptr;
    ui::ScrollView* page_ = nullptr;
    std::unique_ptr<RowStack> rows_;
    StatusView* status_ = nullptr;
    std::string query_;
    int pending_ = 0;
    int found_ = 0;
    int failed_ = 0;
    jellyfin::ApiError lastError_;
    ui::Timeline::Handle debounce_ = 0;
    CancelToken token_;
};

}  // namespace karafin::screens
