#pragma once

#include "client/block_icons.hpp"
#include "client/demo_world.hpp"
#include "client/player_controller.hpp"
#include "common/block_catalog.hpp"
#include "game_settings.hpp"
#include "game_state.hpp"
#include "server/world_store.hpp"
#include "ui/screens/menu_services.hpp"

#include <vxe/app/application.hpp>
#include <vxe/core/time.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/render/texture_pack_manager.hpp>
#include <vxe/ui/screen.hpp>
#include <vxe/ui/ui_context.hpp>
#include <vxe/ui/ui_input.hpp>
#include <vxe/ui/ui_renderer.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <memory>
#include <optional>

namespace vr {

// The game application (Section 20.2 app module): owns the state machine,
// registers the game's settings and actions, and drives each state.
//
// Until the menus (G4) and the real world (G0.2, G3, E9.7) exist, startup
// goes Boot -> LoadingCoreAssets (block definitions) ->
// StartingIntegratedServer -> LoadingWorld (the demo world generates and
// meshes) -> InGame, and InGame is a free flight over the demo world. Escape pauses (the camera stops and the
// mouse is free), F11 switches fullscreen, F3 shows the overlay and Grave the console.
//
// Thread safety: main thread only, like every Application.
class VoxelRethinkedApp final : public vxe::Application {
public:
    vxe::Result<void> onInit(vxe::EngineContext& context) override;
    void onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) override;
    void onQuitRequested(vxe::EngineContext& context) override;
    void onShutdown(vxe::EngineContext& context) override;

    [[nodiscard]] GameState state() const noexcept { return m_states.current(); }

private:
    void onTransition(vxe::EngineContext& context, GameState from, GameState to);
    void advanceStartup(vxe::EngineContext& context);
    [[nodiscard]] vxe::Result<void> loadCoreAssets(vxe::EngineContext& context);
    void handleGlobalActions(vxe::EngineContext& context);
    void updateWorld(vxe::EngineContext& context, const vxe::FrameTime& time);
    void setMouseCaptured(vxe::EngineContext& context, bool captured);
    void registerCommands(vxe::EngineContext& context);
    void startDemoWorld(vxe::EngineContext& context);
    void placeSpawnCamera();
    // Marks a world as played now and starts it.
    void playWorld(vxe::EngineContext& context, const WorldInfo& world);
    // Saves the player and commits edits; `waitForDisk` for leaving the world.
    void saveWorld(vxe::EngineContext& context, bool waitForDisk);
    [[nodiscard]] PlayerData playerData() const;
    void setFreeCamera(bool on);
    void updateHotbar(vxe::EngineContext& context, vxe::f64 deltaSeconds);
    void updateTextures(vxe::EngineContext& context);
    void drawUi(vxe::EngineContext& context, const vxe::FrameTime& time);
    // Puts the screens the game state and the debug settings ask for on the stack.
    void rebuildScreens(vxe::EngineContext& context);
    [[nodiscard]] MenuServices menuServices(vxe::EngineContext& context);

    GameStateMachine m_states;
    std::optional<BlockCatalog> m_blocks;
    std::unique_ptr<vxe::voxel::BlockRegistry> m_blockRegistry;
    std::unique_ptr<DemoWorld> m_demo;
    // Block textures from the texture packs (G2.2); with a renderer only.
    std::unique_ptr<vxe::render::TexturePackManager> m_texturePacks;
    // The game UI (E12); with a renderer only.
    std::unique_ptr<vxe::ui::UiRenderer> m_ui;
    std::unique_ptr<vxe::ui::UiContext> m_uiContext;
    std::unique_ptr<vxe::ui::ScreenManager> m_screens;
    // Menu navigation (Tab, arrows, Enter, Escape) in the menu context.
    std::unique_ptr<vxe::ui::UiActions> m_uiActions;
    bool m_widgetSampleShown = false;
    // The world being played, with its own seed, edits and far terrain cache.
    // Empty for the play console command, which saves nothing.
    std::optional<WorldInfo> m_world;
    std::unique_ptr<PlayerController> m_player;
    std::unique_ptr<BlockInteraction> m_interaction;
    Hotbar m_hotbar;
    BlockIcons m_blockIcons;
    bool m_inventoryOpen = false;
    bool m_freeCamera = false; // The debug camera (F6, autofly) instead of the player.
    std::vector<vxe::platform::PlatformEvent> m_inputUps; // Tapped by the input command, for the next frame.
    vxe::u64 m_inGameFrames = 0;
    vxe::u64 m_waitUntil = 0; // The wait command's frame, 0 when none runs.
    vxe::f64 m_sinceAutosave = 0.0;
    // How far the menu's panorama has turned, radians.
    vxe::f32 m_panoramaYaw = 0.0F;
    // Measures launch to the first frame of the menu, logged once.
    vxe::Stopwatch m_sinceInit;
    bool m_menuShown = false;
    std::unique_ptr<GameSettings> m_settings;
    std::unique_ptr<GameActions> m_actions;
    vxe::render::Camera m_camera;
    std::unique_ptr<vxe::render::FreeCameraController> m_flight;
    bool m_mouseCaptured = false;
    vxe::f64 m_autoFlySpeed = 0.0; // The autofly command, blocks per second.
    // The camera command: where to put the camera, applied once the world
    // has placed it at the spawn (so --exec works before the world exists).
    std::optional<vxe::render::Camera> m_cameraOverride;
    vxe::input::InputSystem* m_input = nullptr;
};

} // namespace vr
