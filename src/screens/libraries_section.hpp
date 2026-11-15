#pragma once

#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/main_screen.hpp"
#include "screens/widgets/skeleton.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/widgets/grid.hpp"

namespace karafin::screens {

// "Libraries" tab: the user's media libraries as large tiles, plus Favorites.
class LibrariesSection : public SectionView {
public:
    LibrariesSection();
    ~LibrariesSection() override;

    void onSectionShown(bool first) override;
    float headerScrim() const override;

private:
    void load();

    ui::Grid* grid_ = nullptr;
    SkeletonGrid* skeleton_ = nullptr;
    StatusView* status_ = nullptr;
    std::vector<jellyfin::BaseItem> libs_;
    CancelToken token_;
};

}  // namespace karafin::screens
