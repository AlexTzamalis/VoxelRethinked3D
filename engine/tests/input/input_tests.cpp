#include <vxe/input/input.hpp>

#include <doctest/doctest.h>

#include <vector>

using vxe::input::ActionDesc;
using vxe::input::ActionId;
using vxe::input::ActionType;
using vxe::input::Binding;
using vxe::input::BindingKind;
using vxe::input::InputContextId;
using vxe::input::InputSystem;
using vxe::platform::KeyEvent;
using vxe::platform::Modifiers;
using vxe::platform::MouseButton;
using vxe::platform::MouseButtonEvent;
using vxe::platform::MouseMotionEvent;
using vxe::platform::MouseWheelEvent;
using vxe::platform::PlatformEvent;
using vxe::platform::TextInputEvent;
using vxe::platform::WindowFocusChanged;
namespace sc = vxe::input::scancode;

namespace {

PlatformEvent keyDown(vxe::u16 code) {
    return KeyEvent{.scancode = code, .down = true, .repeat = false, .modifiers = Modifiers::None};
}
PlatformEvent keyUp(vxe::u16 code) {
    return KeyEvent{.scancode = code, .down = false, .repeat = false, .modifiers = Modifiers::None};
}

// A game like setup: Global, Gameplay, Menu and Console contexts.
struct Fixture {
    InputSystem input;
    InputContextId global =
        input.registerContext({.name = "Global", .blocksLowerContexts = false, .alwaysActive = true});
    InputContextId gameplay = input.registerContext({.name = "Gameplay"});
    InputContextId menu = input.registerContext({.name = "Menu"});
    InputContextId console = input.registerContext({.name = "Console", .blocksLowerContexts = true});

    ActionId forward = add("gameplay.forward", gameplay, ActionType::Button, Binding::key(sc::kW));
    ActionId pause = add("gameplay.pause", gameplay, ActionType::Button, Binding::key(sc::kEscape));
    ActionId back = add("menu.back", menu, ActionType::Button, Binding::key(sc::kEscape));
    ActionId fullscreen = add("global.fullscreen", global, ActionType::Button, Binding::key(sc::kF11));
    ActionId overlay = add("debug.overlay", global, ActionType::Toggle, Binding::key(sc::kF3));
    ActionId hotbar = add("gameplay.hotbar", gameplay, ActionType::Axis, Binding::wheel(BindingKind::WheelUp),
                          Binding::wheel(BindingKind::WheelDown));
    ActionId save =
        add("gameplay.quicksave", gameplay, ActionType::Button, Binding::key(sc::kS, Modifiers::Ctrl));
    ActionId chunkBorders =
        add("debug.chunkBorders", global, ActionType::Button, Binding::chord(sc::kF3, sc::kG));
    ActionId breakBlock =
        add("gameplay.break", gameplay, ActionType::Button, Binding::mouse(MouseButton::Left));

    Fixture() {
        input.pushContext(global);
        input.pushContext(gameplay);
    }

    ActionId add(std::string_view id, InputContextId context, ActionType type, Binding primary,
                 Binding secondary = {}) {
        return input.registerAction(ActionDesc{.id = id,
                                               .displayName = id,
                                               .category = "Test",
                                               .context = context,
                                               .type = type,
                                               .defaults = {primary, secondary, Binding{}}});
    }

    void frame(std::vector<PlatformEvent> events = {}) { input.update(events); }
};

} // namespace

TEST_CASE("Button actions report pressed, held and released on the right frames") {
    Fixture f;
    f.frame({keyDown(sc::kW)});
    CHECK(f.input.isPressed(f.forward));
    CHECK(f.input.isHeld(f.forward));
    CHECK_FALSE(f.input.isReleased(f.forward));

    f.frame();
    CHECK_FALSE(f.input.isPressed(f.forward));
    CHECK(f.input.isHeld(f.forward));

    f.frame({keyUp(sc::kW)});
    CHECK_FALSE(f.input.isHeld(f.forward));
    CHECK(f.input.isReleased(f.forward));

    f.frame();
    CHECK_FALSE(f.input.isReleased(f.forward));
}

TEST_CASE("A tap within one frame still counts once") {
    Fixture f;
    f.frame({keyDown(sc::kW), keyUp(sc::kW)});
    CHECK(f.input.isPressed(f.forward));
    f.frame();
    CHECK(f.input.isReleased(f.forward));
    CHECK_FALSE(f.input.isHeld(f.forward));
}

TEST_CASE("Key repeats do not count as new presses") {
    Fixture f;
    f.frame({keyDown(sc::kW)});
    f.frame({KeyEvent{.scancode = sc::kW, .down = true, .repeat = true, .modifiers = Modifiers::None}});
    CHECK_FALSE(f.input.isPressed(f.forward));
    CHECK(f.input.isHeld(f.forward));
}

TEST_CASE("Toggle actions flip on every press") {
    Fixture f;
    CHECK_FALSE(f.input.isToggledOn(f.overlay));
    f.frame({keyDown(sc::kF3)});
    CHECK(f.input.isToggledOn(f.overlay));
    f.frame({keyUp(sc::kF3)});
    CHECK(f.input.isToggledOn(f.overlay));
    f.frame({keyDown(sc::kF3)});
    CHECK_FALSE(f.input.isToggledOn(f.overlay));
}

