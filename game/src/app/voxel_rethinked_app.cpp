#include "voxel_rethinked_app.hpp"

#include "client/default_pack.hpp"
#include "ui/screens/keybinds_screen.hpp"
#include "ui/screens/loading_screen.hpp"
#include "ui/screens/main_menu_screen.hpp"
#include "ui/screens/pause_screen.hpp"
#include "ui/screens/settings_screen.hpp"
#include "ui/screens/widget_sample.hpp"

#include <vxe/core/log.hpp>
#include <vxe/core/version.hpp>
#include <vxe/debug/developer_console.hpp>
#include <vxe/debug/overlay.hpp>
#include <vxe/render/debug_draw.hpp>
#include <vxe/render/renderer.hpp>
#include <vxe/ui/dialog.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vr {
namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the level changes at runtime.
constinit vxe::log::Category gameLog{"game", vxe::log::Level::Info};

// The menu's panorama: how it tilts, how wide it looks and how fast it turns
// (radians per second, one turn in about five minutes).
constexpr vxe::f32 kPanoramaPitch = -0.14F;
constexpr vxe::f32 kPanoramaFovY = 70.0F;
constexpr vxe::f32 kPanoramaTurnSpeed = 0.02F;

// The UI font, panorama and theme inside the texture packs (Section 15.1).
constexpr std::string_view kFontImage = "ui/font.png";
constexpr std::string_view kPanoramaImage = "ui/panorama.jpg";
constexpr std::string_view kThemeFile = "ui/theme.toml";

