#include <vxe/input/keybinds.hpp>

#include <doctest/doctest.h>

#include <string>
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
using vxe::platform::MouseWheelEvent;
using vxe::platform::PlatformEvent;
namespace sc = vxe::input::scancode;
using Outcome = InputSystem::CaptureOutcome;

namespace {

PlatformEvent keyDown(vxe::u16 code) {
    return KeyEvent{.scancode = code, .down = true, .repeat = false, .modifiers = Modifiers::None};
}
PlatformEvent keyUp(vxe::u16 code) {
    return KeyEvent{.scancode = code, .down = false, .repeat = false, .modifiers = Modifiers::None};
}

struct Fixture {
    InputSystem input;
    InputContextId global =
        input.registerContext({.name = "Global", .blocksLowerContexts = false, .alwaysActive = true});
    InputContextId gameplay = input.registerContext({.name = "Gameplay"});
    InputContextId menu = input.registerContext({.name = "Menu"});

    ActionId jump = add("gameplay.jump", "Jump", "Movement", gameplay, Binding::key(sc::kSpace));
    ActionId forward = add("gameplay.forward", "Move forward", "Movement", gameplay, Binding::key(sc::kW));
    ActionId save =
        add("gameplay.quicksave", "Quick save", "Interface", gameplay, Binding::key(sc::kS, Modifiers::Ctrl));
    ActionId breakBlock =
        add("gameplay.break", "Break block", "Interaction", gameplay, Binding::mouse(MouseButton::Left));
    ActionId back = add("menu.back", "Back", "Interface", menu, Binding::key(sc::kEscape));
    ActionId borders =
        add("debug.chunkBorders", "Chunk borders", "Debug", global, Binding::chord(sc::kF3, sc::kG));
    ActionId screenshot = add("global.screenshot", "Screenshot", "Interface", global, Binding::key(sc::kF2));

    Fixture() {
        input.pushContext(global);
        input.pushContext(gameplay);
    }

    ActionId add(std::string_view id, std::string_view name, std::string_view category,
                 InputContextId context, Binding primary) {
        return input.registerAction(ActionDesc{.id = id,
                                               .displayName = name,
                                               .category = category,
                                               .context = context,
                                               .type = ActionType::Button,
                                               .defaults = {primary, Binding{}, Binding{}}});
    }
};

} // namespace

TEST_CASE("Bindings convert to readable text and back") {
    const std::vector<Binding> bindings{
        Binding::key(sc::kW),
        Binding::key(sc::kSpace),
        Binding::key(sc::kLeftShift),
        Binding::key(sc::kS, Modifiers::Ctrl),
        Binding::key(sc::kE, Modifiers::Ctrl | Modifiers::Shift | Modifiers::Alt),
        Binding::chord(sc::kF3, sc::kG),
        Binding::mouse(MouseButton::Right),
        Binding::mouse(MouseButton::X2),
        Binding::wheel(BindingKind::WheelUp),
        Binding::wheel(BindingKind::WheelLeft),
        Binding{},
    };
    for (const Binding& binding : bindings) {
        const std::string text = vxe::input::bindingToString(binding);
        CAPTURE(text);
        const auto parsed = vxe::input::bindingFromString(text);
        REQUIRE(parsed.hasValue());
        CHECK(parsed.value() == binding);
    }
    CHECK(vxe::input::bindingToString(Binding::key(sc::kS, Modifiers::Ctrl)) == "Ctrl+S");
    CHECK(vxe::input::bindingToString(Binding::chord(sc::kF3, sc::kG)) == "F3+G");
    CHECK(vxe::input::bindingToString(Binding::mouse(MouseButton::Left)) == "Mouse Left");
    CHECK(vxe::input::bindingToString(Binding{}).empty());
}

TEST_CASE("Hand written binding text is forgiving about case and spaces") {
    CHECK(vxe::input::bindingFromString("  ctrl+shift+s ").value() ==
          Binding::key(sc::kS, Modifiers::Ctrl | Modifiers::Shift));
    CHECK(vxe::input::bindingFromString("mouse left").value() == Binding::mouse(MouseButton::Left));
    CHECK(vxe::input::bindingFromString("wheel down").value() == Binding::wheel(BindingKind::WheelDown));
    CHECK(vxe::input::bindingFromString("Keypad +").value().kind ==
          BindingKind::Key); // A plus inside a key name.
    CHECK(vxe::input::bindingFromString("Scancode 300").value() == Binding::key(300));
    CHECK(vxe::input::bindingFromString("NoSuchKey").hasError());
    CHECK(vxe::input::bindingFromString("Mouse Tail").hasError());
}

TEST_CASE("Keybinds round trip through TOML") {
    Fixture original;
    original.input.setBinding(original.jump, 1, Binding::mouse(MouseButton::Middle));
    original.input.setBinding(original.forward, 0, Binding::key(sc::kE));
    const std::string text = vxe::input::saveKeybindsToString(original.input);
    CHECK(text.find("[gameplay]") != std::string::npos);
    CHECK(text.find("# Jump (Movement). Default: Space") != std::string::npos);
    CHECK(text.find("jump = [\"Space\", \"Mouse Middle\", \"\"]") != std::string::npos);

    Fixture loaded;
    const auto report = vxe::input::loadKeybindsFromString(loaded.input, text);
    REQUIRE(report.hasValue());
    CHECK(report.value().warnings.empty());
    CHECK(loaded.input.action(loaded.jump).bindings == original.input.action(original.jump).bindings);
    CHECK(loaded.input.action(loaded.forward).bindings[0] == Binding::key(sc::kE));
    CHECK(loaded.input.action(loaded.borders).bindings[0] == Binding::chord(sc::kF3, sc::kG));
}

