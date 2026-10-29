#include "voxel_rethinked_app.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/renderer.hpp>

#include <format>
#include <string>

namespace vr {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the level changes at runtime.
constinit vxe::log::Category gameLog{"game", vxe::log::Level::Info};

// Where the camera starts: above the debug grid, looking slightly down. The
// demo world (after G1) replaces the grid with terrain.
constexpr vxe::f64 kGridHeight = 64.0;

} // namespace

vxe::Result<void> VoxelRethinkedApp::onInit(vxe::EngineContext& context) {
    vxe::log::registerCategory(gameLog);
    VXE_LOG_INFO(gameLog, "Voxel Rethinked {} started", vxe::engineVersion());
    if (context.window != nullptr) {
        context.window->setTitle("Voxel Rethinked");
    }
    m_input = &context.input;
    m_settings = std::make_unique<GameSettings>(context.settings);
    m_actions = std::make_unique<GameActions>(context.input);
    m_input->pushContext(m_actions->global);
    m_flight = std::make_unique<vxe::render::FreeCameraController>(
        vxe::render::FreeCameraDesc{.input = &context.input, .settings = &context.settings});
    m_camera.position = vxe::DVec3(8.5, kGridHeight + 6.0, 8.5);
    m_camera.pitch = glm::radians(-20.0F);
    m_states.setListener([this, &context](GameState from, GameState to) { onTransition(context, from, to); });
    registerCommands(context);
    if (context.overlay != nullptr) {
        context.overlay->addOverviewLine([this] {
            const vxe::CameraPosition position = m_camera.renderPosition();
            return std::format("{}   XYZ {:.1f} {:.1f} {:.1f}   chunk {} {} {}", nameOf(m_states.current()),
                               m_camera.position.x, m_camera.position.y, m_camera.position.z,
                               position.chunk.x, position.chunk.y, position.chunk.z);
        });
    }
    return {};
}

void VoxelRethinkedApp::registerCommands(vxe::EngineContext& context) {
    context.commands.registerCommand(
        {.name = "state",
         .usage = "state",
         .description = "Shows the game state.",
         .handler = [this](std::span<const std::string> /*arguments*/) {
             return vxe::config::CommandOutput{
                 .lines = {
                     std::format("{} ({} transitions)", nameOf(m_states.current()), m_states.transitions())}};
         }});
}

void VoxelRethinkedApp::onTransition(vxe::EngineContext& context, GameState from, GameState to) {
    VXE_LOG_INFO(gameLog, "State {} -> {}", nameOf(from), nameOf(to));
    // Gameplay input only counts in the world; a menu over it blocks it.
    if (isWorldState(to) && !isWorldState(from)) {
        m_input->pushContext(m_actions->gameplay);
        // Until the player controller (G6.1) the free camera flies, so its
        // keys go first; pushing moves its context above Gameplay.
        m_input->pushContext(m_flight->inputContext());
    } else if (!isWorldState(to) && isWorldState(from)) {
        m_input->popContext(m_actions->gameplay);
    }
    if (isMenuState(to) && !isMenuState(from)) {
        m_input->pushContext(m_actions->menu);
    } else if (!isMenuState(to) && isMenuState(from)) {
        m_input->popContext(m_actions->menu);
    }
    context.inMenu = isMenuState(to);
    if (to == GameState::Paused) {
        setMouseCaptured(context, false);
    }
}

vxe::Result<void> VoxelRethinkedApp::loadCoreAssets(vxe::EngineContext& context) {
    // Block definitions (G1.2) from assets:/data/blocks; texture packs (G2)
    // come here next.
    auto folder = context.vfs.resolve("assets:/data/blocks");
    if (!folder) {
        return std::move(folder).error();
    }
    auto catalog = BlockCatalog::loadFolder(folder.value());
    if (!catalog) {
        return std::move(catalog).error();
    }
    auto view = catalog.value().buildRegistry();
    if (!view) {
        return std::move(view).error();
    }
    m_blocks = std::move(catalog).value();
    m_blockRegistry = std::make_unique<vxe::voxel::BlockRegistry>(&context.stats);
    m_blockRegistry->publish(std::move(view).value());
    VXE_LOG_INFO(gameLog, "Loaded {} blocks with {} states and {} texture keys", m_blocks->blockCount(),
                 m_blocks->stateCount(), m_blocks->textureKeys().size());
    return {};
}