TEST_CASE("Axis actions sum the wheel notches of the frame") {
    Fixture f;
    f.frame({MouseWheelEvent{.x = 0.0F, .y = 1.0F}, MouseWheelEvent{.x = 0.0F, .y = 1.0F}});
    CHECK(f.input.axis(f.hotbar) == 2.0F);
    f.frame({MouseWheelEvent{.x = 0.0F, .y = -1.0F}});
    CHECK(f.input.axis(f.hotbar) == -1.0F);
    f.frame();
    CHECK(f.input.axis(f.hotbar) == 0.0F);
}

TEST_CASE("A context above claims the inputs it uses") {
    Fixture f;
    f.input.pushContext(f.menu); // Menu over gameplay: Escape means back, not pause.
    f.frame({keyDown(sc::kEscape)});
    CHECK(f.input.isPressed(f.back));
    CHECK_FALSE(f.input.isHeld(f.pause));
    CHECK(f.input.isHeld(f.forward) == false);

    // Inputs the menu does not use still reach gameplay.
    f.frame({keyDown(sc::kW)});
    CHECK(f.input.isPressed(f.forward));

    f.frame({keyUp(sc::kEscape)});
    f.input.popContext(f.menu);
    f.frame({keyDown(sc::kEscape)});
    CHECK(f.input.isPressed(f.pause));
}

TEST_CASE("A blocking context hides lower contexts, except always active ones") {
    Fixture f;
    f.input.pushContext(f.console);
    f.frame({keyDown(sc::kW), keyDown(sc::kF11), TextInputEvent{.text = "w"}});
    CHECK_FALSE(f.input.isHeld(f.forward)); // Typing in the console does not walk.
    CHECK(f.input.isPressed(f.fullscreen)); // F11 still works.
    CHECK(f.input.typedText() == "w");      // The console reads the raw text.
    CHECK(f.input.isKeyDown(sc::kW));       // Raw state is never filtered.

    f.input.popContext(f.console);
    f.frame();
    CHECK(f.input.isPressed(f.forward)); // Still held, now visible to gameplay.
}

TEST_CASE("Actions of contexts that are not on the stack never fire") {
    Fixture f;
    f.frame({keyDown(sc::kEscape)});
    CHECK_FALSE(f.input.isHeld(f.back)); // Menu is not active.
    CHECK(f.input.isPressed(f.pause));
    CHECK(f.input.isContextActive(f.gameplay));
    CHECK_FALSE(f.input.isContextActive(f.menu));
}

TEST_CASE("Modifiers and chords narrow when a binding fires") {
    Fixture f;
    f.frame({keyDown(sc::kS)});
    CHECK_FALSE(f.input.isHeld(f.save)); // S alone is not Ctrl+S.
    f.frame({keyDown(sc::kLeftCtrl)});
    CHECK(f.input.isPressed(f.save)); // Ctrl now held with S.
    CHECK(vxe::platform::hasAll(f.input.heldModifiers(), Modifiers::Ctrl));

    f.frame({keyDown(sc::kG)});
    CHECK_FALSE(f.input.isHeld(f.chunkBorders)); // G alone.
    f.frame({keyUp(sc::kG), keyDown(sc::kF3), keyDown(sc::kG)});
    CHECK(f.input.isPressed(f.chunkBorders)); // F3+G.
}

TEST_CASE("Mouse buttons, motion and focus loss") {
    Fixture f;
    f.frame({MouseButtonEvent{.button = MouseButton::Left, .down = true, .clicks = 1, .x = 5.0F, .y = 6.0F},
             MouseMotionEvent{.x = 7.0F, .y = 8.0F, .dx = 1.5F, .dy = -2.0F},
             MouseMotionEvent{.x = 9.0F, .y = 9.0F, .dx = 0.5F, .dy = 1.0F}});
    CHECK(f.input.isPressed(f.breakBlock));
    CHECK(f.input.isMouseButtonDown(MouseButton::Left));
    CHECK(f.input.mouseDelta() == vxe::Vec2(2.0F, -1.0F));
    CHECK(f.input.mousePosition() == vxe::Vec2(9.0F, 9.0F));

    f.frame();
    CHECK(f.input.mouseDelta() == vxe::Vec2(0.0F)); // Delta is per frame.

    // Alt tab while holding W and the mouse button: nothing may stay stuck.
    f.frame({keyDown(sc::kW)});
    f.frame({WindowFocusChanged{.focused = false}});
    CHECK_FALSE(f.input.isHeld(f.forward));
    CHECK_FALSE(f.input.isHeld(f.breakBlock));
}

TEST_CASE("Bindings can be replaced and actions found by id") {
    Fixture f;
    CHECK(f.input.findAction("gameplay.forward") == f.forward);
    CHECK_FALSE(f.input.findAction("nope").isValid());
    f.input.setBinding(f.forward, 0, Binding::key(sc::kE));
    f.frame({keyDown(sc::kW)});
    CHECK_FALSE(f.input.isHeld(f.forward));
    f.frame({keyDown(sc::kE)});
    CHECK(f.input.isPressed(f.forward));
    CHECK(f.input.action(f.forward).defaults[0] == Binding::key(sc::kW)); // Defaults stay for "reset".
}
