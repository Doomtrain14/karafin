#include "screens/search_section.hpp"

#include <algorithm>
#include <format>

#include "core/settings.hpp"
#include "gfx/text.hpp"
#include "jellyfin/client.hpp"
#include "screens/common.hpp"
#include "screens/library_lock.hpp"
#include "screens/keyboard.hpp"
#include "screens/widgets/error_row.hpp"
#include "screens/widgets/item_card.hpp"
#include "screens/widgets/skeleton.hpp"
#include "ui/actions.hpp"
#include "ui/root.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/row_carousel.hpp"

namespace karafin::screens {

using namespace karafin::ui;
namespace jf = karafin::jellyfin;

namespace {
constexpr float kFieldW = 760;
constexpr float kFieldH = 56;
constexpr float kFieldY = 104;
constexpr float kPageY = 176;
constexpr size_t kMaxRecent = 8;
constexpr const char* kRecentKey = "ui.recent_searches";
constexpr int kWarningOrder = 5;  // "some results couldn't be loaded" row, above the groups

struct Group {
    int order;
    const char* title;
    const char* types;
    CardShape shape;
};
constexpr Group kGroups[] = {
    {10, "Movies", "Movie", CardShape::Poster},
    {20, "Shows", "Series", CardShape::Poster},
    {30, "Episodes", "Episode", CardShape::Wide},
    {40, "Collections", "BoxSet", CardShape::Poster},
};
}  // namespace

// Rounded search box; A opens the keyboard.
class SearchSection::Field : public View {
public:
    Field() {
        setFocusable(true);
        setHints({{Key::A, "Search"}});
    }
    std::string text;

