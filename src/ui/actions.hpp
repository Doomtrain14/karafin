#pragma once

// Contextual actions (X) collected from the focus chain and shown in one grouped side sheet.
//
//   card->actions = [this](ui::ActionMenu& m) {
//       m.setTitle(name, "Movie");
//       m.section("Playback").add("Play", gfx::Icon::Play, [=] { play(); });
//   };
//
// ui::Root walks from the focused view up to the screen; inner views add their groups first, so a
// poster's actions come before the page's ("This page"). Groups with the same heading merge.
// Selecting an action closes the sheet first and runs the callback on the next frame, so callbacks
// may rebuild lists, navigate or present another modal safely.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/icons.hpp"
#include "gfx/renderer.hpp"
#include "ui/input.hpp"

namespace karafin::ui {

class Modal;

struct Action {
    std::string title;
    gfx::Icon icon = gfx::Icon::None;
    std::function<void()> run;
    std::string detail;    // right-aligned current value ("Recently added")
    std::string subtitle;  // second line
    bool enabled = true;
    bool destructive = false;
    bool checked = false;  // current choice (gets the check mark and initial focus)
};

class ActionMenu {
public:
    struct Group {
        std::string heading;
        std::vector<Action> actions;
    };

    // The first caller wins (the innermost view describes the selection).
    void setTitle(std::string title, std::string subtitle = {});
    const std::string& title() const { return title_; }
    const std::string& subtitle() const { return subtitle_; }
    // Drawn instead of the title text (SideSheet::setTitleArt), e.g. the app wordmark.
    using TitleArt = std::function<void(gfx::Renderer&, const gfx::Rect&)>;
    void setTitleArt(float height, TitleArt art) {
        titleArtHeight_ = height;
        titleArt_ = std::move(art);
    }
    float titleArtHeight() const { return titleArtHeight_; }
    const TitleArt& titleArt() const { return titleArt_; }

    // Selects (creating if needed) the group that following add() calls append to.
    ActionMenu& section(const std::string& heading);
    ActionMenu& add(std::string title, gfx::Icon icon, std::function<void()> run, std::string detail = {});
    ActionMenu& add(Action action);

    bool empty() const;
    const std::vector<Group>& groups() const { return groups_; }

private:
    std::string title_;
    std::string subtitle_;
    float titleArtHeight_ = 0;
    TitleArt titleArt_;
    std::vector<Group> groups_;
    size_t current_ = 0;
};

// Presents `menu` as a side sheet; pressing `toggleKey` again (X / +) or B closes it.
// Returns false (and presents nothing) for an empty menu.
bool presentActions(ActionMenu menu, Key toggleKey = Key::X);

}  // namespace karafin::ui
