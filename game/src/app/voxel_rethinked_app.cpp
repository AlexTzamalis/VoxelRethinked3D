#include "voxel_rethinked_app.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/renderer.hpp>

#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>

namespace vr {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the level changes at runtime.
constinit vxe::log::Category gameLog{"game", vxe::log::Level::Info};

// Where the camera starts: above the debug grid, looking slightly down. The
// demo world (after G1) replaces the grid with terrain.
constexpr vxe::f64 kGridHeight = 64.0;

// The demo world's far terrain cache, or empty (no cache) if saves:/ is
// not writable.
std::filesystem::path demoLodFolder(const vxe::EngineContext& context) {
    auto folder = context.vfs.resolveForWrite("saves:/demo/lod");
    if (!folder) {
        VXE_LOG_WARN(gameLog, "No far terrain cache for the demo world: {}", folder.error().describe());
        return {};
    }
    return folder.value();
}

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
            return std::format("{}   XYZ {:.1f} {:.1f} {:.1f}   chunk {} {} {}{}", nameOf(m_states.current()),
                               m_camera.position.x, m_camera.position.y, m_camera.position.z,
                               position.chunk.x, position.chunk.y, position.chunk.z,
                               m_demo ? "   " + m_demo->progressText() : std::string());
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
    // Puts the camera somewhere exact, for repeatable captures and measurements.
    context.commands.registerCommand(
        {.name = "camera",
         .usage = "camera <x> <y> <z> [yaw pitch]",
         .description = "Moves the camera to a block position, angles in degrees (yaw 0 looks along -Z).",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             if (arguments.size() != 3 && arguments.size() != 5) {
                 output.lines.push_back(
                     std::format("Camera at {:.1f} {:.1f} {:.1f}, yaw {:.0f}, pitch {:.0f}",
                                 m_camera.position.x, m_camera.position.y, m_camera.position.z,
                                 glm::degrees(m_camera.yaw), glm::degrees(m_camera.pitch)));
                 return output;
             }
             std::array<vxe::f64, 5> values{0.0, 0.0, 0.0, static_cast<vxe::f64>(glm::degrees(m_camera.yaw)),
                                            static_cast<vxe::f64>(glm::degrees(m_camera.pitch))};
             for (vxe::usize i = 0; i < arguments.size(); ++i) {
                 const std::string& text = arguments[i];
                 const auto [end, error] =
                     std::from_chars(text.data(), text.data() + text.size(), values.at(i));
                 if (error != std::errc{} || end != text.data() + text.size()) {
                     output.lines.emplace_back("Expected numbers: camera <x> <y> <z> [yaw pitch]");
                     output.success = false;
                     return output;
                 }
             }
             vxe::render::Camera camera = m_camera;
             camera.position = vxe::DVec3(values[0], values[1], values[2]);
             camera.yaw = glm::radians(static_cast<vxe::f32>(values[3]));
             camera.pitch = glm::radians(static_cast<vxe::f32>(values[4]));
             m_camera = camera;
             m_cameraOverride = camera;
             output.lines.emplace_back("Camera moved");
             return output;
         }});
    // A debug autopilot until the benchmark flight (G9.2): measurement runs
    // pass it with --exec, for example "autofly 300" for a fast flight.
    context.commands.registerCommand(
        {.name = "autofly",
         .usage = "autofly <blocks per second>",
         .description = "Flies the camera forward by itself. 0 stops.",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             if (arguments.size() != 1) {
                 output.lines.push_back(std::format("Flying at {:.0f} blocks per second", m_autoFlySpeed));
                 return output;
             }
             vxe::f64 speed = 0.0;
             const std::string& text = arguments[0];
             const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), speed);
             if (error != std::errc{} || end != text.data() + text.size() || speed < 0.0) {
                 output.lines.emplace_back("Expected a speed of 0 or more blocks per second");
                 output.success = false;
                 return output;
             }
             m_autoFlySpeed = speed;
             output.lines.push_back(std::format("Flying at {:.0f} blocks per second", m_autoFlySpeed));
             return output;
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
    if (to == GameState::LoadingWorld) {
        startDemoWorld(context);
    }
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
        // The demo world stands in for the real one until G0.2 and G3; it
        // streams in from updateWorld() and is entered once the area around
        // the spawn is ready.
        if (m_demo && !m_demo->isLoaded()) {
            break;
        }
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
            if (m_autoFlySpeed > 0.0) {
                // Level flight in the view direction, so looking down does
                // not fly into the ground.
                const vxe::Vec3 forward = m_camera.forward();
                const vxe::DVec3 level(forward.x, 0.0, forward.z);
                if (glm::length(level) > 1.0e-6) {
                    m_camera.position += glm::normalize(level) * (m_autoFlySpeed * time.deltaSeconds);
                }
            }
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
    if (m_demo) {
        m_demo->update(m_camera);
        return;
    }
    // Without a world, a grid shows where the ground would be.
    constexpr vxe::u32 kMinor = vxe::render::debugColor(90, 96, 110, 160);
    constexpr vxe::u32 kMajor = vxe::render::debugColor(90, 150, 255, 220);
    renderer.debugDraw().grid(m_camera.position, kGridHeight, 96, 1.0, kMinor, kMajor, 16);
}

void VoxelRethinkedApp::startDemoWorld(vxe::EngineContext& context) {
    if (!m_blocks || !m_blockRegistry) {
        return;
    }
    auto demo = DemoWorld::create(
        {.jobs = &context.jobs,
         .settings = &context.settings,
         .catalog = &*m_blocks,
         .blocks = m_blockRegistry.get(),
         .stats = &context.stats,
         .device = context.device,
         .chunks = context.renderer != nullptr ? &context.renderer->chunks() : nullptr,
         .lod = context.renderer != nullptr ? &context.renderer->lod() : nullptr,
         .debugDraw = context.renderer != nullptr ? &context.renderer->debugDraw() : nullptr,
         .lodCacheFolder = demoLodFolder(context)});
    if (!demo) {
        VXE_LOG_ERROR(gameLog, "The demo world could not start: {}", demo.error().describe());
        return;
    }
    m_demo = std::move(demo).value();
    // Streaming centers on the camera, so it waits at the spawn while the
    // world loads around it.
    m_camera.position = m_demo->spawnPosition();
    m_camera.yaw = 0.0F;
    m_camera.pitch = glm::radians(-25.0F);
    if (m_cameraOverride) {
        m_camera = *m_cameraOverride;
    }
}

void VoxelRethinkedApp::onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) {
    handleGlobalActions(context);
    advanceStartup(context);
    // The world streams in while loading (no loading screen before G5.3).
    if (isWorldState(m_states.current()) || m_states.current() == GameState::LoadingWorld) {
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
    // Its jobs must finish while the job system still runs.
    if (m_demo) {
        m_demo->shutdown();
        m_demo.reset();
    }
    m_flight.reset();
    VXE_LOG_INFO(gameLog, "Voxel Rethinked shutting down");
}

} // namespace vr
