#pragma once

#include <vxe/ui/ui_context.hpp>

#include <functional>
#include <string>
#include <vector>

namespace vxe::ui {

struct DialogButton {
    std::string text{};
    std::function<void()> onClick{}; // Runs after the dialog closed.
    // Back (Escape) presses this button; it also gets the focus first, so a
    // stray Enter never confirms something destructive.
    bool cancel = false;
};

struct DialogDesc {
    std::string title{};
    std::string message{}; // Wraps to the dialog's width.
    std::vector<DialogButton> buttons{};
    f32 width = 240.0F; // UI pixels.
};

// Opens a modal dialog (Section 19.1's ModalDialog): a panel centered over
// a dimmed screen with a title, a message and a row of buttons. Every button
// closes the dialog and then runs its action. Returns the dialog's layer.
Widget& showDialog(UiContext& context, DialogDesc desc);

} // namespace vxe::ui
