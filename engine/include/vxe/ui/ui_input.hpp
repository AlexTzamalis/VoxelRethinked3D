#pragma once

#include <vxe/core/types.hpp>
#include <vxe/input/input.hpp>

#include <string_view>

namespace vxe::ui {

// The pointer as the window sees it this frame.
struct PointerInput {
    f32 x = 0.0F; // Window pixels from the top left.
    f32 y = 0.0F;
    bool primaryDown = false; // The left mouse button (later a gamepad's confirm).
    bool available = true;    // False while the game captures the mouse for looking around.
};

// Keyboard navigation of one frame: true on the frame the action fired.
struct NavigationInput {
    bool next = false;     // Tab: the next focusable widget.
    bool previous = false; // Shift+Tab.
    bool up = false;       // Arrows: the nearest focusable widget that way.
    bool down = false;
    bool left = false;
    bool right = false;
    bool activate = false; // Enter: press the focused widget.
    bool back = false;     // Escape: close a dialog, or leave the screen.
};

// Text editing of one frame: typed text plus the editing keys with the
// operating system's key repeat, so holding Backspace keeps deleting. These
// keys are fixed (like in every text field), not rebindable actions.
struct TextEditInput {
    std::string_view typed{};
    bool erase = false;     // Backspace.
    bool eraseNext = false; // Delete.
    bool caretLeft = false;
    bool caretRight = false;
    bool home = false;
    bool end = false;
};

// Everything the UI reads in a frame.
struct UiInput {
    PointerInput pointer{};
    NavigationInput navigation{};
    TextEditInput text{};
    f32 deltaSeconds = 0.0F; // For tooltip delays and the caret blink.
};

// The menu navigation actions (Section 17.5's Interface category), registered
// in the caller's menu context so they are rebindable like every action:
// ui.next (Tab), ui.previous (Shift+Tab), ui.up, ui.down, ui.left, ui.right
// (arrows), ui.activate (Enter, keypad Enter) and ui.back (Escape).
//
// Thread safety: main thread, like the input system.
class UiActions {
public:
    UiActions(input::InputSystem& input, input::InputContextId menuContext);

    // This frame's navigation and text editing from the input system.
    [[nodiscard]] NavigationInput navigation(const input::InputSystem& input) const;
    [[nodiscard]] static TextEditInput textEditing(const input::InputSystem& input);

    input::ActionId next;
    input::ActionId previous;
    input::ActionId up;
    input::ActionId down;
    input::ActionId left;
    input::ActionId right;
    input::ActionId activate;
    input::ActionId back;
};

} // namespace vxe::ui
