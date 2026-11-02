#include "voxel_rethinked_app.hpp"

#include "client/default_pack.hpp"
#include "ui/screens/hud_screen.hpp"
#include "ui/screens/inventory_screen.hpp"
#include "ui/screens/keybinds_screen.hpp"
#include "ui/screens/loading_screen.hpp"
#include "ui/screens/main_menu_screen.hpp"
#include "ui/screens/pause_screen.hpp"
#include "ui/screens/settings_screen.hpp"
#include "ui/screens/widget_sample.hpp"
#include "ui/screens/world_screens.hpp"

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
#include <chrono>
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
// The HUD's pack images, in the atlas under their short names.
constexpr std::array<std::string_view, 3> kHudImages = {"crosshair", "hotbar_slot", "hotbar_selected"};
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
    // Sound first, so its volumes are in the settings like any other. Without
    // a sound card or sound files the game simply stays silent.
    m_audio = std::make_unique<vxe::audio::AudioSystem>(vxe::audio::AudioDesc{.settings = &context.settings});
    if (const auto folder = context.vfs.resolve("sounds:/"); folder) {
        m_sounds = std::make_unique<SoundDirector>(*m_audio, folder.value());
    } else {
        VXE_LOG_WARN(gameLog, "No sounds folder: the game stays silent");
    }
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
                                          m_world.reset();
                                          m_states.request(GameState::StartingIntegratedServer);
                                          return output;
                                      }});
    // Drive the real input path from scripts: keys and buttons, mouse motion,
    // and a pause between them. Startup commands run in order, so "wait" fails
    // until its frame has come and holds back the ones after it.
    context.commands.registerCommand(
        {.name = "input",
         .usage = "input <down|up|tap> <left|right|middle|wheelup|wheeldown|space|shift|ctrl|escape|a-z|1-9>",
         .description = "Sends a key or mouse button as if pressed.",
         .handler = [this, &context](std::span<const std::string> arguments) {
             namespace platform = vxe::platform;
             vxe::config::CommandOutput output;
             if (arguments.size() != 2) {
                 output.success = false;
                 return output;
             }
             const std::string& name = arguments[1];
             const auto send = [&](bool down, bool later) {
                 std::optional<platform::PlatformEvent> event;
                 if (name == "left" || name == "right" || name == "middle") {
                     const platform::MouseButton button = name == "left"    ? platform::MouseButton::Left
                                                          : name == "right" ? platform::MouseButton::Right
                                                                            : platform::MouseButton::Middle;
                     event = platform::MouseButtonEvent{.button = button, .down = down};
                 } else if (name == "wheelup" || name == "wheeldown") {
                     if (down) {
                         event = platform::MouseWheelEvent{.y = name == "wheelup" ? 1.0F : -1.0F};
                     }
                 } else {
                     vxe::u16 code = 0;
                     if (name == "space") {
                         code = 44;
                     } else if (name == "shift") {
                         code = 225;
                     } else if (name == "ctrl") {
                         code = 224;
                     } else if (name == "escape") {
                         code = 41;
                     } else if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') {
                         code = static_cast<vxe::u16>(4 + (name[0] - 'a'));
                     } else if (name.size() == 1 && name[0] >= '1' && name[0] <= '9') {
                         code = static_cast<vxe::u16>(30 + (name[0] - '1'));
                     }
                     event = platform::KeyEvent{.scancode = code, .down = down};
                 }
                 if (event && later) {
                     m_inputUps.push_back(*event);
                 } else if (event) {
                     context.platform.injectEvent(*event);
                 }
             };
             if (arguments[0] == "tap") {
                 send(true, false);
                 send(false, true);
             } else {
                 send(arguments[0] == "down", false);
             }
             return output;
         }});
    context.commands.registerCommand({.name = "look",
                                      .usage = "look <dx> <dy>",
                                      .description = "Sends mouse motion, in pixels.",
                                      .handler = [&context](std::span<const std::string> arguments) {
                                          vxe::config::CommandOutput output;
                                          if (arguments.size() != 2) {
                                              output.success = false;
                                              return output;
                                          }
                                          context.platform.injectEvent(vxe::platform::MouseMotionEvent{
                                              .dx = std::stof(arguments[0]), .dy = std::stof(arguments[1])});
                                          return output;
                                      }});
    context.commands.registerCommand(
        {.name = "wait",
         .usage = "wait <frames>",
         .description = "Fails until this many frames have been played, holding back later startup commands.",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             if (m_waitUntil == 0 && m_states.current() == GameState::InGame && arguments.size() == 1) {
                 m_waitUntil = m_inGameFrames + std::stoull(arguments[0]);
             }
             output.success = m_waitUntil != 0 && m_inGameFrames >= m_waitUntil;
             if (output.success) {
                 m_waitUntil = 0;
             }
             return output;
         }});
    context.commands.registerCommand(
        {.name = "playerinfo",
         .usage = "playerinfo",
         .description = "Logs where the player is and what is targeted.",
         .handler = [this](std::span<const std::string> /*arguments*/) {
             vxe::config::CommandOutput output;
             if (!m_player || !m_demo) {
                 output.success = false;
                 return output;
             }
             const PlayerState& state = m_player->state();
             const auto& target = m_interaction->target();
             std::string aimed = "nothing";
             if (target) {
                 aimed = std::format("{} at {} {} {}",
                                     m_blocks->stateName(m_demo->world().getBlock(target->block)),
                                     target->block.x, target->block.y, target->block.z);
             }
             VXE_LOG_INFO(gameLog, "Player feet {:.2f} {:.2f} {:.2f} ground {} flying {} target {} slot {}",
                          state.position.x, state.position.y, state.position.z, state.onGround, state.flying,
                          aimed, m_hotbar.selected + 1);
             return output;
         }});
    // Reads and changes single blocks, for scripted runs and debugging.
    context.commands.registerCommand(
        {.name = "setblock",
         .usage = "setblock <x> <y> <z> <state>",
         .description = "Changes one block of the loaded world.",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             const auto state =
                 arguments.size() == 4 && m_blocks ? m_blocks->stateId(arguments[3]) : std::nullopt;
             if (!m_demo || !state) {
                 output.lines.emplace_back("Needs a loaded world and a known state.");
                 output.success = false;
                 return output;
             }
             const vxe::BlockPos position{
                 .x = std::stoll(arguments[0]), .y = std::stoll(arguments[1]), .z = std::stoll(arguments[2])};
             output.success = m_demo->setBlock(position, *state);
             return output;
         }});
    context.commands.registerCommand(
        {.name = "placeon",
         .usage = "placeon <x> <z> <state>",
         .description = "Puts a block on the highest solid block of a column.",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             const auto state =
                 arguments.size() == 3 && m_blocks ? m_blocks->stateId(arguments[2]) : std::nullopt;
             if (!m_demo || !state) {
                 output.success = false;
                 return output;
             }
             const vxe::i64 x = std::stoll(arguments[0]);
             const vxe::i64 z = std::stoll(arguments[1]);
             constexpr vxe::i64 kTop = (vxe::i64{DemoWorld::kMaxChunkY} + 1) * vxe::kChunkSize;
             constexpr vxe::i64 kBottom = vxe::i64{DemoWorld::kMinChunkY} * vxe::kChunkSize;
             // Only the chunks around the camera are loaded, so missing ones are skipped.
             for (vxe::i64 y = kTop - 2; y >= kBottom; --y) {
                 const vxe::BlockPos ground{.x = x, .y = y, .z = z};
                 if (m_demo->world().findChunk(vxe::toChunk(ground)) == nullptr) {
                     continue;
                 }
                 if (m_blocks->isSolid(m_demo->world().getBlock(ground))) {
                     output.success = m_demo->setBlock({.x = x, .y = y + 1, .z = z}, *state);
                     if (const auto upper = m_blocks->tallUpper(*state); upper != vxe::voxel::kAirState) {
                         static_cast<void>(m_demo->setBlock({.x = x, .y = y + 2, .z = z}, upper));
                     }
                     return output;
                 }
             }
             output.success = false;
             return output;
         }});
    context.commands.registerCommand(
        {.name = "getblock",
         .usage = "getblock <x> <y> <z>",
         .description = "Shows one block of the loaded world.",
         .handler = [this](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             if (!m_demo || arguments.size() != 3) {
                 output.success = false;
                 return output;
             }
             const vxe::BlockPos position{
                 .x = std::stoll(arguments[0]), .y = std::stoll(arguments[1]), .z = std::stoll(arguments[2])};
             if (m_demo->world().findChunk(vxe::toChunk(position)) == nullptr) {
                 output.lines.emplace_back("Not loaded yet.");
                 output.success = false;
                 return output;
             }
             const std::string name(m_blocks->stateName(m_demo->world().getBlock(position)));
             VXE_LOG_INFO(gameLog, "Block at {} {} {} is {}; player at {:.1f} {:.1f} {:.1f}", position.x,
                          position.y, position.z, name, m_camera.position.x, m_camera.position.y,
                          m_camera.position.z);
             output.lines.push_back(name);
             return output;
         }});
    // Presses a button of the UI by its text, so scripted runs can walk through
    // real screens the way a player does.
    context.commands.registerCommand({.name = "uiclick",
                                      .usage = "uiclick <button text>",
                                      .description = "Presses the button with this text.",
                                      .handler = [this](std::span<const std::string> arguments) {
                                          vxe::config::CommandOutput output;
                                          std::string text;
                                          for (const std::string& word : arguments) {
                                              text += (text.empty() ? "" : " ") + word;
                                          }
                                          std::vector<vxe::ui::Widget*> stack;
                                          if (m_uiContext) {
                                              stack.push_back(&m_uiContext->topWidget());
                                          }
                                          // Last children first: what is drawn on top (a dialog) wins.
                                          vxe::ui::Button* last = nullptr;
                                          while (!stack.empty() && last == nullptr) {
                                              vxe::ui::Widget* widget = stack.back();
                                              stack.pop_back();
                                              if (auto* button = dynamic_cast<vxe::ui::Button*>(widget);
                                                  button != nullptr && button->text() == text &&
                                                  button->enabled()) {
                                                  last = button;
                                              }
                                              for (const auto& child : widget->children()) {
                                                  stack.push_back(child.get());
                                              }
                                          }
                                          if (last == nullptr) {
                                              output.lines.emplace_back("No such button.");
                                              output.success = false;
                                              return output;
                                          }
                                          last->activate();
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
             if (m_player) {
                 // Scripts place the eyes, and the player hovers there.
                 constexpr vxe::f64 kEyes = 1.62;
                 m_player->place(camera.position - vxe::DVec3(0.0, kEyes, 0.0), camera.yaw, camera.pitch,
                                 true);
             }
             m_cameraOverride = camera;
             output.lines.emplace_back("Camera moved");
             return output;
         }});
    // Jumps to a biome, for looking at world generation on foot.
    context.commands.registerCommand(
        {.name = "locate",
         .usage = "locate <biome>",
         .description = "Moves the camera to the nearest column of a biome, such as locate birch forest.",
         .handler = [this, &context](std::span<const std::string> arguments) {
             vxe::config::CommandOutput output;
             std::string wanted;
             for (const std::string& word : arguments) {
                 wanted += (wanted.empty() ? "" : " ") + word;
             }
             const auto same = [](std::string_view a, std::string_view b) {
                 return std::ranges::equal(a, b, [](char l, char r) {
                     return std::tolower(static_cast<unsigned char>(l)) ==
                            std::tolower(static_cast<unsigned char>(r));
                 });
             };
             const auto named =
                 std::ranges::find_if(kBiomeNames, [&](std::string_view name) { return same(name, wanted); });
             if (!m_demo || named == kBiomeNames.end()) {
                 output.lines.emplace_back("Usage: locate <biome>, while in a world. Biomes:");
                 for (const std::string_view name : kBiomeNames) {
                     output.lines.emplace_back(name);
                 }
                 output.success = false;
                 return output;
             }
             const auto biome = static_cast<Biome>(named - kBiomeNames.begin());
             const vxe::BlockPos from{.x = static_cast<vxe::i64>(std::floor(m_camera.position.x)),
                                      .y = static_cast<vxe::i64>(std::floor(m_camera.position.y)),
                                      .z = static_cast<vxe::i64>(std::floor(m_camera.position.z))};
             const auto found = m_demo->terrain().findBiome(biome, from.x, from.z);
             if (!found) {
                 output.lines.emplace_back("Not found within 12 km");
                 output.success = false;
                 return output;
             }
             output.lines.push_back(std::format("{} at {} {} {}", wanted, found->x, found->y, found->z));
             static_cast<void>(
                 context.commands.execute(std::format("camera {} {} {}", found->x, found->y + 6, found->z)));
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
             setFreeCamera(speed > 0.0);
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
    } else if (!isWorldState(to) && isWorldState(from)) {
        setFreeCamera(false);
        m_input->popContext(m_actions->gameplay);
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
        return state == GameState::MainMenu || state == GameState::LoadingWorld ||
               state == GameState::Paused || state == GameState::InGame;
    };
    if (hasScreens(to) || hasScreens(from)) {
        rebuildScreens(context);
    }
}

PlayerData VoxelRethinkedApp::playerData() const {
    PlayerData player;
    player.position = m_player->state().position;
    player.yaw = m_player->yaw();
    player.pitch = m_player->pitch();
    player.flying = m_player->state().flying;
    player.hotbarSlot = static_cast<vxe::u8>(m_hotbar.selected);
    player.clockHours = m_clockHours;
    for (vxe::usize index = 0; index < m_hotbar.slots.size(); ++index) {
        if (m_hotbar.slots.at(index) != vxe::voxel::kAirState) {
            player.hotbar.at(index) = std::string(m_blocks->stateName(m_hotbar.slots.at(index)));
        }
    }
    return player;
}

void VoxelRethinkedApp::saveWorld(vxe::EngineContext& context, bool waitForDisk) {
    if (!m_world || !m_demo) {
        return;
    }
    m_sinceAutosave = 0.0;
    m_world->player = playerData();
    if (waitForDisk) {
        // Only when leaving: the edits go with the world's shutdown right after.
        const auto saved = savePlayerData(menuServices(context).saves, m_world->folder, *m_world->player);
        if (!saved) {
            VXE_LOG_WARN(gameLog, "Could not save the player: {}", saved.error().describe());
        }
        return;
    }
    m_demo->saveEdits();
    auto save =
        std::make_shared<std::pair<std::filesystem::path, WorldInfo>>(menuServices(context).saves, *m_world);
    const auto write = [save] {
        if (const auto saved = savePlayerData(save->first, save->second.folder, *save->second.player);
            !saved) {
            VXE_LOG_WARN(gameLog, "Could not save the player: {}", saved.error().describe());
        }
    };
    static_cast<void>(context.jobs.schedule(write, {vxe::jobs::Priority::Normal, {}, vxe::jobs::Pool::Io}));
}

void VoxelRethinkedApp::playWorld(vxe::EngineContext& context, const WorldInfo& world) {
    m_world = world;
    m_world->lastPlayedSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    VXE_LOG_INFO(gameLog, "Playing world '{}' (folder {}, seed {})", m_world->name, m_world->folder,
                 m_world->seed);
    // A small write, but disk work stays off the main thread all the same.
    auto info = std::make_shared<WorldInfo>(*m_world);
    const auto saves = menuServices(context).saves;
    const auto save = [info, saves = std::make_shared<std::filesystem::path>(saves)] {
        if (const auto saved = saveWorldInfo(*saves, *info); !saved) {
            VXE_LOG_WARN(gameLog, "Could not save world '{}': {}", info->name, saved.error().describe());
        }
    };
    static_cast<void>(context.jobs.schedule(save, {vxe::jobs::Priority::Normal, {}, vxe::jobs::Pool::Io}));
    m_states.request(GameState::StartingIntegratedServer);
}

MenuServices VoxelRethinkedApp::menuServices(vxe::EngineContext& context) {
    auto saves = context.vfs.resolveForWrite("saves:/");
    return {.settings = context.settings,
            .input = context.input,
            .ui = *m_uiContext,
            .jobs = context.jobs,
            .saves = saves ? saves.value() : std::filesystem::path()};
}

void VoxelRethinkedApp::rebuildScreens(vxe::EngineContext& context) {
    if (!m_screens) {
        return;
    }
    m_screens->clear();
    if (m_states.current() == GameState::MainMenu) {
        m_screens->push(std::make_unique<MainMenuScreen>(
            menuServices(context),
            [this, &context] {
                m_screens->push(std::make_unique<WorldSelectScreen>(
                    menuServices(context),
                    [this, &context](const WorldInfo& world) { playWorld(context, world); }));
            },
            [this, &context] { onQuitRequested(context); }));
    }
    if (m_states.current() == GameState::LoadingWorld) {
        m_screens->push(std::make_unique<LoadingScreen>());
    }
    if (m_states.current() == GameState::InGame) {
        m_screens->push(std::make_unique<HudScreen>(m_hotbar, m_blockIcons, *m_blocks));
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
    m_player = std::make_unique<PlayerController>(*m_input, *m_actions, *m_settings, *m_blocks);
    m_interaction = std::make_unique<BlockInteraction>(*m_input, *m_actions, *m_blocks);
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
        for (const std::string_view name : kHudImages) {
            images.push_back(std::format("ui/{}.png", name));
        }
        // Block textures too, as PNG files: the icons are baked from them.
        for (const std::string& key : m_blocks->textureKeys()) {
            images.push_back(vxe::render::textureFile(key) + ".png");
        }
        for (const std::string_view path : vxe::render::kSkyBodyImages) {
            images.emplace_back(path);
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
        // A window close passes through here too, on its way out of the main loop.
        saveWorld(context, true);
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
    for (const vxe::platform::PlatformEvent& event : m_inputUps) {
        context.platform.injectEvent(event);
    }
    m_inputUps.clear();
    m_inGameFrames += m_states.current() == GameState::InGame ? 1U : 0U;
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
            updateHotbar(context, time.deltaSeconds);
            const bool steering = !consoleOpen && !m_settings->freezeCamera.get();
            setMouseCaptured(context, steering && !m_inventoryOpen);
            if (m_input->isPressed(m_actions->freeCamera)) {
                setFreeCamera(!m_freeCamera);
            }
            if (steering && m_freeCamera && !m_inventoryOpen) {
                m_flight->update(m_camera, time.deltaSeconds);
            } else if (steering && m_player && m_demo && !m_freeCamera) {
                m_player->update(m_camera, m_demo->world(), time.deltaSeconds,
                                 vxe::i64{DemoWorld::kMinChunkY} * vxe::kChunkSize,
                                 (vxe::i64{DemoWorld::kMaxChunkY} + 1) * vxe::kChunkSize, !m_inventoryOpen);
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
    const auto fieldOfView = static_cast<vxe::f32>(m_settings->fieldOfView.get());
    m_camera.fovYDegrees = m_player ? m_player->fieldOfView(fieldOfView) : fieldOfView;

    if (context.renderer == nullptr) {
        return;
    }
    vxe::render::Renderer& renderer = *context.renderer;
    renderer.beginFrame(m_camera, time);
    m_clockHours = renderer.sky().clockHours();
    if (m_demo) {
        m_sinceAutosave += time.deltaSeconds;
        if (const vxe::i32 minutes = m_settings->autosaveMinutes.get();
            minutes > 0 && m_sinceAutosave >= minutes * 60.0 && m_states.current() == GameState::InGame) {
            saveWorld(context, false);
        }
        m_demo->update(m_camera);
        const bool playing = m_states.current() == GameState::InGame && !m_freeCamera && !m_inventoryOpen &&
                             !m_settings->freezeCamera.get() &&
                             (context.console == nullptr || !context.console->isOpen());
        if (playing && m_interaction) {
            m_interaction->update(
                m_camera, m_player->state(), m_demo->world(), m_hotbar,
                [this](const vxe::BlockPos& position, vxe::voxel::BlockStateId state) {
                    const vxe::voxel::BlockStateId before =
                        m_demo->world().getBlock(position, vxe::voxel::kAirState);
                    const bool changed = m_demo->setBlock(position, state);
                    if (changed && m_sounds) {
                        const vxe::DVec3 middle(static_cast<vxe::f64>(position.x) + 0.5,
                                                static_cast<vxe::f64>(position.y) + 0.5,
                                                static_cast<vxe::f64>(position.z) + 0.5);
                        if (state == vxe::voxel::kAirState && before != vxe::voxel::kAirState &&
                            !m_blocks->isLiquid(before)) {
                            // Plants have no material of their own: they rustle.
                            const std::string_view name = m_blocks->stateName(before);
                            const StepSound material =
                                m_blocks->isSolid(before) ? stepSoundOf(name) : StepSound::Leaves;
                            m_sounds->blockBroken(middle, material,
                                                  name.find("glass") != std::string_view::npos);
                        } else if (state != vxe::voxel::kAirState) {
                            const StepSound material = m_blocks->isSolid(state)
                                                           ? stepSoundOf(m_blocks->stateName(state))
                                                           : StepSound::Leaves;
                            m_sounds->blockPlaced(middle, material);
                        }
                    }
                    return changed;
                },
                &renderer.debugDraw(), time.deltaSeconds);
        }
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
         .seed = m_world ? m_world->seed : DemoWorldDesc{}.seed,
         .lodCacheFolder =
             m_world ? menuServices(context).saves / m_world->folder / "lod" : demoLodFolder(context),
         .saveFolder =
             m_world ? menuServices(context).saves / m_world->folder / "regions" : std::filesystem::path()});
    if (!demo) {
        VXE_LOG_ERROR(gameLog, "The demo world could not start: {}", demo.error().describe());
        return;
    }
    m_demo = std::move(demo).value();
    if (context.renderer != nullptr) {
        context.renderer->clouds().setSeed(m_world ? m_world->seed : DemoWorldDesc{}.seed);
        context.renderer->sky().setClockHours(m_world && m_world->player ? m_world->player->clockHours
                                                                         : kFirstMorning);
    }
    if (const vxe::render::BlockTextures* textures = m_texturePacks ? m_texturePacks->textures() : nullptr) {
        m_demo->setTextureColors(textures->averageColors);
    }
}

// Slots, the HUD and the creative inventory while playing.
void VoxelRethinkedApp::updateHotbar(vxe::EngineContext& context, vxe::f64 deltaSeconds) {
    if (!m_screens) {
        return;
    }
    // The inventory is any screen over the HUD; the menu context lets it use
    // the keyboard and blocks gameplay keys until it closes.
    const bool open = m_screens->size() > 1;
    if (open != m_inventoryOpen) {
        m_inventoryOpen = open;
        if (open) {
            m_input->pushContext(m_actions->menu);
        } else {
            m_input->popContext(m_actions->menu);
        }
    }
    if (!open) {
        for (vxe::usize slot = 0; slot < m_actions->hotbar.size(); ++slot) {
            if (m_input->isPressed(m_actions->hotbar.at(slot))) {
                m_hotbar.selected = slot;
            }
        }
        if (m_player == nullptr || !m_player->isZooming()) {
            const vxe::usize count = m_hotbar.slots.size();
            if (m_input->isPressed(m_actions->hotbarNext)) {
                m_hotbar.selected = (m_hotbar.selected + 1) % count;
            }
            if (m_input->isPressed(m_actions->hotbarPrevious)) {
                m_hotbar.selected = (m_hotbar.selected + count - 1) % count;
            }
        }
        if (m_input->isPressed(m_actions->inventory) && m_blocks) {
            m_screens->push(
                std::make_unique<InventoryScreen>(menuServices(context), *m_blocks, m_blockIcons, m_hotbar));
        }
    }
    if (auto* hud = dynamic_cast<HudScreen*>(m_screens->bottom()); hud != nullptr && hud->hud() != nullptr) {
        hud->hud()->sync(m_settings->showHud.get(), m_settings->hudOpacity.get(),
                         static_cast<vxe::f32>(deltaSeconds));
    }
}

void VoxelRethinkedApp::setFreeCamera(bool on) {
    if (on == m_freeCamera) {
        return;
    }
    m_freeCamera = on;
    // Pushing puts its keys above Gameplay; they would block the player's.
    if (on) {
        m_input->pushContext(m_flight->inputContext());
    } else {
        m_input->popContext(m_flight->inputContext());
        m_autoFlySpeed = 0.0;
    }
}

void VoxelRethinkedApp::placeSpawnCamera() {
    // Streaming centers on the camera, so it waits at the spawn while the
    // world loads around it.
    m_hotbar = Hotbar{};
    const auto slot = [this](vxe::usize index, std::string_view name) {
        if (const auto state = m_blocks->stateId(name); state) {
            m_hotbar.slots.at(index) = *state;
        }
    };
    if (m_world && m_world->player) {
        const PlayerData& saved = *m_world->player;
        m_player->place(saved.position, saved.yaw, saved.pitch, saved.flying);
        m_hotbar.selected = saved.hotbarSlot;
        for (vxe::usize index = 0; index < saved.hotbar.size(); ++index) {
            slot(index, saved.hotbar.at(index));
        }
    } else if (m_demo) {
        m_player->place(m_demo->spawnGround(), 0.0F, glm::radians(-10.0F), false);
        constexpr std::array<std::string_view, 9> kStartBlocks = {
            "stone", "dirt",        "grass_block", "oak_log[axis=y]", "oak_leaves", "sand",
            "glass", "lumen_block", "water"};
        for (vxe::usize index = 0; index < kStartBlocks.size(); ++index) {
            slot(index, kStartBlocks.at(index));
        }
    }
    m_player->applyTo(m_camera);
}

// Hands newly loaded block textures to the chunk renderer and the far terrain.
void VoxelRethinkedApp::updateTextures(vxe::EngineContext& context) {
    if (!m_texturePacks || !m_texturePacks->update() || context.renderer == nullptr) {
        return;
    }
    const vxe::render::BlockTextures* textures = m_texturePacks->textures();
    std::array<const vxe::render::PackImage*, vxe::render::kSkyBodyImages.size()> bodies{};
    std::ranges::transform(vxe::render::kSkyBodyImages, bodies.begin(),
                           [this](std::string_view path) { return m_texturePacks->image(path); });
    context.renderer->sky().setBodies(bodies);
    context.renderer->chunks().setTextures(textures->texture, textures->sampler);
    context.renderer->lod().setTextures(textures->texture, textures->sampler,
                                        static_cast<vxe::u32>(m_blocks->textureKeys().size()));
    context.renderer->chunks().setTextureAnimations(m_texturePacks->animations());
    context.renderer->chunks().setTextureLooks(m_blocks->textureLooks());
    context.renderer->chunks().setTintColors(m_blocks->tintColors());
    if (m_demo) {
        m_demo->setTextureColors(textures->averageColors);
    }
    if (const vxe::render::PackImage* font = m_texturePacks->image(kFontImage); font != nullptr && m_ui) {
        // The font and every theme image a pack provides share one atlas.
        std::vector<vxe::ui::AtlasImage> images;
        const auto addImage = [&](std::string_view name) {
            if (const vxe::render::PackImage* image = m_texturePacks->image(std::format("ui/{}.png", name))) {
                images.push_back({.name = std::string(name),
                                  .width = image->width,
                                  .height = image->height,
                                  .pixels = image->pixels});
            }
        };
        std::ranges::for_each(vxe::ui::kThemeImages, addImage);
        std::ranges::for_each(kHudImages, addImage);
        m_blockIcons.bake(*m_blocks, [this](std::string_view key) {
            return m_texturePacks->image(vxe::render::textureFile(key) + ".png");
        });
        for (const BlockIcons::Icon& icon : m_blockIcons.icons()) {
            images.push_back({.name = icon.name,
                              .width = BlockIcons::kSize,
                              .height = BlockIcons::kSize,
                              .pixels = icon.pixels});
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
    updateSounds(context, time);
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

// What surrounds the player, read from the blocks around them. Runs a few
// times per second: ambience fades over seconds, so more would change nothing.
SoundScene VoxelRethinkedApp::soundScene(const vxe::EngineContext& context) const {
    SoundScene scene;
    scene.inWorld = true;
    if (context.renderer != nullptr) {
        scene.daylight = context.renderer->chunks().daylight();
    }
    const vxe::voxel::VoxelWorld& world = m_demo->world();
    const Terrain& terrain = m_demo->terrain();
    const auto floorOf = [](vxe::f64 value) { return static_cast<vxe::i64>(std::floor(value)); };
    const vxe::i64 x = floorOf(m_camera.position.x);
    const vxe::i64 y = floorOf(m_camera.position.y);
    const vxe::i64 z = floorOf(m_camera.position.z);
    const auto wx0 = static_cast<vxe::f64>(x);
    const auto wz0 = static_cast<vxe::f64>(z);
    scene.biome = terrain.roughBiome(wx0, wz0);
    scene.altitude = static_cast<vxe::f32>(y - Terrain::kSeaLevel);
    scene.cover =
        static_cast<vxe::f32>(std::max(terrain.roughGround(wx0, wz0).height - m_camera.position.y, 0.0));
    scene.underWater = m_blocks->isLiquid(world.getBlock({.x = x, .y = y, .z = z}, vxe::voxel::kAirState));
    if (m_player && !m_freeCamera) {
        const PlayerState& player = m_player->state();
        const vxe::f64 speed = std::hypot(player.velocity.x, player.velocity.z);
        scene.walking = player.onGround && !player.flying && speed > 0.8;
        scene.sprinting = speed > 6.0;
        const vxe::voxel::BlockStateId underfoot = world.getBlock({.x = floorOf(player.position.x),
                                                                   .y = floorOf(player.position.y - 0.2),
                                                                   .z = floorOf(player.position.z)},
                                                                  vxe::voxel::kAirState);
        if (m_blocks->isSolid(underfoot)) {
            scene.step = stepSoundOf(m_blocks->stateName(underfoot));
        }
    }
    // Rings of blocks around the player, nearest first: water in earshot.
    constexpr std::array<vxe::i64, 3> kRings = {6, 13, 24};
    constexpr std::array<std::array<vxe::i64, 2>, 8> kWays = {
        {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}}};
    for (const vxe::i64 ring : kRings) {
        for (const auto& way : kWays) {
            const vxe::i64 wx = x + (way[0] * ring);
            const vxe::i64 wz = z + (way[1] * ring);
            for (const vxe::i64 down : {1, 5, 10, 18}) {
                if (!m_blocks->isLiquid(
                        world.getBlock({.x = wx, .y = y - down, .z = wz}, vxe::voxel::kAirState))) {
                    continue;
                }
                const vxe::f32 close = 1.0F - (static_cast<vxe::f32>(ring) / 32.0F);
                vxe::f32& kind = y - down <= Terrain::kSeaLevel ? scene.sea : scene.nearWater;
                kind = std::max(kind, close);
            }
        }
    }
    return scene;
}

void VoxelRethinkedApp::updateSounds(vxe::EngineContext& context, const vxe::FrameTime& time) {
    if (!m_audio) {
        return;
    }
    m_audio->setListener(m_camera.position, m_camera.forward());
    if (m_sounds) {
        const GameState state = m_states.current();
        const bool inMenu = state == GameState::MainMenu;
        if (inMenu != m_wasInMenu) {
            m_wasInMenu = inMenu;
            if (inMenu) {
                m_sounds->enterMenu();
            } else {
                m_sounds->leaveMenu();
            }
        }
        if (m_screens) {
            const vxe::ui::Screen* top = m_screens->top();
            if (top != m_lastScreen && m_lastScreen != nullptr && top != nullptr) {
                m_sounds->screenChanged();
            }
            m_lastScreen = top;
        }
        if (m_uiContext && !m_clickSoundSet) {
            m_clickSoundSet = true;
            m_uiContext->setOnPress([this] { m_sounds->click(); });
        }
        const bool inWorld = m_demo && m_demo->isLoaded() && isWorldState(state);
        m_soundSceneWait -= time.deltaSeconds;
        if (inWorld && m_soundSceneWait <= 0.0) {
            m_soundSceneWait = 0.2;
            m_soundScene = soundScene(context);
            if (m_soundScene.underWater && !m_wasUnderWater) {
                m_sounds->splash(m_camera.position);
            }
            m_wasUnderWater = m_soundScene.underWater;
        } else if (!inWorld) {
            m_soundScene = SoundScene{};
        }
        m_sounds->update(m_soundScene, time.deltaSeconds);
    }
    m_audio->update(time.deltaSeconds);
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
        context.renderer->lod().setTextures({}, {}, 0);
        context.renderer->setUi({});
    }
    m_screens.reset();
    m_uiContext.reset();
    m_ui.reset();
    m_texturePacks.reset();
    m_flight.reset();
    m_sounds.reset();
    m_audio.reset();
    VXE_LOG_INFO(gameLog, "Voxel Rethinked shutting down");
}

} // namespace vr
