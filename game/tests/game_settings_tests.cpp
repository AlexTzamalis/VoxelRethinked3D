#include "app/game_settings.hpp"

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>

#include <doctest/doctest.h>

#include <string_view>

using vxe::input::Binding;
namespace scancode = vxe::input::scancode;

TEST_CASE("Game settings register in their categories with their defaults") {
    vxe::config::SettingsRegistry settings;
    const vr::GameSettings game(settings);
    CHECK(game.fieldOfView.get() == 70);
    CHECK(game.tickRate.get() == 30); // ADR-007: 30 ticks per second by default.
    CHECK(game.showHud.get());
    for (const std::string_view id :
         {"Gameplay.FieldOfView", "Gameplay.ViewBobbing", "Controls.MouseSensitivity", "Controls.InvertY",
          "Controls.ToggleSprint", "Controls.ToggleCrouch", "Interface.UiScale", "Interface.HudOpacity",
          "Interface.ShowHud", "Accessibility.ScreenShake", "Accessibility.FovEffects", "Server.TickRate",
          "Server.AutosaveIntervalMinutes", "Server.SimulationDistance"}) {
        INFO(id);
        CHECK(settings.value(id).hasValue());
    }
    // Values are clamped like every setting.
    REQUIRE(settings.set("Gameplay.FieldOfView", vxe::config::SettingValue{500}).hasValue());
    CHECK(game.fieldOfView.get() == 110);
}

TEST_CASE("Default actions follow Section 17.5 without conflicts") {
    vxe::input::InputSystem input;
    const vr::GameActions actions(input);
    const auto binding = [&](vxe::input::ActionId action) { return input.action(action).bindings[0]; };
    CHECK(binding(actions.moveForward) == Binding::key(scancode::kW));
    CHECK(binding(actions.jump) == Binding::key(scancode::kSpace));
    CHECK(binding(actions.crouch) == Binding::key(scancode::kLeftShift));
    CHECK(binding(actions.sprint) == Binding::key(scancode::kLeftCtrl));
    CHECK(binding(actions.pause) == Binding::key(scancode::kEscape));
    CHECK(binding(actions.menuBack) == Binding::key(scancode::kEscape));
    CHECK(binding(actions.fullscreen) == Binding::key(scancode::kF11));
    CHECK(binding(actions.hotbar[0]) == Binding::key(30)); // The 1 key.
    CHECK(binding(actions.hotbar[8]) == Binding::key(38)); // The 9 key.
    CHECK(binding(actions.breakBlock) == Binding::mouse(vxe::platform::MouseButton::Left));

    // No game action shares its binding with another that can be active at
    // the same time (Escape is in Gameplay and in Menu, which never compete:
    // the menu blocks Gameplay).
    for (const vxe::input::ActionId action :
         {actions.moveForward, actions.moveBack,   actions.moveLeft,   actions.moveRight,
          actions.jump,        actions.crouch,     actions.sprint,     actions.breakBlock,
          actions.placeBlock,  actions.pickBlock,  actions.hotbarNext, actions.hotbarPrevious,
          actions.inventory,   actions.pause,      actions.toggleHud,  actions.freeCamera,
          actions.menuBack,    actions.screenshot, actions.fullscreen, actions.hotbar[0],
          actions.hotbar[8]}) {
        const auto& info = input.action(action);
        INFO(info.id);
        CHECK(input.findConflicts(info.bindings[0], info.context, action).empty());
    }
}
