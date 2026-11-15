#pragma once

#include <cstdint>
#include <vector>

#include "core/cancel.hpp"
#include "jellyfin/models.hpp"
#include "screens/main_screen.hpp"
#include "screens/widgets/item_card.hpp"
#include "screens/widgets/skeleton.hpp"
#include "screens/widgets/status_view.hpp"
#include "ui/widgets/grid.hpp"

namespace karafin::screens {

// "Libraries" tab: the user's media libraries as large tiles, plus Favorites. X locks / unlocks
// a library (LibraryLock); Y asks the server to scan it (administrators only).
class LibrariesSection : public SectionView {
public:
    LibrariesSection();
    ~LibrariesSection() override;

    void onSectionShown(bool first) override;
    float headerScrim() const override;

private:
    void load();
    void bindLibrary(ItemCard& card, const jellyfin::BaseItem& lib);
    // Rebinds the tiles after the lock state changed.
    void refreshCards();

    ui::Grid* grid_ = nullptr;
    SkeletonGrid* skeleton_ = nullptr;
    StatusView* status_ = nullptr;
    std::vector<jellyfin::BaseItem> libs_;
    CancelToken token_;
    uint64_t lockRevision_ = 0;
};

}  // namespace karafin::screens
