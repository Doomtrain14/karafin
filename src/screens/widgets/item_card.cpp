#include "screens/widgets/item_card.hpp"

#include <cmath>

#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "ui/actions.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;

namespace {
std::string typeLabel(const jellyfin::BaseItem& item) {
    if (item.type == "BoxSet") return "Collection";
    if (item.type == "MusicVideo") return "Music video";
    if (item.type == "CollectionFolder" || item.type == "UserView") return "Library";
    return item.type;
}
}  // namespace

ItemCard::ItemCard(CardShape shape, bool showTitles) : PosterCard(shape, showTitles) {
    onActivate = [this] {
        if (onOpen) onOpen(item_);
        else openItem(item_);
    };
    actions = [this](ActionMenu& m) {
        if (loading_ || item_.id.empty()) return;
        m.setTitle(cardTitle(item_), typeLabel(item_));
        if (item_.isPlayable() || item_.isSeries()) {
            m.section("Playback");
            if (item_.isPlayable()) m.add("Play", gfx::Icon::Play, guard([this] { playItem(item_); }));
            else m.add("Play next episode", gfx::Icon::Play, guard([this] { playSeries(item_.id, token_); }));
        }
        m.section("Title");
        const bool folder = item_.isFolder && !item_.isSeries() && !item_.isSeason();
        m.add(folder ? "Open" : "Details", gfx::Icon::Info, guard([this] { onActivate(); }));
        if (item_.isPlayable() || item_.isSeries() || item_.isSeason())
            m.add(item_.userData.played ? "Mark as unwatched" : "Mark as watched",
                  item_.userData.played ? gfx::Icon::Close : gfx::Icon::Check, guard([this] { toggleWatched(); }));
        if (extraActions) extraActions(m);
    };
}

ItemCard::~ItemCard() { token_.cancel(); }

void ItemCard::setLoading() {
    loading_ = true;
    item_ = {};
    setTitle({});
    setSubtitle({});
    setProgress(-1.0f);
    setWatched(false);
    setBadge({});
    imageView()->clear();
    imageUrl_.clear();
    setPlaceholder(theme::color::skeleton, gfx::Icon::None);
    setHints({});
}

void ItemCard::update(float dt) {
    if (loading_) phase_ = std::fmod(phase_ + dt / 1.4f, 1.0f);
    PosterCard::update(dt);
}

void ItemCard::drawOverlay(Renderer& r) {
    if (!loading_) {
        PosterCard::drawOverlay(r);
        return;
    }
    const Rect art = artRect();
    r.fillShimmer(art, theme::card::radius, theme::color::skeleton, phase_);
    r.fillShimmer({art.x + 2, art.bottom() + 13, art.w * 0.7f, 11}, 5, theme::color::skeleton, phase_);
    PosterCard::drawOverlay(r);  // focus ring
}

void ItemCard::setItem(const jellyfin::BaseItem& item) {
    loading_ = false;
    item_ = item;
    bindCard(*this, item_, &imageUrl_);
}

void ItemCard::toggleWatched() {
    auto client = jellyfin::currentClient();
    if (!client || watchedPendingId_ == item_.id) return;
    const bool played = !item_.userData.played;
    watchedPendingId_ = item_.id;
    client->setPlayed(item_.id, played).then(
        [this, played, id = item_.id](jellyfin::Result<jellyfin::UserData> r) {
            if (watchedPendingId_ == id) watchedPendingId_.clear();
            if (!r) {
                toast(errorMessage(r.error), ToastKind::Error, gfx::Icon::Warning);
                return;
            }
            toast(played ? "Marked as watched" : "Marked as unwatched", ToastKind::Success, gfx::Icon::Check, 1.8f);
            if (item_.id != id) return;  // recycled grid cell
            jellyfin::BaseItem updated = item_;
            updated.userData.played = r->played;
            updated.userData.playbackPositionTicks = r->playbackPositionTicks;
            updated.userData.playedPercentage = r->playedPercentage;
            updated.userData.unplayedItemCount = r->unplayedItemCount;
            setItem(updated);
        },
        token_);
}

bool updateCardsInPlace(View* row, const std::vector<jellyfin::BaseItem>& items) {
    auto* rc = dynamic_cast<RowCarousel*>(row);
    if (!rc || rc->itemCount() != items.size()) return false;
    for (size_t i = 0; i < items.size(); ++i) {
        auto* card = dynamic_cast<ItemCard*>(rc->itemAt(i));
        if (!card || card->item().id != items[i].id) return false;
    }
    for (size_t i = 0; i < items.size(); ++i) static_cast<ItemCard*>(rc->itemAt(i))->setItem(items[i]);
    return true;
}

}  // namespace karafin::screens
