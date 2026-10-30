#include "vxe/ui/ui_input.hpp"

namespace vxe::ui {
namespace {

using input::Binding;
namespace scancode = input::scancode;

input::ActionId registerNavigation(input::InputSystem& input, input::InputContextId context,
                                   std::string_view id, std::string_view name, Binding primary,
                                   Binding secondary = {}) {
    return input.registerAction({.id = id,
                                 .displayName = name,
                                 .category = "Interface",
                                 .context = context,
                                 .defaults = {primary, secondary, Binding{}}});
}

} // namespace

UiActions::UiActions(input::InputSystem& input, input::InputContextId menuContext)
    : next(registerNavigation(input, menuContext, "ui.next", "Next control", Binding::key(scancode::kTab)))
    , previous(registerNavigation(input, menuContext, "ui.previous", "Previous control",
                                  Binding::key(scancode::kTab, platform::Modifiers::Shift)))
    , up(registerNavigation(input, menuContext, "ui.up", "Menu up", Binding::key(scancode::kUp)))
    , down(registerNavigation(input, menuContext, "ui.down", "Menu down", Binding::key(scancode::kDown)))
    , left(registerNavigation(input, menuContext, "ui.left", "Menu left", Binding::key(scancode::kLeft)))
    , right(registerNavigation(input, menuContext, "ui.right", "Menu right", Binding::key(scancode::kRight)))
    , activate(registerNavigation(input, menuContext, "ui.activate", "Confirm",
                                  Binding::key(scancode::kReturn), Binding::key(scancode::kKeypadEnter)))
    , back(registerNavigation(input, menuContext, "ui.back", "Back", Binding::key(scancode::kEscape))) {}

NavigationInput UiActions::navigation(const input::InputSystem& input) const {
    NavigationInput navigation{.next = input.isPressed(next),
                               .previous = input.isPressed(previous),
                               .up = input.isPressed(up),
                               .down = input.isPressed(down),
                               .left = input.isPressed(left),
                               .right = input.isPressed(right),
                               .activate = input.isPressed(activate),
                               .back = input.isPressed(back)};
    // A binding allows extra held modifiers, so Shift+Tab also fires Tab.
    if (navigation.previous) {
        navigation.next = false;
    }
    return navigation;
}

TextEditInput UiActions::textEditing(const input::InputSystem& input) {
    return {.typed = input.typedText(),
            .erase = input.wasKeyTyped(scancode::kBackspace),
            .eraseNext = input.wasKeyTyped(scancode::kDelete),
            .caretLeft = input.wasKeyTyped(scancode::kLeft),
            .caretRight = input.wasKeyTyped(scancode::kRight),
            .home = input.wasKeyTyped(scancode::kHome),
            .end = input.wasKeyTyped(scancode::kEnd)};
}

} // namespace vxe::ui
