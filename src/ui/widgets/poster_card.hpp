#pragma once

#include <string>

#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::ui {

enum class CardShape : uint8_t {
    Poster,  // 2:3 (movies, series)
    Wide,    // 16:9 (episodes, continue watching, libraries)
    Square,  // 1:1 (music, avatars)
};

// Media card: artwork with focus scale-up, glow + shadow, white focus ring and a subtle shine;
// optional progress bar, watched check, count/text badge, and title/subtitle under the art.
//
//   auto* card = row->items()->add<PosterCard>(CardShape::Poster);
//   card->setTitle(item.name);
//   card->setSubtitle("2024");
//   card->setImageUrl(url, headers);
//   card->setProgress(0.35f);
//   card->onActivate = [=] { nav().push(...); };
class PosterCard : public View {
public:
    explicit PosterCard(CardShape shape = CardShape::Poster, bool showTitles = true);

    static Vec2 artSize(CardShape shape);

    void setImageUrl(std::string url, http::Headers headers = {});
    void setImageRequest(gfx::ImageRequest request);
    void setImage(gfx::ImagePtr image);
    ImageView* imageView() const { return image_; }

    void setTitle(std::string title);
    void setSubtitle(std::string subtitle);
    // 0..1 shows a progress bar; negative hides it.
    void setProgress(float progress);
    void setWatched(bool watched) { watched_ = watched; }
    // Top-right pill ("12" unplayed episodes, "NEW", ...). Empty hides it.
    void setBadge(std::string badge) { badge_ = std::move(badge); }
    // Color for the placeholder art (when there is no image / it failed).
    void setPlaceholder(Color color, gfx::Icon icon = gfx::Icon::Movie);

    CardShape shape() const { return shape_; }
    Rect artRect() const;

    Vec2 measure(Vec2 available) override;
    void layout() override;
    void onFocus() override;
    void onBlur() override;
    bool activate() override;
    void onTouchDown() override;
    void onTouchCancel() override;
    void update(float dt) override;
    void draw(Renderer& r) override;
    void drawOverlay(Renderer& r) override;

private:
    CardShape shape_;
    bool showTitles_;
    ImageView* image_ = nullptr;
    Label* title_ = nullptr;
    Label* subtitle_ = nullptr;
    std::string titleText_;
    std::string badge_;
    Color placeholder_ = theme::color::surfaceHigh;
    gfx::Icon placeholderIcon_ = gfx::Icon::Movie;
    Animated<float> focus_{0.0f};
    Animated<float> progress_{-1.0f};
    bool watched_ = false;
};

}  // namespace karafin::ui
