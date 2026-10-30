#include "game_settings.hpp"

#include <string>

namespace vr {
namespace {

using vxe::config::ApplyMode;
using vxe::config::Visibility;
using vxe::input::Binding;
using vxe::input::BindingKind;
using vxe::platform::MouseButton;
namespace scancode = vxe::input::scancode;

// Scancodes of the digit row (USB HID usage ids, as SDL numbers them): 1 is
// 30, 9 is 38.
constexpr vxe::u16 kDigit1 = 30;

} // namespace

GameSettings::GameSettings(vxe::config::SettingsRegistry& settings)
    : fieldOfView(settings.registerInt({{.id = "Gameplay.FieldOfView",
                                         .displayName = "Field of view",
                                         .description = "Vertical field of view in degrees."},
                                        70,
                                        30,
                                        110,
                                        1}))
    , viewBobbing(settings.registerBool({{.id = "Gameplay.ViewBobbing",
                                          .displayName = "View bobbing",
                                          .description = "The camera sways a little while walking."},
                                         true}))
    , mouseSensitivity(settings.registerFloat({{.id = "Controls.MouseSensitivity",
                                                .displayName = "Mouse sensitivity",
                                                .description = "How fast the view turns with the mouse."},
                                               1.0F,
                                               0.1F,
                                               5.0F,
                                               0.05F}))
    , invertY(settings.registerBool({{.id = "Controls.InvertY",
                                      .displayName = "Invert mouse",
                                      .description = "Moving the mouse up looks down."},
                                     false}))
    , toggleSprint(settings.registerBool({{.id = "Controls.ToggleSprint",
                                           .displayName = "Toggle sprint",
                                           .description = "Press sprint once to start and again to stop."},
                                          false}))
    , toggleCrouch(settings.registerBool({{.id = "Controls.ToggleCrouch",
                                           .displayName = "Toggle crouch",
                                           .description = "Press crouch once to start and again to stop."},
                                          false}))
    , hudOpacity(settings.registerFloat({{.id = "Interface.HudOpacity",
                                          .displayName = "HUD opacity",
                                          .description = "How solid the HUD looks."},
                                         1.0F,
                                         0.0F,
                                         1.0F,
                                         0.05F}))
    , showHud(settings.registerBool({{.id = "Interface.ShowHud",
                                      .displayName = "Show HUD",
                                      .description = "The crosshair and hotbar (F1)."},
                                     true}))
    , fontSample(settings.registerBool({{.id = "Debug.Ui.FontSample",
                                         .displayName = "Font sample",
                                         .description = "Shows the UI font at scales 1 to 4.",
                                         .visibility = Visibility::Developer},
                                        false}))
    , freezeCamera(settings.registerBool({{.id = "Debug.FreeCamera.Frozen",
                                           .displayName = "Freeze camera",
                                           .description = "Mouse and keys leave the camera alone.",
                                           .visibility = Visibility::Developer},
                                          false}))
    , widgetSample(settings.registerBool({{.id = "Debug.Ui.WidgetSample",
                                           .displayName = "Widget sample",
                                           .description = "Shows a screen with every UI widget.",
                                           .visibility = Visibility::Developer},
                                          false}))
    , screenShake(settings.registerFloat({{.id = "Accessibility.ScreenShake",
                                           .displayName = "Screen shake",
                                           .description = "Strength of camera shake effects."},
                                          1.0F,
                                          0.0F,
                                          1.0F,
                                          0.05F}))
    , fovEffects(settings.registerBool({{.id = "Accessibility.FovEffects",
                                         .displayName = "Field of view effects",
                                         .description = "The view widens a little while sprinting."},
                                        true}))
    , tickRate(settings.registerInt({{.id = "Server.TickRate",
                                      .displayName = "Tick rate",
                                      .description = "World simulation steps per second.",
                                      .applyMode = ApplyMode::RequiresWorldReload,
                                      .visibility = Visibility::Advanced},
                                     30,
                                     10,
                                     120,
                                     1}))
    , autosaveMinutes(
          settings.registerInt({{.id = "Server.AutosaveIntervalMinutes",
                                 .displayName = "Autosave interval",
                                 .description = "Minutes between automatic saves. 0 turns them off."},
                                5,
                                0,
                                60,
                                1}))
    , simulationDistance(
          settings.registerInt({{.id = "Server.SimulationDistance",
                                 .displayName = "Simulation distance",
                                 .description = "Chunks around the player where the world runs.",
                                 .visibility = Visibility::Advanced},
                                8,
                                2,
                                32,
                                1})) {}

GameActions::GameActions(vxe::input::InputSystem& input)
    : global(input.registerContext({.name = "Global", .alwaysActive = true}))
    , gameplay(input.registerContext({.name = "Gameplay"}))
    , menu(input.registerContext({.name = "Menu", .blocksLowerContexts = true})) {
    const auto action = [&](std::string_view id, std::string_view name, std::string_view category,
                            vxe::input::InputContextId context, Binding binding) {
        return input.registerAction({.id = id,
                                     .displayName = name,
                                     .category = category,
                                     .context = context,
                                     .defaults = {binding, Binding{}, Binding{}}});
    };
    moveForward =
        action("gameplay.move_forward", "Move forward", "Movement", gameplay, Binding::key(scancode::kW));
    moveBack = action("gameplay.move_back", "Move back", "Movement", gameplay, Binding::key(scancode::kS));
    moveLeft = action("gameplay.move_left", "Move left", "Movement", gameplay, Binding::key(scancode::kA));
    moveRight = action("gameplay.move_right", "Move right", "Movement", gameplay, Binding::key(scancode::kD));
    jump = action("gameplay.jump", "Jump", "Movement", gameplay, Binding::key(scancode::kSpace));
    crouch = action("gameplay.crouch", "Crouch", "Movement", gameplay, Binding::key(scancode::kLeftShift));
    sprint = action("gameplay.sprint", "Sprint", "Movement", gameplay, Binding::key(scancode::kLeftCtrl));
    breakBlock =
        action("gameplay.break", "Break block", "Interaction", gameplay, Binding::mouse(MouseButton::Left));
    placeBlock =
        action("gameplay.place", "Place block", "Interaction", gameplay, Binding::mouse(MouseButton::Right));
    pickBlock =
        action("gameplay.pick", "Pick block", "Interaction", gameplay, Binding::mouse(MouseButton::Middle));
    for (vxe::usize slot = 0; slot < hotbar.size(); ++slot) {
        const std::string id = "gameplay.hotbar_" + std::to_string(slot + 1);
        const std::string name = "Hotbar slot " + std::to_string(slot + 1);
        hotbar.at(slot) =
            action(id, name, "Inventory", gameplay, Binding::key(static_cast<vxe::u16>(kDigit1 + slot)));
    }
    hotbarNext = action("gameplay.hotbar_next", "Next hotbar slot", "Inventory", gameplay,
                        Binding::wheel(BindingKind::WheelDown));
    hotbarPrevious = action("gameplay.hotbar_previous", "Previous hotbar slot", "Inventory", gameplay,
                            Binding::wheel(BindingKind::WheelUp));
    inventory = action("gameplay.inventory", "Inventory", "Inventory", gameplay, Binding::key(scancode::kE));
    pause = action("gameplay.pause", "Pause", "Interface", gameplay, Binding::key(scancode::kEscape));
    toggleHud =
        action("gameplay.toggle_hud", "Toggle HUD", "Interface", gameplay, Binding::key(scancode::kF1));
    freeCamera =
        action("gameplay.free_camera", "Free camera", "Interface", gameplay, Binding::key(scancode::kF6));
    screenshot = action("global.screenshot", "Screenshot", "Interface", global, Binding::key(scancode::kF2));
    fullscreen = action("global.fullscreen", "Fullscreen", "Interface", global, Binding::key(scancode::kF11));
}

} // namespace vr