// Debug.Ui.FontSample: the UI font at every whole scale over a dark panel,
// to judge it at a glance.
void drawFontSample(vxe::ui::UiRenderer& ui) {
    constexpr std::array<std::string_view, 4> kLines = {
        "The quick brown fox jumps over the lazy dog. 0123456789",
        "SINGLEPLAYER  Multiplayer  Settings  Resource Packs  Quit",
        "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~  Render distance: 32 chunks",
        "Create New World  Seed: 12345",
    };
    constexpr vxe::f32 kMargin = 16.0F;
    constexpr vxe::f32 kPadding = 12.0F;
    std::array<std::string, 4> labels;
    vxe::f32 width = 0.0F;
    vxe::f32 height = 0.0F;
    for (vxe::u32 scale = 1; scale <= 4; ++scale) {
        labels[scale - 1] = std::format("{}x  {}", scale, kLines[scale - 1]);
        const vxe::ui::TextSize size = ui.measureText(labels[scale - 1], scale);
        width = std::max(width, size.width);
        height += size.height + static_cast<vxe::f32>(4 * scale);
    }
    ui.fillRect(
        {.x = kMargin, .y = kMargin, .width = width + 2.0F * kPadding, .height = height + 2.0F * kPadding},
        vxe::ui::rgba(16, 18, 24, 200));
    vxe::f32 y = kMargin + kPadding;
    for (vxe::u32 scale = 1; scale <= 4; ++scale) {
        ui.drawText(labels[scale - 1], kMargin + kPadding, y, {.scale = scale});
        y += ui.measureText(labels[scale - 1], scale).height + static_cast<vxe::f32>(4 * scale);
    }
}

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
    m_uiActions = std::make_unique<vxe::ui::UiActions>(context.input, m_actions->menu);
    m_input->pushContext(m_actions->global);
    m_flight = std::make_unique<vxe::render::FreeCameraController>(vxe::render::FreeCameraDesc{
        .input = &context.input, .settings = &context.settings, .alwaysLook = true});
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
    // Leaves the menu for the world, for scripted runs (captures, measurements).
    context.commands.registerCommand({.name = "play",
                                      .usage = "play",
                                      .description = "Enters the world from the main menu.",
                                      .handler = [this](std::span<const std::string> /*arguments*/) {
                                          vxe::config::CommandOutput output;
                                          if (m_states.current() != GameState::MainMenu) {
                                              output.lines.emplace_back("The main menu is not open.");
                                              output.success = false;
                                              return output;
                                          }
                                          m_states.request(GameState::StartingIntegratedServer);
                                          return output;
                                      }});
    // Leaves the world like the pause menu's Quit to title does.
    context.commands.registerCommand({.name = "title",
                                      .usage = "title",
                                      .description = "Goes back to the main menu from the pause menu.",
                                      .handler = [this](std::span<const std::string> /*arguments*/) {
                                          vxe::config::CommandOutput output;
                                          if (m_states.current() != GameState::Paused) {
                                              output.lines.emplace_back("Pause the game first (pause).");
                                              output.success = false;
                                              return output;
                                          }
                                          m_states.request(GameState::SavingAndStopping);
                                          return output;
                                      }});
    // Pauses like Escape does, for captures of the pause screen.
    context.commands.registerCommand({.name = "pause",
                                      .usage = "pause",
                                      .description = "Pauses the game and opens the pause menu.",
                                      .handler = [this](std::span<const std::string> /*arguments*/) {
                                          vxe::config::CommandOutput output;
                                          if (m_states.current() != GameState::InGame) {
                                              output.lines.emplace_back(
                                                  "Only a running world can be paused.");
                                              output.success = false;
                                              return output;
                                          }
                                          m_states.request(GameState::Paused);
                                          return output;
                                      }});
    // Opens the controls screen over the pause menu (captures, quick access).
    context.commands.registerCommand(
        {.name = "controls",
         .usage = "controls",
         .description = "Opens the controls screen (pause the game first).",
         .handler = [this, &context](std::span<const std::string> /*arguments*/) {
             vxe::config::CommandOutput output;
             if (m_states.current() != GameState::Paused || !m_screens || m_screens->empty()) {
                 output.lines.emplace_back("Pause the game first (pause).");
                 output.success = false;
                 return output;
             }
             m_screens->push(std::make_unique<KeybindsScreen>(menuServices(context)));
             return output;
         }});
    // Opens the settings screen over the menu or the pause menu (captures,
    // quick access), at a group and with a search if given.
    context.commands.registerCommand(
        {.name = "settings",
         .usage = "settings [group] [search]",
         .description = "Opens the settings screen at a group (at the menu or paused).",
         .handler = [this, &context](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             const GameState state = m_states.current();
             if ((state != GameState::Paused && state != GameState::MainMenu) || !m_screens ||
                 m_screens->empty()) {
                 output.lines.emplace_back("Pause the game first (pause).");
                 output.success = false;
                 return output;
             }
             m_screens->push(std::make_unique<SettingsScreen>(
                 menuServices(context), arguments.empty() ? std::string() : arguments.front(),
                 arguments.size() < 2 ? std::string() : arguments[1]));
             return output;
         }});
    // Opens the sample confirmation dialog, to look at dialogs while the
    // game holds the mouse (captures, theme work).
    context.commands.registerCommand({.name = "uidialog",
                                      .usage = "uidialog",
                                      .description = "Opens a sample dialog of the game UI.",
                                      .handler = [this](std::span<const std::string> /*arguments*/) {
                                          vxe::config::CommandOutput output;
                                          if (!m_uiContext) {
                                              output.lines.emplace_back("The game UI is not running.");
                                              output.success = false;
                                              return output;
                                          }
                                          // The widest dialog there is: four buttons.
                                          vxe::ui::showDialog(
                                              *m_uiContext, {.title = "Key already used",
                                                             .message = "Left Shift is used by Crouch.",
                                                             .buttons = {{.text = "Swap"},
                                                                         {.text = "Unbind other"},
                                                                         {.text = "Keep both"},
                                                                         {.text = "Cancel", .cancel = true}},
                                                             .width = 320.0F});
                                          return output;
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
        m_input->popContext(m_flight->inputContext());
    }
    if (isMenuState(to) && !isMenuState(from)) {
        m_input->pushContext(m_actions->menu);
    } else if (!isMenuState(to) && isMenuState(from)) {
        m_input->popContext(m_actions->menu);
    }
    context.inMenu = isMenuState(to);
    if (to == GameState::MainMenu && m_demo) {
        // The menu shows an image, so no world stays loaded behind it.
        m_demo->shutdown();
        m_demo.reset();
    } else if (to == GameState::LoadingWorld) {
        startDemoWorld(context);
        placeSpawnCamera();
    }
    if (!isWorldState(to) || to == GameState::Paused) {
        setMouseCaptured(context, false);
    }
    const auto hasScreens = [](GameState state) {
        return state == GameState::MainMenu || state == GameState::LoadingWorld || state == GameState::Paused;
    };
    if (hasScreens(to) || hasScreens(from)) {
        rebuildScreens(context);
    }
}

MenuServices VoxelRethinkedApp::menuServices(vxe::EngineContext& context) {
    return {.settings = context.settings, .input = context.input, .ui = *m_uiContext};
}

void VoxelRethinkedApp::rebuildScreens(vxe::EngineContext& context) {
    if (!m_screens) {
        return;
    }
    m_screens->clear();
    if (m_states.current() == GameState::MainMenu) {
        m_screens->push(std::make_unique<MainMenuScreen>(
            menuServices(context), [this] { m_states.request(GameState::StartingIntegratedServer); },
            [this, &context] { onQuitRequested(context); }));
    }
    if (m_states.current() == GameState::LoadingWorld) {
        m_screens->push(std::make_unique<LoadingScreen>());
    }
    if (m_settings->widgetSample.get()) {
        m_screens->push(std::make_unique<WidgetSampleScreen>(*m_uiContext));
    }
    if (m_states.current() == GameState::Paused) {
        m_screens->push(std::make_unique<PauseScreen>(
            menuServices(context), [this] { m_states.request(GameState::InGame); },
            [this] { m_states.request(GameState::SavingAndStopping); }));
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
    if (context.device != nullptr && context.renderer != nullptr) {
        // Texture packs from textures/ next to the program, over the default
        // pack built into it (Section 15.1); loaded on a job.
        auto builtIn = vxe::render::openEmbeddedPack("default", defaultPackFiles());
        if (!builtIn) {
            return std::move(builtIn).error();
        }
        const auto packsFolder = context.vfs.resolve("textures:/");
        auto packs = vxe::render::TexturePackManager::create(
            {.device = context.device,
             .settings = &context.settings,
             .jobs = &context.jobs,
             .stats = &context.stats,
             .packsFolder = packsFolder ? packsFolder.value() : std::filesystem::path(),
             .watcher = &context.fileWatcher,
             .builtIn = std::move(builtIn).value()});
        if (!packs) {
            return std::move(packs).error();
        }
        m_texturePacks = std::move(packs).value();
        // The UI font comes from the packs too, so packs can replace it.
        // The UI font and images come from the packs too, so packs restyle them.
        std::vector<std::string> images{std::string(kFontImage), std::string(kPanoramaImage)};
        for (const std::string_view name : vxe::ui::kThemeImages) {
            images.push_back(std::format("ui/{}.png", name));
        }
        m_texturePacks->setImages(std::move(images));
        m_texturePacks->setFiles({std::string(kThemeFile)});
        const auto keys = m_blocks->textureKeys();
        m_texturePacks->setKeys(std::vector<std::string>(keys.begin(), keys.end()));

        auto ui = vxe::ui::UiRenderer::create(
            {.device = context.device, .shaders = context.renderer->shaders(), .stats = &context.stats});
        if (!ui) {
            return std::move(ui).error();
        }
        m_ui = std::move(ui).value();
        context.renderer->setUi([ui = m_ui.get()](vxe::rhi::CommandList& commands) { ui->record(commands); });
        m_uiContext = vxe::ui::UiContext::create({.settings = &context.settings, .stats = &context.stats});
        m_screens = std::make_unique<vxe::ui::ScreenManager>(*m_uiContext, &context.stats);
    }
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
        // Without a window there is no menu to wait on.
        m_states.request(m_screens ? GameState::MainMenu : GameState::StartingIntegratedServer);
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
    case GameState::SavingAndStopping:
        // Nothing to save before G5.4. A window close passes through here
        // too, on its way out of the main loop.
        m_states.request(GameState::MainMenu);
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
            // The mouse looks around whenever the world has the input; the
            // open console needs the cursor instead.
            const bool consoleOpen = context.console != nullptr && context.console->isOpen();
            const bool steering = !consoleOpen && !m_settings->freezeCamera.get();
            setMouseCaptured(context, steering);
            if (steering) {
                m_flight->update(m_camera, time.deltaSeconds);
            }
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
    if (m_demo || !m_blocks || !m_blockRegistry) {
        return;
    }
    auto demo = DemoWorld::create(
        {.jobs = &context.jobs,
         .settings = &context.settings,
         .catalog = &*m_blocks,
         .blocks = m_blockRegistry.get(),
         .stats = &context.stats,
         .chunks = context.renderer != nullptr ? &context.renderer->chunks() : nullptr,
         .lod = context.renderer != nullptr ? &context.renderer->lod() : nullptr,
         .debugDraw = context.renderer != nullptr ? &context.renderer->debugDraw() : nullptr,
         .lodCacheFolder = demoLodFolder(context)});
    if (!demo) {
        VXE_LOG_ERROR(gameLog, "The demo world could not start: {}", demo.error().describe());
        return;
    }
    m_demo = std::move(demo).value();
    if (const vxe::render::BlockTextures* textures = m_texturePacks ? m_texturePacks->textures() : nullptr) {
        m_demo->setTextureColors(textures->averageColors);
    }
}

void VoxelRethinkedApp::placeSpawnCamera() {
    // Streaming centers on the camera, so it waits at the spawn while the
    // world loads around it.
    if (m_demo) {
        m_camera.position = m_demo->spawnPosition();
        m_camera.yaw = 0.0F;
        m_camera.pitch = glm::radians(-25.0F);
    }
    if (m_cameraOverride) {
        m_camera = *m_cameraOverride;
    }
}

// Hands newly loaded block textures to the chunk renderer and the far terrain.
void VoxelRethinkedApp::updateTextures(vxe::EngineContext& context) {
    if (!m_texturePacks || !m_texturePacks->update() || context.renderer == nullptr) {
        return;
    }
    const vxe::render::BlockTextures* textures = m_texturePacks->textures();
    context.renderer->chunks().setTextures(textures->texture, textures->sampler);
    context.renderer->chunks().setTextureAnimations(m_texturePacks->animations());
    context.renderer->chunks().setTextureLooks(m_blocks->textureLooks());
    context.renderer->chunks().setTintColors(m_blocks->tintColors());
    if (m_demo) {
        m_demo->setTextureColors(textures->averageColors);
    }
    if (const vxe::render::PackImage* font = m_texturePacks->image(kFontImage); font != nullptr && m_ui) {
        // The font and every theme image a pack provides share one atlas.
        std::vector<vxe::ui::AtlasImage> images;
        for (const std::string_view name : vxe::ui::kThemeImages) {
            if (const vxe::render::PackImage* image = m_texturePacks->image(std::format("ui/{}.png", name))) {
                images.push_back({.name = std::string(name),
                                  .width = image->width,
                                  .height = image->height,
                                  .pixels = image->pixels});
            }
        }
        const vxe::ui::AtlasImage sheet{.width = font->width, .height = font->height, .pixels = font->pixels};
        if (auto set = m_ui->setResources(sheet, images); !set) {
            VXE_LOG_WARN(gameLog, "UI font from pack '{}': {}", font->source, set.error().describe());
        }
    }
    if (const vxe::render::PackImage* panorama = m_texturePacks->image(kPanoramaImage);
        panorama != nullptr && m_ui) {
        if (auto set = m_ui->setPanorama(panorama->pixels, panorama->width, panorama->height); !set) {
            VXE_LOG_WARN(gameLog, "Panorama from pack '{}': {}", panorama->source, set.error().describe());
        }
    }
    if (const std::vector<std::byte>* theme = m_texturePacks->file(kThemeFile);
        theme != nullptr && m_uiContext) {
        std::vector<std::string> warnings;
        auto parsed = vxe::ui::Theme::parse(
            std::string_view(reinterpret_cast<const char*>(theme->data()), theme->size()), warnings);
        for (const std::string& warning : warnings) {
            VXE_LOG_WARN(gameLog, "{}", warning);
        }
        if (parsed) {
            m_uiContext->setTheme(parsed.value());
        } else {
            VXE_LOG_WARN(gameLog, "{}", parsed.error().describe());
        }
    }
}

// The game UI of this frame: the screen stack (the pause menu, the debug
// samples); the main menu comes with G4.1 and the HUD with G6.5.
void VoxelRethinkedApp::drawUi(vxe::EngineContext& context, const vxe::FrameTime& time) {
    if (!m_ui || context.device == nullptr) {
        return;
    }
    const vxe::u32 width = context.device->backBufferWidth();
    const vxe::u32 height = context.device->backBufferHeight();
    if (m_uiContext && m_settings->widgetSample.get() != m_widgetSampleShown) {
        m_widgetSampleShown = m_settings->widgetSample.get();
        rebuildScreens(context);
    }
    m_ui->begin(width, height);
    if (m_states.current() == GameState::MainMenu) {
        m_panoramaYaw += kPanoramaTurnSpeed * static_cast<vxe::f32>(time.deltaSeconds);
        m_ui->drawPanorama(m_panoramaYaw, kPanoramaPitch, kPanoramaFovY);
    }
    if (m_uiContext) {
        // The pointer and keys belong to the UI only while the game does not
        // use them for playing (mouse look captures the pointer, the menu
        // context carries the navigation actions).
        // While a key is being rebound, the next click is the new binding,
        // not a click on the UI.
        const bool capturing = m_input != nullptr && m_input->isCapturing();
        vxe::ui::UiInput input{.pointer = {.available = !m_mouseCaptured && !capturing},
                               .deltaSeconds = static_cast<vxe::f32>(time.deltaSeconds)};
        if (m_input != nullptr) {
            input.pointer.x = m_input->mousePosition().x;
            input.pointer.y = m_input->mousePosition().y;
            // A button that just became a binding is not a click until released.
            input.pointer.primaryDown = m_input->isMouseButtonDown(vxe::platform::MouseButton::Left) &&
                                        !m_input->isMouseButtonSuppressed(vxe::platform::MouseButton::Left);
            input.pointer.wheel = m_input->wheelDelta().y;
            input.navigation = m_uiActions->navigation(*m_input);
            input.text = vxe::ui::UiActions::textEditing(*m_input);
        }
        m_screens->update(input.deltaSeconds);
        m_uiContext->update(width, height, input, *m_ui);
        m_uiContext->draw(*m_ui);
        if (context.window != nullptr) {
            context.window->setTextInputEnabled(m_uiContext->wantsTextInput());
        }
    }
    if (m_settings->fontSample.get()) {
        drawFontSample(*m_ui);
    }
}

void VoxelRethinkedApp::onFrame(vxe::EngineContext& context, const vxe::FrameTime& time) {
    handleGlobalActions(context);
    advanceStartup(context);
    updateTextures(context);
    drawUi(context, time);
    // The world streams in while loading (no loading screen before G5.3).
    const GameState state = m_states.current();
    if (isWorldState(state) || state == GameState::LoadingWorld) {
        updateWorld(context, time);
    }
    if (!m_menuShown && state == GameState::MainMenu && m_ui && m_ui->font() != nullptr) {
        m_menuShown = true;
        VXE_LOG_INFO(gameLog, "Main menu shown {:.0f} ms after the engine started",
                     m_sinceInit.elapsedMilliseconds());
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
    if (context.renderer != nullptr) {
        context.renderer->chunks().setTextures({}, {});
        context.renderer->setUi({});
    }
    m_screens.reset();
    m_uiContext.reset();
    m_ui.reset();
    m_texturePacks.reset();
    m_flight.reset();
    VXE_LOG_INFO(gameLog, "Voxel Rethinked shutting down");
}

} // namespace vr