TEST_CASE("A hand edited keybinds file loads with warnings and keeps unknown actions") {
    constexpr std::string_view kFile = R"(
[gameplay]
jump = "Space"                   # a single string is fine
forward = ["Up", "W"]            # fewer than three slots clears the rest
quicksave = ["Ctrl+NoSuchKey"]   # bad text: warning, binding kept
flyUp = ["Space"]                # an action this version does not have

[menu]
back = [7]                       # not text: warning
)";
    Fixture f;
    const auto report = vxe::input::loadKeybindsFromString(f.input, kFile);
    REQUIRE(report.hasValue());
    CHECK(f.input.action(f.jump).bindings[0] == Binding::key(sc::kSpace));
    CHECK(f.input.action(f.forward).bindings[0] == Binding::key(82)); // Up arrow.
    CHECK(f.input.action(f.forward).bindings[1] == Binding::key(sc::kW));
    CHECK(f.input.action(f.save).bindings[0] == Binding::key(sc::kS, Modifiers::Ctrl));
    CHECK(f.input.action(f.back).bindings[0] == Binding::key(sc::kEscape));
    CHECK(report.value().warnings.size() == 2);

    REQUIRE(f.input.unknownKeybinds().size() == 1);
    CHECK(f.input.unknownKeybinds()[0].first == "gameplay.flyUp");
    CHECK(vxe::input::saveKeybindsToString(f.input).find("flyUp = ") != std::string::npos);
    CHECK(vxe::input::loadKeybindsFromString(f.input, "jump = = 1").hasError());
}

TEST_CASE("Conflicts are found in the same context and against always active contexts") {
    Fixture f;
    // W is already move forward in gameplay.
    const auto sameContext = f.input.findConflicts(Binding::key(sc::kW), f.gameplay, f.jump);
    REQUIRE(sameContext.size() == 1);
    CHECK(sameContext[0].action == f.forward);
    CHECK(sameContext[0].slot == 0);

    // F2 is a global screenshot, so binding it in gameplay conflicts too.
    CHECK(f.input.findConflicts(Binding::key(sc::kF2), f.gameplay).size() == 1);

    // Escape in gameplay does not conflict with the menu: the context stack decides.
    CHECK(f.input.findConflicts(Binding::key(sc::kEscape), f.gameplay).empty());

    // An action never conflicts with itself.
    CHECK(f.input.findConflicts(Binding::key(sc::kSpace), f.gameplay, f.jump).empty());

    // Swap and unbind, the two resolutions of the conflict dialog.
    f.input.setBinding(f.jump, 1, Binding::key(sc::kW));
    f.input.swapBindings(f.jump, 1, f.forward, 0);
    CHECK(f.input.action(f.forward).bindings[0] == Binding::key(sc::kW));
    CHECK(f.input.action(f.jump).bindings[1] == Binding::key(sc::kW));
    f.input.clearBinding(f.jump, 1);
    CHECK(f.input.action(f.jump).bindings[1].isNone());

    f.input.resetAllBindings();
    CHECK(f.input.resetCategory("Movement") == 2);
}

TEST_CASE("Capture reports the next input instead of triggering actions") {
    Fixture f;
    Outcome outcome = Outcome::Cancelled;
    Binding captured;
    const auto capture = [&] {
        f.input.beginCapture([&](Outcome result, const Binding& binding) {
            outcome = result;
            captured = binding;
        });
    };

    capture();
    CHECK(f.input.isCapturing());
    f.input.update(std::vector<PlatformEvent>{keyDown(sc::kLeftCtrl), keyDown(sc::kS)});
    CHECK_FALSE(f.input.isCapturing());
    CHECK(outcome == Outcome::Captured);
    CHECK(captured == Binding::key(sc::kS, Modifiers::Ctrl));
    CHECK_FALSE(f.input.isHeld(f.save)); // Did not fire while capturing...
    f.input.update({});
    CHECK_FALSE(f.input.isHeld(f.save)); // ...nor right after, while S is still held.
    f.input.update(std::vector<PlatformEvent>{keyUp(sc::kS), keyUp(sc::kLeftCtrl)});

    // A modifier pressed and released alone is captured as itself.
    capture();
    f.input.update(std::vector<PlatformEvent>{keyDown(sc::kLeftShift)});
    CHECK(f.input.isCapturing());
    f.input.update(std::vector<PlatformEvent>{keyUp(sc::kLeftShift)});
    CHECK(captured == Binding::key(sc::kLeftShift));

    capture();
    f.input.update(std::vector<PlatformEvent>{
        MouseButtonEvent{.button = MouseButton::X1, .down = true, .clicks = 1, .x = 0.0F, .y = 0.0F}});
    CHECK(captured == Binding::mouse(MouseButton::X1));

    capture();
    f.input.update(std::vector<PlatformEvent>{MouseWheelEvent{.x = 0.0F, .y = -1.0F}});
    CHECK(captured == Binding::wheel(BindingKind::WheelDown));

    capture();
    f.input.update(std::vector<PlatformEvent>{keyDown(sc::kEscape)});
    CHECK(outcome == Outcome::Cancelled);
    capture();
    f.input.update(std::vector<PlatformEvent>{keyUp(sc::kEscape), keyDown(sc::kBackspace)});
    CHECK(outcome == Outcome::Cleared);

    capture();
    f.input.cancelCapture();
    CHECK_FALSE(f.input.isCapturing());
    CHECK(outcome == Outcome::Cancelled);
}

TEST_CASE("The bind by key label setting is registered") {
    vxe::config::SettingsRegistry settings;
    const auto setting = vxe::input::registerInputSettings(settings);
    CHECK_FALSE(setting.get());
    CHECK(settings.find("Controls.BindByKeyName") != nullptr);
}
