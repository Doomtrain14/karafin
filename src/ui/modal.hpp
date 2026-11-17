#pragma once

// Modal layer above the screen stack (dialogs, side sheets, menus). The top modal is the focus
// scope; focus returns to the screen's remembered view when it closes.
//
//   ui::present(std::make_unique<MyModal>());
//   ...
//   dismiss();   // animates out, then the root destroys it
//
// Subclasses lay out their content inside the full-screen modal frame and use appear() (0..1,
// spring-driven) to animate it. The root draws the dim backdrop (dim() * appear()).

#include "ui/view.hpp"

namespace karafin::ui {

class Modal : public View {
public:
    Modal();

    void dismiss();
    bool dismissing() const { return dismissing_; }
    // Dismissed and fully faded out: the root removes it.
    bool finished() const { return dismissing_ && appear_.value() <= 0.01f; }
    float appear() const { return appear_.value(); }

    void setDim(float a) { dim_ = a; }
    float dim() const { return dim_; }
    void setDismissOnBack(bool v) { dismissOnBack_ = v; }
    bool dismissOnBack() const { return dismissOnBack_; }
    void setDismissOnBackdropTap(bool v) { dismissOnBackdropTap_ = v; }

    std::function<void()> onDismissed;  // after removal

    virtual void onPresented() {}
    bool onInput(const InputEvent& e) override;
    bool onTap(Vec2 local) override;
    void update(float dt) override;

protected:
    Animated<float> appear_{0.0f};

private:
    friend class Root;
    float dim_ = 0.6f;
    bool dismissing_ = false;
    bool dismissOnBack_ = true;
    bool dismissOnBackdropTap_ = true;
};

}  // namespace karafin::ui
