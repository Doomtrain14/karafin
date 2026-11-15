#include "screens/widgets/item_card.hpp"

#include <cmath>

#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "ui/root.hpp"

namespace karafin::screens {

using namespace karafin::ui;

ItemCard::ItemCard(CardShape shape, bool showTitles) : PosterCard(shape, showTitles) {
    onActivate = [this] {
        if (onOpen) onOpen(item_);
        else openItem(item_);
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
    std::vector<Hint> hints;
    if (item_.isPlayable() || item_.isSeries()) hints.push_back({Key::X, "Play"});
    if (item_.isPlayable() || item_.isSeries() || item_.isSeason()) hints.push_back({Key::Y, item_.userData.played ? "Unwatched" : "Watched"});
    setHints(std::move(hints));
}

bool ItemCard::onInput(const InputEvent& e) {
    if (onKey && onKey(e)) return true;
    if (e.pressed() && e.button == Key::X) {
        if (item_.isPlayable()) {
            playItem(item_);
            return true;
        }
        if (item_.isSeries()) {
            playSeries(item_.id, token_);
            return true;
        }
    }
    if (e.pressed() && e.button == Key::Y && (item_.isPlayable() || item_.isSeries() || item_.isSeason())) {
        toggleWatched();
        return true;
    }
    return PosterCard::onInput(e);
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
