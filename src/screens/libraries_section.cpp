#include "screens/libraries_section.hpp"

#include <memory>

#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/library_screen.hpp"
#include "screens/widgets/item_card.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr const char* kFavoritesId = "karafin:favorites";

bool listed(const jf::BaseItem& view) {
    const std::string& c = view.collectionType;
    return c != "livetv" && c != "music" && c != "books" && c != "photos";
}
}  // namespace

LibrariesSection::LibrariesSection() {
    const Vec2 cell = PosterCard(CardShape::Wide).measure({});
    const Vec2 cellSize{cell.x * 1.2f, cell.y * 1.2f};
    skeleton_ = add<SkeletonGrid>(cellSize, Vec2{cellSize.x, cellSize.x * 9.0f / 16.0f}, 26, 26);
    skeleton_->setFrame({0, kHeaderHeight + 8, theme::layout::screenWidth, theme::layout::screenHeight - kHeaderHeight - 8});
    skeleton_->setVisible(false);
    grid_ = add<Grid>(cellSize, 26, 26);
    grid_->setFrame({0, kHeaderHeight + 8, theme::layout::screenWidth, theme::layout::screenHeight - kHeaderHeight - 8});
    grid_->count = [this] { return libs_.size(); };
    grid_->createCell = [] {
        auto card = std::make_unique<ItemCard>(CardShape::Wide);
        card->setPreferredSize(theme::card::wideWidth * 1.2f, -1);
        return card;
    };
    grid_->bindCell = [this](View& v, size_t i) {
        auto& card = static_cast<ItemCard&>(v);
        const jf::BaseItem& lib = libs_[i];
        card.setItem(lib);
        if (lib.id == kFavoritesId) {
            card.setSubtitle("Movies and shows you love");
            card.setPlaceholder(Color::hex(0x6B2F45), gfx::Icon::Heart);
            card.onOpen = [](const jf::BaseItem&) {
                LibrarySpec spec;
                spec.title = "Favorites";
                spec.includeTypes = "Movie,Series,Episode";
                spec.filters = "IsFavorite";
                nav().push(std::make_unique<LibraryScreen>(std::move(spec)));
            };
        } else {
            card.setSubtitle({});
            card.onOpen = nullptr;
        }
    };
    status_ = add<StatusView>();
    status_->setFrame({0, kHeaderHeight, theme::layout::screenWidth, theme::layout::screenHeight - kHeaderHeight - 60});
    load();
}

LibrariesSection::~LibrariesSection() { token_.cancel(); }

void LibrariesSection::load() {
    token_.cancel();
    token_.reset();
    auto client = jf::currentClient();
    if (!client) return;
    grid_->setVisible(false);
    status_->hide();
    skeleton_->setVisible(true);
    client->userViews().then(
        [this](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
            skeleton_->setVisible(false);
            if (!r) {
                status_->showError(errorMessage(r.error), [this] { load(); });
                if (visibleInTree() && !hasFocusWithin()) status_->focusAction();
                return;
            }
            libs_.clear();
            for (auto& v : r->items)
                if (listed(v)) libs_.push_back(v);
            jf::BaseItem fav;
            fav.id = kFavoritesId;
            fav.name = "Favorites";
            fav.type = "Folder";
            fav.isFolder = true;
            libs_.push_back(fav);
            status_->hide();
            grid_->setVisible(true);
            grid_->alpha.set(0.0f);
            grid_->alpha.tweenTo(1.0f, theme::motion::slow, ease::outCubic);
            grid_->reload();
        },
        token_);
}

void LibrariesSection::onSectionShown(bool first) {
    if (!first && !grid_->visible() && !status_->showing() && !skeleton_->visible()) load();
}

float LibrariesSection::headerScrim() const { return 1.0f; }

}  // namespace karafin::screens
