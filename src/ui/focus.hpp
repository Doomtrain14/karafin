#pragma once

// Focus engine: one focused view per app, limited to the current scope (top modal or top screen).
//
// Directional moves: the focused view's ancestors get first say through View::navigateFocus
// (rows move by index, grids by column, ...); otherwise a spatial search picks the nearest
// focusable view in that direction within the scope. Entering a container that remembers focus
// (View::setRememberFocus) restores its last focused descendant.

#include <cstdint>
#include <string>
#include <vector>

#include "ui/view.hpp"

namespace karafin::ui {

class FocusManager {
public:
    View* focused() const { return focused_.get(); }
    View* scope() const { return scope_.get(); }

    // Focuses `v`, or v->defaultFocus() if `v` itself cannot take focus. Null blurs.
    void setFocus(View* v);
    // Moves focus in `dir`. Returns true if focus changed (or a container consumed the move).
    bool navigate(NavDir dir);
    // Changes the scope; focus moves inside it (restoring the scope's remembered focus).
    void setScope(View* scope);
    // Called once per frame: re-homes focus if the focused view died, hid, or left the scope.
    void validate();
    void clear();

    // Nearest focusable view from `from` in `dir` among focusable descendants of `scope`.
    static View* findInDirection(View* scope, View* from, NavDir dir);

private:
    View* resolve(View* v) const;
    View* redirectToMemory(View* target, View* from) const;
    void apply(View* target);
    // Focusable views under `scope` in tree order, reused until View::treeGeneration_ moves.
    const std::vector<View*>& focusables(View* scope);

    Ref<View> focused_;
    Ref<View> scope_;
    std::vector<View*> focusables_;
    const View* focusablesScope_ = nullptr;
    uint64_t focusablesGeneration_ = 0;
    // focusId of the last view apply() focused. Kept after that view is destroyed so a rebuild
    // can hand focus back without replaying the gain animation.
    std::string lastFocusId_;
};

}  // namespace karafin::ui
