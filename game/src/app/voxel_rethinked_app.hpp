#pragma once

#include "common/block_catalog.hpp"
#include "game_settings.hpp"
#include "game_state.hpp"

#include <vxe/app/application.hpp>
#include <vxe/render/camera.hpp>
#include <vxe/voxel/block_registry.hpp>

#include <memory>
#include <optional>

namespace vr {

// The game application (Section 20.2 app module): owns the state machine,
// registers the game's settings and actions, and drives each state.
//
// Until the menus (G4) and the world (G0.2 onwards) exist, startup goes
// Boot -> LoadingCoreAssets -> StartingIntegratedServer -> LoadingWorld ->
// InGame, one state per frame, and InGame is a free flight over a debug
// grid under the sky. Escape pauses (the camera stops and the mouse is
// free), F11 switches fullscreen, F3 shows the overlay and Grave the console.
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

    GameStateMachine m_states;
    std::optional<BlockCatalog> m_blocks;
    std::unique_ptr<vxe::voxel::BlockRegistry> m_blockRegistry;
    std::unique_ptr<GameSettings> m_settings;
    std::unique_ptr<GameActions> m_actions;
    vxe::render::Camera m_camera;
    std::unique_ptr<vxe::render::FreeCameraController> m_flight;
    bool m_mouseCaptured = false;
    vxe::input::InputSystem* m_input = nullptr;
};

} // namespace vr
