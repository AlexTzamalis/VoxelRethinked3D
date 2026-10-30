#pragma once

#include <vxe/config/settings.hpp>
#include <vxe/input/input.hpp>

#include <array>

namespace vr {

// The game's own settings (Section 16.4), registered at startup in their
// categories so the settings file, the console and later the settings
// screen (G4.2) know them. Values are read live wherever they are used.
// Several only take effect once their feature exists (the HUD in G6.5, the
// player controller in G6.1, the server tick in G0.2); registering them now
// fixes their names and defaults.
struct GameSettings {
    explicit GameSettings(vxe::config::SettingsRegistry& settings);

    // Gameplay
    vxe::config::Setting<vxe::i32> fieldOfView; // Degrees, vertical.
    vxe::config::Setting<bool> viewBobbing;
    // Controls
    vxe::config::Setting<vxe::f32> mouseSensitivity;
    vxe::config::Setting<bool> invertY;
    vxe::config::Setting<bool> toggleSprint; // Press once instead of holding.
    vxe::config::Setting<bool> toggleCrouch;
    // Interface
    vxe::config::Setting<vxe::f32> hudOpacity;
    vxe::config::Setting<bool> showHud; // F1 flips it.
    // Debug
    vxe::config::Setting<bool> fontSample;   // The UI font at scales 1 to 4.
    vxe::config::Setting<bool> freezeCamera; // Mouse and keys leave the camera alone (baking).
    vxe::config::Setting<bool> widgetSample; // A screen with every widget.
    // Accessibility
    vxe::config::Setting<vxe::f32> screenShake;
    vxe::config::Setting<bool> fovEffects;
    // Server
    vxe::config::Setting<vxe::i32> tickRate;
    vxe::config::Setting<vxe::i32> autosaveMinutes;
    vxe::config::Setting<vxe::i32> simulationDistance;
};

// The game's input contexts and default actions (Section 17.5). Debug keys
// (F3 and its chords, the console, chunk borders) belong to the engine's
// debug tools and are not repeated here.
//
// Contexts, lowest first: Gameplay (in the world), Menu (blocks Gameplay
// while a menu is up) and Global (always active).
struct GameActions {
    explicit GameActions(vxe::input::InputSystem& input);

    vxe::input::InputContextId global;
    vxe::input::InputContextId gameplay;
    vxe::input::InputContextId menu;

    // Movement
    vxe::input::ActionId moveForward;
    vxe::input::ActionId moveBack;
    vxe::input::ActionId moveLeft;
    vxe::input::ActionId moveRight;
    vxe::input::ActionId jump;
    vxe::input::ActionId crouch;
    vxe::input::ActionId sprint;
    // Interaction
    vxe::input::ActionId breakBlock;
    vxe::input::ActionId placeBlock;
    vxe::input::ActionId pickBlock;
    // Inventory
    std::array<vxe::input::ActionId, 9> hotbar{};
    vxe::input::ActionId hotbarNext;
    vxe::input::ActionId hotbarPrevious;
    vxe::input::ActionId inventory;
    // Interface
    vxe::input::ActionId pause;
    vxe::input::ActionId toggleHud;
    vxe::input::ActionId freeCamera;
    vxe::input::ActionId screenshot;
    vxe::input::ActionId fullscreen;
};

} // namespace vr
