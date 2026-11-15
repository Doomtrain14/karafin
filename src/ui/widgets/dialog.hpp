#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gfx/icons.hpp"
#include "ui/layout.hpp"
#include "ui/modal.hpp"
#include "ui/widgets/button.hpp"
#include "ui/widgets/label.hpp"

namespace karafin::ui {

struct DialogButton {
    std::string label;
    ButtonStyle style = ButtonStyle::Secondary;
    std::function<void()> onPress;
};

struct DialogSpec {
    std::string title;
    std::string message;
    gfx::Icon icon = gfx::Icon::None;
    Color iconColor = theme::color::accent;
    std::vector<DialogButton> buttons;
    int cancelIndex = -1;   // button triggered by B (-1: B just closes)
    int defaultIndex = 0;   // initially focused button
};

// Centered card with icon, title, wrapped message and a row of buttons. Pressing a button runs
// its callback and closes the dialog.
//
//   ui::present(Dialog::confirm("Delete download?", "This removes 4.2 GB.", "Delete", "Cancel",
//                               [this](bool ok) { if (ok) remove(); }));
class Dialog : public Modal {
public:
    explicit Dialog(DialogSpec spec);

    static std::unique_ptr<Dialog> confirm(std::string title, std::string message, std::string confirmLabel,
                                           std::string cancelLabel, std::function<void(bool)> onResult);
    static std::unique_ptr<Dialog> alert(std::string title, std::string message, std::string okLabel = "OK",
                                         std::function<void()> onClose = {});

    bool onInput(const InputEvent& e) override;
    void layout() override;
    void update(float dt) override;
    void draw(Renderer& r) override;

private:
    void press(size_t index);

    DialogSpec spec_;
    Box* card_ = nullptr;
    std::vector<Button*> buttons_;
    bool resolved_ = false;
};

}  // namespace karafin::ui
