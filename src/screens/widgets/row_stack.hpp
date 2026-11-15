#pragma once

#include <map>
#include <memory>

#include "ui/layout.hpp"

namespace karafin::screens {

// Keeps rows (carousels, skeletons, ...) in a Box ordered by an integer key, independent of the
// order in which async results arrive. Replacing a row that holds focus keeps focus on the same
// card index; removing it moves focus to the nearest remaining row. Rows that replace a
// placeholder (or appear for the first time) fade in.
class RowStack {
public:
    // Rows are inserted after the first `leading` children of `box` (e.g. a hero or header).
    RowStack(ui::Box* box, size_t leading) : box_(box), leading_(leading) {}

    ui::View* place(int order, std::unique_ptr<ui::View> row);
    void remove(int order);
    void clear();
    ui::View* get(int order) const;
    bool has(int order) const { return rows_.count(order) > 0; }
    const std::map<int, ui::View*>& rows() const { return rows_; }

private:
    void focusNear(int order, size_t index);

    ui::Box* box_;
    size_t leading_;
    std::map<int, ui::View*> rows_;
};

}  // namespace karafin::screens