void VoxelRethinkedApp::advanceStartup(vxe::EngineContext& context) {
    // One state per frame, so each shows up in the log and the overlay. The
    // integrated server arrives in G0.2 and the world after that.
    switch (m_states.current()) {
    case GameState::Boot:
        m_states.request(GameState::LoadingCoreAssets);
        break;
    case GameState::LoadingCoreAssets:
        if (vxe::Result<void> loaded = loadCoreAssets(context); !loaded) {
            // Without its blocks the game cannot show a world.
            VXE_LOG_CRITICAL(gameLog, "Core assets failed to load: {}", loaded.error().describe());
            context.requestQuit();
            break;
        }
        m_states.request(GameState::StartingIntegratedServer); // No main menu before G4.
        break;
    case GameState::StartingIntegratedServer:
        m_states.request(GameState::LoadingWorld);
        break;
    case GameState::LoadingWorld:
        m_states.request(GameState::InGame);
        break;
    default:
        break;
    }
}

void VoxelRethinkedApp::setMouseCaptured(vxe::EngineContext& context, bool captured) {
    if (captured == m_mouseCaptured || context.window == nullptr) {
        return;
    }
    m_mouseCaptured = captured;
    static_cast<void>(context.window->setRelativeMouseMode(captured));
}

void VoxelRethinkedApp::handleGlobalActions(vxe::EngineContext& context) {
    if (context.window != nullptr && m_input->isPressed(m_actions->fullscreen)) {
        const vxe::platform::WindowMode mode = context.window->mode() == vxe::platform::WindowMode::Windowed
                                                   ? vxe::platform::WindowMode::Borderless
                                                   : vxe::platform::WindowMode::Windowed;
        if (auto changed = context.window->setMode(mode); !changed) {
            VXE_LOG_WARN(gameLog, "Could not change the window mode: {}", changed.error().describe());
        } else {
            VXE_LOG_INFO(gameLog, "Window mode: {}",
                         mode == vxe::platform::WindowMode::Windowed ? "windowed" : "borderless fullscreen");
        }
    }
    if (m_input->isPressed(m_actions->screenshot)) {
        VXE_LOG_INFO(gameLog, "Screenshots are not implemented yet");
    }
}

void VoxelRethinkedApp::updateWorld(vxe::EngineContext& context, const vxe::FrameTime& time) {
    if (m_states.current() == GameState::InGame) {
        if (m_input->isPressed(m_actions->pause)) {
            m_states.request(GameState::Paused);
        } else {
            if (m_input->isPressed(m_actions->toggleHud)) {
                static_cast<void>(m_settings->showHud.set(!m_settings->showHud.get()));
            }
            m_flight->update(m_camera, time.deltaSeconds);
            setMouseCaptured(context, m_flight->isLooking());
        }
    } else if (m_states.current() == GameState::Paused && m_input->isPressed(m_actions->menuBack)) {
        m_states.request(GameState::InGame);
    }
    // The free camera has its own field of view setting for debugging; in the
    // game the player's choice wins.
    m_camera.fovYDegrees = static_cast<vxe::f32>(m_settings->fieldOfView.get());

    if (context.renderer == nullptr) {
        return;
    }
    vxe::render::Renderer& renderer = *context.renderer;
    renderer.beginFrame(m_camera, time);
    constexpr vxe::u32 kMinor = vxe::render::debugColor(90, 96, 110, 160);
    constexpr vxe::u32 kMajor = vxe::render::debugColor(90, 150, 255, 220);
    renderer.debugDraw().grid(m_camera.position, kGridHeight, 96, 1.0, kMinor, kMajor, 16);
}

void VoxelRethinkedApp::onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) {
    handleGlobalActions(context);
    advanceStartup(context);
    if (isWorldState(m_states.current())) {
        updateWorld(context, time);
    }
}

void VoxelRethinkedApp::onQuitRequested(vxe::EngineContext& context) {
    // Nothing to save yet (G5.4); the SavingAndStopping state is passed
    // through so the order of a real save stays visible in the log.
    if (isWorldState(m_states.current())) {
        m_states.request(GameState::SavingAndStopping);
    }
    context.requestQuit();
}

void VoxelRethinkedApp::onShutdown(vxe::EngineContext& context) {
    setMouseCaptured(context, false);
    m_flight.reset();
    VXE_LOG_INFO(gameLog, "Voxel Rethinked shutting down");
}

} // namespace vr