    void onFocus() override {
        focus_.springTo(1.0f, theme::motion::snappy);
        scale.springTo(1.02f, theme::motion::focus);
    }
    void onBlur() override {
        focus_.springTo(0.0f, theme::motion::snappy);
        scale.springTo(1.0f, theme::motion::focus);
    }
    void update(float dt) override {
        focus_.update(dt);
        caret_ = std::fmod(caret_ + dt, 1.0f);
        View::update(dt);
    }
    void draw(Renderer& r) override {
        using namespace theme;
        const float f = std::clamp(focus_.value(), 0.0f, 1.0f);
        const Rect b = bounds();
        const float rad = b.h * 0.5f;
        r.drawShadow(b, rad, 18 + 14 * f, Color::black(0.35f + 0.2f * f), {0, 6});
        r.fillRoundRect(b, rad, Color::lerp(Color::hex(0x1E1E29), Color::hex(0x2A2A37), f));
        r.strokeRoundRect(b, rad, 1.5f, Color::lerp(color::stroke, color::accentBright, f));
        gfx::drawIcon(r, gfx::Icon::Search, {24, (b.h - 22) * 0.5f, 22, 22},
                      Color::lerp(color::textSecondary, color::textPrimary, f));
        const bool empty = text.empty();
        const std::string& shown = empty ? kPlaceholder : text;
        if (!layout_ || shown != layoutText_ || b.w != layoutWidth_) {
            layout_ = gfx::text::layout(shown, type::title3, {.maxWidth = b.w - 120});
            layoutText_ = shown;
            layoutWidth_ = b.w;
        }
        if (layout_)
            r.drawText(*layout_, {62, (b.h - layout_->height) * 0.5f}, empty ? color::textTertiary : color::textPrimary);
        if (f > 0.5f && caret_ < 0.55f) {
            const float cx = 62 + (empty || !layout_ ? 0.0f : layout_->width + 3);
            r.fillRect({cx, b.h * 0.5f - 12, 2, 24}, color::accentBright);
        }
    }

private:
    inline static const std::string kPlaceholder = "Search movies, shows and episodes";
    Animated<float> focus_{0.0f};
    float caret_ = 0;
    gfx::TextLayoutPtr layout_;
    std::string layoutText_;
    float layoutWidth_ = -1;
};

SearchSection::SearchSection() {
    field_ = add<Field>();
    field_->setFrame({(theme::layout::screenWidth - kFieldW) * 0.5f, kFieldY, kFieldW, kFieldH});
    field_->onActivate = [this] { openKeyboard(); };

    recent_ = add<Box>(Axis::Horizontal);
    recent_->setSpacing(10).setAlign(Align::Center);
    recent_->setRememberFocus(true);

    page_ = add<ScrollView>(Axis::Vertical);
    page_->setFrame({0, kPageY, theme::layout::screenWidth, theme::layout::screenHeight - kPageY});
    page_->setRevealWholeChild(true);
    page_->setRevealInsets(20, 64);
    page_->setShowsIndicator(true);
    // Scrolled rows must not slide over the search field.
    page_->setClipsChildren(true);
    page_->content()->setSpacing(theme::layout::rowSpacing);
    page_->content()->setPadding({0, 8, 0, 70});
    rows_ = std::make_unique<RowStack>(page_->content(), 0);

    status_ = add<StatusView>();
    status_->setFrame({0, kPageY + 60, theme::layout::screenWidth, theme::layout::screenHeight - kPageY - 120});
    setRememberedFocus(field_);
    actions = [this](ActionMenu& m) {
        m.section("Search");
        m.add("Search", gfx::Icon::Search, guard([this] { openKeyboard(); }), query_);
        if (!query_.empty()) m.add("Clear search", gfx::Icon::Close, guard([this] { setQuery({}); }));
    };
    showIdle();
}

SearchSection::~SearchSection() { token_.cancel(); }

void SearchSection::onSectionShown(bool first) {
    if (first) focus().setFocus(field_);
    else if (!query_.empty() && lockRevision_ != LibraryLock::instance().revision()) {
        showSkeletons();
        run();
    }
}

void SearchSection::openKeyboard() {
    KeyboardOptions opts;
    opts.header = "Search";
    opts.guide = "Movie, show or episode title";
    opts.initial = query_;
    opts.okLabel = "Search";
    opts.maxLength = 100;
    opts.allowEmpty = true;
    requestText(std::move(opts), guard([this](std::string text) { setQuery(std::move(text)); }));
}

void SearchSection::setQuery(std::string query) {
    // Trim.
    while (!query.empty() && (query.back() == ' ' || query.back() == '\n')) query.pop_back();
    size_t start = 0;
    while (start < query.size() && query[start] == ' ') ++start;
    query.erase(0, start);
    if (query == query_ && (pending_ > 0 || !rows_->rows().empty())) return;
    query_ = query;
    field_->text = query_;
    if (debounce_) timeline().cancel(debounce_);
    debounce_ = 0;
    if (query_.empty()) {
        showIdle();
        return;
    }
    token_.cancel();
    token_.reset();
    if (recent_->hasFocusWithin()) focus().setFocus(field_);
    recent_->setVisible(false);
    showSkeletons();
    debounce_ = timeline().after(0.3f, [this] {
        debounce_ = 0;
        run();
    }, lifetime());
}

void SearchSection::showSkeletons() {
    if (status_->hasFocusWithin()) focus().setFocus(field_);
    status_->hide();
    rows_->clear();
    rows_->place(10, std::make_unique<SkeletonRow>(CardShape::Poster, 8));
    rows_->place(20, std::make_unique<SkeletonRow>(CardShape::Poster, 8));
}

void SearchSection::run() {
    auto client = jf::currentClient();
    if (!client || query_.empty()) return;
    token_.cancel();
    token_.reset();
    if (status_->showing()) showSkeletons();  // retry after an error
    remember(query_);
    pending_ = static_cast<int>(std::size(kGroups));
    found_ = 0;
    failed_ = 0;
    lockRevision_ = LibraryLock::instance().revision();
    // Titles from locked libraries are left out, so their list has to be known first.
    LibraryLock::instance().prepare([this, tok = token_] {
        if (!tok.cancelled()) runQueries();
    });
}

void SearchSection::runQueries() {
    auto client = jf::currentClient();
    if (!client) return;
    lockRevision_ = LibraryLock::instance().revision();
    const std::string query = query_;
    for (const Group& g : kGroups) {
        jf::ItemsQuery q;
        q.searchTerm(query).includeItemTypes(g.types).recursive().limit(24).sortBy("SortName");
        const int order = g.order;
        const std::string title = g.title;
        const CardShape shape = g.shape;
        client->items(q).then(
            [this, order, title, shape, query](jf::Result<jf::QueryResult<jf::BaseItem>> r) {
                --pending_;
                bool filtered = false;
                const auto items = r ? LibraryLock::instance().filter(r->items, &filtered) : std::vector<jf::BaseItem>();
                if (!items.empty()) {
                    found_ += static_cast<int>(items.size());
                    const std::string heading = !filtered && r->totalRecordCount > static_cast<int>(items.size())
                                                    ? std::format("{} ({})", title, formatCount(r->totalRecordCount))
                                                    : title;
                    auto row = std::make_unique<RowCarousel>(heading);
                    for (const auto& it : items) row->items()->add<ItemCard>(shape)->setItem(it);
                    rows_->place(order, std::move(row));
                } else {
                    rows_->remove(order);
                    if (!r) {
                        ++failed_;
                        lastError_ = r.error;
                    }
                }
                if (pending_ > 0) return;
                if (found_ == 0 && failed_ > 0) {
                    const bool hadFocus = page_->hasFocusWithin();
                    rows_->clear();
                    status_->showError(errorMessage(lastError_), [this] { run(); }, {}, "Search failed");
                    if (hadFocus) status_->focusAction();
                    return;
                }
                if (found_ == 0) {
                    const bool hadFocus = page_->hasFocusWithin();
                    rows_->clear();
                    status_->showEmpty(gfx::Icon::Search, "No results",
                                       std::format("Nothing matches \u201C{}\u201D. Try a different spelling.", query),
                                       {{"Search again", gfx::Icon::Search, [this] { openKeyboard(); }, true}});
                    if (hadFocus) status_->focusAction();
                    return;
                }
                if (failed_ > 0) {
                    // Partial results: keep them and offer to re-run the same search above them.
                    std::string message = std::format("Some results couldn't be loaded. {}", errorMessage(lastError_));
                    if (auto* er = dynamic_cast<ErrorRow*>(rows_->get(kWarningOrder))) {
                        er->setMessage(std::move(message));
                        er->setBusy(false);
                    } else {
                        rows_->place(kWarningOrder, std::make_unique<ErrorRow>("", std::move(message), [this] { run(); }));
                    }
                } else {
                    rows_->remove(kWarningOrder);
                }
            },
            token_);
    }
}

void SearchSection::showIdle() {
    token_.cancel();
    token_.reset();
    rows_->clear();
    pending_ = 0;
    buildRecent();
    status_->showEmpty(gfx::Icon::Search, "Find something to watch",
                       "Press A on the search box or Y anywhere to type. Results update as soon as you confirm.");
}

void SearchSection::buildRecent() {
    recent_->clearChildren();
    std::vector<std::string> recent;
    const auto j = Settings::instance().getJson(kRecentKey);
    if (j.is_array())
        for (const auto& s : j)
            if (s.is_string()) recent.push_back(s.get<std::string>());
    if (recent.empty()) {
        recent_->setVisible(false);
        return;
    }
    recent_->add<Label>("Recent", theme::type::caption, theme::color::textTertiary)->setMargin({0, 0, 6, 0});
    float w = 60;
    for (const auto& q : recent) {
        auto* b = recent_->add<Button>(q, gfx::Icon::Clock, ButtonStyle::Ghost);
        const float bw = b->measure({1000, 100}).x;
        if (w + bw > kFieldW + 200) {
            recent_->removeChild(b);
            break;
        }
        w += bw + 10;
        b->onActivate = [this, q] { timeline().after(0.0f, [this, q] { setQuery(q); }, lifetime()); };
    }
    const Vec2 m = recent_->measure({theme::layout::screenWidth, 60});
    recent_->setFrame({(theme::layout::screenWidth - m.x) * 0.5f, kFieldY + kFieldH + 14, m.x, theme::button::height});
    recent_->setVisible(true);
}

void SearchSection::remember(const std::string& query) {
    std::vector<std::string> recent;
    const auto j = Settings::instance().getJson(kRecentKey);
    if (j.is_array())
        for (const auto& s : j)
            if (s.is_string() && s.get<std::string>() != query) recent.push_back(s.get<std::string>());
    recent.insert(recent.begin(), query);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
    Settings::instance().set(kRecentKey, recent);
}

}  // namespace karafin::screens
