#pragma once

// ui module umbrella header. See src/ui/README.md for the API overview.
//
// Depends on: core, gfx. Main thread only; background results arrive via main_thread::post
// (wrap callbacks with View::guard so they are dropped once the view is gone).

#include "ui/anim.hpp"
#include "ui/focus.hpp"
#include "ui/hint_bar.hpp"
#include "ui/input.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/root.hpp"
#include "ui/screen.hpp"
#include "ui/scroll.hpp"
#include "ui/theme.hpp"
#include "ui/toast.hpp"
#include "ui/view.hpp"
#include "ui/widgets/backdrop.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/grid.hpp"
#include "ui/widgets/image_view.hpp"
#include "ui/widgets/label.hpp"
#include "ui/widgets/list_menu.hpp"
#include "ui/widgets/poster_card.hpp"
#include "ui/widgets/progress.hpp"
#include "ui/widgets/row_carousel.hpp"
#include "ui/widgets/tab_bar.hpp"
